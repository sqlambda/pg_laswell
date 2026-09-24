// Citus intent kinds. One line each, and the line is the only place a kind is
// declared: the enum arm, the name->kind entry, the parse dispatch and the
// planner dispatch are all generated from here, so they cannot drift apart.
//
//   PGLASWELL_KIND(spec_name, EnumId, parse_fn, plan_fn)
//
// Phase 1 of thoughts/CITUS.md: what the pgshard variants stop dead at.
PGLASWELL_KIND(citus_distribute_table,       kCitusDistributeTable,      parse_citus_distribute_table,       plan_citus_distribute_table)
PGLASWELL_KIND(citus_create_reference_table, kCitusCreateReferenceTable, parse_citus_create_reference_table, plan_citus_create_reference_table)
PGLASWELL_KIND(citus_distribute_function,    kCitusDistributeFunction,   parse_citus_distribute_function,    plan_citus_distribute_function)
//
// Phase 2: the lifecycle of a table once Citus holds it. No undistribute kind
// for functions: Citus has no call that reverses create_distributed_function.
PGLASWELL_KIND(citus_alter_distributed_table,        kCitusAlterDistributedTable,       parse_citus_alter_distributed_table,        plan_citus_alter_distributed_table)
PGLASWELL_KIND(citus_undistribute_table,             kCitusUndistributeTable,           parse_citus_undistribute_table,             plan_citus_undistribute_table)
PGLASWELL_KIND(citus_add_local_table_to_metadata,    kCitusAddLocalTableToMetadata,     parse_citus_add_local_table_to_metadata,    plan_citus_add_local_table_to_metadata)
PGLASWELL_KIND(citus_truncate_local_data,            kCitusTruncateLocalData,           parse_citus_truncate_local_data,            plan_citus_truncate_local_data)
//
// Phase 3: the cluster itself. No kind moves or splits a shard by id: shard
// ids come from a per-database sequence, so a specification naming one means a
// different shard, or none, on every other target. See parse.h.
PGLASWELL_KIND(citus_add_node,                       kCitusAddNode,                     parse_citus_add_node,                       plan_citus_add_node)
PGLASWELL_KIND(citus_remove_node,                    kCitusRemoveNode,                  parse_citus_remove_node,                    plan_citus_remove_node)
PGLASWELL_KIND(citus_set_coordinator_host,           kCitusSetCoordinatorHost,          parse_citus_set_coordinator_host,           plan_citus_set_coordinator_host)
PGLASWELL_KIND(citus_set_node_property,              kCitusSetNodeProperty,             parse_citus_set_node_property,              plan_citus_set_node_property)
PGLASWELL_KIND(citus_drain_node,                     kCitusDrainNode,                   parse_citus_drain_node,                     plan_citus_drain_node)
PGLASWELL_KIND(citus_rebalance_shards,               kCitusRebalanceShards,             parse_citus_rebalance_shards,               plan_citus_rebalance_shards)
