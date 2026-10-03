#pragma once
// Plan-level refusals for pgvector indexes.
//
// Every one of these is an error PostgreSQL would raise anyway. What the guard
// buys is WHEN: an HNSW index on a large table is built concurrently, a
// concurrent build cannot be rehearsed (the dry run lists it as unverified),
// and a build refused at execution has already spent its first scan and left
// an invalid index behind -- measured, a failed CREATE INDEX CONCURRENTLY of an
// HNSW index left indisvalid = false, 0 bytes, for the next run to drop. Said
// here, the author reads the rule before anything runs.
//
// A refusal only, never a rewrite (THE RULE, ../README.md): the guard receives
// (spec, obs, refuse) and not the plan. Core emits the CREATE INDEX; this only
// says when pgvector will not take it.
//
// Measured on PostgreSQL 18.6 with pgvector 0.8.6 (2026-10-01). Each refusal
// quotes the pgvector error it pre-empts.
// Included inside namespace pglaswell by planner.h.

// What a column type is, as pgvector reads it: the base type and the declared
// dimensions, if any. format_type renders vector(3) as "vector(3)", and as
// "extensions.vector(3)" when the extension's schema is not on the search path.
struct PgvectorType {
  std::string base;
  long long dims = -1;  // -1: declared without a typmod
};

