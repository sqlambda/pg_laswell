#pragma once
// TimescaleDB planners. PURE: (Intent, Observations, ExecutorConfig) in,
// steps out. Included inside namespace pglaswell.
//
// Measured on TimescaleDB 2.30.2 (PostgreSQL 18, both editions) and 2.28.3
// (PostgreSQL 15), 2026-10-02. Each refusal quotes the error it pre-empts.

inline std::optional<TsInterval> ts_reading_interval(const json& v) {
  if (!v.is_array() || v.size() != 3) return std::nullopt;
  return TsInterval{v[0].get<long long>(), v[1].get<long long>(), v[2].get<long long>()};
}

inline std::string ts_qualified(const Intent& in, const char* name_key = "table") {
  return in.body.value("schema", "") + "." + in.body.value(name_key, "");
}

inline std::string ts_regclass(const std::string& qualified) {
  // A regclass literal, quoted as one identifier pair, so a mixed-case name
  // is the name it is.
  return detail::quote_literal(detail::quote_qualified(qualified)) + "::regclass";
}

inline std::string ts_interval_sql(const std::string& text) {
  return "INTERVAL " + detail::quote_literal(text);
}

struct TsStep {
  Step& step;
  Plan& plan;
  void refuse(std::string why) {
    step.action = Action::kConflict;
    step.why = std::move(why);
    plan.conflicts.push_back(step.why);
  }
};

// Is TimescaleDB here at all? Says why not and returns false when the step is
// finished.
inline bool ts_present(const Intent& in, TsStep& s, const json& ts) {
  if (!ts.empty()) return true;
  s.refuse("the timescaledb extension is not installed in this database, so " +
           in.kind_name + " cannot be planned");
  s.plan.prerequisites.push_back(json{
      {"kind", "extension"},
      {"target", "timescaledb"},
      {"where", "postgresql.conf, then this database, as a superuser"},
      {"requirement",
       "shared_preload_libraries = 'timescaledb' and a restart; CREATE EXTENSION timescaledb"},
      {"verify", "SELECT extversion FROM pg_extension WHERE extname = 'timescaledb'"},
      {"blocking", true}});
  return false;
}

// The Timescale License features: refused on the Apache build, naming it.
inline bool ts_tsl(const Intent& in, TsStep& s, const json& ts) {
  const auto license = ts.value("license", "");
  if (license == "timescale") return true;
  s.refuse(in.kind_name + " needs the Timescale License edition of TimescaleDB, and this "
           "server reports timescaledb.license = '" + license + "'. TimescaleDB: "
           "\"functionality not supported under the current \\\"" + license +
           "\\\" license\". The Apache build is what PGDG and Debian package "
           "(+dfsg) and what the -oss images carry; Timescale's own packages and "
           "images are the other edition.");
  return false;
}

inline const json& ts_hypertable(const json& ts, const std::string& qualified) {
  static const json kEmpty = json::object();
  const auto& h = ts_member(ts, "hypertables");
  const auto it = h.find(qualified);
  return it == h.end() ? kEmpty : *it;
}

inline bool ts_has_function(const json& ts, const char* name) {
  return ts.value("functions", json::object()).value(name, false);
}

inline bool ts_require_hypertable(const Intent& in, TsStep& s, const json& ts,
                                  const std::string& qualified) {
  if (!ts_hypertable(ts, qualified).empty()) return true;
  s.refuse(in.kind_name + ": " + qualified + " is not a hypertable. Make it one with "
           "timescaledb_create_hypertable first, in this specification or an earlier one.");
  return false;
}

// Whether a column type is one by_range partitions with an interval. Both
// spellings: the catalog reports format_type's ("timestamp with time zone"),
// and a table created earlier in the same specification is projected with the
// type as its author wrote it ("timestamptz") -- which the example found, by
// refusing its own hypertable. A precision, timestamptz(3), is the same type.
inline bool ts_is_time_type(std::string type) {
  for (auto& c : type) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  const auto paren = type.find('(');
  if (paren != std::string::npos) {
    const auto close = type.find(')', paren);
    type.erase(paren, close == std::string::npos ? std::string::npos : close - paren + 1);
  }
  std::string norm;
  for (const char c : type) {
    if (c == ' ' && (norm.empty() || norm.back() == ' ')) continue;
    norm += c;
  }
  while (!norm.empty() && norm.back() == ' ') norm.pop_back();
  return norm == "timestamptz" || norm == "timestamp" || norm == "date" ||
         norm == "timestamp with time zone" || norm == "timestamp without time zone" ||
         norm == "pg_catalog.timestamptz" || norm == "pg_catalog.timestamp";
}

