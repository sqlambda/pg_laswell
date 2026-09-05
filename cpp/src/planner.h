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

inline void plan_add_column(const Intent& in, const Observations& obs, Plan& plan,
                            Step& step) {
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
  }
}

inline void plan_create_index(const Intent& in, const Observations& obs,
                              const ExecutorConfig& cfg, Plan& plan, Step& step) {
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

  const auto& indexes = t.value("indexes", json::object());
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

  const long long size = t.value("size_measured", t.value("size_estimate", 0LL));
  const bool measured = t.contains("size_measured");
  const int waiters = t.value("lock_waiters", 0);
  const bool partitioned = t.value("kind", "") == "partitioned_table";

  // S4, measured 2026-09-05 on 18.6: CREATE INDEX CONCURRENTLY is refused
  // outright on a partitioned table.
  if (partitioned) {
    step.action = Action::kConflict;
    step.why = qualified + " is partitioned; CREATE INDEX CONCURRENTLY is not supported there";
    plan.conflicts.push_back(
        step.why +
        ". Build the index CONCURRENTLY on each partition, then create it on "
        "the parent with ONLY and ATTACH PARTITION each one. pg_laswell does "
        "not yet plan that sequence.");
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
  step.detail["size_bytes"] = size;
  step.detail["size_source"] = measured ? "measured" : "estimate";
  step.detail["estimated_from"] = t.value("estimated_from", json());
  step.detail["lock_waiters"] = waiters;
  (void)cfg;
}

inline void plan_backfill(const Intent& in, const Observations& obs,
                          const ExecutorConfig& cfg, Plan& plan, Step& step) {
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

  const auto& columns = t.value("columns", json::object());
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
  const std::string batch_sql =
      "WITH batch AS (\n"
      "  SELECT t." + key + "\n"
      "    FROM " + qualified + " AS t\n"
      "   WHERE t." + key + " > $1 AND (" + where + ")\n"
      "   ORDER BY t." + key + "\n"
      "   LIMIT $2\n"
      "   FOR UPDATE\n"
      ")\n"
      "UPDATE " + qualified + " AS t\n"
      "   SET " + detail::join(assignments, ", ") + "\n" +
      (from.empty() ? "" : "  FROM " + from + ", batch AS b\n") +
      (from.empty() ? "  FROM batch AS b\n" : "") +
      " WHERE t." + key + " = b." + key + "\n"
      "RETURNING t." + key + ";";

  step.sql.push_back(batch_sql);
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
    Step step;
    step.ordinal = ordinal++;
    step.kind = in.kind_name;

    switch (in.kind) {
      case IntentKind::kAddColumn:   plan_add_column(in, projected, plan, step); break;
      case IntentKind::kCreateIndex: plan_create_index(in, projected, cfg, plan, step); break;
      case IntentKind::kBackfill:    plan_backfill(in, projected, cfg, plan, step); break;
    }
    project(in, step, projected);

    // Grouping: consecutive required/optional steps share a transaction, and
    // anything forbidden or self-committing forces a boundary.
    const bool boundary_before =
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
    v.detail = json{{"index", s.detail.value("index_name", json())}};
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