inline PgvectorType pgvector_parse_type(std::string t) {
  for (auto& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  PgvectorType out;
  const auto paren = t.find('(');
  std::string base = t.substr(0, paren);
  while (!base.empty() && base.back() == ' ') base.pop_back();
  const auto dot = base.rfind('.');
  if (dot != std::string::npos) base = base.substr(dot + 1);
  base.erase(std::remove(base.begin(), base.end(), '"'), base.end());
  out.base = base;
  if (paren != std::string::npos) {
    const auto close = t.find(')', paren);
    const auto inner = t.substr(paren + 1, close == std::string::npos ? std::string::npos
                                                                       : close - paren - 1);
    if (!inner.empty() &&
        std::all_of(inner.begin(), inner.end(),
                    [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) {
      out.dims = std::stoll(inner);
    }
  }
  return out;
}

// The most dimensions each method indexes, per type. Not readable from the
// catalog -- pgvector checks it in its build code -- so these are the measured
// values, each with the error that set it:
//   "column cannot have more than 2000 dimensions for hnsw index"   (vector)
//   "column cannot have more than 4000 dimensions for hnsw index"   (halfvec)
//   "column cannot have more than 64000 dimensions for hnsw index"  (bit)
// and the same three numbers for ivfflat. sparsevec has no such limit on an
// HNSW index (measured: sparsevec(100000) built); its limit is 1000 NON-ZERO
// elements per row, which is a property of the data, checked at insert.
// 0 means no limit.
inline long long pgvector_max_dims(const std::string& method, const std::string& base) {
  (void)method;  // identical for both methods, measured; kept for the reader
  if (base == "vector") return 2000;
  if (base == "halfvec") return 4000;
  if (base == "bit") return 64000;
  return 0;
}

// The operator classes, as pgvector 0.8.6 declares them. Used only when the
// specification itself creates the extension, so there is no reading yet; a
// database where it is installed is read instead (observe.h).
inline json pgvector_known_opclasses() {
  json out = json::array();
  const auto add = [&out](const char* m, const char* oc, const char* ty, bool d) {
    out.push_back(json{{"method", m}, {"opclass", oc}, {"type", ty}, {"default", d}});
  };
  for (const char* ty : {"vector", "halfvec", "sparsevec"}) {
    for (const char* op : {"l2", "ip", "cosine", "l1"}) {
      const auto name = std::string(ty) + "_" + op + "_ops";
      out.push_back(json{{"method", "hnsw"}, {"opclass", name}, {"type", ty}, {"default", false}});
    }
  }
  add("hnsw", "bit_hamming_ops", "bit", false);
  add("hnsw", "bit_jaccard_ops", "bit", false);
  for (const char* ty : {"vector", "halfvec"}) {
    for (const char* op : {"l2", "ip", "cosine"}) {
      const auto name = std::string(ty) + "_" + op + "_ops";
      const bool dflt = std::string(ty) == "vector" && std::string(op) == "l2";
      out.push_back(json{{"method", "ivfflat"}, {"opclass", name}, {"type", ty}, {"default", dflt}});
    }
  }
  add("ivfflat", "bit_hamming_ops", "bit", false);
  return out;
}

// Storage parameters each method accepts, and their bounds. Measured:
//   value 1 out of bounds for option "m" -- between "2" and "100"
//   ef_construction                       -- between "4" and "1000"
//   lists                                 -- between "1" and "32768"
//   unrecognized parameter "fillfactor"   (hnsw takes no other)
struct PgvectorParam {
  const char* name;
  long long min;
  long long max;
};

inline std::vector<PgvectorParam> pgvector_params(const std::string& method) {
  if (method == "hnsw") return {{"m", 2, 100}, {"ef_construction", 4, 1000}};
  return {{"lists", 1, 32768}};
}

// An integer storage parameter as written: a JSON integer, or a word made of
// digits. Anything else is left to the server (measured: m = '16' and m = 16.5
// were both accepted, so refusing them here would refuse what works).
inline std::optional<long long> pgvector_int(const json& v) {
  if (v.is_number_integer()) return v.get<long long>();
  if (v.is_string()) {
    const auto& s = v.get_ref<const std::string&>();
    if (!s.empty() && s.size() < 12 &&
        std::all_of(s.begin(), s.end(),
                    [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) {
      return std::stoll(s);
    }
  }
  return std::nullopt;
}

// The type an index EXPRESSION produces, when it is an explicit cast of a
// column or of a function over one -- the shapes pgvector's own documentation
// uses: (embedding::halfvec(3072)), (binary_quantize(embedding)::bit(1536)),
// CAST(embedding AS halfvec(3072)). Anything else is "" and goes unchecked: its
// type is the server's to know, and a guess here would refuse what works.
inline std::string pgvector_cast_type(const std::string& expression) {
  static const std::string inner =
      R"((?:"?[A-Za-z_]\w*"?|[A-Za-z_][\w.]*\s*\(\s*"?[A-Za-z_]\w*"?\s*\)))";
  static const std::string type = R"(([A-Za-z_][\w.]*\s*(?:\(\s*\d+\s*\))?))";
  static const std::regex postfix(R"(^\s*\(?\s*)" + inner + R"(\s*\)?\s*::\s*)" + type +
                                  R"(\s*$)");
  static const std::regex cast(R"(^\s*cast\s*\(\s*)" + inner + R"(\s+as\s+)" + type +
                                   R"(\s*\)\s*$)",
                               std::regex::icase);
  std::smatch m;
  if (std::regex_match(expression, m, postfix) || std::regex_match(expression, m, cast)) {
    return m[1].str();
  }
  return "";
}

// Returns the key column's type as pgvector reads it, for the advisories --
// base "" when it is not known.
template <typename Refuse>
inline PgvectorType pgvector_check_index(const Intent& in, const std::string& qualified,
                                 const std::function<std::string(const std::string&)>& type_of,
                                 const json& opclasses, const json& methods,
                                 const Refuse& refuse) {
  PgvectorType key_type;
  const auto method = in.body.value("method", "btree");
  const auto name = in.body.value("name", "");
  const std::string what = "create_index " + name + " on " + qualified + " (" + method + ")";

  const auto caps = methods.value(method, json::object());
  if (in.body.value("unique", false) && !caps.value("can_unique", false)) {
    refuse(what + " is UNIQUE, and pgvector refuses it: \"access method \\\"" + method +
           "\\\" does not support unique indexes\". A vector index finds what is "
           "near, not what is equal; enforce uniqueness with a separate btree index.");
  }
  const auto columns = in.body.value("columns", json::array());
  if (columns.size() > 1 && !caps.value("can_multi_col", false)) {
    refuse(what + " has " + std::to_string(columns.size()) +
           " key columns, and pgvector refuses it: \"access method \\\"" + method +
           "\\\" does not support multicolumn indexes\". Index the vector column "
           "alone, and filter on the others with a where clause or a separate index.");
  }

  for (const auto& c : columns) {
    std::string col = c.is_string() ? c.get<std::string>() : c.value("name", "");
    std::string type;
    if (col.empty()) {
      // An expression key: checked when it is an explicit cast, the shape
      // used to index a vector wider than 2000 as halfvec. Its column is not
      // looked up -- the cast decides what the index sees.
      col = "(" + c.value("expression", "") + ")";
      type = pgvector_cast_type(c.value("expression", ""));
    } else {
      type = type_of(col);
    }
    if (type.empty()) continue;  // core refuses a missing column itself
    const auto pt = pgvector_parse_type(type);
    key_type = pt;
    const std::string opclass = c.is_object() ? c.value("opclass", "") : "";

    std::vector<std::string> fitting;
    std::string default_opclass;
    std::string declared_type;  // what the named opclass is for, if it exists
    for (const auto& oc : opclasses) {
      if (oc.value("method", "") != method) continue;
      if (oc.value("opclass", "") == opclass) declared_type = oc.value("type", "");
      if (oc.value("type", "") != pt.base) continue;
      fitting.push_back(oc.value("opclass", ""));
      if (oc.value("default", false)) default_opclass = oc.value("opclass", "");
    }
    const auto choices = fitting.empty() ? std::string("none")
                                         : detail::join(fitting, ", ");
    if (fitting.empty()) {
      refuse(what + ": column " + col + " is " + type + ", which " + method +
             " cannot index. pgvector: \"data type " + pt.base +
             " has no default operator class for access method \\\"" + method +
             "\\\"\". " + method + " indexes " +
             (method == "hnsw" ? "vector, halfvec, sparsevec and bit"
                               : "vector, halfvec and bit (not sparsevec)") +
             " columns.");
      continue;
    }
    if (!opclass.empty() && declared_type.empty()) {
      refuse(what + ": operator class " + opclass + " does not exist for access "
             "method " + method + ". For a " + pt.base + " column it is one of: " +
             choices + ".");
      continue;
    }
    if (!opclass.empty() && declared_type != pt.base) {
      refuse(what + ": operator class " + opclass + " is for " + declared_type +
             ", and column " + col + " is " + type + ". pgvector: \"operator class \\\"" +
             opclass + "\\\" does not accept data type " + pt.base + "\". Use one of: " +
             choices + ".");
      continue;
    }
    if (opclass.empty() && default_opclass.empty()) {
      refuse(what + ": column " + col + " names no operator class, and " + method +
             " has no default for " + pt.base + ". The class IS the distance: name "
             "one of " + choices + " in the column's \"opclass\", matching the "
             "operator your queries order by (<-> l2, <#> ip, <=> cosine, <+> l1).");
      continue;
    }
    if (pt.dims < 0) {
      refuse(what + ": column " + col + " is " + type + " with no declared "
             "dimensions, and pgvector refuses it: \"column does not have "
             "dimensions\". Declare the column as " + pt.base + "(n) first "
             "(alter_column_type), or index an expression that casts it.");
      continue;
    }
    const auto max = pgvector_max_dims(method, pt.base);
    if (max > 0 && pt.dims > max) {
      refuse(what + ": column " + col + " is " + type + ", and pgvector refuses it: "
             "\"column cannot have more than " + std::to_string(max) +
             " dimensions for " + method + " index\"." +
             (pt.base == "vector"
                  ? " Index it as halfvec, which allows 4000: an expression key "
                    "(" + col + "::halfvec(" + std::to_string(pt.dims) +
                    ")) with a halfvec_* operator class, queried with the same cast."
                  : ""));
    }
  }

  // Storage parameters: names the method knows, values in its bounds, and for
  // hnsw the rule that ties the two together, with the defaults pgvector uses
  // when one is left out (m 16, ef_construction 64) -- measured: WITH (m = 40)
  // alone is refused, "ef_construction must be greater than or equal to 2 * m".
  const auto with = in.body.value("with", json::object());
  const auto params = pgvector_params(method);
  std::vector<std::string> known;
  for (const auto& p : params) known.push_back(p.name);
  for (auto it = with.begin(); it != with.end(); ++it) {
    const auto p = std::find_if(params.begin(), params.end(),
                                [&](const PgvectorParam& x) { return it.key() == x.name; });
    if (p == params.end()) {
      refuse(what + ": " + method + " has no storage parameter " + it.key() +
             ". pgvector: \"unrecognized parameter \\\"" + it.key() + "\\\"\". It takes " +
             detail::join(known, " and ") + ".");
      continue;
    }
    const auto v = pgvector_int(it.value());
    if (v && (*v < p->min || *v > p->max)) {
      refuse(what + ": " + it.key() + " = " + std::to_string(*v) +
             " is out of bounds. pgvector: \"value " + std::to_string(*v) +
             " out of bounds for option \\\"" + it.key() + "\\\"\" -- between " +
             std::to_string(p->min) + " and " + std::to_string(p->max) + ".");
    }
  }
  if (method == "hnsw") {
    const auto m = with.contains("m") ? pgvector_int(with["m"]) : std::optional<long long>(16);
    const auto ef = with.contains("ef_construction") ? pgvector_int(with["ef_construction"])
                                                     : std::optional<long long>(64);
    if (m && ef && *ef < 2 * *m) {
      refuse(what + ": ef_construction " + std::to_string(*ef) +
             (with.contains("ef_construction") ? "" : " (the default)") +
             " is less than 2 * m (" + std::to_string(2 * *m) +
             "). pgvector: \"ef_construction must be greater than or equal to "
             "2 * m\". Raise ef_construction to at least " +
             std::to_string(2 * *m) + ".");
    }
  }
  return key_type;
}

// ADVISORIES: what pgvector does not refuse but a reader should know before a
// build starts. Each rests on a reading that moves on its own -- row counts
// that autovacuum rewrites -- so they are advisories, outside planDigest
// (Plan::advisories), never warnings.
//
// HNSW graph memory, measured on 0.8.6 by the row at which the build reports
// "hnsw graph no longer fits into maintenance_work_mem after N tuples":
//   vector(128)  m=16  4 MB -> 1222 B/row      vector(384) m=16 -> 2250 B/row
//   vector(768)  m=16  4 MB -> 3796 B/row      vector(128) m=32 -> 1732 B/row
// which is the vector's own bytes plus about 210 + 32*m, within 2% for all four.
inline long long pgvector_graph_bytes_per_row(const PgvectorType& t, long long m) {
  long long vec = 0;
  if (t.base == "vector") vec = 8 + 4 * t.dims;
  else if (t.base == "halfvec") vec = 8 + 2 * t.dims;
  else if (t.base == "bit") vec = 8 + (t.dims + 7) / 8;
  else return 0;  // sparsevec: its size is its non-zero count, a property of the data
  return vec + 210 + 32 * m;
}

template <typename Advise>
inline void pgvector_advise(const Intent& in, const std::string& qualified,
                            const PgvectorType& key_type, bool created_here,
                            const Observations& obs, const json& budget,
                            const std::string& dsm_type, const Advise& advise) {
  const auto method = in.body.value("method", "btree");
  const auto name = in.body.value("name", "");
  const auto with = in.body.value("with", json::object());
  const auto& t = obs.table(qualified);
  // reltuples is 0 until the first ANALYZE; n_live_tup counts from the first
  // insert. The larger of the two is the best this reading has.
  const long long rows =
      created_here ? 0
                   : std::max(t.value("reltuples", 0LL),
                              t.value("stats", json::object()).value("n_live_tup", 0LL));
  const bool known = created_here || t.value("exists", false);

  if (method == "ivfflat" && known) {
    const long long lists =
        with.contains("lists") ? pgvector_int(with["lists"]).value_or(100) : 100;
    if (rows == 0) {
      advise("ivfflat index " + name + " is built on " +
             (created_here ? std::string("a table this specification creates, so it is empty")
                           : qualified + ", which reads as empty") +
             ". IVFFlat picks its " + std::to_string(lists) + " list centres from the "
             "rows present when it is built (pgvector: \"ivfflat index created with "
             "little data\"), and recall is poor once the table fills. Build it in a "
             "later specification, after the data is loaded.");
    } else if (rows < lists) {
      advise("ivfflat index " + name + " on " + qualified + ": about " +
             std::to_string(rows) + " rows for " + std::to_string(lists) +
             " lists. pgvector itself says \"ivfflat index created with little data\" "
             "below one row per list, and recall suffers. Fewer lists, or build it "
             "after more data is loaded.");
    }
  }

  if (method != "hnsw" || rows == 0) return;
  const auto m_budget = budget.value("maintenanceWorkMem", json::object());
  const long long per_step_mb = m_budget.value("perStepMb", 0LL);
  const long long server_bytes = m_budget.value("serverDefaultKb", 0LL) * 1024;
  const auto mb = [](long long b) { return std::to_string((b + 1048575) / 1048576) + " MB"; };
  const long long m = with.contains("m") ? pgvector_int(with["m"]).value_or(16) : 16;
  const long long per_row = pgvector_graph_bytes_per_row(key_type, m);
  const long long need = (per_row > 0 && key_type.dims > 0) ? rows * per_row : 0;
  // The memory the step will REALLY run with: the same rule core applies
  // (index_build_mwm_bytes), so this cannot say one thing while core does
  // another. Without a ceiling, core raises the build toward `need`, up to
  // shared_buffers divided by max_concurrent_jobs.
  const long long mwm_bytes = index_build_mwm_bytes(budget, need);
  if (need > 0 && need > mwm_bytes && mwm_bytes > 0) {
    const bool raised = per_step_mb == 0 && mwm_bytes > server_bytes;
    advise("hnsw index " + name + " on " + qualified + ": its graph needs about " +
           mb(need) + " (" + std::to_string(rows) + " rows x ~" +
           std::to_string(per_row) + " bytes for " + key_type.base + "(" +
           std::to_string(key_type.dims) + ") with m = " + std::to_string(m) +
           ", measured on pgvector 0.8.6), and this step runs with "
           "maintenance_work_mem " + mb(mwm_bytes) +
           (raised ? std::string(" -- already raised from the server's ") + mb(server_bytes) +
                         ", as far as the limit deduced from shared_buffers ("
                         + m_budget.value("derivedFrom", std::string()) + ") allows"
                   : std::string()) +
           ". Past that the build continues on disk, much more slowly -- measured "
           "3.4x for 50 000 rows. " +
           (per_step_mb > 0
                ? "Raise maintenance_work_mem_mb so each step gets at least " + mb(need) +
                      " (it is divided by max_concurrent_jobs)."
                : "Set maintenance_work_mem_mb in the connection's configuration to "
                  "at least " + mb(need) + " per step to give it all of it."));
  }
  // A parallel build allocates the WHOLE maintenance_work_mem in dynamic shared
  // memory. Measured in a container with the default 64 MB /dev/shm: 256 MB
  // failed "could not resize shared memory segment ... No space left on
  // device" and left an invalid index. /dev/shm's size is not visible from
  // SQL, so this says when it matters rather than claiming it does.
  const long long workers = obs.server.value("max_parallel_maintenance_workers", 0LL);
  if (workers > 0 && dsm_type == "posix" && mwm_bytes > 64LL * 1024 * 1024) {
    advise("hnsw index " + name + " on " + qualified + " may be built in parallel "
           "(max_parallel_maintenance_workers " + std::to_string(workers) +
           "), and a parallel build allocates the whole maintenance_work_mem, " +
           mb(mwm_bytes) + ", in POSIX shared memory (/dev/shm). Where /dev/shm is "
           "smaller -- 64 MB is a container's default -- the build fails, \"could not "
           "resize shared memory segment\", and leaves an invalid index. Give the "
           "container a larger --shm-size, or keep maintenance_work_mem within it.");
  }
}

template <typename Refuse, typename Advise>
inline void pgvector_plan_refusals(const Spec& spec, const Observations& obs,
                                   const json& budget, const Refuse& refuse,
                                   const Advise& advise) {
  const auto& pv = obs.extension("pgvector");
  bool creates_extension = false;
  for (const auto& in : spec.intents) {
    if (in.kind == IntentKind::kCreateExtension &&
        in.body.value("name", "") == "vector") {
      creates_extension = true;
    }
  }
  // Not installed here and not created by this specification: there is no
  // pgvector method to refuse for, and an index naming one fails on the
  // missing access method -- core's dry run says so.
  if (pv.empty() && !creates_extension) return;
  const json opclasses = pv.empty() ? pgvector_known_opclasses()
                                    : pv.value("opclasses", json::array());
  const json methods = pv.empty()
                           ? json{{"hnsw", {{"can_unique", false}, {"can_multi_col", false}}},
                                  {"ivfflat", {{"can_unique", false}, {"can_multi_col", false}}}}
                           : pv.value("methods", json::object());

  // Column types as each intent leaves them: the catalog's, overlaid with what
  // earlier intents in this specification declare. The guard runs before
  // projection, so a table created two intents above is otherwise unknown.
  std::map<std::string, std::string> declared;
  std::set<std::string> created;  // tables this spec creates: empty when indexed
  for (const auto& in : spec.intents) {
    const auto q = in.qualified_table();
    switch (in.kind) {
      case IntentKind::kCreateTable:
        created.insert(q);
        for (const auto& col : in.body.value("columns", json::array())) {
          declared[q + "." + col.value("name", "")] = col.value("type", "");
        }
        break;
      case IntentKind::kAddColumn:
      case IntentKind::kAlterColumnType:
        declared[q + "." + in.body.value("column", "")] = in.body.value("type", "");
        break;
      case IntentKind::kCreateIndex: {
        const auto method = in.body.value("method", "btree");
        if (method != "hnsw" && method != "ivfflat") break;
        const std::function<std::string(const std::string&)> type_of =
            [&](const std::string& col) -> std::string {
          const auto it = declared.find(q + "." + col);
          if (it != declared.end()) return it->second;
          const auto& cols = obs.table(q).value("columns", json::object());
          if (!cols.contains(col)) return "";
          return cols[col].value("type", "");
        };
        const auto key_type = pgvector_check_index(in, q, type_of, opclasses, methods, refuse);
        pgvector_advise(in, q, key_type, created.count(q) > 0, obs, budget,
                        pv.value("dynamic_shared_memory_type", ""), advise);
        break;
      }
      default:
        break;
    }
  }
}
