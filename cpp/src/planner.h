#pragma once

// The planner: (Spec, Observations, ExecutorConfig) -> Plan.
//
// THIS HEADER MUST NOT INCLUDE PQXX, and a test asserts it does not. Purity is
// the mechanism that makes "deterministic" testable: with a fixed Observations
// literal, plan_migration() must produce byte-identical JSON with no server in
// the loop. Everything that could vary -- clock, catalog, configuration -- is
// an explicit input.
//
// The planner's job is to answer "how", having been told "what". Its output
// says WHICH MEASUREMENT DECIDED EACH CHOICE, because a plan whose reasoning
// cannot be checked is a recipe, and recipes are what this tool exists to
// replace.

#include <algorithm>
#include <cctype>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "config.h"
#include "observations.h"
#include "spec.h"

namespace pglaswell {

// How a step relates to a transaction. This is the direct answer to "DDL is
// transactional, so wrap it" versus "CREATE INDEX CONCURRENTLY cannot run
// inside a transaction block": both are true, and a step has to say which it
// is before the executor can group anything.
enum class TxnClass {
  kRequired,      // must be atomic with its neighbours
  kOptional,      // works either way; joins the current group
  kForbidden,     // must run outside any transaction block
  kOwnTxnPerBatch // the executor owns the boundaries
};

inline const char* to_string(TxnClass c) {
  switch (c) {
    case TxnClass::kRequired: return "txn_required";
    case TxnClass::kOptional: return "txn_optional";
    case TxnClass::kForbidden: return "txn_forbidden";
    case TxnClass::kOwnTxnPerBatch: return "own_txn_per_batch";
  }
  return "unknown";
}

// What the planner concluded about an intent. `satisfied` is a SUCCESS, not a
// shrug: the ledger records it with the observation that justified it, so a
// later reader can tell "we didn't need to" from "we forgot to".
enum class Action { kApply, kSatisfied, kConflict };

inline const char* to_string(Action a) {
  switch (a) {
    case Action::kApply: return "apply";
    case Action::kSatisfied: return "satisfied";
    case Action::kConflict: return "conflict";
  }
  return "unknown";
}

struct Step {
  int ordinal = 0;
  int txn_group = 0;
  std::string kind;
  TxnClass txn_class = TxnClass::kOptional;
  Action action = Action::kApply;
  std::vector<std::string> sql;  // verbatim, as it will be executed
  std::string why;               // the rule that fired, and the reading behind it
  std::string lock;              // the lock this takes, named
  // Forces a transaction boundary before this step even when its class would
  // otherwise let it join the previous group. NOT VALID must commit before
  // VALIDATE runs, or the strong lock is held across the scan regardless.
  bool own_transaction = false;
  json detail = json::object();

  json to_json() const {
    return json{{"ordinal", ordinal},
                {"txnGroup", txn_group},
                {"kind", kind},
                {"txnClass", to_string(txn_class)},
                {"action", to_string(action)},
                {"sql", sql},
                {"why", why},
                {"lock", lock},
                {"detail", detail}};
  }
};

struct Plan {
  bool ok = true;
  std::string spec_id;
  std::string spec_digest;
  std::vector<Step> steps;
  std::vector<std::string> warnings;
  std::vector<std::string> conflicts;
  json budget = json::object();

  json to_json() const {
    json steps_json = json::array();
    for (const auto& s : steps) steps_json.push_back(s.to_json());
    return json{{"ok", ok},
                {"specId", spec_id},
                {"specDigest", spec_digest},
                {"steps", std::move(steps_json)},
                {"warnings", warnings},
                {"conflicts", conflicts},
                {"budget", budget}};
  }

  // The digest of the plan itself: the determinism receipt. planMigration and
  // startMigration must produce the same one, which is how "what ran is what
  // you were shown" becomes checkable rather than promised.
  std::string digest() const { return digest_hex(to_json()); }