// --- timescaledb_create_hypertable -------------------------------------------

inline void plan_timescaledb_create_hypertable(const Intent& in, const Observations& obs,
                                               const ExecutorConfig& cfg, Plan& plan,
                                               std::vector<Step>& out) {
  (void)cfg;
  Step step;
  step.kind = in.kind_name;
  struct Emit { std::vector<Step>& o; Step& s; ~Emit() { o.push_back(s); } } emit{out, step};
  TsStep s{step, plan};
  const auto& ts = obs.extension("timescaledb");
  if (!ts_present(in, s, ts)) return;

  const auto qualified = ts_qualified(in);
  const auto column = in.body.value("time_column", "");
  const auto& t = obs.table(qualified);
  step.detail["table"] = qualified;
  step.detail["time_column"] = column;

  const auto& h = ts_hypertable(ts, qualified);
  if (!h.empty()) {
    const auto dims = h.value("dimensions", json::array());
    const std::string current = dims.empty() ? "" : dims[0].value("column", "");
    if (current != column) {
      s.refuse(qualified + " is already a hypertable, partitioned on " + current +
               ", not " + column + ". TimescaleDB cannot repartition one in place.");
      return;
    }
    step.action = Action::kSatisfied;
    step.why = qualified + " is already a hypertable on " + column;
    if (in.body.contains("chunk_time_interval") && !dims.empty()) {
      const auto want = ts_parse_interval(in.body.value("chunk_time_interval", ""));
      const auto have = ts_reading_interval(dims[0].value("interval", json()));
      if (want && have && !(*want == *have)) {
        plan.warnings.push_back(
            qualified + " is a hypertable with another chunk interval than this "
            "specification declares. Creating it is satisfied; the interval is "
            "timescaledb_set_chunk_time_interval's to change.");
      }
    }
    return;
  }

  if (!t.value("exists", false)) {
    s.refuse(qualified + " does not exist");
    return;
  }
  if (!ts_has_function(ts, "by_range")) {
    s.refuse("this TimescaleDB (" + ts.value("version", std::string("?")) +
             ") has no by_range(), which this kind calls; it arrived in 2.13.");
    return;
  }

  const auto cols = t.value("columns", json::object());
  if (!cols.contains(column)) {
    s.refuse(qualified + " has no column " + column);
    return;
  }
  const auto type = cols[column].value("type", "");
  if (!ts_is_time_type(type)) {
    s.refuse(column + " is " + type + ". This kind partitions on time -- timestamptz, "
             "timestamp or date -- with an interval; an integer time column needs an "
             "integer chunk size, which it does not take.");
    return;
  }

  // Measured: "cannot create a unique index without the column \"ts\" (used in
  // partitioning)", raised by create_hypertable itself for an existing primary
  // key or unique index that leaves the time column out.
  for (const auto& [iname, ix] : ts_member(t, "indexes").items()) {
    if (!ix.value("is_unique", false)) continue;
    bool has = false;
    for (const auto& c : ix.value("columns", json::array())) {
      if (c == column) has = true;
    }
    if (!has) {
      s.refuse(qualified + " has the unique index " + iname + ", which does not "
               "contain " + column + ". TimescaleDB: \"cannot create a unique index "
               "without the column \\\"" + column + "\\\" (used in partitioning)\". Make "
               "the key (" + column + ", ...) first, or drop it.");
      return;
    }
  }

  const long long rows = std::max(t.value("reltuples", 0LL),
                                  t.value("stats", json::object()).value("n_live_tup", 0LL));
  const bool migrate = in.body.value("migrate_data", false);
  if (rows > 0 && !migrate) {
    s.refuse(qualified + " holds about " + std::to_string(rows) + " rows. TimescaleDB: "
             "\"table \\\"" + in.table() + "\\\" is not empty\" -- set \"migrate_data\": "
             "true to move them into chunks, which holds AccessExclusiveLock on the table "
             "while they move.");
    return;
  }

  std::string args = ts_regclass(qualified) + ", by_range(" + detail::quote_literal(column);
  if (in.body.contains("chunk_time_interval")) {
    args += ", " + ts_interval_sql(in.body.value("chunk_time_interval", ""));
  }
  args += ")";
  if (migrate) args += ", migrate_data => true";

  step.action = Action::kApply;
  step.txn_class = TxnClass::kRequired;
  // Rehearsed by execution: measured, create_hypertable -- migrate_data
  // included -- rolls back, leaving the plain table with its rows.
  step.detail["rehearse_by"] = "execution";
  step.sql.push_back("SELECT create_hypertable(" + args + ");");
  if (migrate && rows > 0) {
    step.lock = "AccessExclusiveLock on " + qualified + " while its rows move into chunks";
    step.why = qualified + " becomes a hypertable on " + column + ", and its ~" +
               std::to_string(rows) + " rows move into chunks";
    plan.warnings.push_back(
        qualified + " is unreadable and unwritable while its ~" + std::to_string(rows) +
        " rows are copied into chunks: create_hypertable holds AccessExclusiveLock for "
        "the whole copy (measured: 200 000 rows in 300 ms on a test container, so "
        "plan on the order of seconds per million rows).");
  } else {
    step.lock = "AccessExclusiveLock on " + qualified + ", briefly: the table is empty";
    step.why = qualified + " becomes a hypertable partitioned on " + column;
  }
  if (!cols[column].value("not_null", false)) {
    plan.warnings.push_back(
        column + " on " + qualified + " is nullable, and create_hypertable makes it NOT "
        "NULL without saying so (measured). A NULL time cannot be placed in a chunk.");
  }
}

