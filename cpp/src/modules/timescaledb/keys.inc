// TimescaleDB's contributions to conflict_keys(). The kinds that name a table
// are keyed on it by core; the continuous-aggregate kinds name a view instead,
// and two specifications creating or scheduling the same one must not run at
// once.
#ifdef PGLASWELL_CONFLICT_KEYS
PGLASWELL_CONFLICT_KEYS(kTimescaledbCreateContinuousAggregate, {
  add("timescaledb:cagg:" + in.body.value("schema", "") + "." + in.body.value("name", ""));
})
PGLASWELL_CONFLICT_KEYS(kTimescaledbAddContinuousAggregatePolicy, {
  add("timescaledb:cagg:" + in.body.value("schema", "") + "." + in.body.value("name", ""));
})
#endif
