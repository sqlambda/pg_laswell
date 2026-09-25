// Which Citus kind projects what. One arm per kind, because -Wswitch demands
// every enum value the module declared be named here -- that demand is the
// guarantee being paid for.
//
// The arms name functions from project.h and pass no scope: see the comment
// there for why, and ../README.md for the rule it enforces.

PGLASWELL_PROJECT(kCitusDistributeTable, citus_project_distribute_table)
PGLASWELL_PROJECT(kCitusCreateReferenceTable, citus_project_reference_table)

// citus_distribute_function never reaches this switch at runtime: it has an
// object key ("function:schema.name"), so the object branch above returns
// first. The arm exists because the compiler cannot know which branch runs, and
// would rather be told than assume.
//
// Nothing to project: a specification distributing the same function twice is
// a duplicate for the author to fix, not a state a later step reads.
PGLASWELL_PROJECT(kCitusDistributeFunction, citus_project_nothing)
PGLASWELL_PROJECT(kCitusAlterDistributedTable, citus_project_alter_distributed_table)
PGLASWELL_PROJECT(kCitusUndistributeTable, citus_project_undistribute_table)
PGLASWELL_PROJECT(kCitusAddLocalTableToMetadata, citus_project_local_table)
// The coordinator's stale copy is not something a later step reads.
PGLASWELL_PROJECT(kCitusTruncateLocalData, citus_project_nothing)
// The cluster kinds name no table; core dispatches them before its tables
// guard, and each projects the node list a later intent reads.
PGLASWELL_PROJECT(kCitusAddNode, citus_project_add_node)
PGLASWELL_PROJECT(kCitusRemoveNode, citus_project_remove_node)
PGLASWELL_PROJECT(kCitusSetCoordinatorHost, citus_project_coordinator_host)
PGLASWELL_PROJECT(kCitusSetNodeProperty, citus_project_node_property)
PGLASWELL_PROJECT(kCitusDrainNode, citus_project_drain_node)
PGLASWELL_PROJECT(kCitusRebalanceShards, citus_project_rebalance)
PGLASWELL_PROJECT(kCitusEnsureWorkers, citus_project_ensure_workers)
PGLASWELL_PROJECT(kCitusDisableNode, citus_project_disable_node)
PGLASWELL_PROJECT(kCitusActivateNode, citus_project_activate_node)
// Sizes in metadata are not something a later step reads.
PGLASWELL_PROJECT(kCitusUpdateTableStatistics, citus_project_nothing)
