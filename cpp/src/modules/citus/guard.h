#pragma once
// Plan-level guards for a Citus cluster: conditions of the TOPOLOGY, which no
// per-kind planner can own because they are true of every kind at once.

PGLASWELL_PLAN_GUARD(
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
    if (propagation == "off" || propagation == "false") {
      plan.ok = false;
      plan.conflicts.push_back(
          "citus.enable_ddl_propagation is off, so DDL would run on the "
          "coordinator and not on the workers. The coordinator's catalog and "
          "the workers' would diverge, and every reading pg_laswell takes -- "
          "including the check that catches drift -- is taken on the "
          "coordinator, so nothing here would ever report it. Turn it on for "
          "the migration: SET citus.enable_ddl_propagation TO on; (or ALTER "
          "SYSTEM, if it is off by default here).");
    }
  }
)