// --- timescaledb_set_chunk_time_interval ---------------------------------------

inline void plan_timescaledb_set_chunk_time_interval(const Intent& in, const Observations& obs,
                                                     const ExecutorConfig& cfg, Plan& plan,
                                                     std::vector<Step>& out) {
  (void)cfg;
  Step step;
  step.kind = in.kind_name;
  struct Emit { std::vector<Step>& o; Step& s; ~Emit() { o.push_back(s); } } emit{out, step};
  TsStep s{step, plan};
  const auto& ts = obs.extension("timescaledb");
  if (!ts_present(in, s, ts)) return;
  const auto qualified = ts_qualified(in);
  if (!ts_require_hypertable(in, s, ts, qualified)) return;
  const auto text = in.body.value("chunk_time_interval", "");
  const auto want = ts_parse_interval(text);
  const auto dims = ts_hypertable(ts, qualified).value("dimensions", json::array());
  const auto have = dims.empty() ? std::nullopt
                                 : ts_reading_interval(dims[0].value("interval", json()));
  if (want && have && *want == *have) {
    step.action = Action::kSatisfied;
    step.why = qualified + "'s chunk interval is already " + text;
    return;
  }
  step.action = Action::kApply;
  step.txn_class = TxnClass::kRequired;
  step.detail["rehearse_by"] = "execution";
  step.lock = "no table lock on the data: catalog only, for chunks created from now on";
  step.why = "new chunks of " + qualified + " cover " + text +
             "; existing chunks keep the interval they were made with";
  step.sql.push_back("SELECT set_chunk_time_interval(" + ts_regclass(qualified) + ", " +
                     ts_interval_sql(text) + ");");
}

// --- timescaledb_set_columnstore ----------------------------------------------

