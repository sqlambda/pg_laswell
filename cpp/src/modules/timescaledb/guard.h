#pragma once
// Plan-level refusals for CORE kinds on a hypertable: what TimescaleDB refuses
// and core would otherwise plan. A refusal only (THE RULE, ../README.md); how
// an index is built or dropped on a hypertable is core's decision, informed by
// index.h. Measured on 2.30.2 (both editions) and 2.28.3. Included inside
// namespace pglaswell.
//
// The guard runs before projection, so it follows the specification in order:
// a table made a hypertable, or given the columnstore, by an earlier intent is
// treated as one by every intent after it.
template <typename Refuse, typename Advise>
inline void timescaledb_plan_refusals(const Spec& spec, const Observations& obs,
                                      const json& /*budget*/, const Refuse& refuse,
                                      const Advise& advise) {
  const auto& ts = obs.extension("timescaledb");
  if (ts.empty()) return;

  // A POLICY CHANGED BY HAND. The ledger's applied steps (read by core, newest
  // first) say what each policy was last declared to be; TimescaleDB's jobs say
  // what it is. A policy is a row anyone with the rights can remove, re-add or
  // pause -- remove_retention_policy, alter_job -- with no migration involved,
  // and a retention policy that quietly stopped, or started dropping sooner, is
  // worth saying on ANY plan against this database. An advisory: the reading
  // is volatile, and the plan in hand may have nothing to do with it.
  {
    static const char* const kWords[][2] = {{"columnstore", "columnstore policy"},
                                            {"retention", "retention policy"},
                                            {"refresh", "refresh policy"}};
    const auto& policies = ts_member(ts, "policies");
    std::set<std::string> seen;
    for (const auto& st : ts.value("applied_steps", json::array())) {
      const auto action = st.value("action", "");
      if (action != "apply" && action != "satisfied") continue;
      const auto d = st.value("detail", json::object());
      // A step from before policies were recorded cannot be compared, and
      // saying nothing about it is the honest answer.
      if (!d.contains("policy")) continue;
      const auto& p = d["policy"];
      const auto target = p.value("target", "");
      const auto which = p.value("which", "");
      if (target.empty() || !seen.insert(target + "/" + which).second) continue;  // newest only
      std::string words = which + " policy";
      for (const auto& w : kWords) if (which == w[0]) words = w[1];
      const auto t = policies.find(target);
      const bool exists = t != policies.end() && t->contains(which);
      const json declared = p.value("declared", json());
      if (declared.is_null()) {
        if (exists) {
          advise(target + "'s " + words + " was removed by an applied specification and "
                 "exists again: added by hand since.");
        }
        continue;
      }
      if (!exists) {
        advise(target + "'s " + words + " was added by an applied specification and is no "
               "longer there: removed by hand since. Re-applying will not restore it; a "
               "specification that adds it again will.");
        continue;
      }
      const auto& have = (*t)[which];
      std::vector<std::string> changed;
      for (const auto& [key, text] : declared.items()) {
        // The reading keeps the columnstore interval under "after" as declared.
        const auto want = text.is_string() ? ts_parse_interval(text.get<std::string>())
                                           : std::nullopt;
        const auto got = ts_reading_interval(have.value(key, json()));
        if (want && got && !(*want == *got)) {
          changed.push_back(key + " is no longer " + text.get<std::string>());
        }
      }
      if (!have.value("scheduled", true)) changed.push_back("the job is paused");
      if (!changed.empty()) {
        advise(target + "'s " + words + " differs from what an applied specification "
               "declared: " + detail::join(changed, ", ") + ", changed by hand since.");
      }
    }
  }

  struct Hyper {
    std::vector<std::string> dims;  // every partitioning column, in order
    bool columnstore = false;
    long long compressed = 0;
  };
  std::map<std::string, Hyper> hyper;
  for (const auto& [name, h] : ts_member(ts, "hypertables").items()) {
    Hyper x;
    for (const auto& d : h.value("dimensions", json::array())) x.dims.push_back(d.value("column", ""));
    x.columnstore = h.value("columnstore", false);
    x.compressed = h.value("compressed_chunks", 0LL);
    hyper[name] = x;
  }

  const auto missing_dim = [](const Hyper& h, const std::vector<std::string>& cols) {
    for (const auto& d : h.dims) {
      if (std::find(cols.begin(), cols.end(), d) == cols.end()) return d;
    }
    return std::string();
  };

  for (const auto& in : spec.intents) {
    const auto q = in.qualified_table();
    if (in.kind == IntentKind::kTimescaledbCreateHypertable) {
      hyper[q].dims = {in.body.value("time_column", "")};
      continue;
    }
    if (in.kind == IntentKind::kTimescaledbSetColumnstore) {
      if (hyper.count(q)) hyper[q].columnstore = true;
      continue;
    }
    const auto it = hyper.find(q);
    if (it == hyper.end()) continue;
    const auto& h = it->second;
    const std::string what = in.kind_name + " on " + q;

    // A UNIQUE key must contain every partitioning column. Measured: "cannot
    // create a unique index without the column \"ts\" (used in partitioning)",
    // for an index, a primary key and a unique constraint alike.
    std::vector<std::string> key;
    bool unique = false;
    if (in.kind == IntentKind::kCreateIndex && in.body.value("unique", false)) {
      unique = true;
      for (const auto& c : in.body.value("columns", json::array())) {
        key.push_back(c.is_string() ? c.get<std::string>() : c.value("name", ""));
      }
    } else if (in.kind == IntentKind::kAddPrimaryKey ||
               in.kind == IntentKind::kAddUniqueConstraint) {
      unique = true;
      for (const auto& c : in.body.value("columns", json::array())) key.push_back(c.get<std::string>());
      if (in.body.value("without_overlaps", false) && !key.empty()) key.pop_back();
    } else if (in.kind == IntentKind::kAddExclusionConstraint) {
      // Measured on 2.30.2: the same refusal for an exclusion constraint that
      // does not compare the partitioning column for equality.
      unique = true;
      for (const auto& e : in.body.value("elements", json::array())) {
        if (e.contains("column") && e.value("with", "") == "=") key.push_back(e.value("column", ""));
      }
    }
    if (unique) {
      const auto d = missing_dim(h, key);
      if (!d.empty()) {
        const bool exclusion = in.kind == IntentKind::kAddExclusionConstraint;
        refuse(what + ": " +
               (exclusion ? "the columns it compares with \"=\" (" : "the key (") +
               detail::join(key, ", ") + ") " + (exclusion ? "do" : "does") + " not contain " + d +
               ", and " + q + " is a hypertable partitioned on it. TimescaleDB: \"cannot "
               "create a unique index without the column \\\"" + d + "\\\" (used in "
               "partitioning)\". Add " + d + (exclusion ? ", compared with \"=\"." : " to the key."));
      }
    }

    // A constraint cannot adopt an index that is already there. Measured on
    // 2.30.2: "hypertables do not support adding a constraint using an existing
    // index". Core adopts a matching unique index wherever it finds one -- a
    // catalog change instead of a build -- and on a hypertable that was the
    // only statement it planned, with no way forward when the dry run refused.
    // Not when the constraint is there already: its own index is such an
    // index, and the intent is then simply satisfied.
    if ((in.kind == IntentKind::kAddPrimaryKey || in.kind == IntentKind::kAddUniqueConstraint) &&
        !in.body.value("without_overlaps", false) && missing_dim(h, key).empty() &&
        !obs.table(q).value("constraints", json::object()).contains(in.body.value("name", ""))) {
      const json indexes = obs.table(q).value("indexes", json::object());
      for (const auto& [iname, ix] : indexes.items()) {
        if (!ix.value("is_unique", false) || !ix.value("is_valid", false)) continue;
        if (!ix.value("predicate", "").empty()) continue;
        std::vector<std::string> have;
        for (const auto& c : ix.value("columns", json::array())) {
          if (c.is_string()) have.push_back(c.get<std::string>());
        }
        if (have != key) continue;
        refuse(what + ": the unique index \"" + iname + "\" already covers (" +
               detail::join(key, ", ") + "), and on a plain table the constraint would "
               "adopt it. TimescaleDB: \"hypertables do not support adding a constraint "
               "using an existing index\". Drop " + iname + " in an earlier intent "
               "(drop_index); the constraint then builds its own index, in one "
               "statement, which blocks writes for the build.");
        break;
      }
    }

    // VALIDATE CONSTRAINT is refused once the columnstore is enabled. That is
    // not a refusal here: constraint.h tells core, which validates in the
    // statement that adds the constraint.

    // ALTER COLUMN TYPE once any chunk is converted: "operation not supported on
    // hypertables with compressed chunks".
    if (h.compressed > 0 && in.kind == IntentKind::kAlterColumnType) {
      refuse(what + ": " + q + " has " + std::to_string(h.compressed) + " chunk(s) in the "
             "columnstore, and TimescaleDB refuses a column type change then (\"operation "
             "not supported on hypertables with compressed chunks\"). Convert them back "
             "first.");
    }
  }
}
