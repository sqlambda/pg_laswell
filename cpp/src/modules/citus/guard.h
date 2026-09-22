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
    // WHAT ACTUALLY FAILS ON A DISTRIBUTED TABLE, per form rather than per kind.
    //
    // This used to refuse backfill, update_rows, delete_rows and merge_rows
    // alike, all with one explanation about FOR UPDATE. Measured on Citus 13,
    // coordinator and two workers, by preparing and EXECUTING each emitted
    // statement against a distributed table:
    //
    //   backfill                     FAILS -- multi-shard FOR UPDATE
    //   delete_rows with `where`     FAILS -- same, same statement shape
    //   merge_rows                   FAILS -- but for MERGE, not FOR UPDATE:
    //                                "non-IMMUTABLE functions are not yet
    //                                 supported in MERGE sql with distributed
    //                                 tables"
    //   update_rows                  WORKS  -- was refused anyway
    //   delete_rows with `values`    WORKS  -- was refused anyway
    //   insert_rows                  WORKS  -- was never refused
    //   copy_rows                    WORKS  -- Citus routes COPY natively
    //
    // The reason only the first two take FOR UPDATE is that only they walk the
    // TARGET table by key. The rest get their rows from the specification, join
    // to them, and take no row locks -- so the explanation was wrong for every
    // kind it was wrong to refuse, which is how over-refusing survived: the
    // message sounded like it applied.
    for (const auto& in : spec.intents) {
      const auto qualified = in.qualified_table();
      const auto tables = citus.value("tables", json::object());
      if (!tables.contains(qualified)) continue;  // local table: unaffected
      const auto method = tables[qualified].value("partmethod", "");
      if (method == "n") continue;  // reference table: one placement per node

      // A keyset walk over the target takes each batch with FOR UPDATE, which is
      // what stops the cursor selecting the same rows twice. Citus refuses that
      // on a multi-shard query, so this is not a performance judgement -- it
      // does not run at all, and without the refusal it would be accepted here
      // and fail partway through with earlier batches already committed.
      //
      // Liftable, and intended to be: confining a batch to ONE shard makes
      // FOR UPDATE legal again (measured: an equality filter on the distribution
      // column returns rows under FOR UPDATE where the multi-shard form
      // errors). That needs the walk to iterate distribution values, which is a
      // change to how the executor walks rather than to what it refuses.
      const bool target_keyset_walk =
          in.kind == IntentKind::kBackfill ||
          (in.kind == IntentKind::kDeleteRows && !in.body.contains("values") &&
           !in.body.contains("select"));
      if (target_keyset_walk) {
        refuse(
            "\"" + in.kind_name + "\" on " + qualified +
            " walks the table by key, and " + qualified +
            " is distributed. Each batch is taken with FOR UPDATE so the keyset "
            "cursor cannot select the same rows twice, and Citus refuses that on "
            "a multi-shard query: \"could not run distributed query with FOR "
            "UPDATE/SHARE commands\". It would be accepted here and fail partway "
            "through, with earlier batches already committed. Supply the rows "
            "explicitly instead -- update_rows, delete_rows with values, and "
            "insert_rows all run on a distributed table -- or undistribute the "
            "table for the migration.");
        continue;
      }

      // MERGE is refused for its own reasons, which have nothing to do with
      // pacing or row locks. Naming the right restriction matters: told it was
      // about FOR UPDATE, an author would look for a way to avoid a row lock
      // that this statement never takes.
      if (in.kind == IntentKind::kMergeRows) {
        refuse(
            "\"merge_rows\" on " + qualified + " cannot run: " + qualified +
            " is distributed, and Citus restricts MERGE against a distributed "
            "target -- measured: \"non-IMMUTABLE functions are not yet supported "
            "in MERGE sql with distributed tables\". This is a limit of MERGE on "
            "Citus, not of the pacing. Express the change as update_rows and "
            "insert_rows, which both run here.");
        continue;
      }
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