inline void plan_timescaledb_set_columnstore(const Intent& in, const Observations& obs,
                                             const ExecutorConfig& cfg, Plan& plan,
                                             std::vector<Step>& out) {
  (void)cfg;
  Step step;
  step.kind = in.kind_name;
  struct Emit { std::vector<Step>& o; Step& s; ~Emit() { o.push_back(s); } } emit{out, step};
  TsStep s{step, plan};
  const auto& ts = obs.extension("timescaledb");
  if (!ts_present(in, s, ts) || !ts_tsl(in, s, ts)) return;
  const auto qualified = ts_qualified(in);
  if (!ts_require_hypertable(in, s, ts, qualified)) return;
  const auto& h = ts_hypertable(ts, qualified);

  std::vector<std::string> seg, ord_cols;
  std::vector<bool> ord_desc;
  for (const auto& e : in.body.value("segment_by", json::array())) seg.push_back(e.get<std::string>());
  for (const auto& e : in.body.value("order_by", json::array())) {
    std::istringstream words(e.get<std::string>());
    std::string c, d;
    words >> c >> d;
    for (auto& ch : d) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    ord_cols.push_back(c);
    ord_desc.push_back(d == "desc");
  }
  const auto as_vec = [](const json& v) {
    std::vector<std::string> r;
    if (v.is_array()) for (const auto& e : v) r.push_back(e.get<std::string>());
    return r;
  };
  std::vector<bool> have_desc;
  if (h.value("orderby_desc", json()).is_array()) {
    for (const auto& e : h["orderby_desc"]) have_desc.push_back(e.get<bool>());
  }
  const bool same = h.value("columnstore", false) &&
                    as_vec(h.value("segmentby", json())) == seg &&
                    as_vec(h.value("orderby", json())) == ord_cols && have_desc == ord_desc;
  if (same) {
    step.action = Action::kSatisfied;
    step.why = qualified + " already has the columnstore, segmented and ordered as declared";
    return;
  }

  std::vector<std::string> opts = {"timescaledb.enable_columnstore = true"};
  if (in.body.contains("segment_by")) {
    opts.push_back("timescaledb.segmentby = " + detail::quote_literal(detail::join(seg, ", ")));
  }
  if (in.body.contains("order_by")) {
    std::vector<std::string> o;
    for (std::size_t i = 0; i < ord_cols.size(); ++i) {
      o.push_back(ord_cols[i] + (ord_desc[i] ? " DESC" : ""));
    }
    opts.push_back("timescaledb.orderby = " + detail::quote_literal(detail::join(o, ", ")));
  }
  step.action = Action::kApply;
  step.txn_class = TxnClass::kRequired;
  step.detail["rehearse_by"] = "execution";
  step.lock = "AccessExclusiveLock on " + qualified + ", briefly (measured): settings only";
  step.why = qualified + " gets the columnstore" +
             (seg.empty() ? std::string() : ", segmented by " + detail::join(seg, ", "));
  step.sql.push_back("ALTER TABLE " + detail::quote_qualified(qualified) + " SET (" +
                     detail::join(opts, ", ") + ");");
  if (h.value("compressed_chunks", 0LL) > 0) {
    plan.warnings.push_back(
        "the new columnstore settings on " + qualified + " apply to chunks converted from "
        "now on; its " + std::to_string(h.value("compressed_chunks", 0LL)) +
        " converted chunks keep the old ones until recompressed (TimescaleDB says so in a "
        "NOTICE).");
  }
  plan.warnings.push_back(
      "with the columnstore enabled on " + qualified + ", TimescaleDB refuses VALIDATE "
      "CONSTRAINT on it, and ALTER COLUMN TYPE once a chunk is converted (measured). "
      "pg_laswell's recipes for set_not_null, add_check_constraint and add_foreign_key "
      "validate in a second step, so they are refused on this table from here on.");
}

// --- policies -------------------------------------------------------------------

// The jobs TimescaleDB keeps for a hypertable or continuous aggregate, from the
// reading -- one per kind of policy.
inline const json& ts_policy(const json& ts, const std::string& target, const char* which) {
  static const json kEmpty = json::object();
  const auto& p = ts_member(ts, "policies");
  const auto it = p.find(target);
  if (it == p.end() || !it->contains(which)) return kEmpty;
  return (*it)[which];
}

