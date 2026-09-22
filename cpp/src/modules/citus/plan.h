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

inline void plan_citus_distribute_table(const Intent& in, const Observations& obs,
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
          "alter_distributed_table, not citus_distribute_table -- it moves every row, "
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
  // Does this Citus HAVE the concurrent call? Read from pg_proc rather than
  // inferred from a version number: the presence of a function is a fact, and a
  // minimum version is a recollection. Without this, a Citus too old to have it
  // gets SQL it cannot parse -- which is the one failure mode the version gate
  // on merge_rows exists to prevent for core.
  const auto calls = citus.value("calls", json::object());
  const bool has_concurrent =
      calls.value("create_distributed_table_concurrently", false);

  const auto when = in.body.value("concurrently", "auto");
  const long long rows = t.value("reltuples", 0LL);
  bool concurrent = false;
  std::string decided_by;
  if (when == "always" && !has_concurrent) {
    // Asked for explicitly and not available: a refusal, not a silent
    // downgrade. Someone who wrote "always" was making a decision about a
    // table too big to lock, and quietly locking it anyway is the worst
    // available answer.
    step.action = Action::kConflict;
    step.why = "concurrently: always was asked for, and this Citus has no "
               "create_distributed_table_concurrently";
    plan.conflicts.push_back(
        step.why + " (citus " + citus.value("version", "?") +
        "). The concurrent form was added in a later Citus; this one can only "
        "distribute under an exclusive lock. Upgrade Citus, or write "
        "concurrently: never to say the lock is acceptable for this table.");
    return;
  }
  if (when == "always") {
    concurrent = true;
    decided_by = "concurrently: always, overriding the row count";
  } else if (when == "never") {
    concurrent = false;
    decided_by = "concurrently: never, overriding the row count";
  } else if (!has_concurrent) {
    // auto, and the call is not there. The blocking form is the only path, and
    // the plan says WHY rather than looking like it weighed the row count and
    // chose this.
    concurrent = false;
    decided_by = "this Citus (" + citus.value("version", "?") +
                 ") has no create_distributed_table_concurrently, so the "
                 "blocking form is the only path -- the " +
                 std::to_string(rows) + " estimated rows did not decide it";
    if (rows > kCitusConcurrentRowThreshold) {
      plan.warnings.push_back(
          "At " + std::to_string(rows) +
          " estimated rows this would have taken the concurrent path if Citus "
          "had it. Writes are blocked for the whole copy instead. Consider "
          "upgrading Citus before running this against a live system.");
    }
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

inline void plan_citus_create_reference_table(const Intent& in, const Observations& obs,
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
  // create_reference_table, NOT citus_create_reference_table. The KIND carries
  // the module's prefix; the Citus FUNCTION does not, and a rename that swept
  // through emitted SQL as well as kind names produced
  // "function citus_create_reference_table(unknown) does not exist" -- caught
  // by the dry run, which is what it is for.
  step.sql.push_back("SELECT create_reference_table(" +
                     detail::quote_literal(qualified) + ");");
  plan.warnings.push_back(
      "Every write to " + qualified +
      " becomes a two-phase commit across all nodes. Reference tables are for "
      "small, rarely-written lookups; a busy one is a cluster-wide bottleneck.");
}

inline void plan_citus_distribute_function(const Intent& in, const Observations& obs,
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


// A paced backfill that walks ONE SHARD AT A TIME.
//
// Why this is a kind of its own rather than a smarter core backfill: core's
// backfill is a CORE kind, and a module may not change the SQL core emits for
// one. Core refuses the distributed case and names this kind; what core emits is
// byte-identical whether or not this module is built. See ../README.md.
//
// HOW IT IS CONFINED, and why it is the only shape that works. Citus refuses
// FOR UPDATE on a multi-shard query, so a batch that locks rows must be provably
// single-shard. Measured on Citus 13:
//
//   WHERE worker_hash(dist) BETWEEN min AND max ... FOR UPDATE
//     -> 0A000: could not run distributed query with FOR UPDATE/SHARE commands
//        (the hash predicate is not pushed down; Citus scans every shard)
//   WHERE dist = <value> AND key > $2 ... FOR UPDATE
//     -> rows, and Task Count 1
//
// So the confinement is an EQUALITY on the distribution column, and the walk
// gains an outer dimension: iterate distribution values, and keyset-walk the key
// inside each. The cursor is therefore a pair.
//
// The speed is NOT the argument. Measured earlier: a shard-ordered walk is ~16%
// better per row than a key-ordered one, not the 7.9x a first measurement
// claimed -- that figure compared rows already confined against rows spread, a
// shape a key-ordered walk cannot produce. The argument is that this makes a
// paced walk POSSIBLE on a distributed table, where core's is refused outright.
inline void plan_citus_distributed_backfill(const Intent& in,
                                          const Observations& obs,
                                          const ExecutorConfig& cfg, Plan& plan,
                                          std::vector<Step>& out) {
  Step step;
  step.kind = in.kind_name;
  struct Emit { std::vector<Step>& o; Step& s; ~Emit() { o.push_back(s); } } emit{out, step};

  const auto qualified = in.qualified_table();
  const auto sql_rel = detail::quote_qualified(qualified);
  const auto rel = detail::quote_identifier(in.table());
  const auto& citus = obs.extension("citus");
  const auto& t = obs.table(qualified);

  const auto refuse = [&](std::string why, std::string detail_text) {
    step.action = Action::kConflict;
    step.why = why;
    plan.conflicts.push_back(why + (detail_text.empty() ? "" : ". " + detail_text));
  };

  if (citus.empty()) {
    refuse("the citus extension is not installed in this database, so " +
               in.kind_name + " cannot be planned",
           "");
    return;
  }
  if (!t.value("exists", false)) {
    refuse(qualified + " does not exist", "");
    return;
  }

  const auto& dist = citus_table(citus, qualified);
  if (dist.empty()) {
    refuse(qualified + " is not distributed, so it does not need a "
           "shard-confined walk",
           "Use \"backfill\", which walks an ordinary table and is not refused "
           "here.");
    return;
  }
  if (dist.value("partmethod", "") == "n") {
    refuse(qualified + " is a reference table, not a distributed one",
           "A reference table has one placement per node, so FOR UPDATE is "
           "legal on it and plain \"backfill\" works -- measured against a "
           "live cluster.");
    return;
  }

  const auto key = in.body.value("key", "");
  const auto dist_column = dist.value("distribution_column", "");
  if (dist_column.empty()) {
    refuse(qualified + " is distributed but this database does not record its "
           "distribution column",
           "Without it a batch cannot be confined to one shard.");
    return;
  }
  // The degenerate case, refused rather than shipped as a walk that makes one
  // row of progress per batch. Confinement is an equality on the distribution
  // column, so when that column IS the cursor, one group holds exactly one row.
  if (dist_column == key) {
    refuse(qualified + " is distributed on " + dist_column +
               ", which is also the walk key",
           "A batch is confined by an equality on the distribution column, so "
           "with the key and that column the same, every batch would be one "
           "row. Walk a different unique column, or run this change as "
           "update_rows with the rows supplied -- which is not refused on a "
           "distributed table.");
    return;
  }

  const json columns = t.value("columns", json::object());
  if (!columns.contains(key)) {
    refuse(qualified + " has no column " + key + " to walk", "");
    return;
  }
  if (!columns.contains(dist_column)) {
    refuse(qualified + " has no column " + dist_column +
               ", which this database records as its distribution column",
           "");
    return;
  }
  // WHAT UNIQUENESS THIS WALK ACTUALLY NEEDS, which is not what core's backfill
  // needs, and the difference is forced by Citus rather than chosen.
  //
  // Core's backfill wants the key to lead a unique index, because its cursor
  // walks the whole table and a repeated key would make a batch boundary fall
  // inside a run of equal keys. On a DISTRIBUTED table that requirement cannot
  // be met for any column but the distribution column: Citus refuses a unique
  // index that does not contain it -- measured, create_distributed_table fails
  // on a table carrying a unique index on the walk key alone.
  //
  // It does not need to be met. This walk is confined to ONE distribution value
  // at a time, and a unique index on (distribution column, key) makes the key
  // unique WITHIN such a value, which is exactly the scope the cursor moves in.
  // That index is also the one that serves `dist = X AND key > Y ORDER BY key`
  // without a sort -- and it is the primary key of any table following the
  // convention that the tenant column comes first.
  std::string supporting_index;
  // Bound to a name, not iterated off .value() -- that returns a temporary and
  // the reference outlives it. The same bug shape has appeared five times in
  // this project and cpp/test/no-dangling-items.sh exists because of it.
  const json indexes = t.value("indexes", json::object());
  for (const auto& [name, ix] : indexes.items()) {
    if (!ix.value("is_unique", false) || !ix.value("is_valid", false)) continue;
    const auto cols = ix.value("columns", json::array());
    const auto keyed = static_cast<std::size_t>(
        ix.value("key_column_count", static_cast<int>(cols.size())));
    if (keyed != 2 || cols.size() < 2) continue;
    if (cols[0] == dist_column && cols[1] == key) {
      supporting_index = name;
      break;
    }
  }
  if (supporting_index.empty()) {
    refuse("no unique index on (" + dist_column + ", " + key + ") covers " +
               qualified,
           "This walk is confined to one " + dist_column +
               " at a time, so it needs " + key + " to be unique WITHIN one of "
               "them -- a unique index on (" + dist_column + ", " + key +
               ") in that order, which is also the index that serves the walk "
               "without a sort. A unique index on " + key +
               " alone cannot exist here: Citus refuses one that does not "
               "contain the distribution column.");
    return;
  }

  std::vector<std::string> assignments;
  for (auto it = in.body["set"].begin(); it != in.body["set"].end(); ++it) {
    assignments.push_back(detail::quote_identifier(it.key()) + " = " +
                          it.value().get<std::string>());
  }
  const auto where = in.body.value("where", "");
  const auto k = detail::quote_identifier(key);
  const auto d = detail::quote_identifier(dist_column);
  const auto key_type = columns.value(key, json::object()).value("type", "text");
  const auto dist_type =
      columns.value(dist_column, json::object()).value("type", "text");

  // THREE statements, and the order is the algorithm.
  //
  //   [0] which distribution values remain -- an ordinary multi-shard read,
  //       taking no locks, keyset-walked so a cluster with many tenants does
  //       not materialise them all.
  //   [1] the keys to do inside ONE value -- single-shard, and therefore
  //       allowed to take FOR UPDATE.
  //   [2] the change, to exactly those keys, still carrying the equality so it
  //       stays single-shard.
  step.sql.push_back(
      "SELECT DISTINCT " + rel + "." + d + "\n"
      "  FROM " + sql_rel + "\n"
      // NULL means "before the first one". An empty string cannot serve: the
      // parameter takes the distribution column's own type, and '' is not a
      // bigint -- measured, "invalid input syntax for type bigint".
      // The COMPARISON comes first so PREPARE can infer the parameter's type.
      // With `$1 IS NULL` leading, a bare PREPARE fails 42P08 "could not
      // determine data type of parameter $1" -- nothing before it says what $1
      // is. A cast would also fix it and would break EXPLAIN (GENERIC_PLAN)
      // instead, so the order is the fix that satisfies both.
      " WHERE (" + rel + "." + d + " > $1 OR $1 IS NULL) AND (" + where + ")\n"
      " ORDER BY " + rel + "." + d + "\n"
      " LIMIT $2;");
  step.sql.push_back(
      "SELECT " + rel + "." + k + "\n"
      "  FROM " + sql_rel + "\n"
      // No ::type on a scalar parameter: EXPLAIN (GENERIC_PLAN) refuses one
      // with "no value found for parameter 1", and the comparison already fixes
      // the type. The ARRAY cast below is different -- ANY() needs it.
      " WHERE " + rel + "." + d + " = $1\n"
      // Likewise: a group just picked up has no position inside it yet.
      "   AND (" + rel + "." + k + " > $2 OR $2 IS NULL) AND (" + where + ")\n"
      " ORDER BY " + rel + "." + k + "\n"
      " LIMIT $3\n"
      " FOR UPDATE OF " + rel + ";");
  step.sql.push_back(
      "UPDATE " + sql_rel + "\n"
      "   SET " + detail::join(assignments, ", ") + "\n"
      " WHERE " + rel + "." + d + " = $1\n"
      "   AND " + rel + "." + k + " = ANY($2::" + key_type + "[])"
      " AND (" + where + ")\n"
      "RETURNING " + rel + "." + k + ";");

  step.txn_class = TxnClass::kOwnTxnPerBatch;
  step.detail["batch_mode"] = "shard_confined";
  step.detail["qualified"] = qualified;
  step.detail["key"] = key;
  step.detail["key_type"] = key_type;
  step.detail["distribution_column"] = dist_column;
  step.detail["distribution_type"] = dist_type;
  step.detail["where"] = where;
  step.detail["batch_rows"] = cfg.batch_rows;
  step.detail["batch_bytes"] = cfg.batch_bytes;
  step.detail["commit_interval_ms"] = cfg.commit_interval_ms;
  step.detail["batch_cap_rows"] = cfg.batch_cap_rows;
  step.detail["supporting_index"] = supporting_index;
  step.detail["shard_count"] = dist.value("shard_count", 0);
  step.detail["commits_on"] =
      json::array({"lock_waiter", "interval", "batch_cap"});
  if (in.body.contains("verify_remaining")) {
    step.detail["verify_remaining"] = in.body["verify_remaining"];
  }
  if (in.body.contains("assert_invariants")) {
    step.detail["assert_invariants"] = in.body["assert_invariants"];
  }
  step.lock = "RowExclusiveLock on " + qualified +
              " plus row locks inside one shard, released at every commit";
  step.why =
      "walking " + qualified + " one shard at a time: batches confined by " +
      dist_column + " = <value>, keyset on " + key + " via " + supporting_index +
      ". Confinement is what makes FOR UPDATE legal here -- Citus refuses it on "
      "a multi-shard query, which is why plain backfill is refused on this table";
  step.detail["expected"] = "single-shard batches (Task Count 1)";
}
