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

// --- Node membership --------------------------------------------------------
//
// The node kinds change `nodes`, and a later intent in the same specification
// reads the cluster as they will have left it -- the rule core teaches for
// tables, reported from pgshard as missing here: registering the coordinator
// and then setting its property was refused, "not a node of this cluster".
//
// A projected node carries groupid -1 (Citus assigns the real one) and
// placements 0 (a new node holds nothing until a rebalance). Any change of
// membership also marks the rebalance plan read at the start of the
// specification as stale: it was computed for a different set of nodes, so a
// later citus_rebalance_shards must not read it as "balanced".

inline json* citus_projected_node(json& mine, const std::string& host, int port) {
  if (!mine["nodes"].is_array()) mine["nodes"] = json::array();
  for (auto& n : mine["nodes"]) {
    if (n.value("nodename", "") == host && n.value("nodeport", 0) == port) return &n;
  }
  return nullptr;
}

inline void citus_recount_workers(json& mine, const Step& step) {
  int workers = 0;
  for (const auto& n : mine["nodes"]) {
    if (n.value("groupid", -1) != 0 && n.value("isactive", false) &&
        n.value("noderole", "") == "primary") {
      ++workers;
    }
  }
  mine["worker_count"] = workers;
  mine["rebalance_moves_stale_after_step"] = step.ordinal;
}

inline void citus_project_add_node_entry(json& mine, const std::string& host, int port,
                                         bool shards, const Step& step) {
  if (auto* existing = citus_projected_node(mine, host, port)) {
    (*existing)["shouldhaveshards"] = shards;
    (*existing)["projected_by_step"] = step.ordinal;
    return;
  }
  mine["nodes"].push_back(json{{"nodename", host}, {"nodeport", port},
                               {"groupid", -1}, {"isactive", true},
                               {"noderole", "primary"}, {"shouldhaveshards", shards},
                               {"placements", 0}, {"projected_by_step", step.ordinal}});
}

inline void citus_project_add_node(const Intent& in, const std::string&,
                                   const Step& step, json& mine) {
  citus_project_add_node_entry(mine, in.body.value("host", ""),
                               in.body.value("port", 5432),
                               in.body.value("should_have_shards", true), step);
  citus_recount_workers(mine, step);
}

inline void citus_project_remove_node(const Intent& in, const std::string&,
                                      const Step& step, json& mine) {
  if (!mine["nodes"].is_array()) return;
  json kept = json::array();
  for (const auto& n : mine["nodes"]) {
    if (n.value("nodename", "") == in.body.value("host", "") &&
        n.value("nodeport", 0) == in.body.value("port", 5432)) {
      continue;
    }
    kept.push_back(n);
  }
  mine["nodes"] = kept;
  citus_recount_workers(mine, step);
}

// The coordinator is the node in group 0; Citus registers it taking no shards.
inline void citus_project_coordinator_host(const Intent& in, const std::string&,
                                           const Step& step, json& mine) {
  if (!mine["nodes"].is_array()) mine["nodes"] = json::array();
  for (auto& n : mine["nodes"]) {
    if (n.value("groupid", -1) == 0) {
      n["nodename"] = in.body.value("host", "");
      n["nodeport"] = in.body.value("port", 5432);
      n["projected_by_step"] = step.ordinal;
      return;
    }
  }
  mine["nodes"].push_back(json{{"nodename", in.body.value("host", "")},
                               {"nodeport", in.body.value("port", 5432)},
                               {"groupid", 0}, {"isactive", true},
                               {"noderole", "primary"}, {"shouldhaveshards", false},
                               {"placements", 0}, {"projected_by_step", step.ordinal}});
}

inline void citus_project_node_property(const Intent& in, const std::string&,
                                        const Step& step, json& mine) {
  auto* n = citus_projected_node(mine, in.body.value("host", ""),
                                 in.body.value("port", 5432));
  if (n == nullptr) return;
  (*n)["shouldhaveshards"] = in.body.value("should_have_shards", true);
  (*n)["projected_by_step"] = step.ordinal;
  mine["rebalance_moves_stale_after_step"] = step.ordinal;
}

// Drained: nothing of a distributed table left on it, and nothing new arriving.
// Where the shards went is Citus's choice, so the other nodes' counts are left
// as read -- and the rebalance plan is stale.
inline void citus_project_drain_node(const Intent& in, const std::string&,
                                     const Step& step, json& mine) {
  auto* n = citus_projected_node(mine, in.body.value("host", ""),
                                 in.body.value("port", 5432));
  if (n == nullptr) return;
  (*n)["shouldhaveshards"] = false;
  (*n)["placements"] = 0;
  (*n)["projected_by_step"] = step.ordinal;
  mine["rebalance_moves_stale_after_step"] = step.ordinal;
}

// The configured workers are what the step resolved and recorded: it adds
// `adding` and removes `removing`, and the projection repeats exactly that.
inline void citus_project_ensure_workers(const Intent&, const std::string&,
                                         const Step& step, json& mine) {
  const auto split = [](const std::string& hp, std::string& host, int& port) {
    const auto colon = hp.rfind(':');
    host = hp.substr(0, colon);
    port = std::stoi(hp.substr(colon + 1));
  };
  for (const auto& a : step.detail.value("adding", json::array())) {
    std::string host; int port = 5432;
    split(a.get<std::string>(), host, port);
    citus_project_add_node_entry(mine, host, port, true, step);
  }
  for (const auto& r : step.detail.value("removing", json::array())) {
    std::string host; int port = 5432;
    split(r.get<std::string>(), host, port);
    json kept = json::array();
    for (const auto& n : mine["nodes"]) {
      if (n.value("nodename", "") == host && n.value("nodeport", 0) == port) continue;
      kept.push_back(n);
    }
    mine["nodes"] = kept;
  }
  citus_recount_workers(mine, step);
}

// After a rebalance the cluster is balanced by Citus's own definition.
inline void citus_project_rebalance(const Intent&, const std::string&,
                                    const Step& step, json& mine) {
  mine["rebalance_moves"] = json::array();
  mine.erase("rebalance_moves_stale_after_step");
  (void)step;
}

// Disabled or active: the node stays, its isactive changes, and with it the set
// of nodes that take work -- so the rebalance plan read earlier is stale.
inline void citus_project_node_active(json& mine, const Intent& in, const Step& step,
                                      bool active) {
  auto* n = citus_projected_node(mine, in.body.value("host", ""),
                                 in.body.value("port", 5432));
  if (n == nullptr) return;
  (*n)["isactive"] = active;
  (*n)["projected_by_step"] = step.ordinal;
  citus_recount_workers(mine, step);
}
inline void citus_project_disable_node(const Intent& in, const std::string&,
                                       const Step& step, json& mine) {
  citus_project_node_active(mine, in, step, false);
}
inline void citus_project_activate_node(const Intent& in, const std::string&,
                                        const Step& step, json& mine) {
  citus_project_node_active(mine, in, step, true);
}