inline void plan_timescaledb_add_columnstore_policy(const Intent& in, const Observations& obs,
                                                    const ExecutorConfig& cfg, Plan& plan,
                                                    std::vector<Step>& out) {
  (void)cfg;
  Step step;
  step.kind = in.kind_name;
  struct Emit { std::vector<Step>& o; Step& s; ~Emit() { o.push_back(s); } } emit{out, step};
  TsStep s{step, plan};
  const auto& ts = obs.extension("timescaledb");
  if (!ts_present(in, s, ts) || !ts_tsl(in, s, ts)) return;
  const auto qualified = ts_qualified(in);
  if (!ts_require_hypertable(in, s, ts, qualified)) return;
  // Measured: "columnstore not enabled on hypertable".
  if (!ts_hypertable(ts, qualified).value("columnstore", false)) {
    s.refuse(qualified + " does not have the columnstore enabled. TimescaleDB: "
             "\"columnstore not enabled on hypertable\" -- add timescaledb_set_columnstore "
             "before this intent.");
    return;
  }
  const auto text = in.body.value("after", "");
  const auto& existing = ts_policy(ts, qualified, "columnstore");
  const auto have = ts_reading_interval(existing.value("after", json()));
  if (!existing.empty() && have && *have == *ts_parse_interval(text)) {
    step.action = Action::kSatisfied;
    step.why = qualified + " already converts chunks older than " + text;
    return;
  }
  // add_columnstore_policy is a PROCEDURE (measured: "is a procedure ... use
  // CALL"); add_compression_policy, its older name, a function. Whichever this
  // server has.
  const bool modern = ts_has_function(ts, "add_columnstore_policy");
  step.action = Action::kApply;
  step.txn_class = TxnClass::kRequired;
  step.detail["rehearse_by"] = "execution";
  step.lock = "no table lock: a background job, run by TimescaleDB's scheduler";
  if (!existing.empty()) {
    // Measured: a second add with other arguments only warns ("A policy already
    // exists with different arguments") and changes nothing. Remove, then add.
    step.sql.push_back(modern ? "CALL remove_columnstore_policy(" + ts_regclass(qualified) + ");"
                              : "SELECT remove_compression_policy(" + ts_regclass(qualified) + ");");
    step.why = qualified + "'s columnstore policy changes to chunks older than " + text;
  } else {
    step.why = qualified + " converts chunks older than " + text + " to the columnstore";
  }
  step.sql.push_back(modern ? "CALL add_columnstore_policy(" + ts_regclass(qualified) +
                                  ", after => " + ts_interval_sql(text) + ");"
                            : "SELECT add_compression_policy(" + ts_regclass(qualified) +
                                  ", compress_after => " + ts_interval_sql(text) + ");");
}

inline void plan_timescaledb_remove_columnstore_policy(const Intent& in, const Observations& obs,
                                                       const ExecutorConfig& cfg, Plan& plan,
                                                       std::vector<Step>& out) {
  (void)cfg;
  Step step;
  step.kind = in.kind_name;
  struct Emit { std::vector<Step>& o; Step& s; ~Emit() { o.push_back(s); } } emit{out, step};
  TsStep s{step, plan};
  const auto& ts = obs.extension("timescaledb");
  if (!ts_present(in, s, ts) || !ts_tsl(in, s, ts)) return;
  const auto qualified = ts_qualified(in);
  if (ts_policy(ts, qualified, "columnstore").empty()) {
    step.action = Action::kSatisfied;
    step.why = qualified + " has no columnstore policy";
    return;
  }
  const bool modern = ts_has_function(ts, "remove_columnstore_policy");
  step.action = Action::kApply;
  step.txn_class = TxnClass::kRequired;
  step.detail["rehearse_by"] = "execution";
  step.lock = "no table lock: removes a background job";
  step.why = qualified + " stops converting chunks; those already converted stay so";
  step.sql.push_back(modern ? "CALL remove_columnstore_policy(" + ts_regclass(qualified) + ");"
                            : "SELECT remove_compression_policy(" + ts_regclass(qualified) + ");");
}

