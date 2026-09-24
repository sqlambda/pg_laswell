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
    // The distribution columns must be the SAME type -- exactly, not the same
    // family. Measured on Citus 13: colocating an int column with a bigint
    // group is refused as firmly as text with bigint, "cannot colocate tables
    // ... Distribution column types don't match". Checked by oid, the one
    // comparison that cannot disagree with Citus's.
    //
    // Only when both types are READ. A target distributed earlier in this same
    // specification is a projection with no column types, and a table created
    // earlier in it may be too. The dry run executes create_distributed_table
    // inside the transaction it rolls back (rehearse_by = "execution"), so that
    // case is still caught before anything commits -- by Citus rather than here.
    // Until that flag existed this comment claimed the same and it was false:
    // the call was only ever planned.
    const auto want_oid = other.value("distribution_type_oid", 0LL);
    const auto have_oid =
        columns.value(column, json::object()).value("type_oid", 0LL);
    if (want_oid != 0 && have_oid != 0 && want_oid != have_oid) {
      step.action = Action::kConflict;
      const auto have_type =
          columns.value(column, json::object()).value("type", "?");
      const auto want_type = other.value("distribution_type", "?");
      step.why = qualified + "." + column + " is " + have_type + ", and " +
                 with + " is distributed on a " + want_type + " column";
      plan.conflicts.push_back(
          step.why + ". Citus colocates only tables whose distribution columns "
          "are the same type exactly -- int and bigint are refused as firmly as "
          "text and bigint (\"Distribution column types don't match\"). Change "
          "the column's type in an earlier specification, or colocate with "
          "none.");
      return;
    }
    step.detail["colocate_with"] = with;
    step.detail["colocation_group"] = other.value("colocationid", 0);
    step.detail["adopted_shard_count"] = other.value("shard_count", 0);
    // Adopted, not checked: measured, a session asking for a different
    // citus.shard_replication_factor joins the group with the GROUP's factor
    // and no error. Recorded so the plan says what the table will actually get.
    step.detail["adopted_replication_factor"] =
        other.value("replication_factor", 0);
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
  // Rehearsed by EXECUTION. Every Citus call here is a function invoked through
  // SELECT, and a dry run classifying statements by their first word planned it
  // and never ran it -- so no dry run ever distributed anything, and a later
  // specification colocating with this table was refused for colocating with
  // something "not distributed". Executed inside the rolled-back transaction, a
  // dry run now proves the call succeeds; on a large table it is bounded by
  // dry_run_statement_timeout_ms like any other DDL that does real work.
  step.detail["rehearse_by"] = "execution";
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
    // partmethod 'n' alone does not make a reference table: Citus records a
    // CITUS-MANAGED LOCAL table the same way, and tells them apart by repmodel
    // -- 't' replicated, 's' a single placement on the coordinator. Calling the
    // second "already a reference table" would report success for a table that
    // is on one node. Measured: create_reference_table converts it (to n/t) with
    // no error, so it is simply applied.
    if (method == "n" && d.value("repmodel", "") == "t") {
      step.action = Action::kSatisfied;
      step.why = qualified + " is already a reference table";
      return;
    }
    if (method == "n") {
      already = false;  // a managed local table: convert it, below
    }
  }
  if (already) {
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
  // Rehearsed by execution: see plan_citus_distribute_table.
  step.detail["rehearse_by"] = "execution";
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

  // Whether it is ALREADY distributed as asked is decided below, once the
  // argument and group are resolved: "Citus knows this function" is not the
  // question, because on Citus 11+ it knows every function that was created.
  const auto dist_map = citus.value("function_distribution", json::object());
  const json existing = dist_map.value(qualified_fn, json(nullptr));
  int routed_index = -1;           // 0-based, as Citus stores it; -1 = not routed
  long long target_group = -1;

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
    target_group = other.value("colocationid", -1LL);

    // WHICH ARGUMENT, and whether it can route. Measured on Citus 13:
    //
    //   a name the function does not have, or $n out of range
    //     -> refused, 22023 "the distribution argument is not valid"
    //   a bigint argument on a bigint group  -> pushed down to the shard
    //   an int argument on a bigint group    -> pushed down (Citus coerces)
    //   a text argument on a bigint group    -> ACCEPTED, and then EVERY CALL
    //                                           fails: "Cannot coerce text to
    //                                           bigint"
    //
    // The last is the one worth catching: Citus says yes to a distribution that
    // breaks every invocation of the function. Across type categories there is
    // no implicit coercion for it to use; within one there usually is, so a
    // same-category mismatch is a warning, not a refusal.
    const json arguments = fn.value("arguments", json(nullptr));
    if (arguments.is_array()) {
      int index = -1;
      if (!dist_arg.empty() && dist_arg[0] == '$') {
        try {
          index = std::stoi(dist_arg.substr(1)) - 1;
        } catch (const std::exception&) {
          index = -1;
        }
        if (index < 0 || index >= static_cast<int>(arguments.size())) index = -1;
      } else {
        for (std::size_t i = 0; i < arguments.size(); ++i) {
          if (arguments[i].value("name", json(nullptr)) == json(dist_arg)) {
            index = static_cast<int>(i);
            break;
          }
        }
      }
      if (index < 0) {
        std::vector<std::string> names;
        for (std::size_t i = 0; i < arguments.size(); ++i) {
          const auto n = arguments[i].value("name", json(nullptr));
          names.push_back((n.is_string() ? n.get<std::string>() + " " : "") + "$" +
                          std::to_string(i + 1) + " " +
                          arguments[i].value("type", "?"));
        }
        step.action = Action::kConflict;
        step.why = "distribution_argument " + dist_arg + " is not an argument of " +
                   qualified_fn;
        plan.conflicts.push_back(
            step.why + ". Its inputs are: " +
            (names.empty() ? std::string("none") : detail::join(names, ", ")) +
            ". Name one, or give its position as $n; Citus refuses anything else "
            "(22023).");
        return;
      }
      const auto& arg = arguments[static_cast<std::size_t>(index)];
      const auto arg_oid = arg.value("type_oid", 0LL);
      const auto want_oid = other.value("distribution_type_oid", 0LL);
      if (arg_oid != 0 && want_oid != 0 && arg_oid != want_oid) {
        const auto arg_cat = arg.value("category", "");
        const auto want_cat = other.value("distribution_type_category", "");
        const auto arg_type = arg.value("type", "?");
        const auto want_type = other.value("distribution_type", "?");
        if (!arg_cat.empty() && !want_cat.empty() && arg_cat != want_cat) {
          step.action = Action::kConflict;
          step.why = dist_arg + " is " + arg_type + ", and " + with +
                     " is distributed on a " + want_type + " column";
          plan.conflicts.push_back(
              step.why + ". Citus ACCEPTS this distribution and then fails every "
              "call to the function -- measured, \"Cannot coerce " + arg_type +
              " to " + want_type + "\" -- because a routed call has to turn the "
              "argument into the distribution column's type and there is no "
              "implicit way to. Distribute on an argument of type " + want_type +
              ", or colocate with a table distributed on " + arg_type + ".");
          return;
        }
        plan.warnings.push_back(
            dist_arg + " is " + arg_type + " and " + with + " is distributed on " +
            want_type + ". Citus coerces within a type category -- measured, an "
            "int argument on a bigint group is pushed down -- so this is expected "
            "to route, but only int on bigint was measured.");
      }
      step.detail["distribution_argument_index"] = index + 1;
      routed_index = index;
    }
  }

  // ALREADY AS ASKED? Compared against the spec, not against "Citus lists it".
  //
  //   colocated: satisfied only when the function already routes on the SAME
  //     argument in the SAME group. Anything else is re-applied, and that
  //     converges: measured, calling create_distributed_function again on a
  //     distributed function switches its argument, with no error.
  //   not colocated: satisfied when Citus knows the function at all -- on
  //     Citus 11+ propagation alone replicates it to every node, which is what
  //     this form asks for.
  if (!existing.is_null()) {
    const auto have_index = existing.value("argument_index", json(nullptr));
    const auto have_group = existing.value("colocationid", json(nullptr));
    if (colocated) {
      if (routed_index >= 0 && have_index.is_number() &&
          have_index.get<int>() == routed_index && have_group.is_number() &&
          have_group.get<long long>() == target_group) {
        step.action = Action::kSatisfied;
        step.why = signature + " already routes on " + dist_arg + " to the group of " +
                   with;
        return;
      }
      if (have_index.is_number()) {
        plan.warnings.push_back(
            signature + " is already distributed, routing on argument $" +
            std::to_string(have_index.get<int>() + 1) + " in colocation group " +
            (have_group.is_number() ? std::to_string(have_group.get<long long>())
                                    : std::string("?")) +
            "; this re-distributes it as the specification asks. Citus switches "
            "an existing distribution without error, so calls route the new way "
            "from the moment this commits.");
      }
    } else if (!have_index.is_number()) {
      step.action = Action::kSatisfied;
      step.why = signature + " is already replicated to every node";
      return;
    }
  }

  step.action = Action::kApply;
  step.txn_class = TxnClass::kRequired;
  // Rehearsed by execution: see plan_citus_distribute_table.
  step.detail["rehearse_by"] = "execution";
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
