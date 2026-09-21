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
