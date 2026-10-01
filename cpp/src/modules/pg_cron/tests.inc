// What the self-policing tests must know about this module's kinds. Generated
// into the core tests, because these kinds do not exist when the module is
// disabled.

#ifdef PGLASWELL_DEFERRED_CASE
// The conformance suite runs against a plain PostgreSQL, where pg_cron is not
// loaded. cpp/test/extensions-tests.sh runs both kinds against servers that
// have it, in both layouts (examples/docker/extensions).
PGLASWELL_DEFERRED_CASE("pg_cron_schedule",
    "needs pg_cron in shared_preload_libraries and created in the database "
    "cron.database_name names")
PGLASWELL_DEFERRED_CASE("pg_cron_unschedule",
    "needs pg_cron in shared_preload_libraries and a job to remove")
#endif

#ifdef PGLASWELL_KIND_BODY
PGLASWELL_KIND_BODY("pg_cron_schedule",
  R"({"kind":"pg_cron_schedule","name":"nightly","schedule":"0 3 * * *",
      "command":"SELECT 1"})")
PGLASWELL_KIND_BODY("pg_cron_unschedule",
  R"({"kind":"pg_cron_unschedule","name":"nightly"})")
#endif
