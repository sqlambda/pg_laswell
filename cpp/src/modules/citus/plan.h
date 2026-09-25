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

// Is Citus here at all. Returns false, having refused and named the
// prerequisite, when it is not.
inline bool citus_present(const Intent& in, Step& step, Plan& plan,
                          const json& citus) {
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
  return true;
}

// Shared preamble: is Citus here at all, does the table exist, is it already
// distributed. Returns false when the step is finished and the caller should
// stop.
inline bool citus_precondition(const Intent& in, const Observations& obs,
                               Step& step, Plan& plan, const json& citus,
                               const std::string& qualified,
                               bool& already_distributed) {
  if (!citus_present(in, step, plan, citus)) return false;
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

// --- Readings several kinds share ---------------------------------------------

// Workers (by name) whose wal_level is known and is not logical. A worker that
// did not answer is not listed: its setting is unknown, not wrong.
inline std::vector<std::string> citus_workers_not_logical(const json& citus) {
  std::vector<std::string> out;
  const auto workers = citus.value("workers", json::object());
  for (auto it = workers.begin(); it != workers.end(); ++it) {
    if (!it.value().is_object()) continue;
    const auto w = it.value().value("wal_level", "");
    if (!w.empty() && w != "logical") out.push_back(it.key() + " (" + w + ")");
  }
  return out;
}

// Whether the reading has a node list at all. An older reading, or a test
// fixture, may not; absent is "not read", never "no nodes".
inline bool citus_nodes_read(const json& citus) {
  const auto it = citus.find("nodes");
  return it != citus.end() && it->is_array();
}

// Active primary nodes allowed to hold shards.
inline int citus_shard_holders(const json& citus) {
  int n = 0;
  for (const auto& nd : citus.value("nodes", json::array())) {
    if (nd.value("isactive", false) && nd.value("noderole", "") == "primary" &&
        nd.value("shouldhaveshards", false)) {
      ++n;
    }
  }
  return n;
}

// THE EMPTY-CLUSTER TRAP, which CITUS.md calls the highest-value refusal for a
// lab. Measured on a database with nothing in pg_dist_node:
// create_distributed_table, create_reference_table and
// citus_add_local_table_to_metadata all SUCCEED -- each registers the
// coordinator as "localhost" taking shards, and a distributed table's 32 shards
// all land on it -- and every citus_add_node afterwards is refused: "cannot add
// a worker node when the coordinator hostname is set to localhost". Nothing
// fails at the time; the cluster is quietly a single node that cannot grow.
inline bool citus_refuse_empty_cluster(const Intent& in, const json& citus,
                                       Step& step, Plan& plan) {
  if (!citus_nodes_read(citus) || !citus.value("nodes", json::array()).empty()) {
    return false;
  }
  step.action = Action::kConflict;
  step.why = "no node is registered in this database's Citus metadata, so " +
             in.kind_name + " would register the coordinator as \"localhost\"";
  plan.conflicts.push_back(
      step.why + " and place everything on it. Measured: the call succeeds, and "
      "every citus_add_node after it is refused (\"cannot add a worker node when "
      "the coordinator hostname is set to localhost\"), so the cluster stays one "
      "node. Register the coordinator (citus_set_coordinator_host) and the workers "
      "(citus_ensure_workers) first -- earlier in this specification is enough. "
      "For a deliberate single-node cluster, register the coordinator and set its "
      "should_have_shards to true.");
  return true;
}

// Where the reference tables are NOT yet. Measured: adding a node copies none
// of them; the next step that needs them everywhere -- create_distributed_table
// was measured -- copies all of them to every node lacking them.
inline void citus_note_reference_copy(const json& citus, Step& step, Plan& plan,
                                      const std::string& what) {
  const auto refs = citus.value("reference_tables", json::object());
  const int count = refs.value("count", 0);
  if (count <= 0) return;
  std::vector<std::string> lacking;
  for (const auto& nd : citus.value("nodes", json::array())) {
    if (!nd.value("isactive", false) || nd.value("noderole", "") != "primary") continue;
    if (nd.value("reference_placements", 0) < count) {
      lacking.push_back(nd.value("nodename", "") + ":" +
                        std::to_string(nd.value("nodeport", 0)));
    }
  }
  if (lacking.empty()) return;
  const auto b = refs.value("bytes", json(nullptr));
  step.detail["reference_copy_to"] = lacking;
  step.detail["reference_bytes"] = b;
  const std::string size =
      b.is_number() ? std::to_string(b.get<long long>() / (1024 * 1024)) +
                          " MiB per copy, measured on the coordinator's copy"
                    : std::string("size not read: the coordinator holds no copy");
  plan.warnings.push_back(
      what + " first copies the " + std::to_string(count) + " reference table(s) (" +
      size + ") to " + detail::join(lacking, ", ") +
      ", which do not hold them yet. Adding a node copies nothing (measured); "
      "the next step needing them everywhere pays for it, and this is that step.");
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

  if (citus_refuse_empty_cluster(in, citus, step, plan)) return;
  // Measured with the coordinator registered as taking no shards and no
  // workers: "replication_factor (1) exceeds number of worker nodes (0)".
  if (citus_nodes_read(citus) && citus_shard_holders(citus) == 0) {
    step.action = Action::kConflict;
    step.why = "no active node may hold shards, so " + qualified +
               " has nowhere to be distributed to";
    plan.conflicts.push_back(
        step.why + ". Citus refuses it (\"replication_factor exceeds number of "
        "worker nodes\"). Add workers with citus_ensure_workers, or let a node "
        "hold shards with citus_set_node_property -- earlier in this "
        "specification is enough.");
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

  // What stops the concurrent form, READ before it is chosen. It runs outside
  // a transaction and is never rehearsed, and a failure is not clean: measured,
  // a failed create_distributed_table_concurrently left the table registered
  // as a Citus-managed local table (partmethod n, repmodel s), half converted.
  //   - it needs wal_level = logical on the COORDINATOR as well as the workers
  //     (measured: "logical decoding requires wal_level >= logical", raised on
  //     the coordinator);
  //   - a NULL in the distribution column fails it (measured: "the partition
  //     column value cannot be NULL"), and whether a NULL exists is not read --
  //     so a nullable column is enough to rule it out.
  std::vector<std::string> blockers;
  {
    const auto cw = citus.value("coordinator", json::object()).value("wal_level", "");
    if (!cw.empty() && cw != "logical") {
      blockers.push_back("the coordinator is at wal_level " + cw);
    }
    const auto nl = citus_workers_not_logical(citus);
    if (!nl.empty()) {
      blockers.push_back(detail::join(nl, ", ") + " not at wal_level logical");
    }
    const auto col_reading = columns.value(column, json::object());
    if (col_reading.contains("not_null") && !col_reading.value("not_null", false)) {
      blockers.push_back(column + " is nullable, and a NULL in it fails the "
                         "concurrent copy halfway");
    }
  }
  if (when == "always" && has_concurrent && !blockers.empty()) {
    step.action = Action::kConflict;
    step.why = "concurrently: always was asked for, and " + detail::join(blockers, "; ");
    plan.conflicts.push_back(
        step.why + ". The concurrent form is not rehearsed and does not roll back: "
        "measured, a failed one left the table half converted, registered as a "
        "Citus-managed local table. Fix what is named, or write concurrently: "
        "never to accept the blocking copy.");
    return;
  }
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
  } else if (rows > kCitusConcurrentRowThreshold && !blockers.empty()) {
    // auto would have chosen the concurrent form, and something rules it out.
    concurrent = false;
    decided_by = std::to_string(rows) + " estimated rows would take the "
                 "concurrent form, but " + detail::join(blockers, "; ") +
                 ", so the blocking form is the only safe path";
    plan.warnings.push_back(
        "At " + std::to_string(rows) + " estimated rows this would have been "
        "distributed concurrently, and " + detail::join(blockers, "; ") +
        ". Writes are blocked for the whole copy instead.");
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

  // A nullable distribution column accepts the distribution and refuses every
  // write that leaves it NULL -- measured, "cannot perform an INSERT with NULL
  // in the partition column". Existing NULL rows fail the blocking copy, which
  // the dry run executes, so that case is caught before anything commits.
  {
    const auto col_reading = columns.value(column, json::object());
    if (col_reading.contains("not_null") && !col_reading.value("not_null", false)) {
      plan.warnings.push_back(
          qualified + "." + column + " is nullable. Citus distributes it, and then "
          "refuses every write that leaves it NULL (measured). Make it NOT NULL, "
          "in an earlier intent, unless NULL genuinely never occurs.");
    }
  }
  citus_note_reference_copy(citus, step, plan, "Distributing " + qualified);

  if (concurrent) {
    // Measured: a table with NO primary key distributes concurrently too; what
    // it loses is UPDATE and DELETE during the copy. The earlier wording here
    // said such a table "cannot use it", which was false.
    plan.warnings.push_back(
        "create_distributed_table_concurrently keeps writes running. Without a "
        "primary key or REPLICA IDENTITY on " + qualified + ", UPDATE and DELETE "
        "on it fail until the copy finishes; INSERT still works. It cannot run "
        "inside a transaction block, so this step commits on its own and is not "
        "rehearsed.");
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

  if (citus_refuse_empty_cluster(in, citus, step, plan)) return;
  citus_note_reference_copy(citus, step, plan,
                            "Making " + qualified + " a reference table");

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

// --- Phase 2: the lifecycle of a distributed table --------------------------

// What Citus records a table as: "distributed", "reference", "local" (a
// Citus-managed local table, partmethod 'n' with repmodel 's'), or "" when
// Citus does not record it at all. The same distinction guard.h draws.
inline std::string citus_kind_of(const json& citus, const std::string& qualified) {
  const auto& d = citus_table(citus, qualified);
  if (d.empty()) return {};
  if (d.value("partmethod", "") != "n") return "distributed";
  return d.value("repmodel", "") == "t" ? "reference" : "local";
}

// The catalog names a related table by regclass::text, which drops the schema
// when it is on the search_path. Every table the Citus reading is keyed by is
// schema.table, so an unqualified name is read as public -- the only schema on
// the default search_path that holds tables.
inline std::string citus_qualify(const std::string& name) {
  return name.find('.') == std::string::npos ? "public." + name : name;
}

// Every table a foreign key joins to `t`, in either direction, as
// schema.table. `referenced_by` entries read "conname on table".
struct CitusForeignKey {
  std::string name;
  std::string other;
  bool outgoing;
};
inline std::vector<CitusForeignKey> citus_foreign_keys(const json& t) {
  std::vector<CitusForeignKey> out;
  const json constraints = t.value("constraints", json::object());
  for (auto it = constraints.begin(); it != constraints.end(); ++it) {
    if (it.value().value("type", "") != "f") continue;
    const auto ref = it.value().value("references", json(nullptr));
    if (!ref.is_string()) continue;
    out.push_back({it.key(), citus_qualify(ref.get<std::string>()), true});
  }
  for (const auto& e : t.value("referenced_by", json::array())) {
    if (!e.is_string()) continue;
    const auto s = e.get<std::string>();
    const auto on = s.find(" on ");
    if (on == std::string::npos) continue;
    out.push_back({s.substr(0, on), citus_qualify(s.substr(on + 4)), false});
  }
  return out;
}

// The other members of a table's colocation group, by reading.
inline std::vector<std::string> citus_colocated_with(const json& citus,
                                                     const std::string& qualified) {
  std::vector<std::string> out;
  const auto group = citus_table(citus, qualified).value("colocationid", -1LL);
  if (group <= 0) return out;
  const auto tables = citus.value("tables", json::object());
  for (auto it = tables.begin(); it != tables.end(); ++it) {
    if (it.key() == qualified) continue;
    if (it.value().value("colocationid", -2LL) == group) out.push_back(it.key());
  }
  return out;
}

inline void plan_citus_alter_distributed_table(const Intent& in,
                                               const Observations& obs,
                                               const ExecutorConfig& cfg,
                                               Plan& plan, std::vector<Step>& out) {
  (void)cfg;
  Step step;
  step.kind = in.kind_name;
  struct Emit { std::vector<Step>& o; Step& s; ~Emit() { o.push_back(s); } } emit{out, step};

  const auto qualified = in.qualified_table();
  const auto& citus = obs.extension("citus");
  bool already = false;
  if (!citus_precondition(in, obs, step, plan, citus, qualified, already)) return;

  // Measured: "cannot alter table because the table is not distributed" -- a
  // reference table included.
  if (citus_kind_of(citus, qualified) != "distributed") {
    step.action = Action::kConflict;
    step.why = qualified + " is not a distributed table" +
               (already ? " (Citus records it as a " +
                              citus_kind_of(citus, qualified) + " table)"
                        : std::string());
    plan.conflicts.push_back(
        step.why + ". alter_distributed_table changes how a table is "
        "distributed; to distribute it in the first place, use "
        "citus_distribute_table.");
    return;
  }

  const auto& t = obs.table(qualified);
  const auto& d = citus_table(citus, qualified);
  const json columns = t.value("columns", json::object());
  const auto current_col = d.value("distribution_column", "");
  const auto current_group = d.value("colocationid", -1LL);
  const auto members = citus_colocated_with(citus, qualified);

  // WHAT CHANGES. Each property is compared against the reading, and a call
  // that would change nothing is not made -- measured, Citus raises "this call
  // doesn't change any properties of the table" for it, so re-running a
  // specification whose change already happened would FAIL rather than be a
  // no-op without this.
  std::vector<std::string> changes;
  bool breaks_colocation = false;
  const auto col = in.body.value("distribution_column", "");
  const bool col_changes = !col.empty() && col != current_col;
  if (col_changes) {
    if (!columns.contains(col)) {
      step.action = Action::kConflict;
      step.why = qualified + " has no column " + col + " to distribute on";
      plan.conflicts.push_back(step.why);
      return;
    }
    // Measured: the rewrite gets as far as re-adding the primary key and then
    // fails -- "Distributed relations cannot have UNIQUE ... constraints that
    // do not include the partition column" -- after the data was copied.
    std::string offender, offender_columns;
    if (!citus_unique_keys_include(t, col, offender, offender_columns)) {
      step.action = Action::kConflict;
      step.why = offender + " on " + qualified + " covers (" + offender_columns +
                 "), which does not include the new distribution column " + col;
      plan.conflicts.push_back(
          step.why + ". Citus rewrites the table and then fails re-adding the "
          "key. Add " + col + " to that key first, in an earlier specification.");
      return;
    }
    changes.push_back("distribution column " + current_col + " -> " + col);
    breaks_colocation = !members.empty();
  }

  const bool cascade = in.body.value("cascade_to_colocated", false);
  if (in.body.contains("shard_count")) {
    const int want = in.body["shard_count"].get<int>();
    const int have = d.value("shard_count", 0);
    if (want != have) {
      // Measured: "cascade_to_colocated parameter is necessary" when the group
      // has other members. Refused here with the same choice spelled out, since
      // either answer is a decision about tables this intent does not name.
      if (!members.empty() && !in.body.contains("cascade_to_colocated")) {
        step.action = Action::kConflict;
        step.why = qualified + " shares colocation group " +
                   std::to_string(current_group) + " with " +
                   detail::join(members, ", ") +
                   ", and changing its shard count has to say what happens to them";
        plan.conflicts.push_back(
            step.why + ". Write cascade_to_colocated: true to change them all "
            "(each is rewritten too), or false to move " + qualified +
            " out of the group on its own.");
        return;
      }
      changes.push_back("shard count " + std::to_string(have) + " -> " +
                        std::to_string(want));
      if (!cascade) breaks_colocation = breaks_colocation || !members.empty();
    }
  }

  const auto with = in.body.value("colocate_with", "");
  long long target_group = -1;
  if (with == "none") {
    // "A group of its own". Measured, Citus rewrites a table that is already
    // alone into a NEW group rather than refusing, so alone is read as done.
    if (!members.empty()) {
      changes.push_back("leaves colocation group " + std::to_string(current_group));
      breaks_colocation = true;
    }
  } else if (!with.empty()) {
    const auto& other = citus_table(citus, with);
    if (citus_kind_of(citus, with) != "distributed") {
      step.action = Action::kConflict;
      step.why = "colocate_with names " + with +
                 ", which this database does not record as distributed";
      plan.conflicts.push_back(
          step.why + ". A table can only join a colocation group that exists.");
      return;
    }
    target_group = other.value("colocationid", -1LL);
    if (target_group != current_group) {
      // Same exact-type rule as distributing: checked against the column the
      // table WILL be distributed on.
      const auto on = col.empty() ? current_col : col;
      const auto want_oid = other.value("distribution_type_oid", 0LL);
      const auto have_oid =
          columns.value(on, json::object()).value("type_oid", 0LL);
      if (want_oid != 0 && have_oid != 0 && want_oid != have_oid) {
        step.action = Action::kConflict;
        step.why = qualified + "." + on + " is " +
                   columns.value(on, json::object()).value("type", "?") + ", and " +
                   with + " is distributed on a " +
                   other.value("distribution_type", "?") + " column";
        plan.conflicts.push_back(
            step.why + ". Citus colocates only tables whose distribution "
            "columns are the same type exactly (\"Distribution column types "
            "don't match\").");
        return;
      }
      changes.push_back("joins colocation group " + std::to_string(target_group) +
                        " of " + with + ", adopting its " +
                        std::to_string(other.value("shard_count", 0)) + " shards");
      breaks_colocation = breaks_colocation || !members.empty();
      step.detail["colocation_group"] = target_group;
    }
  }

  if (changes.empty()) {
    step.action = Action::kSatisfied;
    step.why = qualified + " is already distributed as asked (on " + current_col +
               ", " + std::to_string(d.value("shard_count", 0)) +
               " shards, colocation group " + std::to_string(current_group) + ")";
    return;
  }

  // FOREIGN KEYS THAT WOULD BE DROPPED. The finding that decided this check:
  // moving a table out of its group while a distributed table holds a foreign
  // key to it SUCCEEDS, with a WARNING -- "foreign key b_fk will be dropped" --
  // and the constraint is gone. Nothing fails, so nothing downstream would
  // notice. A key to or from a REFERENCE table survives (measured), and a
  // cascaded shard-count change keeps the group together and re-creates its
  // keys (measured), so only a key to another distributed table across a
  // change that breaks colocation is refused.
  if (breaks_colocation || col_changes) {
    for (const auto& fk : citus_foreign_keys(t)) {
      if (citus_kind_of(citus, fk.other) != "distributed") continue;
      if (fk.other == qualified) continue;
      step.action = Action::kConflict;
      step.why = "foreign key " + fk.name + (fk.outgoing ? " from " : " to ") +
                 qualified + (fk.outgoing ? " to " : " from ") + fk.other +
                 " would be dropped";
      plan.conflicts.push_back(
          step.why + ". This change moves " + qualified + " out of the colocation "
          "group it shares with " + fk.other + ", and Citus drops the key with a "
          "warning rather than failing -- measured, the call succeeds and the "
          "constraint is gone. Drop the key in an earlier intent and re-add it "
          "once both tables are colocated again, or change the whole group with "
          "cascade_to_colocated: true.");
      return;
    }
  }

  // The group the table ends up in, for the projection: the target's, the
  // current one when the change keeps the group together, or 0 -- "a group not
  // read yet" -- when it leaves for a new one.
  if (!step.detail.contains("colocation_group")) {
    step.detail["colocation_group"] =
        (breaks_colocation || col_changes || with == "none") ? 0LL : current_group;
  }

  std::string args = detail::quote_literal(qualified);
  if (col_changes) args += ", distribution_column := " + detail::quote_literal(col);
  if (in.body.contains("shard_count") &&
      in.body["shard_count"].get<int>() != d.value("shard_count", 0)) {
    args += ", shard_count := " + std::to_string(in.body["shard_count"].get<int>());
  }
  if (with == "none" && !members.empty()) {
    args += ", colocate_with := 'none'";
  } else if (!with.empty() && with != "none" && target_group != current_group) {
    args += ", colocate_with := " + detail::quote_literal(with);
  }
  if (in.body.contains("cascade_to_colocated")) {
    args += std::string(", cascade_to_colocated := ") + (cascade ? "true" : "false");
  }
  step.sql.push_back("SELECT alter_distributed_table(" + args + ");");

  const long long rows = t.value("reltuples", 0LL);
  step.action = Action::kApply;
  step.txn_class = TxnClass::kRequired;
  // Rehearsed by execution: see plan_citus_distribute_table.
  step.detail["rehearse_by"] = "execution";
  step.detail["changes"] = changes;
  step.detail["rows_estimated"] = rows;
  step.why = detail::join(changes, "; ");
  // Measured: Citus creates a new table, moves the data, drops the old one and
  // renames -- a full copy under an exclusive lock, with no concurrent form.
  std::vector<std::string> rewritten{qualified};
  if (cascade && !members.empty()) {
    rewritten.insert(rewritten.end(), members.begin(), members.end());
    step.detail["cascades_to"] = members;
  }
  step.lock = "AccessExclusiveLock on " + detail::join(rewritten, ", ") +
              " (each is rewritten: new table, data copied, old one dropped)";
  plan.warnings.push_back(
      "alter_distributed_table rewrites " + detail::join(rewritten, ", ") +
      " in full -- " + std::to_string(rows) + " estimated rows for " + qualified +
      " -- under an exclusive lock, with no concurrent form. Reads and writes "
      "wait for the whole copy.");
}

inline void plan_citus_undistribute_table(const Intent& in, const Observations& obs,
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

  // Measured: "cannot undistribute table because the table is not
  // distributed", so a re-run needs this to be read as done.
  if (!already) {
    step.action = Action::kSatisfied;
    step.why = qualified + " is not a Citus table";
    return;
  }

  // Measured: a foreign key in EITHER direction to another table stops it --
  // "cannot complete operation because table a is referenced by a foreign key"
  // -- unless cascade_via_foreign_keys, which undistributes every table the
  // keys connect. That is a decision about other tables, so it is written down
  // or refused.
  const auto& t = obs.table(qualified);
  const bool cascade = in.body.value("cascade_via_foreign_keys", false);
  std::vector<std::string> connected;
  for (const auto& fk : citus_foreign_keys(t)) {
    if (fk.other == qualified) continue;
    if (citus_kind_of(citus, fk.other).empty()) continue;
    if (std::find(connected.begin(), connected.end(), fk.other) == connected.end()) {
      connected.push_back(fk.other);
    }
  }
  if (!connected.empty() && !cascade) {
    step.action = Action::kConflict;
    step.why = qualified + " is joined by foreign keys to " +
               detail::join(connected, ", ");
    plan.conflicts.push_back(
        step.why + ", which Citus also records. Citus refuses to undistribute "
        "one end of a key alone. Write cascade_via_foreign_keys: true to "
        "undistribute every table the keys connect -- they are rewritten too.");
    return;
  }

  const auto kind = citus_kind_of(citus, qualified);
  const long long rows = t.value("reltuples", 0LL);
  step.action = Action::kApply;
  step.txn_class = TxnClass::kRequired;
  // Rehearsed by execution: see plan_citus_distribute_table.
  step.detail["rehearse_by"] = "execution";
  step.detail["was"] = kind;
  step.detail["rows_estimated"] = rows;
  std::vector<std::string> rewritten{qualified};
  if (cascade && !connected.empty()) {
    rewritten.insert(rewritten.end(), connected.begin(), connected.end());
    step.detail["cascades_to"] = connected;
  }
  step.lock = "AccessExclusiveLock on " + detail::join(rewritten, ", ") +
              " (each is rewritten onto the coordinator)";
  step.why = qualified + " is a " + kind +
             " table; its rows are gathered back into an ordinary table on the "
             "coordinator";
  std::string call = "SELECT undistribute_table(" + detail::quote_literal(qualified);
  if (in.body.contains("cascade_via_foreign_keys")) {
    call += std::string(", cascade_via_foreign_keys := ") + (cascade ? "true" : "false");
  }
  step.sql.push_back(call + ");");
  plan.warnings.push_back(
      "undistribute_table copies every row of " + detail::join(rewritten, ", ") +
      " onto the coordinator -- " + std::to_string(rows) + " estimated rows for " +
      qualified + " -- under an exclusive lock. The coordinator needs the disk "
      "for all of it, and queries lose the workers' parallelism from then on.");
}

inline void plan_citus_add_local_table_to_metadata(const Intent& in,
                                                   const Observations& obs,
                                                   const ExecutorConfig& cfg,
                                                   Plan& plan,
                                                   std::vector<Step>& out) {
  (void)cfg;
  Step step;
  step.kind = in.kind_name;
  struct Emit { std::vector<Step>& o; Step& s; ~Emit() { o.push_back(s); } } emit{out, step};

  const auto qualified = in.qualified_table();
  const auto& citus = obs.extension("citus");
  bool already = false;
  if (!citus_precondition(in, obs, step, plan, citus, qualified, already)) return;

  const auto kind = citus_kind_of(citus, qualified);
  // Measured: a second call on a managed local table is silent, so reading it
  // as done changes nothing but the plan's honesty. On a distributed or
  // reference table Citus refuses -- "table is already distributed".
  if (kind == "local") {
    step.action = Action::kSatisfied;
    step.why = qualified + " is already in Citus metadata as a local table";
    return;
  }
  if (!kind.empty()) {
    step.action = Action::kConflict;
    step.why = qualified + " is already a " + kind + " table";
    plan.conflicts.push_back(
        step.why + ", and Citus refuses to add it again as a local one. "
        "Undistribute it first, which moves its data back, if that is the "
        "intent.");
    return;
  }

  // Measured: a foreign key to any other table stops it -- "relation is
  // involved in a foreign key relationship with another table" -- unless
  // cascade_via_foreign_keys adds every connected table too. A key to a table
  // Citus already records is the case this kind exists for and does not need
  // the cascade; one to a plain local table does.
  const auto& t = obs.table(qualified);
  const bool cascade = in.body.value("cascade_via_foreign_keys", false);
  std::vector<std::string> plain;
  for (const auto& fk : citus_foreign_keys(t)) {
    if (fk.other == qualified) continue;
    if (!citus_kind_of(citus, fk.other).empty()) continue;
    if (std::find(plain.begin(), plain.end(), fk.other) == plain.end()) {
      plain.push_back(fk.other);
    }
  }
  if (!plain.empty() && !cascade) {
    step.action = Action::kConflict;
    step.why = qualified + " is joined by foreign keys to " +
               detail::join(plain, ", ") + ", which Citus does not record";
    plan.conflicts.push_back(
        step.why + ". Citus adds a table to its metadata only together with "
        "the tables its keys connect. Write cascade_via_foreign_keys: true to "
        "add them all.");
    return;
  }

  if (citus_refuse_empty_cluster(in, citus, step, plan)) return;

  step.action = Action::kApply;
  step.txn_class = TxnClass::kRequired;
  // Rehearsed by execution: see plan_citus_distribute_table.
  step.detail["rehearse_by"] = "execution";
  if (cascade && !plain.empty()) step.detail["cascades_to"] = plain;
  step.lock = "AccessExclusiveLock on " + qualified +
              " (metadata only: the rows stay where they are)";
  step.why = qualified +
             " stays on the coordinator and becomes visible to Citus, so it can "
             "hold foreign keys with reference tables and join with them";
  std::string call = "SELECT citus_add_local_table_to_metadata(" +
                     detail::quote_literal(qualified);
  if (in.body.contains("cascade_via_foreign_keys")) {
    call += std::string(", cascade_via_foreign_keys := ") + (cascade ? "true" : "false");
  }
  step.sql.push_back(call + ");");
}

inline void plan_citus_truncate_local_data(const Intent& in, const Observations& obs,
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

  // Measured: on a table Citus does not distribute, "supplied parameter is not
  // a distributed relation". A reference table is accepted.
  const auto kind = citus_kind_of(citus, qualified);
  if (kind != "distributed" && kind != "reference") {
    step.action = Action::kConflict;
    step.why = qualified + " is not a distributed or reference table" +
               (kind.empty() ? std::string() : " (it is a Citus-managed local one)");
    plan.conflicts.push_back(
        step.why + ", so its local rows are its only rows. Nothing here would be "
        "left behind by a distribution.");
    return;
  }

  // THE CASCADE. Measured: the call truncates with CASCADE, and a local table
  // holding a foreign key to this one was emptied -- "truncate cascades to table
  // lref" -- and that table's rows were its real, only rows. Cascading into a
  // DISTRIBUTED table is harmless (measured: it reached the coordinator's stale
  // copy and Citus still served every row), so only other referencing tables
  // are refused.
  const auto& t = obs.table(qualified);
  for (const auto& fk : citus_foreign_keys(t)) {
    if (fk.outgoing || fk.other == qualified) continue;
    if (citus_kind_of(citus, fk.other) == "distributed") continue;
    step.action = Action::kConflict;
    step.why = fk.other + " holds foreign key " + fk.name + " to " + qualified +
               ", and is not distributed";
    plan.conflicts.push_back(
        step.why + ". This call truncates with CASCADE, so " + fk.other +
        "'s own rows -- not a stale copy -- would be deleted too. Measured: a "
        "local table referencing a reference table was emptied. Drop the key "
        "first, or distribute " + fk.other + ".");
    return;
  }

  step.action = Action::kApply;
  step.txn_class = TxnClass::kRequired;
  // Rehearsed by execution: see plan_citus_distribute_table.
  step.detail["rehearse_by"] = "execution";
  step.lock = "AccessExclusiveLock on " + qualified + " (a TRUNCATE of the "
              "coordinator's copy; the shards are not touched)";
  step.why = "create_distributed_table leaves the coordinator's rows in place "
             "after copying them to the shards; this frees that space";
  step.sql.push_back("SELECT truncate_local_data_after_distributing_table(" +
                     detail::quote_literal(qualified) + ");");
  plan.warnings.push_back(
      "This TRUNCATEs the coordinator's local copy of " + qualified +
      ". The rows Citus serves live in the shards and are untouched; the local "
      "copy cannot be recovered afterwards other than by undistributing.");
}

// --- Phase 3: the cluster itself --------------------------------------------

inline std::string citus_node_name(const Intent& in) {
  return in.body.value("host", "") + ":" + std::to_string(in.body.value("port", 5432));
}

// The node the intent names, as read, or an empty object.
inline const json& citus_node(const json& citus, const Intent& in) {
  static const json kEmpty = json::object();
  const auto host = in.body.value("host", "");
  const int port = in.body.value("port", 5432);
  const auto nodes = citus.find("nodes");
  if (nodes == citus.end() || !nodes->is_array()) return kEmpty;
  for (const auto& n : *nodes) {
    if (n.value("nodename", "") == host && n.value("nodeport", 0) == port) return n;
  }
  return kEmpty;
}

// The SQL spelling of the node the intent names: 'host', port.
inline std::string citus_node_args(const Intent& in) {
  return detail::quote_literal(in.body.value("host", "")) + ", " +
         std::to_string(in.body.value("port", 5432));
}

inline void citus_refuse_absent_node(const Intent& in, Step& step, Plan& plan) {
  step.action = Action::kConflict;
  step.why = citus_node_name(in) + " is not a node of this cluster";
  plan.conflicts.push_back(
      step.why + ". Add it first -- with citus_add_node or citus_ensure_workers "
      "in an earlier intent of this specification, or in an earlier "
      "specification.");
}

// A shard move needs logical replication unless it blocks writes. Refused
// before anything runs, because the measured failure is not clean: a drain
// failed at its first shard having already marked the node as holding none.
inline bool citus_transfer_mode_possible(const Intent& in, const json& citus,
                                         Step& step, Plan& plan) {
  const auto mode = in.body.value("transfer_mode", "auto");
  if (mode == "block_writes") return true;
  const auto not_logical = citus_workers_not_logical(citus);
  if (not_logical.empty()) return true;
  step.action = Action::kConflict;
  step.why = "transfer_mode " + mode + " moves shards by logical replication, and " +
             detail::join(not_logical, ", ") + " not at wal_level = logical";
  plan.conflicts.push_back(
      step.why + ". Measured: the move fails at its first shard (\"logical "
      "decoding requires wal_level >= logical\"). Set wal_level = logical on the "
      "workers and restart them, or write transfer_mode: block_writes -- writes to "
      "each shard then wait while it is copied.");
  return false;
}

// Measured: with the coordinator registered as localhost, citus_add_node is
// refused -- "cannot add a worker node when the coordinator hostname is set to
// localhost" -- because the workers could not connect back to it. Read from the
// group-0 entry, so a citus_set_coordinator_host earlier in the same
// specification (projected) clears it.
inline bool citus_refuse_localhost_coordinator(const json& citus, Step& step,
                                               Plan& plan) {
  for (const auto& n : citus.value("nodes", json::array())) {
    if (n.value("groupid", -1) != 0) continue;
    const auto host = n.value("nodename", "");
    if (host != "localhost" && host != "127.0.0.1" && host != "::1") return false;
    step.action = Action::kConflict;
    step.why = "the coordinator is registered as " + host +
               ", which the workers cannot connect back to";
    plan.conflicts.push_back(
        step.why + ". Citus refuses to add a worker then (\"cannot add a worker "
        "node when the coordinator hostname is set to localhost\"). Register its "
        "real address with citus_set_coordinator_host first -- earlier in this "
        "specification is enough. A coordinator becomes localhost when a table is "
        "distributed before anything was registered.");
    return true;
  }
  return false;
}

inline void plan_citus_add_node(const Intent& in, const Observations& obs,
                                const ExecutorConfig& cfg, Plan& plan,
                                std::vector<Step>& out) {
  (void)cfg;
  Step step;
  step.kind = in.kind_name;
  struct Emit { std::vector<Step>& o; Step& s; ~Emit() { o.push_back(s); } } emit{out, step};
  const auto& citus = obs.extension("citus");
  if (!citus_present(in, step, plan, citus)) return;

  const auto name = citus_node_name(in);
  const auto& node = citus_node(citus, in);
  const bool want_shards = in.body.value("should_have_shards", true);
  if (!node.empty()) {
    if (!node.value("isactive", true)) {
      step.action = Action::kConflict;
      step.why = name + " is registered and inactive";
      plan.conflicts.push_back(
          step.why + ". Adding it again does not activate it; that is "
          "citus_activate_node, and a node that was disabled usually was for a "
          "reason someone should read first.");
      return;
    }
    if (!in.body.contains("should_have_shards") ||
        node.value("shouldhaveshards", true) == want_shards) {
      step.action = Action::kSatisfied;
      step.why = name + " is already a node of this cluster";
      return;
    }
  }

  if (node.empty()) {
    if (citus_refuse_localhost_coordinator(citus, step, plan)) return;
    const auto refs = citus.value("reference_tables", json::object());
    if (refs.value("count", 0) > 0) {
      plan.warnings.push_back(
          name + " joins holding none of the " +
          std::to_string(refs.value("count", 0)) + " reference table(s) (" +
          (refs.value("bytes", json(nullptr)).is_number()
               ? std::to_string(refs["bytes"].get<long long>() / (1024 * 1024)) + " MiB per copy"
               : std::string("size not read")) +
          "). Adding it copies nothing -- measured -- and the next "
          "distribute or rebalance copies them all to it.");
    }
  }
  step.action = Action::kApply;
  step.txn_class = TxnClass::kRequired;
  // Rehearsed by execution. Measured: citus_add_node runs in a transaction and
  // a ROLLBACK removes the node again -- and it connects to the node as it
  // runs, so a host that cannot be reached is found by the dry run ("could not
  // translate host name") rather than by the migration.
  step.detail["rehearse_by"] = "execution";
  step.detail["node"] = name;
  step.lock = "no table lock: cluster metadata, propagated to every node";
  if (node.empty()) {
    step.why = name + " joins the cluster";
    step.sql.push_back("SELECT citus_add_node(" + citus_node_args(in) + ");");
    plan.warnings.push_back(
        name + " holds no shards when it joins. Existing shards move onto it only "
        "when the cluster is rebalanced -- citus_rebalance_shards, in a later "
        "specification.");
  } else {
    step.why = name + " is already a node; only whether it may hold shards changes";
  }
  if (in.body.contains("should_have_shards") &&
      (node.empty() ? !want_shards
                    : node.value("shouldhaveshards", true) != want_shards)) {
    step.sql.push_back("SELECT citus_set_node_property(" + citus_node_args(in) +
                       ", 'shouldhaveshards', " + (want_shards ? "true" : "false") +
                       ");");
  }
}

inline void plan_citus_remove_node(const Intent& in, const Observations& obs,
                                   const ExecutorConfig& cfg, Plan& plan,
                                   std::vector<Step>& out) {
  (void)cfg;
  Step step;
  step.kind = in.kind_name;
  struct Emit { std::vector<Step>& o; Step& s; ~Emit() { o.push_back(s); } } emit{out, step};
  const auto& citus = obs.extension("citus");
  if (!citus_present(in, step, plan, citus)) return;

  const auto name = citus_node_name(in);
  const auto& node = citus_node(citus, in);
  // Measured: removing a node that is not there is an error, "node at ... does
  // not exist". Gone is what was asked for.
  if (node.empty()) {
    step.action = Action::kSatisfied;
    step.why = name + " is not a node of this cluster";
    return;
  }
  if (node.value("groupid", -1) == 0) {
    step.action = Action::kConflict;
    step.why = name + " is the coordinator";
    plan.conflicts.push_back(
        step.why + ". Removing it from the metadata strands every reference and "
        "Citus-managed local table's coordinator placement. Change its address "
        "with citus_set_coordinator_host instead.");
    return;
  }
  // Measured: "cannot remove or disable the node ... because it contains the
  // only shard placement for shard ...". The count is in the reading, so the
  // author learns it before, with what to do about it.
  const auto placements = node.value("placements", 0LL);
  if (placements > 0) {
    step.action = Action::kConflict;
    step.why = name + " holds " + std::to_string(placements) +
               " shard placements of distributed tables";
    plan.conflicts.push_back(
        step.why + ", and Citus refuses to remove a node holding the only copy of "
        "a shard. Drain it first with citus_drain_node, in an earlier intent or "
        "specification.");
    return;
  }

  step.action = Action::kApply;
  step.txn_class = TxnClass::kRequired;
  // Rehearsed by execution: measured, citus_remove_node rolls back cleanly.
  step.detail["rehearse_by"] = "execution";
  step.detail["node"] = name;
  step.lock = "no table lock: cluster metadata, propagated to every node";
  step.why = name + " holds no distributed shards and leaves the cluster";
  step.sql.push_back("SELECT citus_remove_node(" + citus_node_args(in) + ");");
}

inline void plan_citus_set_coordinator_host(const Intent& in,
                                            const Observations& obs,
                                            const ExecutorConfig& cfg, Plan& plan,
                                            std::vector<Step>& out) {
  (void)cfg;
  Step step;
  step.kind = in.kind_name;
  struct Emit { std::vector<Step>& o; Step& s; ~Emit() { o.push_back(s); } } emit{out, step};
  const auto& citus = obs.extension("citus");
  if (!citus_present(in, step, plan, citus)) return;

  const auto name = citus_node_name(in);
  std::string current;
  for (const auto& n : citus.value("nodes", json::array())) {
    if (n.value("groupid", -1) == 0) {
      current = n.value("nodename", "") + ":" + std::to_string(n.value("nodeport", 0));
    }
  }
  if (current == name) {
    step.action = Action::kSatisfied;
    step.why = "the coordinator is already registered as " + name;
    return;
  }
  step.action = Action::kApply;
  step.txn_class = TxnClass::kRequired;
  // Rehearsed by execution: measured, a changed coordinator host rolls back.
  step.detail["rehearse_by"] = "execution";
  step.detail["node"] = name;
  step.lock = "no table lock: cluster metadata, propagated to every node";
  step.why = current.empty()
                 ? "the coordinator is registered as " + name +
                       ", so the workers can reach it"
                 : "the coordinator's registered address changes from " + current +
                       " to " + name;
  step.sql.push_back("SELECT citus_set_coordinator_host(" + citus_node_args(in) +
                     ");");
  if (!current.empty()) {
    plan.warnings.push_back(
        "The workers reach the coordinator at " + name +
        " from now on. If that address does not resolve from them, queries that "
        "touch reference or Citus-managed local tables fail.");
  }
}

inline void plan_citus_set_node_property(const Intent& in, const Observations& obs,
                                         const ExecutorConfig& cfg, Plan& plan,
                                         std::vector<Step>& out) {
  (void)cfg;
  Step step;
  step.kind = in.kind_name;
  struct Emit { std::vector<Step>& o; Step& s; ~Emit() { o.push_back(s); } } emit{out, step};
  const auto& citus = obs.extension("citus");
  if (!citus_present(in, step, plan, citus)) return;

  const auto name = citus_node_name(in);
  const auto& node = citus_node(citus, in);
  if (node.empty()) {
    citus_refuse_absent_node(in, step, plan);
    return;
  }
  const bool want = in.body.value("should_have_shards", true);
  if (node.value("shouldhaveshards", !want) == want) {
    step.action = Action::kSatisfied;
    step.why = name + (want ? " already may" : " already may not") + " hold shards";
    return;
  }
  step.action = Action::kApply;
  step.txn_class = TxnClass::kRequired;
  // Rehearsed by execution: measured, the property change rolls back.
  step.detail["rehearse_by"] = "execution";
  step.detail["node"] = name;
  step.lock = "no table lock: cluster metadata, propagated to every node";
  step.why = name + (want ? " may hold shards" : " may no longer hold new shards");
  step.sql.push_back("SELECT citus_set_node_property(" + citus_node_args(in) +
                     ", 'shouldhaveshards', " + (want ? "true" : "false") + ");");
  const auto placements = node.value("placements", 0LL);
  if (!want && placements > 0) {
    plan.warnings.push_back(
        name + " keeps the " + std::to_string(placements) +
        " placements it holds: this only stops new shards arriving. "
        "citus_drain_node moves the existing ones.");
  }
}

inline void plan_citus_drain_node(const Intent& in, const Observations& obs,
                                  const ExecutorConfig& cfg, Plan& plan,
                                  std::vector<Step>& out) {
  (void)cfg;
  Step step;
  step.kind = in.kind_name;
  struct Emit { std::vector<Step>& o; Step& s; ~Emit() { o.push_back(s); } } emit{out, step};
  const auto& citus = obs.extension("citus");
  if (!citus_present(in, step, plan, citus)) return;

  const auto name = citus_node_name(in);
  const auto& node = citus_node(citus, in);
  if (node.empty()) {
    citus_refuse_absent_node(in, step, plan);
    return;
  }
  const auto placements = node.value("placements", 0LL);
  if (placements == 0 && !node.value("shouldhaveshards", true)) {
    step.action = Action::kSatisfied;
    step.why = name + " holds no distributed shards and takes no new ones";
    return;
  }
  // Somewhere for the shards to go.
  int elsewhere = 0;
  for (const auto& n : citus.value("nodes", json::array())) {
    if (n.value("nodename", "") == node.value("nodename", "") &&
        n.value("nodeport", 0) == node.value("nodeport", 0)) {
      continue;
    }
    if (n.value("shouldhaveshards", false) && n.value("isactive", false) &&
        n.value("noderole", "") == "primary") {
      ++elsewhere;
    }
  }
  if (elsewhere == 0 && placements > 0) {
    step.action = Action::kConflict;
    step.why = "no other node may hold shards, so " + name + "'s " +
               std::to_string(placements) + " placements have nowhere to go";
    plan.conflicts.push_back(
        step.why + ". Add a node, or let one hold shards with "
        "citus_set_node_property, in an earlier intent or specification.");
    return;
  }
  if (!citus_transfer_mode_possible(in, citus, step, plan)) return;

  const auto mode = in.body.value("transfer_mode", "auto");
  step.action = Action::kApply;
  // OUTSIDE a transaction, and not rehearsed. Measured: citus_drain_node marks
  // the node shouldhaveshards = false on a connection of its own, and that
  // survives the caller's ROLLBACK -- even when the drain itself then fails. A
  // dry run that executed it would change the cluster; one that wrapped it in
  // a transaction would claim an atomicity Citus does not give. kForbidden
  // says both: the dry run lists the step as unverified, and the executor
  // runs it on its own.
  step.txn_class = TxnClass::kForbidden;
  step.detail["node"] = name;
  step.detail["placements"] = placements;
  step.detail["transfer_mode"] = mode;
  step.detail["on_failure"] =
      "a failed drain keeps the shards it had already moved, and leaves " + name +
      " marked as taking no new shards. Nothing needs undoing: running the "
      "specification again drains the rest.";
  step.lock = mode == "block_writes"
                  ? "writes to each shard wait while that shard is copied"
                  : "each shard is copied by logical replication; writes wait "
                    "only for the final switch";
  step.why = std::to_string(placements) + " placements move off " + name +
             ", and it takes no new shards";
  step.sql.push_back("SELECT citus_drain_node(" + citus_node_args(in) +
                     ", shard_transfer_mode := " + detail::quote_literal(mode) +
                     ");");
  plan.warnings.push_back(
      "citus_drain_node is not atomic. It marks " + name +
      " as taking no shards before it moves any -- measured, that mark stays even "
      "when the drain then fails -- and each shard move commits on its own. A "
      "failed drain leaves some shards moved; running the specification again "
      "carries on from there.");
}

inline void plan_citus_rebalance_shards(const Intent& in, const Observations& obs,
                                        const ExecutorConfig& cfg, Plan& plan,
                                        std::vector<Step>& out) {
  (void)cfg;
  Step step;
  step.kind = in.kind_name;
  struct Emit { std::vector<Step>& o; Step& s; ~Emit() { o.push_back(s); } } emit{out, step};
  const auto& citus = obs.extension("citus");
  if (!citus_present(in, step, plan, citus)) return;

  // STALE when an earlier intent of this specification changed membership:
  // the plan Citus gave was for the cluster before it. Then the rebalance runs
  // unconditionally -- Citus plans again when it runs, and a rebalance with
  // nothing to move does nothing -- rather than reading an old "balanced".
  const bool unreadable = citus.value("rebalance_moves", json(nullptr)).is_string();
  if (citus.contains("rebalance_moves_stale_after_step") || unreadable) {
    if (!citus_transfer_mode_possible(in, citus, step, plan)) return;
    const auto mode = in.body.value("transfer_mode", "auto");
    const int after = citus.value("rebalance_moves_stale_after_step", 0);
    step.action = Action::kApply;
    step.txn_class = TxnClass::kForbidden;
    step.detail["transfer_mode"] = mode;
    step.detail["moves"] =
        unreadable ? "unknown: " + citus.value("rebalance_moves", std::string())
                   : "unknown: step " + std::to_string(after) +
                         " of this plan changes the cluster's nodes first";
    step.detail["on_failure"] =
        "a failed rebalance keeps the moves it had already made. Nothing needs "
        "undoing: running the specification again re-reads Citus's plan and "
        "makes the rest.";
    step.lock = mode == "block_writes"
                    ? "writes to each shard wait while that shard is copied"
                    : "each shard is copied by logical replication; writes wait "
                      "only for the final switch";
    step.why = unreadable
                   ? std::string("Citus's rebalance plan could not be read here, "
                                 "so the moves are decided by Citus when this runs")
                   : std::string("an earlier intent changes the cluster's nodes, so "
                                 "the moves are decided by Citus when this runs");
    step.sql.push_back("SELECT rebalance_table_shards(shard_transfer_mode := " +
                       detail::quote_literal(mode) + ");");
    plan.warnings.push_back(
        "The rebalance follows a change of the cluster's nodes in this same "
        "specification, so how many shards move is not known when planning. "
        "Each move commits on its own; a failure keeps the ones before it.");
    return;
  }
  const auto moves = citus.value("rebalance_moves", json(nullptr));
  if (!moves.is_array()) {
    step.action = Action::kConflict;
    step.why = "Citus cannot plan a rebalance here: fewer nodes may hold shards "
               "than citus.shard_replication_factor requires";
    plan.conflicts.push_back(
        step.why + ". Measured, Citus raises \"Shard replication factor cannot "
        "be greater than number of nodes with should_have_shards=true\". Add a "
        "node, or let one hold shards, in an earlier intent or specification.");
    return;
  }
  // Balanced by Citus's own definition: the rebalancer's plan is empty.
  if (moves.empty()) {
    step.action = Action::kSatisfied;
    step.why = "the cluster is balanced: Citus's rebalance plan has no moves";
    return;
  }
  if (!citus_transfer_mode_possible(in, citus, step, plan)) return;

  const auto mode = in.body.value("transfer_mode", "auto");
  step.action = Action::kApply;
  // OUTSIDE a transaction, and not rehearsed. Measured: inside BEGIN ...
  // ROLLBACK, rebalance_table_shards MOVED A SHARD AND KEPT IT MOVED -- every
  // move runs and commits on connections of its own. See plan_citus_drain_node.
  step.txn_class = TxnClass::kForbidden;
  step.detail["moves"] = moves.size();
  json shown = json::array();
  for (std::size_t i = 0; i < moves.size() && i < 20; ++i) shown.push_back(moves[i]);
  step.detail["first_moves"] = shown;
  step.detail["transfer_mode"] = mode;
  step.detail["on_failure"] =
      "a failed rebalance keeps the moves it had already made. Nothing needs "
      "undoing: running the specification again re-reads Citus's plan and makes "
      "the rest.";
  step.lock = mode == "block_writes"
                  ? "writes to each shard wait while that shard is copied"
                  : "each shard is copied by logical replication; writes wait "
                    "only for the final switch";
  step.why = std::to_string(moves.size()) +
             " shard moves, as Citus's own rebalance plan lists them now";
  citus_note_reference_copy(citus, step, plan, "The rebalance");
  // Synchronous: the step finishes when the rebalance does, so the ledger's
  // "applied" means balanced. citus_rebalance_start returns at once and leaves
  // a background job the ledger would know nothing about.
  step.sql.push_back("SELECT rebalance_table_shards(shard_transfer_mode := " +
                     detail::quote_literal(mode) + ");");
  plan.warnings.push_back(
      "rebalance_table_shards is not atomic: each of the " +
      std::to_string(moves.size()) +
      " moves commits on its own, and a failure leaves the ones before it done. "
      "Running the specification again re-reads the plan and carries on. The "
      "moves listed are the plan at the time of reading; Citus decides again "
      "when it runs.");
}

// A glob with * and ?, nothing else -- enough to say "*.citus.internal" or
// "10.20.30.*", and small enough to read. Written here rather than taken from
// <fnmatch.h> because this header is included inside namespace pglaswell,
// where a system header cannot go.
inline bool citus_glob(const std::string& pat, const std::string& s) {
  std::size_t p = 0, i = 0, star = std::string::npos, mark = 0;
  while (i < s.size()) {
    if (p < pat.size() && (pat[p] == '?' || pat[p] == s[i])) {
      ++p; ++i;
    } else if (p < pat.size() && pat[p] == '*') {
      star = p++; mark = i;
    } else if (star != std::string::npos) {
      p = star + 1; i = ++mark;
    } else {
      return false;
    }
  }
  while (p < pat.size() && pat[p] == '*') ++p;
  return p == pat.size();
}

inline void plan_citus_ensure_workers(const Intent& in, const Observations& obs,
                                      const ExecutorConfig& cfg, Plan& plan,
                                      std::vector<Step>& out) {
  Step step;
  step.kind = in.kind_name;
  struct Emit { std::vector<Step>& o; Step& s; ~Emit() { o.push_back(s); } } emit{out, step};
  const auto& citus = obs.extension("citus");
  if (!citus_present(in, step, plan, citus)) return;

  const auto refuse = [&](const std::string& why, const std::string& more) {
    step.action = Action::kConflict;
    step.why = why;
    plan.conflicts.push_back(why + ". " + more);
  };

  // WHICH WORKERS: from this connection's configuration, never from the spec.
  const auto it = cfg.module_settings.find("citus.workers");
  if (it == cfg.module_settings.end() || detail::trim(it->second).empty()) {
    refuse("this connection's configuration lists no citus.workers",
           "The specification says the cluster's workers are the configured "
           "set, and there is no set: add `citus.workers = host1:5432, host2` "
           "to this connection's section of the configuration file. With only "
           "a connection URL there is no section to put it in.");
    return;
  }
  struct Worker { std::string host; int port; };
  std::vector<Worker> want;
  std::vector<std::string> bad;
  {
    const auto& raw = it->second;
    std::size_t start = 0;
    while (start <= raw.size()) {
      const auto comma = raw.find(',', start);
      const auto item = detail::trim(raw.substr(
          start, comma == std::string::npos ? std::string::npos : comma - start));
      start = comma == std::string::npos ? raw.size() + 1 : comma + 1;
      if (item.empty()) { bad.push_back("(an empty entry)"); continue; }
      std::string host = item;
      int port = 5432;
      if (const auto colon = item.rfind(':'); colon != std::string::npos) {
        host = item.substr(0, colon);
        try {
          std::size_t used = 0;
          port = std::stoi(item.substr(colon + 1), &used);
          if (used != item.size() - colon - 1 || port <= 0 || port > 65535) port = -1;
        } catch (const std::exception&) {
          port = -1;
        }
      }
      if (host.empty() || port < 0 ||
          host.find_first_of(" \t'\"") != std::string::npos) {
        bad.push_back(item);
        continue;
      }
      want.push_back({host, port});
    }
  }
  if (!bad.empty()) {
    refuse("citus.workers has entries that are not host or host:port: " +
               detail::join(bad, ", "),
           "Write it as a comma-separated list, e.g. `citus.workers = "
           "worker1:5432, worker2`; the port defaults to 5432.");
    return;
  }
  std::vector<std::string> names;
  for (const auto& w : want) {
    const auto n = w.host + ":" + std::to_string(w.port);
    if (std::find(names.begin(), names.end(), n) != names.end()) {
      refuse("citus.workers lists " + n + " twice",
             "A duplicate is almost always a copy-paste of the wrong line; "
             "the other host it was meant to be is then missing.");
      return;
    }
    names.push_back(n);
  }
  step.detail["configured"] = names;
  step.detail["source"] = "citus.workers in this connection's configuration";

  // THE SIGNED BOUNDS on the unsigned list.
  if (in.body.contains("hosts_matching")) {
    const auto glob = in.body.value("hosts_matching", "");
    std::vector<std::string> outside;
    for (const auto& w : want) {
      if (!citus_glob(glob, w.host)) outside.push_back(w.host);
    }
    if (!outside.empty()) {
      refuse("citus.workers names " + detail::join(outside, ", ") +
                 ", outside hosts_matching \"" + glob + "\"",
             "The specification limits which hosts may become workers, and "
             "the configuration names one it does not allow. A worker receives "
             "shards of the data, so this is refused rather than warned: fix "
             "the configuration, or sign a specification that allows it.");
      return;
    }
  }
  const int n = static_cast<int>(want.size());
  if (in.body.contains("min_workers") && n < in.body["min_workers"].get<int>()) {
    refuse("citus.workers lists " + std::to_string(n) + " workers, fewer than "
               "min_workers " + std::to_string(in.body["min_workers"].get<int>()),
           "The specification sets a floor, and this configuration is under it.");
    return;
  }
  if (in.body.contains("max_workers") && n > in.body["max_workers"].get<int>()) {
    refuse("citus.workers lists " + std::to_string(n) + " workers, more than "
               "max_workers " + std::to_string(in.body["max_workers"].get<int>()),
           "The specification sets a ceiling, and this configuration is over it.");
    return;
  }

  // AGAINST THE CLUSTER.
  const auto nodes = citus.value("nodes", json::array());
  std::vector<std::string> add_sql, adding, removing, remove_sql, unlisted;
  for (const auto& w : want) {
    const auto name = w.host + ":" + std::to_string(w.port);
    const json* found = nullptr;
    for (const auto& nd : nodes) {
      if (nd.value("nodename", "") == w.host && nd.value("nodeport", 0) == w.port) {
        found = &nd;
      }
    }
    if (found == nullptr) {
      adding.push_back(name);
      add_sql.push_back("SELECT citus_add_node(" + detail::quote_literal(w.host) +
                        ", " + std::to_string(w.port) + ");");
      continue;
    }
    if (found->value("groupid", -1) == 0) {
      refuse("citus.workers lists " + name + ", which is the coordinator",
             "The coordinator is registered separately (citus_set_coordinator_host); "
             "listing it as a worker is a mistake in the configuration.");
      return;
    }
    if (!found->value("isactive", true)) {
      refuse(name + " is registered and inactive",
             "Adding it again does not activate it; that is citus_activate_node, "
             "and a node that was disabled usually was for a reason someone "
             "should read first.");
      return;
    }
  }
  const auto policy = in.body.value("unlisted", "keep");
  for (const auto& nd : nodes) {
    if (nd.value("groupid", -1) == 0 || nd.value("noderole", "") != "primary") continue;
    const auto name = nd.value("nodename", "") + ":" +
                      std::to_string(nd.value("nodeport", 0));
    if (std::find(names.begin(), names.end(), name) != names.end()) continue;
    unlisted.push_back(name);
    if (policy == "remove") {
      const auto placements = nd.value("placements", 0LL);
      if (placements > 0) {
        refuse(name + " is not in citus.workers and holds " +
                   std::to_string(placements) + " shard placements",
               "unlisted: remove takes out only a worker that holds no shards. "
               "Drain it first with citus_drain_node, in an earlier intent or "
               "specification.");
        return;
      }
      removing.push_back(name);
      remove_sql.push_back("SELECT citus_remove_node(" +
                           detail::quote_literal(nd.value("nodename", "")) + ", " +
                           std::to_string(nd.value("nodeport", 0)) + ");");
    }
  }
  if (!unlisted.empty() && policy == "refuse") {
    refuse("registered workers are not in citus.workers: " +
               detail::join(unlisted, ", "),
           "unlisted: refuse asks for the cluster to be exactly the configured "
           "set. Add them to the configuration, or drain and remove them.");
    return;
  }
  if (!unlisted.empty() && policy == "keep") {
    plan.warnings.push_back(
        "Registered workers not in citus.workers are kept: " +
        detail::join(unlisted, ", ") +
        ". Write unlisted: refuse to make that an error, or unlisted: remove to "
        "take out the ones that hold no shards.");
    step.detail["kept_unlisted"] = unlisted;
  }

  if (adding.empty() && removing.empty()) {
    step.action = Action::kSatisfied;
    step.why = "the cluster's workers are the " + std::to_string(n) +
               " configured for this connection";
    return;
  }
  if (!adding.empty()) {
    if (citus_refuse_localhost_coordinator(citus, step, plan)) return;
    const auto refs = citus.value("reference_tables", json::object());
    if (refs.value("count", 0) > 0) {
      plan.warnings.push_back(
          detail::join(adding, ", ") + " join holding none of the " +
          std::to_string(refs.value("count", 0)) + " reference table(s) (" +
          (refs.value("bytes", json(nullptr)).is_number()
               ? std::to_string(refs["bytes"].get<long long>() / (1024 * 1024)) + " MiB per copy, to each"
               : std::string("size not read")) +
          "). Adding copies nothing -- measured -- and the "
          "next distribute or rebalance copies them all.");
    }
  }
  step.action = Action::kApply;
  step.txn_class = TxnClass::kRequired;
  // Rehearsed by execution: both calls roll back cleanly (measured), and
  // citus_add_node connects to the node as it runs, so an unreachable
  // configured host is reported by the dry run, not the migration.
  step.detail["rehearse_by"] = "execution";
  if (!adding.empty()) step.detail["adding"] = adding;
  if (!removing.empty()) step.detail["removing"] = removing;
  step.lock = "no table lock: cluster metadata, propagated to every node";
  std::vector<std::string> what;
  if (!adding.empty()) what.push_back("adds " + detail::join(adding, ", "));
  if (!removing.empty()) what.push_back("removes " + detail::join(removing, ", "));
  step.why = detail::join(what, "; ") + ", so the workers are the " +
             std::to_string(n) + " configured for this connection";
  for (const auto& q : add_sql) step.sql.push_back(q);
  for (const auto& q : remove_sql) step.sql.push_back(q);
  if (!adding.empty()) {
    plan.warnings.push_back(
        "New workers hold no shards until the cluster is rebalanced: a "
        "citus_rebalance_shards after this intent does it.");
  }
}

// --- Disable and activate ---------------------------------------------------
//
// Measured on Citus 13: citus_disable_node on a node holding the only copy of a
// shard is refused, as removal is; on the coordinator it is refused ("cannot
// change isactive field of the coordinator node"). Both calls run in a
// transaction and roll back cleanly. The DEFAULT, asynchronous, disable leaves
// the workers' metadata out of sync for a while, and the next citus_activate_node
// or citus_remove_node then fails -- "worker1:5432 is a metadata node, but is
// out of sync" -- so this always asks for synchronous => true, after which both
// ran at once, in the same transaction too.

inline void plan_citus_disable_node(const Intent& in, const Observations& obs,
                                    const ExecutorConfig& cfg, Plan& plan,
                                    std::vector<Step>& out) {
  (void)cfg;
  Step step;
  step.kind = in.kind_name;
  struct Emit { std::vector<Step>& o; Step& s; ~Emit() { o.push_back(s); } } emit{out, step};
  const auto& citus = obs.extension("citus");
  if (!citus_present(in, step, plan, citus)) return;

  const auto name = citus_node_name(in);
  const auto& node = citus_node(citus, in);
  if (node.empty()) {
    citus_refuse_absent_node(in, step, plan);
    return;
  }
  if (node.value("groupid", -1) == 0) {
    step.action = Action::kConflict;
    step.why = name + " is the coordinator";
    plan.conflicts.push_back(
        step.why + ", and Citus refuses to disable it (\"cannot change isactive "
        "field of the coordinator node\").");
    return;
  }
  if (!node.value("isactive", true)) {
    step.action = Action::kSatisfied;
    step.why = name + " is already disabled";
    return;
  }
  const auto placements = node.value("placements", 0LL);
  if (placements > 0) {
    step.action = Action::kConflict;
    step.why = name + " holds " + std::to_string(placements) +
               " shard placements of distributed tables";
    plan.conflicts.push_back(
        step.why + ", and Citus refuses to disable a node holding the only copy of "
        "a shard (measured, the same refusal as removing it). Drain it first with "
        "citus_drain_node, in an earlier intent or specification.");
    return;
  }
  step.action = Action::kApply;
  step.txn_class = TxnClass::kRequired;
  step.detail["rehearse_by"] = "execution";
  step.detail["node"] = name;
  step.lock = "no table lock: cluster metadata, propagated to every node";
  step.why = name + " holds no distributed shards and is taken out of service";
  step.sql.push_back("SELECT citus_disable_node(" + citus_node_args(in) +
                     ", synchronous => true);");
  plan.warnings.push_back(
      name + " stays registered and keeps its copies of the reference tables; "
      "Citus stops sending it work. citus_activate_node brings it back; "
      "citus_remove_node takes it out for good.");
}

inline void plan_citus_activate_node(const Intent& in, const Observations& obs,
                                     const ExecutorConfig& cfg, Plan& plan,
                                     std::vector<Step>& out) {
  (void)cfg;
  Step step;
  step.kind = in.kind_name;
  struct Emit { std::vector<Step>& o; Step& s; ~Emit() { o.push_back(s); } } emit{out, step};
  const auto& citus = obs.extension("citus");
  if (!citus_present(in, step, plan, citus)) return;

  const auto name = citus_node_name(in);
  const auto& node = citus_node(citus, in);
  if (node.empty()) {
    citus_refuse_absent_node(in, step, plan);
    return;
  }
  // Measured: activating an active node returns its id and changes nothing.
  if (node.value("isactive", false)) {
    step.action = Action::kSatisfied;
    step.why = name + " is already active";
    return;
  }
  step.action = Action::kApply;
  step.txn_class = TxnClass::kRequired;
  step.detail["rehearse_by"] = "execution";
  step.detail["node"] = name;
  step.lock = "no table lock: cluster metadata, propagated to every node";
  step.why = name + " is put back into service";
  step.sql.push_back("SELECT citus_activate_node(" + citus_node_args(in) + ");");
}

// Measured: it writes each placement's current size into
// pg_dist_placement.shardlength -- 0 before, 3.4 to 3.9 MB after on a 4-shard
// table -- rolls back cleanly, took 5 ms for 32 shards, works on distributed and
// reference tables, and is refused on anything else ("relation is not
// distributed"). No reading says the recorded sizes are stale -- current and
// out of date look the same -- so it is always applied, and it is harmless to
// repeat.
inline void plan_citus_update_table_statistics(const Intent& in,
                                               const Observations& obs,
                                               const ExecutorConfig& cfg,
                                               Plan& plan, std::vector<Step>& out) {
  (void)cfg;
  Step step;
  step.kind = in.kind_name;
  struct Emit { std::vector<Step>& o; Step& s; ~Emit() { o.push_back(s); } } emit{out, step};
  const auto qualified = in.qualified_table();
  const auto& citus = obs.extension("citus");
  bool already = false;
  if (!citus_precondition(in, obs, step, plan, citus, qualified, already)) return;
  const auto kind = citus_kind_of(citus, qualified);
  if (kind != "distributed" && kind != "reference") {
    step.action = Action::kConflict;
    step.why = qualified + " is not a distributed or reference table" +
               (kind.empty() ? std::string() : " (it is a Citus-managed local one)");
    plan.conflicts.push_back(
        step.why + ", so Citus has no shard sizes to record for it (\"relation is "
        "not distributed\").");
    return;
  }
  step.action = Action::kApply;
  step.txn_class = TxnClass::kRequired;
  step.detail["rehearse_by"] = "execution";
  step.lock = "no table lock: reads each shard's size, writes Citus metadata";
  step.why = "records the current size of each of " + qualified +
             "'s shards in pg_dist_placement.shardlength";
  step.sql.push_back("SELECT citus_update_table_statistics(" +
                     detail::quote_literal(qualified) + ");");
}
