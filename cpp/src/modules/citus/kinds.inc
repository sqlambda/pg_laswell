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
// The paced walk core cannot do here. Core's backfill takes each batch with
// FOR UPDATE over the whole table, which Citus refuses across shards -- so core
// refuses the distributed case and names THIS kind, rather than a module
// rewriting what core emits. See ../README.md.
PGLASWELL_KIND(citus_distributed_backfill,   kCitusDistributedBackfill,  parse_citus_distributed_backfill,   plan_citus_distributed_backfill)
