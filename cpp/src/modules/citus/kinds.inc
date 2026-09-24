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