  std::string render() const;
};

namespace detail {

inline std::string human_bytes(long long b) {
  const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
  double v = static_cast<double>(b);
  int u = 0;
  while (v >= 1024.0 && u < 4) {
    v /= 1024.0;
    ++u;
  }
  char buf[64];
  std::snprintf(buf, sizeof(buf), (u == 0 ? "%.0f %s" : "%.1f %s"), v, units[u]);
  return buf;
}

inline std::string quote_literal(const std::string& s) {
  std::string out = "'";
  for (const char c : s) {
    if (c == '\'') out += '\'';
    out += c;
  }
  out += "'";
  return out;
}

inline std::string join(const std::vector<std::string>& v, const char* sep) {
  std::string out;
  for (std::size_t i = 0; i < v.size(); ++i) {
    if (i != 0) out += sep;
    out += v[i];
  }
  return out;
}

// The connection budget (spike S12): one blocked query pins exactly one
// connection, so T_exhaust = headroom / arrival_rate. The ceiling that matters
// is the APPLICATION's pool, which is invisible from the server -- so when it
// is not configured, say so rather than substituting max_connections and
// pretending the number means what it does not.
inline json compute_budget(const Observations& obs, const ExecutorConfig& cfg) {
  const int max_conn = obs.server.value("max_connections", 0);
  const int reserved = obs.server.value("reserved_connections", 0);
  const int current = obs.server.value("current_backends", 0);
  const int server_headroom = std::max(0, max_conn - reserved - current);

  json b{{"serverMaxConnections", max_conn},
         {"serverHeadroom", server_headroom},
         {"appPoolSize", cfg.app_pool_size},
         {"appStatementTimeoutMs", cfg.app_statement_timeout_ms},
         {"safetyPercent", cfg.safety_percent}};

  if (cfg.app_pool_size > 0) {
    b["effectiveHeadroom"] = std::min(server_headroom, cfg.app_pool_size);
    b["headroomSource"] = "app_pool_size";
  } else {
    b["effectiveHeadroom"] = server_headroom;
    b["headroomSource"] = "max_connections";
    b["caveat"] =
        "app_pool_size is not configured, so this is the server's ceiling, not "
        "the application's. An application collapses when its own pool fills, "
        "which usually happens first and is invisible from here.";
  }
  return b;
}

}  // namespace detail

// --- per-intent rules ------------------------------------------------------

// Defined below, after the rule that reaches for it: a partitioned parent
// refuses CREATE INDEX CONCURRENTLY, so the create_index rule hands off here.
inline void plan_partitioned_index(const Intent& in, const json& t, Plan& plan,
                                   std::vector<Step>& out, Step& parent_step);

inline void plan_add_column(const Intent& in, const Observations& obs, Plan& plan,
                            std::vector<Step>& out) {
  Step step;
  step.kind = in.kind_name;
  struct Emit { std::vector<Step>& o; Step& s; ~Emit() { o.push_back(s); } } emit{out, step};
  const auto qualified = in.qualified_table();
  const auto& t = obs.table(qualified);
  const auto column = in.body.value("column", "");
  const auto type = in.body.value("type", "");
  const bool nullable = in.body.value("nullable", true);
  const bool has_default = in.body.contains("default") && !in.body["default"].is_null();

  step.txn_class = TxnClass::kRequired;  // the ALTER and its COMMENT are one unit
  step.lock = "AccessExclusiveLock";

  if (!t.value("exists", false)) {
    step.action = Action::kConflict;
    step.why = qualified + " does not exist";
    plan.conflicts.push_back(qualified + " does not exist, so " + column +
                             " cannot be added to it");
    return;
  }

  const auto& columns = t.value("columns", json::object());
  if (columns.contains(column)) {
    const auto& existing = columns[column];
    const auto existing_type = existing.value("type", "");
    // Compared on the resolved type name from format_type(), not on the
    // spec's spelling: "varchar" and "character varying" are the same type
    // and must not read as a conflict.
    if (existing_type != type && !type.empty()) {
      step.action = Action::kConflict;
      step.why = qualified + "." + column + " exists as " + existing_type +
                 ", spec declares " + type;
      plan.conflicts.push_back(step.why);
      return;
    }
    if (existing.value("not_null", false) && nullable) {
      step.action = Action::kConflict;
      step.why = qualified + "." + column + " exists as NOT NULL, spec declares nullable";
      plan.conflicts.push_back(step.why);
      return;
    }
    step.action = Action::kSatisfied;
    step.why = "column already present as " + existing_type +
               (existing.value("not_null", false) ? " NOT NULL" : " NULL");
    return;
  }

  std::string sql = "ALTER TABLE " + qualified + " ADD COLUMN " + column + " " + type;
  if (has_default) {
    sql += " DEFAULT " + in.body["default"].get<std::string>();
  }
  if (!nullable) sql += " NOT NULL";
  step.sql.push_back(sql + ";");
  step.sql.push_back("COMMENT ON COLUMN " + qualified + "." + column + " IS " +
                     detail::quote_literal(in.body.value("comment", "")) + ";");

  // Measured 2026-09-05 on 18.6 (spike S8): a non-volatile default is
  // metadata-only, INCLUDING when the column is NOT NULL. A volatile default
  // rewrites the table. The general caution against one-step NOT NULL applies
  // to NOT NULL *without* a default -- which fails outright if any row is null
  // -- and to volatile defaults.
  if (!nullable && !has_default) {
    step.action = Action::kConflict;
    step.why = "NOT NULL with no default requires every existing row to be non-null";
    plan.conflicts.push_back(
        qualified + "." + column +
        " is declared NOT NULL with no default; on a non-empty table that "
        "fails outright. Add the column nullable, backfill it, then set NOT "
        "NULL in a later spec.");
    return;
  }

  step.why = has_default
                 ? "column absent; non-volatile default is catalog-only on PG 11+"
                 : "column absent; nullable with no default is catalog-only";
  step.detail["expected"] = "metadata only, no table rewrite";
}

// Applies a planned step's effect to the projected catalog, so that later
// intents in the same spec are planned against the state their predecessors
// will have produced.
//
// Without this, the commonest spec there is -- add a column, then backfill it
// -- refuses, because the backfill is measured against a catalog in which the
// column does not exist yet. Intents are ORDERED, and each one must be planned
// against the state after the ones before it. The planner still does not
// REORDER intents; it only accounts for the order the author chose.
inline void project(const Intent& in, const Step& step, Observations& projected) {
  if (step.action != Action::kApply) return;
  const auto qualified = in.qualified_table();
  if (!projected.tables.contains(qualified)) return;

  switch (in.kind) {
    case IntentKind::kAddColumn:
      projected.tables[qualified]["columns"][in.body.value("column", "")] =
          json{{"type", in.body.value("type", "")},
               {"not_null", !in.body.value("nullable", true)},
               {"projected_by_step", step.ordinal}};
      return;
    case IntentKind::kCreateIndex: {
      std::vector<std::string> cols;
      for (const auto& c : in.body.value("columns", json::array())) {
        cols.push_back(c.get<std::string>());
      }
      projected.tables[qualified]["indexes"][in.body.value("name", "")] =
          json{{"is_valid", true},
               {"is_unique", in.body.value("unique", false)},
               {"leading_column", cols.empty() ? "" : cols[0]},
               {"definition", "(planned by step " + std::to_string(step.ordinal) + ")"},
               {"projected_by_step", step.ordinal}};
      return;
    }
    case IntentKind::kBackfill:
      return;  // changes rows, not structure
    case IntentKind::kDropIndex:
      projected.tables[qualified]["indexes"].erase(in.body.value("name", ""));
      return;
    case IntentKind::kSetNotNull: {
      const auto col = in.body.value("column", "");
      if (projected.tables[qualified]["columns"].contains(col)) {
        projected.tables[qualified]["columns"][col]["not_null"] = true;
      }
      return;
    }
    case IntentKind::kAddForeignKey:
    case IntentKind::kAddCheckConstraint:
      // A constraint, not a relation or a column: nothing a later intent in
      // this spec reads through Observations changes.
      return;
  }
}

inline void plan_create_index(const Intent& in, const Observations& obs,
                              const ExecutorConfig& cfg, Plan& plan,
                              std::vector<Step>& out) {
  Step step;
  step.kind = in.kind_name;
  // Suppressible, because this rule can hand off: a partitioned parent needs a
  // whole recipe rather than one step, and the handler emits its own. Without
  // the flag the guard would also push this now-unused stub, and the plan
  // would carry a step that does nothing.
  bool handed_off = false;
  struct Emit {
    std::vector<Step>& o; Step& s; const bool& skip;
    ~Emit() { if (!skip) o.push_back(s); }
  } emit{out, step, handed_off};
  const auto qualified = in.qualified_table();
  const auto& t = obs.table(qualified);
  const auto name = in.body.value("name", "");
  const bool unique = in.body.value("unique", false);
  const auto method = in.body.value("method", "btree");
  const auto where = in.body.value("where", "");

  std::vector<std::string> columns;
  for (const auto& c : in.body.value("columns", json::array())) {
    columns.push_back(c.get<std::string>());
  }

  if (!t.value("exists", false)) {
    step.action = Action::kConflict;
    step.why = qualified + " does not exist";
    plan.conflicts.push_back(step.why);
    return;
  }

  const json indexes = t.value("indexes", json::object());
  bool rebuild_after_drop = false;
  if (indexes.contains(name)) {
    const auto& existing = indexes[name];
    // S5, measured 2026-09-05: a failed CREATE INDEX CONCURRENTLY leaves an
    // index that appears in pg_indexes like any other, with indisvalid=false.
    // An idempotence check that asked "does an index of this name exist?"
    // would answer yes and skip the rebuild forever. indisvalid must be read.
    if (!existing.value("is_valid", false)) {
      rebuild_after_drop = true;
    } else {
      step.action = Action::kSatisfied;
      step.why = "index already present and valid";
      step.detail["existing_definition"] = existing.value("definition", "");
      return;
    }
  }

  // An index identical to one already present, under a different name, is a
  // permanent cost paid on every write forever. The name check above does not
  // catch it, because the name is the one thing that differs.
  //
  // Identical is a CONFLICT: there is no reading of the database that makes
  // building it correct. Prefix-redundancy is a WARNING, because there are
  // legitimate reasons to want a narrower index -- a smaller one, a different
  // fillfactor -- and refusing would be this tool overriding a judgement it is
  // not equipped to make.
  {
    // PostgreSQL renders a predicate its own way: a spec's
    //   status = 'open'
    // comes back from pg_get_expr as
    //   (status = 'open'::text)
    // so a raw string comparison never matches. Measured on 18.6.
    //
    // This normalises the common shapes -- outer parens, ::type casts,
    // whitespace, case -- and NOTHING MORE. Two SQL expressions being
    // equivalent is not decidable by string munging, so a match here upgrades
    // to a conflict and a mismatch only warns. Claiming a duplicate on the
    // strength of a normaliser would be exactly the confidently-wrong answer
    // this tool must not give.
    const auto normalize = [](const std::string& p) {
      std::string norm;
      bool in_string = false;
      for (std::size_t i = 0; i < p.size(); ++i) {
        const char c = p[i];
        if (c == '\'') {
          in_string = !in_string;
          norm += c;
          continue;
        }
        if (in_string) {  // never touch the inside of a literal
          norm += c;
          continue;
        }
        if (c == ':' && i + 1 < p.size() && p[i + 1] == ':') {
          // Skip ::identifier, and an optional (n) or [] suffix.
          i += 2;
          while (i < p.size() && (std::isalnum(static_cast<unsigned char>(p[i])) ||
                                  p[i] == '_' || p[i] == '.')) {
            ++i;
          }
          if (i < p.size() && p[i] == '(') {
            while (i < p.size() && p[i] != ')') ++i;
          } else if (i + 1 < p.size() && p[i] == '[' && p[i + 1] == ']') {
            ++i;
          } else {
            --i;
          }
          continue;
        }
        if (c == ' ' || c == '\n' || c == '\t') {
          if (!norm.empty() && norm.back() != ' ') norm += ' ';
          continue;
        }
        norm += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      }
      // Strip balanced outer parentheses.
      auto trim = [](std::string v) {
        while (!v.empty() && v.front() == ' ') v.erase(v.begin());
        while (!v.empty() && v.back() == ' ') v.pop_back();
        return v;
      };
      norm = trim(norm);
      while (norm.size() > 1 && norm.front() == '(' && norm.back() == ')') {
        int depth = 0;
        bool wraps = true;
        for (std::size_t i = 0; i < norm.size(); ++i) {
          if (norm[i] == '(') ++depth;
          if (norm[i] == ')') --depth;
          if (depth == 0 && i + 1 < norm.size()) { wraps = false; break; }
        }
        if (!wraps) break;
        norm = trim(norm.substr(1, norm.size() - 2));
      }
      return norm;
    };

    const auto want_pred = normalize(where);
    for (auto it = indexes.begin(); it != indexes.end(); ++it) {
      if (it.key() == name) continue;
      const auto& ex = it.value();
      if (!ex.value("is_valid", false)) continue;
      // An index over expressions cannot be compared by column name, and a
      // partial comparison would be worse than none.
      if (ex.value("has_expressions", false)) continue;
      if (ex.value("method", "") != method) continue;

      std::vector<std::string> ex_cols;
      for (const auto& c : ex.value("columns", json::array())) {
        ex_cols.push_back(c.get<std::string>());
      }
      const auto ex_pred = normalize(ex.value("predicate", ""));
      const bool same_shape =
          ex_cols == columns && ex.value("is_unique", false) == unique;

      // Same columns, predicates that did not normalise to the same string.
      // Equivalence is not provable here, so this warns and names both rather
      // than refusing on a guess.
      //
      // But only when BOTH are non-empty. A full index and a partial index are
      // definitively different -- one covers rows the other does not -- so
      // there is no ambiguity to report, and warning about it would be noise
      // on every narrow index built beside a partial one.
      const bool both_partial = !ex_pred.empty() && !want_pred.empty();
      if (same_shape && ex_pred != want_pred && both_partial) {
        plan.warnings.push_back(
            "\"" + name + "\" has the same columns and method as the existing "
            "index \"" + it.key() + "\" on " + qualified +
            ", but their predicates differ as written: this spec has [" +
            (where.empty() ? "none" : where) + "] and the existing index has [" +
            (ex.value("predicate", "").empty() ? "none" : ex.value("predicate", "")) +
            "]. If those are equivalent, this would be a duplicate. pg_licht "
            "duplicateIndexes compares them properly.");
        continue;
      }
      if (same_shape && ex_pred != want_pred) continue;  // definitively different
      if (same_shape) {
        const auto policy = in.body.value("on_equivalent_index", "rename");

        // Renaming a constraint-backed index renames the CONSTRAINT too --
        // measured on 18.6. Turning a primary key called orders_pkey into
        // orders_open_by_region_idx is not a name correction, it is a
        // different and much larger change, so it is never done implicitly.
        if (ex.value("constraint_backed", false)) {
          step.action = Action::kConflict;
          step.why = "an equivalent index already exists as " + it.key() +
                     ", and it backs a constraint";
          plan.conflicts.push_back(
              "\"" + name + "\" duplicates \"" + it.key() + "\" on " + qualified +
              ", which backs a constraint. Renaming it would rename the "
              "constraint as well, which is a larger change than adopting an "
              "index name, so pg_laswell will not do it implicitly. Drop this "
              "intent: the constraint's index already serves these lookups.");
          return;
        }

        if (policy == "refuse") {
          step.action = Action::kConflict;
          step.why = "an index identical to this one already exists as " + it.key();
          plan.conflicts.push_back(
              "\"" + name + "\" would duplicate the existing index \"" + it.key() +
              "\" on " + qualified + ": same method, same columns (" +
              detail::join(columns, ", ") +
              "), same predicate. A redundant index costs write time on every "
              "INSERT, UPDATE and DELETE for as long as it exists.");
          return;
        }

        if (policy == "adopt") {
          step.action = Action::kSatisfied;
          step.why = "an equivalent index already exists as " + it.key() +
                     "; adopted where it is, nothing renamed";
          step.detail["adopted"] = it.key();
          return;
        }

        // rename: converge the name. The same spec then creates the index on a
        // database that lacks it and corrects the name on one where it was
        // made by hand -- which is the point of a migration repository.
        step.txn_class = TxnClass::kOptional;
        step.lock = "ShareUpdateExclusiveLock on the index; the table is not "
                    "locked at all";
        step.sql.push_back("ALTER INDEX " + in.schema() + "." + it.key() +
                           " RENAME TO " + name + ";");
        step.sql.push_back("COMMENT ON INDEX " + in.schema() + "." + name +
                           " IS " +
                           detail::quote_literal(in.body.value("comment", "")) + ";");
        step.why = "an equivalent index already exists as \"" + it.key() +
                   "\" (same method, columns and predicate); renaming it to the "
                   "declared name rather than building a second one";
        step.detail["renamed_from"] = it.key();
        step.detail["risk"] =
            "the old name may be referenced outside the database -- a "
            "monitoring dashboard, a planner hint, a deployment script. "
            "pg_laswell cannot see those. Set on_equivalent_index to \"adopt\" "
            "to leave the name alone.";
        return;
      }
      // The planned index is a prefix of an existing one: the existing index
      // already serves these lookups.
      if (ex_cols.size() > columns.size() &&
          std::equal(columns.begin(), columns.end(), ex_cols.begin())) {
        plan.warnings.push_back(
            "\"" + name + "\" on (" + detail::join(columns, ", ") +
            ") is a prefix of the existing index \"" + it.key() + "\" on (" +
            detail::join(ex_cols, ", ") +
            "), which already serves those lookups. Build it only if the "
            "narrower index is wanted for its size. pg_licht evaluateIndex "
            "will say whether the planner would actually use it.");
      }
      // The planned index is a superset: the existing one becomes redundant.
      if (columns.size() > ex_cols.size() &&
          std::equal(ex_cols.begin(), ex_cols.end(), columns.begin())) {
        plan.warnings.push_back(
            "\"" + name + "\" on (" + detail::join(columns, ", ") +
            ") makes the existing index \"" + it.key() + "\" on (" +
            detail::join(ex_cols, ", ") +
            ") redundant. Dropping it afterwards is a separate migration; "
            "pg_licht duplicateIndexes confirms the redundancy and "
            "evaluateIndex hide will say whether anything still needs it.");
      }
    }
  }

  const long long size = t.value("size_measured", t.value("size_estimate", 0LL));
  const bool measured = t.contains("size_measured");
  const int waiters = t.value("lock_waiters", 0);
  const bool partitioned = t.value("kind", "") == "partitioned_table";

  // S4, measured 2026-09-05 on 18.6: CREATE INDEX CONCURRENTLY is refused
  // outright on a partitioned parent. The recipe builds concurrently on each
  // partition, creates the parent index ON ONLY -- ShareLock on a relation
  // holding no data, so brief -- and attaches each child.
  //
  // The parent index stays indisvalid = false until EVERY partition is
  // attached, measured, which is why the last step verifies it: a recipe that
  // stopped after the attaches would leave an index that exists and is not
  // used, and nothing would say so.
  if (partitioned) {
    handed_off = true;
    plan_partitioned_index(in, t, plan, out, step);
    // A refusal still needs a step to carry it.
    if (step.action == Action::kConflict) out.push_back(step);
    return;
  }

  std::string columns_sql = detail::join(columns, ", ");
  const std::string tail = " ON " + qualified + " USING " + method + " (" +
                           columns_sql + ")" +
                           (where.empty() ? "" : " WHERE " + where) + ";";

  // The rule that replaces a hardcoded size ceiling (S12). A plain build takes
  // ShareLock for its whole duration, so what matters is not the size but how
  // long that lock is held against how long the application can survive it.
  // Size enters only through build time, which is the only way it was ever
  // relevant: an idle 100 GB table is safe to build plainly, and a hot 10 MB
  // table is not.
  const long long kPlainCeilingBytes = 64LL * 1024 * 1024;
  const bool small_enough = size < kPlainCeilingBytes;
  const bool quiet = waiters == 0;
  const bool plain = small_enough && quiet && !unique;

  if (rebuild_after_drop) {
    step.sql.push_back("DROP INDEX CONCURRENTLY " + in.schema() + "." + name + ";");
    step.detail["recovering_invalid_index"] = true;
  }

  if (plain) {
    step.txn_class = TxnClass::kOptional;
    step.lock = "ShareLock (blocks writes for the whole build)";
    step.sql.push_back("CREATE INDEX " + name + tail);
    step.why = "size " + detail::human_bytes(size) + " < 64 MiB ceiling, " +
               std::to_string(waiters) + " lock waiters -> plain build " +
               "(transactional, cannot leave an invalid index)";
  } else {
    // CIC is txn_forbidden, and that forces a transaction-group boundary. The
    // rendered plan shows that boundary, because it is exactly where
    // atomicity ends and a reader needs to see it.
    step.txn_class = TxnClass::kForbidden;
    step.lock = "ShareUpdateExclusiveLock (two table scans)";
    step.sql.push_back("CREATE " + std::string(unique ? "UNIQUE " : "") +
                       "INDEX CONCURRENTLY " + name + tail);
    std::string reason;
    if (!small_enough) reason = "size " + detail::human_bytes(size) + " >= 64 MiB ceiling";
    else if (!quiet) reason = std::to_string(waiters) + " lock waiters already on the table";
    else reason = "unique index; a failed plain build would hold ShareLock for the whole scan";
    step.why = reason + " -> concurrent build";
    step.detail["must_verify_valid"] = true;
    step.detail["failure_mode"] =
        "a failed CREATE INDEX CONCURRENTLY leaves an INVALID index that still "
        "appears in pg_indexes; the next step reads indisvalid, and a re-plan "
        "will DROP INDEX CONCURRENTLY before rebuilding";
  }

  step.sql.push_back("COMMENT ON INDEX " + in.schema() + "." + name + " IS " +
                     detail::quote_literal(in.body.value("comment", "")) + ";");
  step.detail["schema"] = in.schema();
  step.detail["index"] = name;
  step.detail["qualified"] = qualified;
  step.detail["size_bytes"] = size;
  step.detail["size_source"] = measured ? "measured" : "estimate";
  step.detail["estimated_from"] = t.value("estimated_from", json());
  step.detail["lock_waiters"] = waiters;
  (void)cfg;
}

inline void plan_backfill(const Intent& in, const Observations& obs,
                          const ExecutorConfig& cfg, Plan& plan,
                          std::vector<Step>& out) {
  Step step;
  step.kind = in.kind_name;
  struct Emit { std::vector<Step>& o; Step& s; ~Emit() { o.push_back(s); } } emit{out, step};
  const auto qualified = in.qualified_table();
  const auto& t = obs.table(qualified);
  const auto key = in.body.value("key", "");

  step.txn_class = TxnClass::kOwnTxnPerBatch;
  step.lock = "RowExclusiveLock plus row locks, released at every commit";

  if (!t.value("exists", false)) {
    step.action = Action::kConflict;
    step.why = qualified + " does not exist";
    plan.conflicts.push_back(step.why);
    return;
  }

  const json columns = t.value("columns", json::object());
  for (auto it = in.body["set"].begin(); it != in.body["set"].end(); ++it) {
    if (!columns.contains(it.key())) {
      step.action = Action::kConflict;
      step.why = qualified + "." + it.key() + " does not exist";
      plan.conflicts.push_back(
          step.why +
          "; if an earlier intent in this spec adds it, the backfill must come "
          "after that intent, and the planner does not reorder intents.");
      return;
    }
  }

  // A keyset walk needs a unique key, or the cursor can skip or repeat rows.
  // Refused loudly rather than falling back to OFFSET, which degrades to a
  // full scan per batch and is quadratic in the table size.
  bool key_is_unique = false;
  std::string supporting_index;
  // Bound to a local: json::value() returns BY VALUE, so calling it in both
  // begin() and end() yields iterators into two different temporaries. Same
  // defect class as std::ostringstream::str() -- it compiles, and nlohmann
  // catches it at runtime with "cannot compare iterators of different
  // containers", which is a much better outcome than the silent corruption
  // the equivalent std:: idiom would give.
  const json indexes = t.value("indexes", json::object());
  for (auto it = indexes.begin(); it != indexes.end(); ++it) {
    if (it.value().value("leading_column", "") != key) continue;
    supporting_index = it.key();
    if (it.value().value("is_unique", false) && it.value().value("is_valid", false)) {
      key_is_unique = true;
      break;
    }
  }
  if (!key_is_unique) {
    step.action = Action::kConflict;
    step.why = "no unique index leads with " + key;
    plan.conflicts.push_back(
        qualified + " has no valid unique index whose leading column is \"" + key +
        "\", so a keyset walk could skip or repeat rows. Create one first: "
        "CREATE UNIQUE INDEX CONCURRENTLY ... ON " + qualified + " (" + key + ");");
    return;
  }

  const long long rows = t.value("reltuples", 0LL);
  const auto where = in.body.value("where", "");
  const auto from = in.body.value("from", "");

  std::vector<std::string> assignments;
  for (auto it = in.body["set"].begin(); it != in.body["set"].end(); ++it) {
    assignments.push_back(it.key() + " = " + it.value().get<std::string>());
  }

  // FOR UPDATE without SKIP LOCKED, deliberately. SKIP LOCKED would silently
  // skip contended rows while the cursor advanced past them, leaving a
  // backfill that reports complete and is not. It looks like the obviously
  // right pacing idiom and is wrong here.
  // The `from` items appear in BOTH the CTE and the outer UPDATE, because the
  // filter may reference them -- a backfill whose predicate joins to another
  // table is the ordinary case, not the exotic one. An earlier version put
  // them only in the outer UPDATE, which produced a CTE referencing an alias
  // that was not in scope: SQL that renders convincingly and does not parse.
  //
  // FOR UPDATE OF t, not a bare FOR UPDATE: with a join, a bare FOR UPDATE
  // locks rows in every table named, so the backfill would take row locks on
  // the lookup table it merely reads. That is contention this tool exists to
  // avoid, inflicted by its own batch statement.
  // The target table is referenced by its OWN NAME, not by an alias.
  //
  // A spec's `where` and `set` expressions have to name the target somehow, and
  // that choice is an interface contract: `orders.warehouse_id = w.id` and
  // `t.warehouse_id = w.id` are both plausible, and a spec written for one
  // fails against the other. Using the table's own name is what someone
  // writing this SQL by hand would do, and it needs no explanation -- an alias
  // would be a convention every author had to learn from a footnote.
  //
  // The `from` items appear in BOTH the CTE and the outer UPDATE, because the
  // filter may reference them: a backfill whose predicate joins to another
  // table is the ordinary case. An earlier version put them only in the outer
  // UPDATE, producing a CTE that referenced an alias not in scope -- SQL that
  // renders convincingly and does not parse.
  //
  // FOR UPDATE OF <target>, not a bare FOR UPDATE: with a join, a bare FOR
  // UPDATE locks rows in every table named, so the backfill would take row
  // locks on the lookup table it merely reads. That is contention this tool
  // exists to avoid, inflicted by its own batch statement.
  const std::string rel = in.table();

  // The pre-image, captured in the SAME STATEMENT as the update.
  //
  // Data-modifying CTEs all see one snapshot, so an INSERT ... SELECT reading
  // the target inside this statement sees the rows as they were BEFORE the
  // UPDATE in the same statement. That is what makes the capture atomic with
  // the change: there is no window in which one committed and the other did
  // not, and a crash leaves the backup and the data agreeing.
  //
  // This is what replaced the pinned-snapshot idea. A snapshot lets you LOOK at
  // the old values while holding back the xmin horizon for the whole backfill;
  // this KEEPS them, durably, and doubles as the revert path -- which a
  // snapshot can never be.
  std::string preserve_cte;
  if (in.body.contains("preserve")) {
    const auto pschema = in.body["preserve"].value("schema", "");
    const auto ptable = in.body["preserve"].value("table", "");
    const auto preserved = pschema + "." + ptable;

    std::vector<std::string> saved_cols{key};
    for (auto it = in.body["set"].begin(); it != in.body["set"].end(); ++it) {
      saved_cols.push_back(it.key());
    }

    // The side table is created by its own step, from the target's real column
    // types -- LIKE would carry constraints and defaults that have no business
    // on a backup.
    std::string cols_ddl;
    for (const auto& c : saved_cols) {
      const auto type = columns.contains(c)
                            ? columns[c].value("type", "text")
                            : std::string("text");
      if (!cols_ddl.empty()) cols_ddl += ", ";
      cols_ddl += c + " " + type;
    }
    Step create;
    create.kind = in.kind_name;
    create.txn_class = TxnClass::kRequired;
    create.own_transaction = true;
    create.lock = "AccessExclusiveLock on the new table only";
    create.sql.push_back("CREATE TABLE IF NOT EXISTS " + preserved + " (" +
                         cols_ddl + ", laswell_saved_at timestamptz NOT NULL DEFAULT now());");
    create.sql.push_back("COMMENT ON TABLE " + preserved + " IS " +
                         detail::quote_literal(
                             "Pre-image captured by pg_laswell before backfilling " +
                             qualified + ". Each row is what the target looked "
                             "like before the change, written in the same "
                             "transaction as the change itself.") + ";");
    create.why = "preserve: the pre-image needs somewhere to live, and it must "
                 "exist before the first batch writes to it";
    out.push_back(std::move(create));

    std::string select_cols;
    for (const auto& c : saved_cols) {
      if (!select_cols.empty()) select_cols += ", ";
      select_cols += rel + "." + c;
    }
    preserve_cte = ", preserved AS (\n"
                   "  INSERT INTO " + preserved + " (" +
                   detail::join(saved_cols, ", ") + ")\n"
                   "  SELECT " + select_cols + "\n"
                   "    FROM " + qualified + ", batch AS pb\n"
                   "   WHERE " + rel + "." + key + " = pb." + key + "\n"
                   ")";
    step.detail["preserve"] = preserved;
  }

  const std::string batch_sql =
      "WITH batch AS (\n"
      "  SELECT " + rel + "." + key + "\n"
      "    FROM " + qualified + (from.empty() ? "" : ", " + from) + "\n"
      "   WHERE " + rel + "." + key + " > $1 AND (" + where + ")\n"
      "   ORDER BY " + rel + "." + key + "\n"
      "   LIMIT $2\n"
      "   FOR UPDATE OF " + rel + "\n"
      ")" + preserve_cte + "\n"
      "UPDATE " + qualified + "\n"
      "   SET " + detail::join(assignments, ", ") + "\n"
      "  FROM batch AS b" + (from.empty() ? "" : ", " + from) + "\n"
      " WHERE " + rel + "." + key + " = b." + key + "\n"
      "RETURNING " + rel + "." + key + ";";

  step.sql.push_back(batch_sql);
  step.detail["qualified"] = qualified;
  // Carried so the executor can check a resume cursor against the predicate
  // rather than trusting it.
  step.detail["where"] = where;
  step.detail["batch_rows"] = cfg.batch_rows;
  step.detail["commit_interval_ms"] = cfg.commit_interval_ms;
  step.detail["batch_cap_rows"] = cfg.batch_cap_rows;
  step.detail["rows_estimated"] = rows;
  step.detail["key"] = key;
  step.detail["supporting_index"] = supporting_index;
  step.detail["commits_on"] =
      json::array({"lock_waiter", "interval", "batch_cap"});

  if (in.body.contains("verify_remaining")) {
    step.detail["verify_remaining"] = in.body["verify_remaining"];
    // Runs on the WORKER connection, after the last batch. Verification that
    // must see in-flight rows cannot run anywhere else: an imported snapshot
    // does not see the exporter's uncommitted changes (spike S1).
    step.detail["verify_runs_on"] = "worker connection, after the final batch";
  }
  if (in.body.contains("assert_invariants")) {
    step.detail["assert_invariants"] = in.body["assert_invariants"];
    step.detail["invariants_run_on"] =
        "the worker connection, before the first batch and after the last. "
        "They ask whether the work broke something, which verify_remaining "
        "does not: a backfill can complete every row and still halve a total.";
  }

  step.why = std::to_string(rows) + " rows estimated; keyset walk on " + key +
             " via " + supporting_index + ", " + std::to_string(cfg.batch_rows) +
             " rows per batch, committing on a lock waiter or " +
             std::to_string(cfg.commit_interval_ms) + "ms";

  // Without an index that supports (key) under the filter, each batch may
  // rescan from the start -- quadratic in the table size. A warning rather
  // than a refusal, because on a small table it does not matter and the
  // operator may know that.
  bool filtered_index = false;
  for (auto it = indexes.begin(); it != indexes.end(); ++it) {
    const auto def = it.value().value("definition", "");
    if (def.find(" WHERE ") != std::string::npos &&
        it.value().value("leading_column", "") == key) {
      filtered_index = true;
    }
  }
  if (!filtered_index && rows > 1000000) {
    plan.warnings.push_back(
        "no partial index supports (" + key + ") under the backfill's filter on " +
        qualified + "; at " + std::to_string(rows) +
        " estimated rows each batch may rescan already-updated rows. Consider "
        "a partial index before running this against a live system.");
  }
}

// The partitioned-index recipe. Emitted as separate steps because each
// concurrent build must run outside a transaction block, and they run one at a
// time rather than together: two concurrent builds on one parent's children
// contend for the same catalog rows without buying any parallelism worth
// having.
inline void plan_partitioned_index(const Intent& in, const json& t, Plan& plan,
                                   std::vector<Step>& out, Step& parent_step) {
  const auto qualified = in.qualified_table();
  const auto name = in.body.value("name", "");
  const auto method = in.body.value("method", "btree");
  const auto where = in.body.value("where", "");
  const bool unique = in.body.value("unique", false);

  std::vector<std::string> columns;
  for (const auto& c : in.body.value("columns", json::array())) {
    columns.push_back(c.get<std::string>());
  }
  const std::string cols = detail::join(columns, ", ");
  const std::string tail = " USING " + method + " (" + cols + ")" +
                           (where.empty() ? "" : " WHERE " + where);

  const auto partitions = t.value("partitions", json::array());
  if (partitions.empty()) {
    parent_step.action = Action::kConflict;
    parent_step.why = qualified + " is partitioned but has no partitions";
    plan.conflicts.push_back(
        qualified +
        " is a partitioned table with no partitions. Create the index once "
        "there is something to build it on, or attach a partition first.");
    return;
  }

  // A unique index on a partitioned table must include the partition key, and
  // this planner does not read the partition key. Refusing beats emitting a
  // recipe whose last step fails.
  if (unique) {
    parent_step.action = Action::kConflict;
    parent_step.why = "a unique index on a partitioned table must include the "
                      "partition key, which this planner does not read";
    plan.conflicts.push_back(
        "\"" + name + "\" is UNIQUE on the partitioned table " + qualified +
        ". PostgreSQL requires such an index to include the partition key, and "
        "pg_laswell does not read the partition key, so it will not emit a "
        "recipe whose final ATTACH would fail. Add the constraint by hand, or "
        "make the index non-unique.");
    return;
  }

  std::vector<std::string> child_indexes;
  for (const auto& p : partitions) {
    const auto part = p.get<std::string>();
    const auto bare = part.substr(part.find('.') + 1);
    const auto child = bare + "_" + name;
    // Deterministic rather than truncated: a truncated name would differ
    // between two runs that both "succeeded", and the idempotence check keys
    // on the name.
    if (child.size() > 63) {
      parent_step.action = Action::kConflict;
      parent_step.why = "a per-partition index name would exceed 63 bytes";
      plan.conflicts.push_back(
          "the per-partition index for " + part + " would be named \"" + child +
          "\", which PostgreSQL would truncate at 63 bytes -- and a truncated "
          "name is not deterministic, so two runs could disagree about whether "
          "the index exists. Use a shorter index name.");
      return;
    }
    child_indexes.push_back(child);

    Step s;
    s.kind = in.kind_name;
    s.txn_class = TxnClass::kForbidden;
    s.own_transaction = true;
    s.lock = "ShareUpdateExclusiveLock on " + part;
    s.sql.push_back("CREATE INDEX CONCURRENTLY " + child + " ON " + part + tail + ";");
    s.why = "partition " + std::to_string(child_indexes.size()) + " of " +
            std::to_string(partitions.size()) +
            ": CREATE INDEX CONCURRENTLY is refused on the parent, so each "
            "partition is built separately and concurrently";
    s.detail["partition"] = part;
    s.detail["index"] = child;
    s.detail["schema"] = in.schema();
    out.push_back(std::move(s));
  }

  Step parent;
  parent.kind = in.kind_name;
  parent.txn_class = TxnClass::kOptional;
  parent.own_transaction = true;
  parent.lock = "ShareLock on " + qualified + ", which holds no data itself";
  parent.sql.push_back("CREATE INDEX " + name + " ON ONLY " + qualified + tail + ";");
  parent.sql.push_back("COMMENT ON INDEX " + in.schema() + "." + name + " IS " +
                       detail::quote_literal(in.body.value("comment", "")) + ";");
  parent.why =
      "ON ONLY means the parent index is a catalog entry with no data, so this "
      "scans nothing. It stays INVALID until every partition is attached";
  parent.detail["index"] = name;
  parent.detail["schema"] = in.schema();
  out.push_back(std::move(parent));

  for (std::size_t i = 0; i < child_indexes.size(); ++i) {
    Step s;
    s.kind = in.kind_name;
    s.txn_class = TxnClass::kOptional;
    s.lock = "AccessExclusiveLock on the two indexes, briefly";
    s.sql.push_back("ALTER INDEX " + in.schema() + "." + name +
                    " ATTACH PARTITION " + in.schema() + "." + child_indexes[i] + ";");
    s.why = "attach " + std::to_string(i + 1) + " of " +
            std::to_string(child_indexes.size()) +
            "; the parent index becomes valid only when the last one lands";
    out.push_back(std::move(s));
  }

  Step verify;
  verify.kind = "verify_index_valid";
  verify.txn_class = TxnClass::kOptional;
  verify.lock = "none (catalog read)";
  verify.why =
      "a partitioned index that is missing even one attachment exists, is "
      "INVALID, and is never used -- and nothing else would say so";
  verify.detail = json{{"schema", in.schema()}, {"index", name}};
  out.push_back(std::move(verify));
}

// --- add_check_constraint --------------------------------------------------
//
// The same two-step shape as a foreign key, and for the same measured reason: a
// validating ADD holds its lock for the whole scan, while NOT VALID plus
// VALIDATE holds the strong lock for a catalog change only and does the scan
// under ShareUpdateExclusiveLock, which blocks neither reads nor writes.
inline void plan_add_check_constraint(const Intent& in, const Observations& obs,
                                      Plan& plan, std::vector<Step>& out) {
  const auto qualified = in.qualified_table();
  const auto& t = obs.table(qualified);
  const auto name = in.body.value("name", "");
  const auto expression = in.body.value("expression", "");

  if (!t.value("exists", false)) {
    Step s;
    s.kind = in.kind_name;
    s.action = Action::kConflict;
    s.why = qualified + " does not exist";
    plan.conflicts.push_back(s.why);
    out.push_back(std::move(s));
    return;
  }

  Step add;
  add.kind = in.kind_name;
  add.txn_class = TxnClass::kRequired;
  add.own_transaction = true;
  add.lock = "ShareRowExclusiveLock, briefly; NOT VALID means no scan";
  add.sql.push_back("ALTER TABLE " + qualified + " ADD CONSTRAINT " + name +
                    " CHECK (" + expression + ") NOT VALID;");
  add.why = "step 1 of 2: NOT VALID costs no scan, so the strong lock is held "
            "for the catalog change only";
  add.detail["constraint"] = name;
  out.push_back(std::move(add));

  Step validate;
  validate.kind = in.kind_name;
  validate.txn_class = TxnClass::kRequired;
  validate.own_transaction = true;
  validate.lock = "ShareUpdateExclusiveLock -- does NOT block reads or writes";
  validate.sql.push_back("ALTER TABLE " + qualified + " VALIDATE CONSTRAINT " +
                         name + ";");
  validate.why = "step 2 of 2: the scan, under a lock the application can work "
                 "through. Its own transaction, or step 1's lock would span it";
  validate.detail["constraint"] = name;
  out.push_back(std::move(validate));
}

// --- drop_index ------------------------------------------------------------
//
// A drop is fast whatever else is true -- a catalog change and a file unlink.
// What costs is ACQUIRING the lock: a plain DROP INDEX takes
// AccessExclusiveLock, so it queues behind every reader and everything arriving
// after it queues behind that. So the decision is about contention, not size,
// which is why this rule reads lock_waiters and ignores the table's bytes.
inline void plan_drop_index(const Intent& in, const Observations& obs, Plan& plan,
                            std::vector<Step>& out) {
  Step step;
  step.kind = in.kind_name;
  struct Emit { std::vector<Step>& o; Step& s; ~Emit() { o.push_back(s); } } emit{out, step};

  const auto qualified = in.qualified_table();
  const auto& t = obs.table(qualified);
  const auto name = in.body.value("name", "");

  if (!t.value("exists", false)) {
    step.action = Action::kConflict;
    step.why = qualified + " does not exist";
    plan.conflicts.push_back(step.why);
    return;
  }
  const json indexes = t.value("indexes", json::object());
  if (!indexes.contains(name)) {
    step.action = Action::kSatisfied;
    step.why = "no index named " + name + " on " + qualified;
    return;
  }
  const auto& ex = indexes[name];

  // A constraint's index cannot be dropped on its own; PostgreSQL refuses, and
  // the honest remedy is to drop the constraint.
  if (ex.value("constraint_backed", false)) {
    step.action = Action::kConflict;
    step.why = name + " backs a constraint and cannot be dropped on its own";
    plan.conflicts.push_back(
        "\"" + name + "\" on " + qualified +
        " backs a constraint. PostgreSQL refuses to drop such an index "
        "directly; drop the constraint instead, which pg_laswell does not yet "
        "plan.");
    return;
  }
  if (ex.value("is_unique", false)) {
    plan.warnings.push_back(
        "\"" + name + "\" is UNIQUE. Dropping it removes the uniqueness it "
        "enforces, not merely a lookup path. pg_licht evaluateIndex with hide "
        "will say whether anything still plans against it.");
  }

  const int waiters = t.value("lock_waiters", 0);
  if (waiters == 0) {
    step.txn_class = TxnClass::kOptional;
    step.lock = "AccessExclusiveLock, briefly";
    step.sql.push_back("DROP INDEX " + in.schema() + "." + name + ";");
    step.why = "0 lock waiters -> plain DROP (transactional, so a failure "
               "leaves nothing behind)";
  } else {
    step.txn_class = TxnClass::kForbidden;
    step.lock = "ShareUpdateExclusiveLock";
    step.sql.push_back("DROP INDEX CONCURRENTLY " + in.schema() + "." + name + ";");
    step.why = std::to_string(waiters) +
               " lock waiters already on " + qualified +
               " -> concurrent drop; a plain DROP would queue behind them and "
               "everything arriving after it would queue behind that";
  }
  step.detail["index"] = name;
  step.detail["schema"] = in.schema();
}

// --- set_not_null ----------------------------------------------------------
//
// A bare SET NOT NULL takes AccessExclusiveLock AND scans the whole table --
// measured -- so the table is unavailable for the length of the scan.
//
// The safe form uses a CHECK constraint to do the scanning under a weaker lock:
//
//   1. ADD CONSTRAINT ... CHECK (col IS NOT NULL) NOT VALID
//      ShareRowExclusiveLock, no scan, brief.
//   2. VALIDATE CONSTRAINT
//      ShareUpdateExclusiveLock -- does NOT block reads or writes -- and this
//      is where the scan happens.
//   3. SET NOT NULL
//      still AccessExclusiveLock, but PostgreSQL skips the scan because the
//      validated CHECK already proves it: 24ms on 5000 rows, measured.
//   4. DROP the CHECK, which is now redundant and costs time on every insert.
//
// Each of the first three MUST be in its own transaction. If 1 and 2 shared
// one, the ShareRowExclusiveLock from 1 would be held across 2's scan and the
// recipe would buy nothing at all.
inline void plan_set_not_null(const Intent& in, const Observations& obs, Plan& plan,
                              std::vector<Step>& out) {
  const auto qualified = in.qualified_table();
  const auto& t = obs.table(qualified);
  const auto column = in.body.value("column", "");
  // Deliberately NOT <table>_<column>_not_null.
  //
  // PostgreSQL 17+ records NOT NULL constraints in pg_constraint and names
  // them exactly that. Measured on 18.6: a temporary CHECK squatting on that
  // name forces the permanent constraint to become t_v_not_null1, leaving the
  // database with an auto-suffixed name for good, as a side effect of how the
  // column was set rather than of anything anyone asked for. The scan is
  // skipped on the strength of the constraint's EXPRESSION, never its name, so
  // a distinct name costs nothing.
  const auto check = in.table() + "_" + column + "_laswell_nn";

  auto fail = [&](const std::string& why, const std::string& detail) {
    Step s;
    s.kind = in.kind_name;
    s.action = Action::kConflict;
    s.why = why;
    plan.conflicts.push_back(detail);
    out.push_back(std::move(s));
  };

  if (!t.value("exists", false)) {
    fail(qualified + " does not exist", qualified + " does not exist");
    return;
  }
  const json columns = t.value("columns", json::object());
  if (!columns.contains(column)) {
    fail(qualified + "." + column + " does not exist",
         qualified + "." + column + " does not exist, so it cannot be set NOT NULL");
    return;
  }
  if (columns[column].value("not_null", false)) {
    Step s;
    s.kind = in.kind_name;
    s.action = Action::kSatisfied;
    s.why = qualified + "." + column + " is already NOT NULL";
    out.push_back(std::move(s));
    return;
  }

  auto make = [&](TxnClass c, const std::string& sql, const std::string& lock,
                  const std::string& why, bool own) {
    Step s;
    s.kind = in.kind_name;
    s.txn_class = c;
    s.own_transaction = own;
    s.lock = lock;
    s.why = why;
    s.sql.push_back(sql);
    s.detail["column"] = column;
    s.detail["qualified"] = qualified;
    out.push_back(std::move(s));
  };

  make(TxnClass::kRequired,
       "ALTER TABLE " + qualified + " ADD CONSTRAINT " + check + " CHECK (" +
           column + " IS NOT NULL) NOT VALID;",
       "ShareRowExclusiveLock, briefly; NOT VALID means no scan",
       "step 1 of 4: a NOT VALID check costs no scan, so the strong lock is "
       "held only for the catalog change",
       /*own=*/true);

  make(TxnClass::kRequired,
       "ALTER TABLE " + qualified + " VALIDATE CONSTRAINT " + check + ";",
       "ShareUpdateExclusiveLock -- does NOT block reads or writes",
       "step 2 of 4: this is where the scan happens, and it happens under a "
       "lock that lets the application keep working. It must be its own "
       "transaction, or step 1's stronger lock would be held across it",
       /*own=*/true);

  make(TxnClass::kRequired,
       "ALTER TABLE " + qualified + " ALTER COLUMN " + column + " SET NOT NULL;",
       "AccessExclusiveLock, but no scan",
       "step 3 of 4: still an exclusive lock, but PostgreSQL skips the scan "
       "because the validated CHECK already proves the column has no nulls "
       "(measured: 24ms on 5000 rows)",
       /*own=*/true);

  if (!in.body.value("keep_check", false)) {
    make(TxnClass::kRequired,
         "ALTER TABLE " + qualified + " DROP CONSTRAINT " + check + ";",
         "AccessExclusiveLock, briefly",
         "step 4 of 4: the CHECK is redundant once the column is NOT NULL, and "
         "a redundant constraint costs time on every insert. Set keep_check to "
         "keep it",
         /*own=*/true);
  }
}

// --- add_foreign_key -------------------------------------------------------
//
// Measured: adding a foreign key in one statement takes ShareRowExclusiveLock
// on the child and RowShareLock on the parent, and HOLDS THEM FOR THE WHOLE
// SCAN -- so writes to both tables are blocked for its duration. The two-step
// form takes the same strong lock for a catalog change only, then does the scan
// under ShareUpdateExclusiveLock, which does not block writes.
//
// The parent's lock is the one people are surprised by: a statement that names
// one table routinely locks two, and the table nobody mentioned is often the
// busier one.
inline void plan_add_foreign_key(const Intent& in, const Observations& obs,
                                 Plan& plan, std::vector<Step>& out) {
  const auto qualified = in.qualified_table();
  const auto& t = obs.table(qualified);
  const auto name = in.body.value("name", "");
  const auto parent = in.body.value("references_schema", "") + "." +
                      in.body.value("references_table", "");

  if (!t.value("exists", false)) {
    Step s;
    s.kind = in.kind_name;
    s.action = Action::kConflict;
    s.why = qualified + " does not exist";
    plan.conflicts.push_back(s.why);
    out.push_back(std::move(s));
    return;
  }

  std::vector<std::string> cols, refs;
  for (const auto& c : in.body.value("columns", json::array())) {
    cols.push_back(c.get<std::string>());
  }
  for (const auto& c : in.body.value("references_columns", json::array())) {
    refs.push_back(c.get<std::string>());
  }

  // Index the FK column, or every parent update and delete scans the child.
  bool supported = false;
  const json indexes = t.value("indexes", json::object());
  for (auto it = indexes.begin(); it != indexes.end(); ++it) {
    if (it.value().value("leading_column", "") == (cols.empty() ? "" : cols[0])) {
      supported = true;
    }
  }
  if (!supported && !cols.empty()) {
    plan.warnings.push_back(
        "no index leads with " + qualified + "." + cols[0] +
        ", so every UPDATE or DELETE on " + parent +
        " will scan " + qualified +
        " to check this constraint. Add the index first, in an earlier intent.");
  }

  std::string clause =
      "FOREIGN KEY (" + detail::join(cols, ", ") + ") REFERENCES " + parent +
      " (" + detail::join(refs, ", ") + ")";
  if (in.body.contains("on_delete")) {
    clause += " ON DELETE " + in.body.value("on_delete", "");
  }
  if (in.body.contains("on_update")) {
    clause += " ON UPDATE " + in.body.value("on_update", "");
  }

  Step add;
  add.kind = in.kind_name;
  add.txn_class = TxnClass::kRequired;
  add.own_transaction = true;
  add.lock = "ShareRowExclusiveLock on " + qualified + " and RowShareLock on " +
             parent + "; NOT VALID means no scan";
  add.sql.push_back("ALTER TABLE " + qualified + " ADD CONSTRAINT " + name + " " +
                    clause + " NOT VALID;");
  add.why =
      "step 1 of 2: NOT VALID takes the same strong locks as a validating add "
      "but for a catalog change only, so they are held for milliseconds rather "
      "than for the length of a scan. Note the second lock: this statement "
      "names one table and locks two, and " + parent +
      " may be the busier one";
  add.detail["constraint"] = name;
  add.detail["references"] = parent;
  out.push_back(std::move(add));

  Step validate;
  validate.kind = in.kind_name;
  validate.txn_class = TxnClass::kRequired;
  validate.own_transaction = true;
  validate.lock = "ShareUpdateExclusiveLock on " + qualified +
                  " -- does NOT block reads or writes";
  validate.sql.push_back("ALTER TABLE " + qualified + " VALIDATE CONSTRAINT " +
                         name + ";");
  validate.why =
      "step 2 of 2: the scan, under a lock that lets the application keep "
      "working. It must be its own transaction, or step 1's "
      "ShareRowExclusiveLock would be held across it and the two-step form "
      "would buy nothing";
  validate.detail["constraint"] = name;
  out.push_back(std::move(validate));
}

// --- the planner ----------------------------------------------------------

inline Plan plan_migration(const Spec& spec, const Observations& obs,
                           const ExecutorConfig& cfg) {
  Plan plan;
  plan.spec_id = spec.id;
  plan.spec_digest = spec.digest;
  plan.budget = detail::compute_budget(obs, cfg);

  if (spec.min_server_version > 0 && obs.server_version > 0 &&
      obs.server_version < spec.min_server_version) {
    plan.ok = false;
    plan.conflicts.push_back(
        "the spec requires server version " + std::to_string(spec.min_server_version) +
        " and this server is " + std::to_string(obs.server_version));
    return plan;
  }
  if (obs.server.value("is_in_recovery", false)) {
    plan.ok = false;
    plan.conflicts.push_back(
        "this connection is a standby (pg_is_in_recovery() is true); "
        "pg_laswell will not apply a migration to a replica");
    return plan;
  }

  int ordinal = 0;
  int group = 1;
  TxnClass previous = TxnClass::kRequired;
  bool first = true;

  // Each intent is planned against the catalog as its predecessors will leave
  // it, not as it is now. See project() above.
  Observations projected = obs;

  // A table we could not read is a table we must not plan against. This is not
  // a degraded reading to work around: observation is bounded by lock_timeout
  // precisely so that "something holds a strong lock on your target right now"
  // arrives as a fact rather than as a hang, and planning a migration on top of
  // an in-flight ALTER TABLE is the thing that must not happen.
  for (const auto& in : spec.intents) {
    const auto& t = projected.table(in.qualified_table());
    if (t.value("observation_blocked", false)) {
      plan.ok = false;
      plan.conflicts.push_back(t.value("blocked_reason", "observation blocked"));
      return plan;
    }
  }

  for (const auto& in : spec.intents) {
    // An intent may need more than one step, and more importantly more than
    // one TRANSACTION. The safe way to add a foreign key is ADD ... NOT VALID
    // and then VALIDATE, and it only works if the first COMMITS before the
    // second runs -- otherwise the ShareRowExclusiveLock is held across the
    // scan anyway and the two-step recipe buys nothing.
    std::vector<Step> emitted;
    switch (in.kind) {
      case IntentKind::kAddColumn:   plan_add_column(in, projected, plan, emitted); break;
      case IntentKind::kCreateIndex: plan_create_index(in, projected, cfg, plan, emitted); break;
      case IntentKind::kBackfill:    plan_backfill(in, projected, cfg, plan, emitted); break;
      case IntentKind::kDropIndex:   plan_drop_index(in, projected, plan, emitted); break;
      case IntentKind::kSetNotNull:  plan_set_not_null(in, projected, plan, emitted); break;
      case IntentKind::kAddForeignKey: plan_add_foreign_key(in, projected, plan, emitted); break;
      case IntentKind::kAddCheckConstraint:
        plan_add_check_constraint(in, projected, plan, emitted); break;
    }
    if (!emitted.empty()) project(in, emitted.front(), projected);

    for (auto& step : emitted) {
      step.ordinal = ordinal++;
      // Grouping: consecutive required/optional steps share a transaction, and
      // anything forbidden, self-committing, or explicitly wanting its own
      // transaction forces a boundary.
      const bool boundary_before =
          step.own_transaction ||
          step.txn_class == TxnClass::kForbidden ||
          step.txn_class == TxnClass::kOwnTxnPerBatch ||
          previous == TxnClass::kForbidden ||
          previous == TxnClass::kOwnTxnPerBatch;
      if (!first && boundary_before) ++group;
      step.txn_group = group;
      previous = step.txn_class;
      first = false;
      plan.steps.push_back(std::move(step));
    }
  }

  // A CIC always gains a validity check. The tool must never report a
  // concurrent build as succeeded merely because the statement returned: it
  // can return, leave an invalid index, and that is a failure that looks like
  // a success (spike S5).
  std::vector<Step> with_verification;
  for (const auto& s : plan.steps) {
    with_verification.push_back(s);
    if (s.kind != "create_index" || s.action != Action::kApply) continue;
    if (s.txn_class != TxnClass::kForbidden) continue;
    Step v;
    v.ordinal = static_cast<int>(with_verification.size());
    v.txn_group = s.txn_group;
    v.kind = "verify_index_valid";
    v.txn_class = TxnClass::kOptional;
    v.lock = "none (catalog read)";
    v.why = "a concurrent build can return without error and leave an invalid index";
    v.detail = json{{"schema", s.detail.value("schema", "")},
                    {"index", s.detail.value("index", "")}};
    with_verification.push_back(std::move(v));
  }
  // Renumber so ordinals stay dense after the insertions.
  for (std::size_t i = 0; i < with_verification.size(); ++i) {
    with_verification[i].ordinal = static_cast<int>(i);
  }
  plan.steps = std::move(with_verification);

  plan.ok = plan.conflicts.empty();
  return plan;
}

inline std::string Plan::render() const {
  std::string out;
  out += "Plan for " + spec_id + "  (digest " + spec_digest.substr(0, 12) + "…)\n";
  if (!ok) {
    out += "\nREFUSED. Nothing will run.\n";
    for (const auto& c : conflicts) out += "  conflict: " + c + "\n";
    return out;
  }

  int current_group = -1;
  for (const auto& s : steps) {
    if (s.txn_group != current_group) {
      current_group = s.txn_group;
      std::string note = "atomic";
      if (s.txn_class == TxnClass::kForbidden) {
        note = "NOT atomic: cannot run inside a transaction block";
      } else if (s.txn_class == TxnClass::kOwnTxnPerBatch) {
        note = "NOT atomic: paced, many commits";
      }
      out += "\n-- transaction group " + std::to_string(current_group) +
             " ---------------- " + note + " --\n";
    }
    out += " " + std::to_string(s.ordinal) + ". " + s.kind;
    if (s.action == Action::kSatisfied) out += "  [already satisfied]";
    if (s.action == Action::kConflict) out += "  [CONFLICT]";
    out += "\n";
    for (const auto& q : s.sql) out += "    " + q + "\n";
    if (!s.lock.empty()) out += "    lock: " + s.lock + "\n";
    if (!s.why.empty()) out += "    why:  " + s.why + "\n";
  }
  for (const auto& w : warnings) out += "\n warn: " + w + "\n";
  return out;
}

}  // namespace pglaswell
