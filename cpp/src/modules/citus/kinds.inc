// Citus intent kinds. One line each, and the line is the only place a kind is
// declared: the enum arm, the name->kind entry, the parse dispatch and the
// planner dispatch are all generated from here, so they cannot drift apart.
//
//   PGLASWELL_KIND(spec_name, EnumId, parse_fn, plan_fn)
//
// Phase 1 of thoughts/CITUS.md: what the pgshard variants stop dead at.
PGLASWELL_KIND(distribute_table,       kDistributeTable,      parse_distribute_table,       plan_distribute_table)
PGLASWELL_KIND(create_reference_table, kCreateReferenceTable, parse_create_reference_table, plan_create_reference_table)
PGLASWELL_KIND(distribute_function,    kDistributeFunction,   parse_distribute_function,    plan_distribute_function)
