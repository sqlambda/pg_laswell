// What the self-policing tests must know about this module's kinds.

#ifdef PGLASWELL_DEFERRED_CASE
// The conformance suite runs on a plain PostgreSQL, where TimescaleDB is not
// loaded. cpp/test/timescaledb-tests.sh runs every kind against both editions
// and the oldest supported line (examples/docker/timescale).
PGLASWELL_DEFERRED_CASE("timescaledb_create_hypertable",
    "needs TimescaleDB preloaded and created in the database")
PGLASWELL_DEFERRED_CASE("timescaledb_set_chunk_time_interval",
    "needs TimescaleDB and a hypertable")
PGLASWELL_DEFERRED_CASE("timescaledb_set_columnstore",
    "needs the Timescale License edition and a hypertable")
PGLASWELL_DEFERRED_CASE("timescaledb_add_columnstore_policy",
    "needs the Timescale License edition and a hypertable with the columnstore")
PGLASWELL_DEFERRED_CASE("timescaledb_remove_columnstore_policy",
    "needs the Timescale License edition and a columnstore policy")
PGLASWELL_DEFERRED_CASE("timescaledb_add_retention_policy",
    "needs the Timescale License edition and a hypertable")
PGLASWELL_DEFERRED_CASE("timescaledb_remove_retention_policy",
    "needs the Timescale License edition and a retention policy")
PGLASWELL_DEFERRED_CASE("timescaledb_create_continuous_aggregate",
    "needs the Timescale License edition and a hypertable to aggregate")
PGLASWELL_DEFERRED_CASE("timescaledb_add_continuous_aggregate_policy",
    "needs the Timescale License edition and a continuous aggregate")
#endif

#ifdef PGLASWELL_KIND_BODY
PGLASWELL_KIND_BODY("timescaledb_create_hypertable",
  R"({"kind":"timescaledb_create_hypertable","schema":"s","table":"t","time_column":"ts"})")
PGLASWELL_KIND_BODY("timescaledb_set_chunk_time_interval",
  R"({"kind":"timescaledb_set_chunk_time_interval","schema":"s","table":"t","chunk_time_interval":"1 day"})")
PGLASWELL_KIND_BODY("timescaledb_set_columnstore",
  R"({"kind":"timescaledb_set_columnstore","schema":"s","table":"t","segment_by":["dev"]})")
PGLASWELL_KIND_BODY("timescaledb_add_columnstore_policy",
  R"({"kind":"timescaledb_add_columnstore_policy","schema":"s","table":"t","after":"7 days"})")
PGLASWELL_KIND_BODY("timescaledb_remove_columnstore_policy",
  R"({"kind":"timescaledb_remove_columnstore_policy","schema":"s","table":"t"})")
PGLASWELL_KIND_BODY("timescaledb_add_retention_policy",
  R"({"kind":"timescaledb_add_retention_policy","schema":"s","table":"t","drop_after":"90 days","acknowledge_data_loss":true})")
PGLASWELL_KIND_BODY("timescaledb_remove_retention_policy",
  R"({"kind":"timescaledb_remove_retention_policy","schema":"s","table":"t"})")
PGLASWELL_KIND_BODY("timescaledb_create_continuous_aggregate",
  R"({"kind":"timescaledb_create_continuous_aggregate","schema":"s","name":"v","query":"SELECT 1"})")
PGLASWELL_KIND_BODY("timescaledb_add_continuous_aggregate_policy",
  R"({"kind":"timescaledb_add_continuous_aggregate_policy","schema":"s","name":"v","start_offset":"3 days","end_offset":"1 hour","schedule_interval":"1 hour"})")
#endif
