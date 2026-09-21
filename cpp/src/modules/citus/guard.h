#pragma once
// Plan-level refusals for a Citus cluster: conditions of the TOPOLOGY, which no
// per-kind planner can own because they are true of every kind at once.
//
// A FUNCTION, not a block, and it receives (spec, obs, refuse) -- deliberately
// NOT the plan. A guard can say no and say why; it cannot reach what core
// emits. An audit found the earlier block form compiled happily while pushing
// a warning, which is the forbidden shape available by accident.
template <typename Refuse>
inline void citus_plan_refusals(const Spec& spec, const Observations& obs,
                                const Refuse& refuse) {

  const auto& citus = obs.extension("citus");
  if (!citus.empty()) {
    const auto settings = citus.value("settings", json::object());
    const auto propagation = settings.value("enable_ddl_propagation", "");

    // DECIDED: a hard refusal, not a warning. CITUS.md §10 asks for the
    // decision and this is it, with the reason.
    //
    // With citus.enable_ddl_propagation off, DDL runs on the coordinator and
    // NOT on the workers. The coordinator's catalog and the workers' then
    // diverge -- and they diverge SILENTLY, because every reading pg_laswell
    // takes, including the drift check itself, is taken on the coordinator.
    // The ledger would record a migration as fully applied, the coordinator
    // would agree, and the shards would not have it.
    //
    // That is the precise failure the whole repository model exists to catch,
    // occurring one level below where the repository can see. A warning would
    // be a false reassurance: the run succeeds, the output looks right, and
    // the cluster is broken in a way nothing here will ever report. Refusing
    // is recoverable in one statement; silent divergence is not recoverable at
    // all without comparing every worker by hand.
    //
    // This applies to EVERY kind, not just this module's: a plain create_table
    // on a Citus cluster is propagated DDL too.
    // PACED DML AGAINST A DISTRIBUTED TABLE IS REFUSED, and this is not a
    // performance judgement -- it does not work at all.
    //
    // The paced walk takes its batch with FOR UPDATE, which is what makes the
    // keyset cursor safe: without it two batches could select the same rows.
    // Citus rejects that outright on a multi-shard query:
    //
    //   ERROR: could not run distributed query with FOR UPDATE/SHARE commands
    //
    // So today this is emitted, accepted by the planner, and fails AT
    // EXECUTION -- partway through a migration, on a cluster, with earlier
    // batches already committed. Refusing beforehand and naming the reading is
    // what this tool does everywhere else.
    //
    // It is liftable: confining a batch to ONE shard makes FOR UPDATE legal
    // again, measured at Task Count 1. That is shard-aligned batching, and it
    // is a change to how the executor walks rather than to what it refuses.
    // Until it exists, this is the honest answer.
    for (const auto& in : spec.intents) {
      const bool paced_dml = in.kind == IntentKind::kBackfill ||
                             in.kind == IntentKind::kUpdateRows ||
                             in.kind == IntentKind::kDeleteRows ||
                             in.kind == IntentKind::kMergeRows;
      if (!paced_dml) continue;
      const auto qualified = in.qualified_table();
      const auto tables = citus.value("tables", json::object());
      if (!tables.contains(qualified)) continue;  // local table: unaffected
      const auto method = tables[qualified].value("partmethod", "");
      if (method == "n") continue;  // reference table: single placement per node
      refuse(
          "\"" + in.kind_name + "\" on " + qualified +
          " is a paced walk, and " + qualified +
          " is distributed. The walk takes each batch with FOR UPDATE so the "
          "keyset cursor cannot select the same rows twice, and Citus refuses "
          "that on a multi-shard query: \"could not run distributed query with "
          "FOR UPDATE/SHARE commands\". It would be accepted here and fail "
          "partway through, with earlier batches already committed. Until "
          "batches are confined to one shard, run this change through a "
          "single-shard path or undistribute the table first.");
    }

    if (propagation == "off" || propagation == "false") {
      refuse(
          "citus.enable_ddl_propagation is off, so DDL would run on the "
          "coordinator and not on the workers. The coordinator's catalog and "
          "the workers' would diverge, and every reading pg_laswell takes -- "
          "including the check that catches drift -- is taken on the "
          "coordinator, so nothing here would ever report it. Turn it on for "
          "the migration: SET citus.enable_ddl_propagation TO on; (or ALTER "
          "SYSTEM, if it is off by default here).");
    }
  }

}
