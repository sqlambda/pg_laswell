// What a Citus step leaves behind for the steps after it, in the same plan.
//
// The case this exists for is the ordinary one: distribute `accounts`, then
// colocate `account_transactions` with it, then distribute a function against
// that group -- all in one specification. Without projection the second and
// third steps read a catalog where `accounts` is not distributed yet, and both
// would be refused for colocating with something that "is not distributed".
//
// Projected entries carry projected_by_step, the same marker core uses, so a
// reader of the plan can tell a reading from a prediction.

PGLASWELL_PROJECT(kCitusDistributeTable, {
  projected.extensions["citus"]["tables"][qualified] = json{
      {"partmethod", "h"},
      {"distribution_column", in.body.value("distribution_column", "")},
      {"colocationid", 0},
      {"projected_by_step", step.ordinal}};
})

PGLASWELL_PROJECT(kCitusCreateReferenceTable, {
  projected.extensions["citus"]["tables"][qualified] = json{
      {"partmethod", "n"},
      {"distribution_column", ""},
      {"colocationid", 0},
      {"projected_by_step", step.ordinal}};
})

// citus_distribute_function never reaches this switch at runtime: it has an object
// key ("function:schema.name"), so the object branch above returns first. The
// case exists because -Wswitch demands every enum value be named, and that is
// the guarantee being paid for -- the compiler cannot know which branch runs,
// and would rather be told than assume.
//
// Nothing to project: a specification distributing the same function twice is
// a duplicate for the author to fix, not a state a later step reads.
PGLASWELL_PROJECT(kCitusDistributeFunction, )
