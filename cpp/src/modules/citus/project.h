#pragma once
// What a Citus step leaves behind for the steps after it, in the same plan.
//
// NO namespace of its own: included INSIDE namespace pglaswell, like the rest
// of the module.
//
// Each function receives `mine` -- the module's OWN subtree of
// Observations::extensions -- and not the projected catalog. That is
// deliberate, and it is the whole reason these are functions rather than the
// blocks they used to be. As blocks they inherited the projection function's
// scope, which meant a module could write `projected.tables[...]` and change
// how a CORE kind plans two steps later, silently, with nothing in the build
// objecting. THE RULE (see ../README.md) forbids that; passing one json& makes
// it unsayable rather than merely disallowed.

// Distributing a table: the ordinary case this exists for is distribute
// `accounts`, then colocate `account_transactions` with it, then distribute a
// function against that group -- all in one specification. Without projection
// the second and third steps read a catalog where `accounts` is not
// distributed yet, and both would be refused for colocating with something
// that "is not distributed".
//
// projected_by_step is the same marker core uses, so a reader of the plan can
// tell a reading from a prediction.
inline void citus_project_distribute_table(const Intent& in,
                                          const std::string& qualified,
                                          const Step& step, json& mine) {
  mine["tables"][qualified] = json{
      {"partmethod", "h"},
      {"repmodel", "s"},
      {"distribution_column", in.body.value("distribution_column", "")},
      {"colocationid", 0},
      {"projected_by_step", step.ordinal}};
}

// A reference table is partmethod 'n' with no distribution column: one copy on
// every node, which is what a later colocate_with => 'none' reads.
inline void citus_project_reference_table(const Intent&,
                                         const std::string& qualified,
                                         const Step& step, json& mine) {
  // repmodel 't' is what makes partmethod 'n' a REFERENCE table rather than a
  // Citus-managed local one; readers tell the two apart by it.
  mine["tables"][qualified] = json{{"partmethod", "n"},
                                  {"repmodel", "t"},
                                  {"distribution_column", ""},
                                  {"colocationid", 0},
                                  {"projected_by_step", step.ordinal}};
}

// Named, rather than an empty block, so that "this kind projects nothing" is a
// statement someone wrote on purpose and not an arm left half-finished.
inline void citus_project_nothing(const Intent&, const std::string&, const Step&,
                                 json&) {}

// Altering a distribution: the column and group a later step reads. The group
// id of a colocation target is copied when the reading has it; a new group
// ("none", or a shard-count change that leaves the group) has no id until it
// exists, and 0 is what a distribution projected in this plan carries too.
inline void citus_project_alter_distributed_table(const Intent& in,
                                                 const std::string& qualified,
                                                 const Step& step, json& mine) {
  auto& entry = mine["tables"][qualified];
  if (!entry.is_object()) entry = json::object();
  if (in.body.contains("distribution_column")) {
    entry["distribution_column"] = in.body["distribution_column"];
    entry.erase("distribution_type_oid");
  }
  if (in.body.contains("shard_count")) entry["shard_count"] = in.body["shard_count"];
  entry["colocationid"] = step.detail.value("colocation_group", 0LL);
  entry["projected_by_step"] = step.ordinal;
}

// Undistributed: Citus no longer records it, which is what a later
// citus_distribute_table in the same plan has to read.
inline void citus_project_undistribute_table(const Intent&,
                                            const std::string& qualified,
                                            const Step&, json& mine) {
  if (mine.contains("tables")) mine["tables"].erase(qualified);
}

// A Citus-managed local table: partmethod 'n', repmodel 's'.
inline void citus_project_local_table(const Intent&, const std::string& qualified,
                                     const Step& step, json& mine) {
  mine["tables"][qualified] = json{{"partmethod", "n"},
                                  {"repmodel", "s"},
                                  {"distribution_column", ""},
                                  {"colocationid", 0},
                                  {"projected_by_step", step.ordinal}};
}
