#pragma once
// Citus planning. PURE: a function of (Intent, Observations, ExecutorConfig)
// and nothing else. No connection, no clock, no query.
//
// Every refusal here is a Citus error that would otherwise surface at
// EXECUTION, halfway through a migration, on a cluster. All of them are
// derivable from the catalog beforehand, which is the same bargain
// drop_constraint already makes.
// NO namespace of its own: this header is included INSIDE namespace pglaswell,
// next to the helpers it is written against. Opening the namespace again would
// nest it -- pglaswell::pglaswell -- and the generated dispatch would not find
// anything.

// Above this many estimated rows, distributing blocks writes long enough to
// matter and the concurrent path earns its extra machinery. Stated as a
// constant rather than buried in a branch, so the plan can quote it.
inline constexpr long long kCitusConcurrentRowThreshold = 100000;

// The whole reading, or a sentence saying there is none. Used everywhere a
// refusal has to name what decided it.
inline std::string citus_reading(const json& citus) {
  if (citus.empty()) return "the citus extension is not installed here";
  return "citus " + citus.value("version", "?");
}

inline const json& citus_table(const json& citus, const std::string& qualified) {
  static const json kEmpty = json::object();
  const auto tables = citus.find("tables");
  if (tables == citus.end()) return kEmpty;
  const auto it = tables->find(qualified);
  return it == tables->end() ? kEmpty : *it;
}

// Shared preamble: is Citus here at all, does the table exist, is it already
// distributed. Returns false when the step is finished and the caller should
// stop.
inline bool citus_precondition(const Intent& in, const Observations& obs,
                               Step& step, Plan& plan, const json& citus,
                               const std::string& qualified,
                               bool& already_distributed) {
  if (citus.empty()) {
    step.action = Action::kConflict;
    step.why = "the citus extension is not installed in this database, so " +
               in.kind_name + " cannot be planned";
    plan.conflicts.push_back(step.why);
    plan.prerequisites.push_back(json{
        {"kind", "extension"},
        {"target", "citus"},
        {"where", "this database, as a superuser"},
        {"requirement", "CREATE EXTENSION citus; and add this node to a cluster"},
        {"verify", "SELECT extversion FROM pg_extension WHERE extname = 'citus'"},
        {"blocking", true}});
    return false;
  }
  const auto& t = obs.table(qualified);
  if (!t.value("exists", false)) {
    step.action = Action::kConflict;
    step.why = qualified + " does not exist, so it cannot be distributed";
    plan.conflicts.push_back(step.why);
    return false;
  }
  already_distributed = !citus_table(citus, qualified).empty();
  return true;
}

// The refusal the document calls the single most common Citus failure: a hash
// distributed table cannot carry a PRIMARY KEY or UNIQUE constraint that omits
// the distribution column, because uniqueness cannot be enforced across shards
// without it. Readable from the constraints already gathered for every table.
inline bool citus_unique_keys_include(const json& t, const std::string& column,
                                      std::string& offender,
                                      std::string& offender_columns) {
  const json constraints = t.value("constraints", json::object());
  for (auto it = constraints.begin(); it != constraints.end(); ++it) {
    const auto type = it.value().value("type", "");
    if (type != "p" && type != "u") continue;
    const auto cols = it.value().value("columns", json::array());
    bool found = false;
    std::string listed;
    for (const auto& c : cols) {
      const auto name = c.get<std::string>();
      if (!listed.empty()) listed += ", ";
      listed += name;
      if (name == column) found = true;
    }
    if (!found) {
      offender = it.key();
      offender_columns = listed;
      return false;
    }
  }
  return true;
}