inline void plan_timescaledb_add_retention_policy(const Intent& in, const Observations& obs,
                                                  const ExecutorConfig& cfg, Plan& plan,
                                                  std::vector<Step>& out) {
  (void)cfg;
  Step step;
  step.kind = in.kind_name;
  struct Emit { std::vector<Step>& o; Step& s; ~Emit() { o.push_back(s); } } emit{out, step};
  TsStep s{step, plan};
  const auto& ts = obs.extension("timescaledb");
  if (!ts_present(in, s, ts) || !ts_tsl(in, s, ts)) return;
  const auto qualified = ts_qualified(in);
  if (!ts_require_hypertable(in, s, ts, qualified)) return;
  const auto text = in.body.value("drop_after", "");
  const auto& existing = ts_policy(ts, qualified, "retention");
  const auto have = ts_reading_interval(existing.value("drop_after", json()));
  if (!existing.empty() && have && *have == *ts_parse_interval(text)) {
    step.action = Action::kSatisfied;
    step.why = qualified + " already drops chunks older than " + text;
    return;
  }
  step.action = Action::kApply;
  step.txn_class = TxnClass::kRequired;
  step.detail["rehearse_by"] = "execution";
  step.lock = "no table lock now; the job drops chunks on its schedule";
  if (!existing.empty()) {
    step.sql.push_back("SELECT remove_retention_policy(" + ts_regclass(qualified) + ");");
  }
  step.sql.push_back("SELECT add_retention_policy(" + ts_regclass(qualified) +
                     ", drop_after => " + ts_interval_sql(text) + ");");
  step.why = (existing.empty() ? qualified + " drops chunks older than "
                               : qualified + "'s retention changes to chunks older than ") +
             text;
  plan.warnings.push_back(
      "DATA LOSS by design: from now on TimescaleDB drops every chunk of " + qualified +
      " older than " + text + ", on a schedule, without asking again. "
      "acknowledge_data_loss says this specification means it.");
}

inline void plan_timescaledb_remove_retention_policy(const Intent& in, const Observations& obs,
                                                     const ExecutorConfig& cfg, Plan& plan,
                                                     std::vector<Step>& out) {
  (void)cfg;
  Step step;
  step.kind = in.kind_name;
  struct Emit { std::vector<Step>& o; Step& s; ~Emit() { o.push_back(s); } } emit{out, step};
  TsStep s{step, plan};
  const auto& ts = obs.extension("timescaledb");
  if (!ts_present(in, s, ts) || !ts_tsl(in, s, ts)) return;
  const auto qualified = ts_qualified(in);
  // Measured: without if_exists an absent policy RAISES ("retention policy not
  // found"), so absent is read as done here instead.
  if (ts_policy(ts, qualified, "retention").empty()) {
    step.action = Action::kSatisfied;
    step.why = qualified + " has no retention policy";
    return;
  }
  step.action = Action::kApply;
  step.txn_class = TxnClass::kRequired;
  step.detail["rehearse_by"] = "execution";
  step.lock = "no table lock: removes a background job";
  step.why = qualified + " stops dropping old chunks";
  step.sql.push_back("SELECT remove_retention_policy(" + ts_regclass(qualified) + ");");
}

// --- continuous aggregates --------------------------------------------------------

