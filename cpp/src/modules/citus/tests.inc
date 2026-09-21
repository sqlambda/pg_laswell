// What the self-policing tests must know about this module's kinds.
//
// Generated into the core tests rather than written there, because these kinds
// do not exist at all when the module is disabled -- a literal naming
// "distribute_table" in test_main.cpp would not compile for a PostgreSQL-only
// build.

#ifdef PGLASWELL_NEEDS_OBJECT
// distribute_function plans against a FUNCTION, so object_key_for must answer
// for it. The pairing test exists because a kind missing there read back as
// absent for a whole batch of kinds, found only end to end.
PGLASWELL_NEEDS_OBJECT("distribute_function")
#endif

#ifdef PGLASWELL_DEFERRED_CASE
// None of these can be proved against a plain PostgreSQL: they need a Citus
// coordinator with workers attached. Deferred with a reason, exactly as the
// replication kinds are, so CoversEveryIntentKind stays a forcing function
// rather than being weakened.
PGLASWELL_DEFERRED_CASE("distribute_table",
    "needs a Citus coordinator with at least one worker; proving shard "
    "placement and the concurrent path needs two")
PGLASWELL_DEFERRED_CASE("create_reference_table",
    "needs a Citus cluster: a reference table is replicated to every node, and "
    "one node cannot show that")
PGLASWELL_DEFERRED_CASE("distribute_function",
    "needs a Citus cluster and a distributed table to colocate against")
#endif

#ifdef PGLASWELL_KIND_BODY
// One representative body per kind, for the governing invariant's own test:
// a kind that is KNOWN must be IMPLEMENTED, and a body that parses and plans is
// what proves the second half. A kind listed here with no planner would fail
// the same test.
PGLASWELL_KIND_BODY("distribute_table",
  R"({"kind":"distribute_table","schema":"s","table":"t",
      "distribution_column":"id"})")
PGLASWELL_KIND_BODY("create_reference_table",
  R"({"kind":"create_reference_table","schema":"s","table":"t"})")
PGLASWELL_KIND_BODY("distribute_function",
  R"({"kind":"distribute_function","schema":"s","name":"f",
      "arguments":"integer"})")
#endif