inline void plan_distribute_table(const Intent& in, const Observations& obs,
                                  const ExecutorConfig& cfg, Plan& plan,
                                  std::vector<Step>& out) {
  (void)cfg;
  Step step;
  step.kind = in.kind_name;
  struct Emit { std::vector<Step>& o; Step& s; ~Emit() { o.push_back(s); } } emit{out, step};

  const auto qualified = in.qualified_table();
  const auto& citus = obs.extension("citus");
  const auto column = in.body.value("distribution_column", "");

  bool already = false;
  if (!citus_precondition(in, obs, step, plan, citus, qualified, already)) return;

  const auto& t = obs.table(qualified);

  if (already) {
    const auto& d = citus_table(citus, qualified);
    const auto current = d.value("distribution_column", "");
    step.action = Action::kSatisfied;
    step.why = qualified + " is already distributed on " +
               (current.empty() ? std::string("an unread column") : current);
    // EQUALITY, not a substring search. The first version used find(), and a
    // planner test caught what that means: "tenant_id" CONTAINS "id", so
    // redistributing a table from tenant_id onto id read as "already
    // distributed on that column" and was silently accepted as satisfied.
    if (!current.empty() && current != column) {
      // Distributing again is not how the column changes, and Citus would
      // refuse it. Naming the right kind beats a message about the wrong one.
      step.action = Action::kConflict;
      step.why = qualified + " is already distributed on " + current +
                 ", and this asks for " + column;
      plan.conflicts.push_back(
          step.why +
          ". Changing the distribution column of a distributed table is "
          "alter_distributed_table, not distribute_table -- it moves every row, "
          "where this kind only creates the distribution.");
    }
    return;
  }

  // The distribution column has to be a column.
  const json columns = t.value("columns", json::object());
  if (!columns.contains(column)) {
    step.action = Action::kConflict;
    step.why = qualified + " has no column " + column +
               " to distribute on";
    plan.conflicts.push_back(step.why);
    return;
  }

  std::string offender, offender_columns;
  if (!citus_unique_keys_include(t, column, offender, offender_columns)) {
    step.action = Action::kConflict;
    step.why = offender + " on " + qualified + " covers (" + offender_columns +
               "), which does not include the distribution column " + column;
    plan.conflicts.push_back(
        step.why +
        ". Citus cannot enforce uniqueness across shards without the "
        "distribution column, so it refuses the distribution rather than the "
        "constraint. Add " + column + " to that key first, in an earlier "
        "specification, or distribute on a column the key already covers.");
    return;
  }

  // Colocation: the target must exist and be distributed, and both facts are
  // in the reading already taken.
  const auto with = in.body.value("colocate_with", "");
  const bool real_colocation =
      !with.empty() && with != "none" && with != "default";
  if (real_colocation) {
    const auto& other = citus_table(citus, with);
    if (other.empty()) {
      step.action = Action::kConflict;
      step.why = "colocate_with names " + with +
                 ", which this database does not record as distributed";
      plan.conflicts.push_back(
          step.why +
          ". A table can only join a colocation group that exists, so " + with +
          " has to be distributed first -- in an earlier specification, which "
          "depends_on will order for you.");
      return;
    }
    step.detail["colocate_with"] = with;
    step.detail["colocation_group"] = other.value("colocationid", 0);
    step.detail["adopted_shard_count"] = other.value("shard_count", 0);
  }

  // concurrently: auto is where the planner earns its keep. The same shape as
  // create_index deciding whether a build may be concurrent: measure, decide,
  // and SAY WHICH READING DECIDED.
  const auto when = in.body.value("concurrently", "auto");
  const long long rows = t.value("reltuples", 0LL);
  bool concurrent = false;
  std::string decided_by;
  if (when == "always") {
    concurrent = true;
    decided_by = "concurrently: always, overriding the row count";
  } else if (when == "never") {
    concurrent = false;
    decided_by = "concurrently: never, overriding the row count";
  } else {
    concurrent = rows > kCitusConcurrentRowThreshold;
    decided_by = std::to_string(rows) + " estimated rows, " +
                 (concurrent ? "above" : "at or below") + " the " +
                 std::to_string(kCitusConcurrentRowThreshold) +
                 " above which the blocking form holds writes long enough to "
                 "matter";
  }

  // The concurrent form cannot run inside a transaction block, exactly like
  // CREATE INDEX CONCURRENTLY -- and the executor already knows what that
  // class means.
  step.txn_class = concurrent ? TxnClass::kForbidden : TxnClass::kRequired;
  step.action = Action::kApply;
  step.lock = concurrent
                  ? "ShareUpdateExclusiveLock on " + qualified +
                        " (writes continue)"
                  : "AccessExclusiveLock on " + qualified +
                        " (writes blocked for the copy)";
  step.why = decided_by;
  step.detail["rows_estimated"] = rows;
  step.detail["concurrent"] = concurrent;
  step.detail["distribution_column"] = column;
  step.detail["shard_method"] = in.body.value("shard_method", "hash");

  // A regclass argument is a STRING, not an identifier. quote_qualified would
  // give "public"."accounts", which SQL reads as a column reference --
  // "missing FROM-clause entry for table public", found by running it against a
  // real cluster rather than by reading.
  std::string args = detail::quote_literal(qualified) + ", " +
                     detail::quote_literal(column);
  if (real_colocation) {
    args += ", colocate_with => " + detail::quote_literal(with);
  } else if (with == "none") {
    args += ", colocate_with => 'none'";
  }
  if (in.body.contains("shard_count")) {
    args += ", shard_count => " +
            std::to_string(in.body["shard_count"].get<int>());
    step.detail["shard_count"] = in.body["shard_count"].get<int>();
  }

  step.sql.push_back("SELECT " +
                     std::string(concurrent ? "create_distributed_table_concurrently"
                                            : "create_distributed_table") +
                     "(" + args + ");");

  if (concurrent) {
    plan.warnings.push_back(
        "create_distributed_table_concurrently keeps writes running, and needs "
        "a replica identity: a table with no primary key cannot use it. It also "
        "cannot run inside a transaction block, so this step commits on its "
        "own.");
  } else {
    plan.warnings.push_back(
        qualified + " is copied to its shards under an exclusive lock. At " +
        std::to_string(rows) +
        " estimated rows that was judged short; if the table has grown since "
        "this plan was made, re-plan rather than trusting the number.");
  }
}

