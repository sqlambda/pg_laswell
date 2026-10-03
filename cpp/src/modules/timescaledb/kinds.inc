// TimescaleDB intent kinds. One line each: the enum arm, the name->kind entry,
// the parse dispatch and the planner dispatch are all generated from here.
//
//   PGLASWELL_KIND(spec_name, EnumId, parse_fn, plan_fn)
//
// Hypertables: in both editions.
PGLASWELL_KIND(timescaledb_create_hypertable,        kTimescaledbCreateHypertable,       parse_timescaledb_create_hypertable,        plan_timescaledb_create_hypertable)
PGLASWELL_KIND(timescaledb_set_chunk_time_interval,  kTimescaledbSetChunkTimeInterval,   parse_timescaledb_set_chunk_time_interval,  plan_timescaledb_set_chunk_time_interval)
//
// The columnstore, its policy, retention and continuous aggregates: the
// Timescale License edition only. A build of the Apache edition (`-oss`, and
// PGDG's and Debian's +dfsg packages) refuses each with "not supported under
// the current \"apache\" license" -- so these are refused before anything runs,
// naming the edition the server reports.
PGLASWELL_KIND(timescaledb_set_columnstore,          kTimescaledbSetColumnstore,         parse_timescaledb_set_columnstore,          plan_timescaledb_set_columnstore)
PGLASWELL_KIND(timescaledb_add_columnstore_policy,   kTimescaledbAddColumnstorePolicy,   parse_timescaledb_add_columnstore_policy,   plan_timescaledb_add_columnstore_policy)
PGLASWELL_KIND(timescaledb_remove_columnstore_policy, kTimescaledbRemoveColumnstorePolicy, parse_timescaledb_remove_columnstore_policy, plan_timescaledb_remove_columnstore_policy)
PGLASWELL_KIND(timescaledb_add_retention_policy,     kTimescaledbAddRetentionPolicy,     parse_timescaledb_add_retention_policy,     plan_timescaledb_add_retention_policy)
PGLASWELL_KIND(timescaledb_remove_retention_policy,  kTimescaledbRemoveRetentionPolicy,  parse_timescaledb_remove_retention_policy,  plan_timescaledb_remove_retention_policy)
PGLASWELL_KIND(timescaledb_create_continuous_aggregate, kTimescaledbCreateContinuousAggregate, parse_timescaledb_create_continuous_aggregate, plan_timescaledb_create_continuous_aggregate)
PGLASWELL_KIND(timescaledb_add_continuous_aggregate_policy, kTimescaledbAddContinuousAggregatePolicy, parse_timescaledb_add_continuous_aggregate_policy, plan_timescaledb_add_continuous_aggregate_policy)