inline void plan_timescaledb_create_continuous_aggregate(const Intent& in, const Observations& obs,
                                                         const ExecutorConfig& cfg, Plan& plan,
                                                         std::vector<Step>& out) {
  (void)cfg;
  Step step;
  step.kind = in.kind_name;
  TsStep s{step, plan};
  const auto& ts = obs.extension("timescaledb");
  const auto finish = [&] { out.push_back(step); };
  if (!ts_present(in, s, ts) || !ts_tsl(in, s, ts)) return finish();
  const auto qualified = ts_qualified(in, "name");
  step.detail["view"] = qualified;
  if (ts.value("caggs", json::object()).contains(qualified)) {
    step.action = Action::kSatisfied;
    step.why = qualified + " is already a continuous aggregate (its query is not compared: "
               "a different one is a drop and a create in a later specification)";
    return finish();
  }
  std::string with = "timescaledb.continuous";
  if (in.body.contains("materialized_only")) {
    with += std::string(", timescaledb.materialized_only = ") +
            (in.body.value("materialized_only", false) ? "true" : "false");
  }
  step.action = Action::kApply;
  step.txn_class = TxnClass::kRequired;
  // Rehearsed by execution: WITH NO DATA runs in a transaction and rolls back
  // (measured), which also checks the query -- "invalid continuous aggregate
  // view" for one that reads no hypertable is the dry run's to report.
  step.detail["rehearse_by"] = "execution";
  step.lock = "AccessShareLock on the hypertables it reads; no data is read: WITH NO DATA";
  step.why = qualified + " is created empty, as a continuous aggregate";
  step.sql.push_back("CREATE MATERIALIZED VIEW " + detail::quote_qualified(qualified) +
                     " WITH (" + with + ") AS " + in.body.value("query", "") +
                     " WITH NO DATA;");
  if (in.body.contains("comment")) {
    // ON VIEW: a continuous aggregate is relkind 'v' -- measured, COMMENT ON
    // MATERIALIZED VIEW is refused, "\"x\" is not a materialized view" (the
    // dry run found it), and COMMENT ON VIEW works on 2.30 and 2.28.
    step.sql.push_back("COMMENT ON VIEW " + detail::quote_qualified(qualified) +
                       " IS " + detail::quote_literal(in.body.value("comment", "")) + ";");
  }
  out.push_back(step);

  // Filling it is a second step, outside any transaction: measured,
  // "refresh_continuous_aggregate() cannot run inside a transaction block". It
  // reads every row of the source, so it is asked for, never assumed.
  if (in.body.value("refresh", false)) {
    Step r;
    r.kind = in.kind_name;
    r.action = Action::kApply;
    r.txn_class = TxnClass::kForbidden;
    r.lock = "reads the whole source hypertable; the view is readable throughout";
    r.why = qualified + " is filled from its source, all of it";
    r.detail["view"] = qualified;
    r.detail["on_failure"] =
        "the view exists, created by the step before, and holds whatever was "
        "materialized before the failure; refreshing it again is safe";
    r.sql.push_back("CALL refresh_continuous_aggregate(" + ts_regclass(qualified) +
                    ", NULL, NULL);");
    out.push_back(r);
  }
}

inline void plan_timescaledb_add_continuous_aggregate_policy(const Intent& in, const Observations& obs,
                                                             const ExecutorConfig& cfg, Plan& plan,
                                                             std::vector<Step>& out) {
  (void)cfg;
  Step step;
  step.kind = in.kind_name;
  struct Emit { std::vector<Step>& o; Step& s; ~Emit() { o.push_back(s); } } emit{out, step};
  TsStep s{step, plan};
  const auto& ts = obs.extension("timescaledb");
  if (!ts_present(in, s, ts) || !ts_tsl(in, s, ts)) return;
  const auto qualified = ts_qualified(in, "name");
  step.detail["view"] = qualified;
  if (!ts.value("caggs", json::object()).contains(qualified)) {
    s.refuse(qualified + " is not a continuous aggregate. Create it with "
             "timescaledb_create_continuous_aggregate first.");
    return;
  }
  const auto start = in.body.value("start_offset", "");
  const auto end = in.body.value("end_offset", "");
  const auto every = in.body.value("schedule_interval", "");
  const auto& existing = ts_policy(ts, qualified, "refresh");
  const auto same = [&](const char* key, const std::string& text) {
    const auto have = ts_reading_interval(existing.value(key, json()));
    return have && *have == *ts_parse_interval(text);
  };
  if (!existing.empty() && same("start_offset", start) && same("end_offset", end) &&
      same("schedule_interval", every)) {
    step.action = Action::kSatisfied;
    step.why = qualified + " is already refreshed as declared";
    return;
  }
  step.action = Action::kApply;
  step.txn_class = TxnClass::kRequired;
  step.detail["rehearse_by"] = "execution";
  step.lock = "no table lock: a background job";
  if (!existing.empty()) {
    // Measured: "continuous aggregate refresh policy already exists".
    step.sql.push_back("SELECT remove_continuous_aggregate_policy(" + ts_regclass(qualified) + ");");
  }
  step.sql.push_back("SELECT add_continuous_aggregate_policy(" + ts_regclass(qualified) +
                     ", start_offset => " + ts_interval_sql(start) +
                     ", end_offset => " + ts_interval_sql(end) +
                     ", schedule_interval => " + ts_interval_sql(every) + ");");
  step.why = qualified + " is refreshed every " + every + ", from " + start + " ago to " +
             end + " ago";
}