inline void plan_create_reference_table(const Intent& in, const Observations& obs,
                                        const ExecutorConfig& cfg, Plan& plan,
                                        std::vector<Step>& out) {
  (void)cfg;
  Step step;
  step.kind = in.kind_name;
  struct Emit { std::vector<Step>& o; Step& s; ~Emit() { o.push_back(s); } } emit{out, step};

  const auto qualified = in.qualified_table();
  const auto& citus = obs.extension("citus");
  bool already = false;
  if (!citus_precondition(in, obs, step, plan, citus, qualified, already)) return;

  if (already) {
    const auto& d = citus_table(citus, qualified);
    const auto method = d.value("partmethod", "");
    if (method == "n") {
      step.action = Action::kSatisfied;
      step.why = qualified + " is already a reference table";
      return;
    }
    step.action = Action::kConflict;
    step.why = qualified + " is already distributed, not a reference table";
    plan.conflicts.push_back(
        step.why +
        ". Turning a distributed table into a reference table means "
        "undistributing it first, which moves data and is its own decision.");
    return;
  }

  step.action = Action::kApply;
  step.txn_class = TxnClass::kRequired;
  step.lock = "AccessExclusiveLock on " + qualified +
              " (the whole table is copied to every node)";
  step.why = "a reference table is replicated in full to every node, so reads "
             "join against it locally and writes are two-phase";
  step.sql.push_back("SELECT create_reference_table(" +
                     detail::quote_literal(qualified) + ");");
  plan.warnings.push_back(
      "Every write to " + qualified +
      " becomes a two-phase commit across all nodes. Reference tables are for "
      "small, rarely-written lookups; a busy one is a cluster-wide bottleneck.");
}

inline void plan_distribute_function(const Intent& in, const Observations& obs,
                                     const ExecutorConfig& cfg, Plan& plan,
                                     std::vector<Step>& out) {
  (void)cfg;
  Step step;
  step.kind = in.kind_name;
  struct Emit { std::vector<Step>& o; Step& s; ~Emit() { o.push_back(s); } } emit{out, step};

  const auto& citus = obs.extension("citus");
  const auto schema = in.body.value("schema", "");
  const auto name = in.body.value("name", "");
  const auto args = in.body.value("arguments", "");
  const auto qualified_fn = schema + "." + name;
  const auto signature = qualified_fn + "(" + args + ")";

  if (citus.empty()) {
    step.action = Action::kConflict;
    step.why = "the citus extension is not installed in this database, so " +
               in.kind_name + " cannot be planned";
    plan.conflicts.push_back(step.why);
    return;
  }

  // object_key_for() puts this kind's reading under "function:schema.name",
  // which is why that pairing had to be declared in keys.inc.
  const auto& fn = obs.object("function:" + qualified_fn);
  if (!fn.value("exists", false)) {
    step.action = Action::kConflict;
    step.why = signature + " does not exist, so it cannot be distributed";
    plan.conflicts.push_back(
        step.why +
        ". Create it in an earlier specification and order the two with "
        "depends_on.");
    return;
  }

  const auto distributed = citus.value("distributed_functions", json::array());
  for (const auto& d : distributed) {
    if (d.get<std::string>() == qualified_fn) {
      step.action = Action::kSatisfied;
      step.why = signature + " is already distributed";
      return;
    }
  }

  const auto with = in.body.value("colocate_with", "");
  const auto dist_arg = in.body.value("distribution_argument", "");
  const bool colocated = !with.empty() && with != "none" && with != "default";
  if (colocated) {
    const auto& other = citus_table(citus, with);
    if (other.empty()) {
      step.action = Action::kConflict;
      step.why = "colocate_with names " + with +
                 ", which this database does not record as distributed";
      plan.conflicts.push_back(
          step.why +
          ". A function routes to the shards of a distributed table, so that "
          "table has to be distributed first.");
      return;
    }
    step.detail["colocation_group"] = other.value("colocationid", 0);
  }

  step.action = Action::kApply;
  step.txn_class = TxnClass::kRequired;
  step.lock = "no table lock: this records routing metadata";
  step.why = colocated
                 ? "routes on " + dist_arg + " to the shards of " + with +
                       ", so the call runs where its rows already are"
                 : "replicated to every node, so any node can run it locally";
  step.detail["signature"] = signature;
  if (colocated) {
    step.detail["distribution_argument"] = dist_arg;
    step.detail["colocate_with"] = with;
  }

  std::string call = "SELECT create_distributed_function(" +
                     detail::quote_literal(signature);
  if (colocated) {
    call += ", " + detail::quote_literal(dist_arg) + ", colocate_with => " +
            detail::quote_literal(with);
  }
  if (in.body.value("force_delegation", false)) {
    call += ", force_delegation => true";
    step.detail["force_delegation"] = true;
  }
  step.sql.push_back(call + ");");

  if (!colocated) {
    plan.warnings.push_back(
        signature +
        " is replicated to every node rather than routed. Every node gets a "
        "copy of the definition, and a later CREATE OR REPLACE has to be "
        "propagated too or the copies diverge.");
  }
}

