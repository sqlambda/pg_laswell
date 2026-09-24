// Citus's contributions to object_key_for() and conflict_keys().
//
// Included twice, once inside each switch, with only the macro that switch
// defines expanded. The other is a no-op there.

#ifdef PGLASWELL_OBJECT_KEY
// citus_distribute_function plans against a FUNCTION, not a relation. A kind missing
// from object_key_for reads back as absent -- which is what cost a whole batch
// of ALTER kinds before the pairing test existed.
PGLASWELL_OBJECT_KEY(kCitusDistributeFunction, "function:" + schema + "." + name)
#endif

#ifdef PGLASWELL_CONFLICT_KEYS
// The edge single-node PostgreSQL has no equivalent of: colocation. Two tables
// in one colocation group affect each other even though they share no name,
// because distributing into a group, or rebalancing it, moves shards for every
// member. Keyed on the GROUP the spec names, since the group id itself is a
// catalog fact the parser cannot see.
//
// When a spec colocates with nothing the edge is absent rather than empty --
// and the repository denies concurrency on incomplete evidence, which is the
// answer that keeps this safe either way.
PGLASWELL_CONFLICT_KEYS(kCitusDistributeTable, {
  const auto with = in.body.value("colocate_with", "");
  if (!with.empty() && with != "none" && with != "default") {
    add("colocation:" + with);
  }
})
PGLASWELL_CONFLICT_KEYS(kCitusDistributeFunction, {
  const auto with = in.body.value("colocate_with", "");
  if (!with.empty() && with != "none" && with != "default") {
    add("colocation:" + with);
  }
})
// Altering a distribution moves every shard of the table, and with
// cascade_to_colocated every member of its group -- so it conflicts with
// anything keyed on its own group as well as on the group it joins.
PGLASWELL_CONFLICT_KEYS(kCitusAlterDistributedTable, {
  add("colocation:" + in.body.value("schema", "") + "." + in.body.value("table", ""));
  const auto with = in.body.value("colocate_with", "");
  if (!with.empty() && with != "none" && with != "default") {
    add("colocation:" + with);
  }
})
PGLASWELL_CONFLICT_KEYS(kCitusUndistributeTable, {
  add("colocation:" + in.body.value("schema", "") + "." + in.body.value("table", ""));
})
// The cluster kinds all touch the one topology every shard lives on: two of
// them at once -- a drain while a rebalance runs -- are Citus's own conflict
// to resolve, and the repository should not start them together.
PGLASWELL_CONFLICT_KEYS(kCitusAddNode, { add("citus:topology"); })
PGLASWELL_CONFLICT_KEYS(kCitusRemoveNode, { add("citus:topology"); })
PGLASWELL_CONFLICT_KEYS(kCitusSetCoordinatorHost, { add("citus:topology"); })
PGLASWELL_CONFLICT_KEYS(kCitusSetNodeProperty, { add("citus:topology"); })
PGLASWELL_CONFLICT_KEYS(kCitusDrainNode, { add("citus:topology"); })
PGLASWELL_CONFLICT_KEYS(kCitusRebalanceShards, { add("citus:topology"); })
#endif
