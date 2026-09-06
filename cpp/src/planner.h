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
  // Out-of-band actions this migration needs, structured so something can ACT
  // on them rather than read them.
  //
  // A warning tells a person something. A prerequisite tells an agent -- or a
  // skill, or a provisioning script -- what must be true, WHERE, and how to
  // check it. The difference matters because several of these are on a machine
  // pg_laswell is not connected to: a replication slot on the publisher, a
  // package on the host, free space in a tablespace. Prose cannot be acted on
  // reliably; a shape can.
  //
  // Each entry: {kind, target, where, requirement, verify, blocking}.
  // `blocking` says whether the migration fails without it or merely does
  // less than it appears to.
  std::vector<json> prerequisites;
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
                {"prerequisites", prerequisites},
                {"budget", budget}};
  }

  // The digest of the plan itself: the determinism receipt. planMigration and
  // startMigration must produce the same one, which is how "what ran is what
  // you were shown" becomes checkable rather than promised.
  std::string digest() const { return digest_hex(to_json()); }

  std::string render() const;
};

namespace detail {

// Records an out-of-band prerequisite. Every field is required because a
// half-filled one is worse than none: a skill that cannot tell WHERE to act, or
// how to check whether it already did, will act on the wrong machine or twice.
inline void require_out_of_band(Plan& plan, const std::string& kind,
                                const std::string& target,
                                const std::string& where,
                                const std::string& requirement,
                                const std::string& verify, bool blocking) {
  plan.prerequisites.push_back(json{{"kind", kind},
                                    {"target", target},
                                    {"where", where},
                                    {"requirement", requirement},
                                    {"verify", verify},
                                    {"blocking", blocking}});
}

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

// Always quotes, rather than quoting only when it looks necessary.
//
// A role or column named "user", "order" or "Select" is a reserved word or is
// case-folded, and deciding case by case needs PostgreSQL's own keyword list.
// Unconditional quoting is correct for every identifier PostgreSQL will hand
// back to us, because what comes out of the catalog is the real name.
inline std::string quote_identifier(const std::string& s) {
  std::string out = "\"";
  for (const char c : s) {
    if (c == '"') out += '"';
    out += c;
  }
  out += "\"";
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

  // Worker capacity, which is what actually limits concurrent migrations --
  // and it is a server-side limit, not a client one. The migrations run inside
  // PostgreSQL; this process contributes three connections and a poll loop per
  // job, so the machine pg_laswell runs on is very nearly irrelevant to how
  // many can run at once.
  const int max_workers = obs.server.value("max_parallel_workers", 0);
  const int max_maint = obs.server.value("max_parallel_maintenance_workers", 0);
  const int workers_busy = obs.server.value("parallel_workers_active", 0);
  const int jobs = std::max(1, cfg.max_concurrent_jobs);
  json w{{"maxParallelWorkers", max_workers},
         {"maxParallelMaintenanceWorkers", max_maint},
         {"maxWorkerProcesses", obs.server.value("max_worker_processes", 0)},
         {"parallelWorkersActive", workers_busy},
         {"parallelWorkersFree", std::max(0, max_workers - workers_busy)},
         {"maxConcurrentJobs", jobs}};
  if (cfg.host_vcpus > 0) {
    w["hostVcpus"] = cfg.host_vcpus;
    w["hostVcpusSource"] = "configuration -- declared by an operator";
  } else {
    w["hostVcpusSource"] =
        "not declared. PostgreSQL exposes no CPU count in SQL -- the only "
        "cpu-named settings are planner cost constants -- so this is "
        "configuration or nothing, never a guess.";
  }

  // The verdict, with the number that decided it. A bare boolean would make
  // the caller guess at the reason, and the reason is the useful half.
  const int connections_per_job = 3;
  const int free = b.value("effectiveHeadroom", 0);
  if (free < connections_per_job) {
    w["canStartAnotherJob"] = false;
    w["verdict"] = "no: " + std::to_string(free) +
                   " connection(s) free and a job needs " +
                   std::to_string(connections_per_job) +
                   " (worker, observer, coordination)";
  } else if (max_workers > 0 && workers_busy >= max_workers) {
    // Not a refusal: a job with no index build does not need a worker slot.
    w["canStartAnotherJob"] = true;
    w["verdict"] = "yes, but all " + std::to_string(max_workers) +
                   " parallel worker slots are in use -- an index build in the "
                   "new job will run single-threaded, and silently, so any "
                   "duration estimated from a measured build will be wrong";
  } else {
    w["canStartAnotherJob"] = true;
    w["verdict"] = "yes: " + std::to_string(free) + " connection(s) and " +
                   std::to_string(std::max(0, max_workers - workers_busy)) +
                   " parallel worker slot(s) free";
  }
  if (jobs > 1 && max_workers > 0 && jobs > max_workers) {
    w["note"] = "max_concurrent_jobs is " + std::to_string(jobs) +
                " and the server has " + std::to_string(max_workers) +
                " parallel worker slots in total, so concurrent index builds "
                "will contend for them regardless of how many jobs are "
                "allowed. Worker slots run out long before connections do.";
  }
  b["workers"] = w;

  // maintenance_work_mem, divided by the job count.
  //
  // PostgreSQL applies it as a limit per OPERATION, not as a budget across
  // them -- and, checked in the documentation rather than assumed, NOT per
  // parallel worker: "parallel utility commands treat the resource limit
  // maintenance_work_mem as a limit to be applied to the entire utility
  // command, regardless of the number of parallel worker processes". So a
  // parallel build does not multiply it and an estimate assuming otherwise
  // would be three times too pessimistic.
  //
  // What DOES multiply it is concurrency, and the documentation's own
  // justification for setting it high is the assumption concurrent migrations
  // void: "an installation normally doesn't have many of them running
  // concurrently". Thirty-two jobs is precisely that, so the configured
  // ceiling is divided by how many may run at once.
  if (cfg.maintenance_work_mem_mb > 0) {
    const int per_job = std::max(1, cfg.maintenance_work_mem_mb / jobs);
    b["maintenanceWorkMem"] =
        json{{"configuredCeilingMb", cfg.maintenance_work_mem_mb},
             {"perStepMb", per_job},
             {"serverDefaultKb", obs.server.value("maintenance_work_mem_kb", 0)},
             {"why", "the ceiling divided by max_concurrent_jobs (" +
                         std::to_string(jobs) +
                         "), because PostgreSQL limits it per operation and "
                         "not across concurrent ones"}};
  } else {
    b["maintenanceWorkMem"] =
        json{{"perStepMb", 0},
             {"serverDefaultKb", obs.server.value("maintenance_work_mem_kb", 0)},
             {"why", "maintenance_work_mem_mb is not configured, so the "
                     "server's own setting is left alone. Raising it is the "
                     "cheapest speed-up available for an index build, a "
                     "foreign-key validation or a table rewrite -- but the "
                     "host's free memory is not visible from SQL, so this tool "
                     "will not guess at it."}};
  }
  return b;
}

}  // namespace detail

// --- per-intent rules ------------------------------------------------------

// Defined below, after the rule that reaches for it: a partitioned parent
// refuses CREATE INDEX CONCURRENTLY, so the create_index rule hands off here.
inline void plan_partitioned_index(const Intent& in, const json& t, Plan& plan,
                                   std::vector<Step>& out, Step& parent_step);

// Likewise: add_primary_key and set_identity both COMPOSE with the NOT NULL
// recipe rather than restating it, because PostgreSQL refuses both over a
// nullable column and the recipe does that scan under a lock the application
// survives. One definition, so the two can never drift apart.
inline void plan_set_not_null(const Intent& in, const Observations& obs,
                              Plan& plan, std::vector<Step>& out);

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

  // Views never block an ADD COLUMN -- measured, spike S14 -- so nothing is
  // rebuilt here. What is worth saying is the opposite: the new column reaches
  // NONE of them, including any written as SELECT *, because the star is
  // expanded at creation and the column list stored. Nothing errors, and the
  // column simply is not there through the view the application reads.
  const json dependent = t.value("dependent_views", json::object());
  if (!dependent.empty()) {
    std::vector<std::string> names;
    for (const auto& [n, v] : dependent.items()) {
      (void)v;
      names.push_back(n);
    }
    plan.warnings.push_back(
        qualified + "." + column + " will not be visible through " +
        std::to_string(names.size()) + " view(s) that read this table: " +
        detail::join(names, ", ") +
        ". A view stores its column list when it is created -- SELECT * is "
        "expanded then and there -- so adding a column never reaches one. "
        "Expose it with a replace_view intent per view that should carry it; "
        "PostgreSQL will not warn, and the column will simply be absent for "
        "anything reading through the view.");
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
  // Non-relation objects are keyed by name, not by schema.table, and they are
  // projected FIRST -- the tables guard below would return early for every one
  // of them, which made all of this dead code until a test looked.
  const auto object_key = object_key_for(in);
  if (!object_key.empty()) {
    const auto oq = in.body.value("schema", "") + "." + in.body.value("name", "");
    switch (in.kind) {
      case IntentKind::kCreateSchema:
        projected.objects[object_key] =
            json{{"exists", true}, {"kind", "schema"},
                 {"contains", json::array()}, {"depended_on_by", json::array()}};
        return;
      case IntentKind::kCreateExtension:
        projected.objects[object_key] =
            json{{"exists", true}, {"kind", "extension"},
                 {"depended_on_by", json::array()}};
        return;
      case IntentKind::kCreateType: {
        json labels = json::array();
        for (const auto& l : in.body.value("labels", json::array())) labels.push_back(l);
        projected.objects[object_key] =
            json{{"exists", true}, {"kind", "type"},
                 {"type_kind", in.body.value("type_kind", "")},
                 {"enum_labels", labels}, {"depended_on_by", json::array()}};
        return;
      }
      case IntentKind::kAddEnumValue:
        if (projected.objects.contains(object_key)) {
          projected.objects[object_key]["enum_labels"].push_back(
              in.body.value("value", ""));
        }
        return;
      case IntentKind::kCreateFunction:
        projected.objects[object_key] =
            json{{"exists", true}, {"kind", "function"},
                 {"returns", in.body.value("returns", "")},
                 {"depended_on_by", json::array()}};
        return;
      case IntentKind::kCreateSequence:
        projected.objects[object_key] =
            json{{"exists", true}, {"kind", "sequence"},
                 {"depended_on_by", json::array()}};
        return;
      case IntentKind::kDropSchema:
      case IntentKind::kDropExtension:
      case IntentKind::kDropType:
      case IntentKind::kDropFunction:
      case IntentKind::kDropSequence:
        projected.objects[object_key] = json{{"exists", false}};
        (void)oq;
        return;
      default: return;
    }
  }
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
    case IntentKind::kDropConstraint:
      projected.tables[qualified]["constraints"].erase(in.body.value("name", ""));
      return;
    case IntentKind::kAlterColumnType:
      projected.tables[qualified]["columns"][in.body.value("column", "")]["type"] =
          in.body.value("type", "");
      return;
    case IntentKind::kDropColumn:
      projected.tables[qualified]["columns"].erase(in.body.value("column", ""));
      return;
    case IntentKind::kCreateTable: {
      json cols = json::object();
      for (const auto& c : in.body.value("columns", json::array())) {
        cols[c.value("name", "")] =
            json{{"type", c.value("type", "")},
                 {"not_null", !c.value("nullable", true)}};
      }
      projected.tables[qualified] =
          json{{"exists", true}, {"kind", "table"}, {"columns", cols},
               {"indexes", json::object()}, {"constraints", json::object()},
               {"dependent_views", json::object()},
               {"referenced_by", json::array()}, {"size_estimate", 0}};
      return;
    }
    case IntentKind::kDropTable:
      projected.tables[qualified] = json{{"exists", false}};
      return;
    case IntentKind::kDeleteRows:
      return;  // rows change, the schema does not
    case IntentKind::kSetRowSecurity:
      projected.tables[qualified]["row_security"] =
          json{{"enabled", in.body.value("enabled", false)},
               {"forced", in.body.value("force", false)}};
      return;
    case IntentKind::kCreatePolicy:
      projected.tables[qualified]["policies"][in.body.value("name", "")] =
          json{{"command", in.body.value("command", "ALL")}};
      return;
    case IntentKind::kDropPolicy:
      projected.tables[qualified]["policies"].erase(in.body.value("name", ""));
      return;
    case IntentKind::kSetTriggerState:
      projected.tables[qualified]["triggers"][in.body.value("trigger", "")]
               ["enabled"] = in.body.value("enabled", false);
      return;
    case IntentKind::kGrant:
    case IntentKind::kRevoke:
      return;  // privileges are not part of the shape later intents plan against
    case IntentKind::kCreateTrigger:
      projected.tables[qualified]["triggers"][in.body.value("name", "")] =
          json{{"enabled", true}};
      return;
    case IntentKind::kDropTrigger:
      projected.tables[qualified]["triggers"].erase(in.body.value("name", ""));
      return;
    case IntentKind::kDropView:
      projected.tables[qualified] = json{{"exists", false}};
      return;
    case IntentKind::kRenameTable: {
      const auto to = in.body.value("schema", "") + "." + in.body.value("to", "");
      projected.tables[to] = projected.tables[qualified];
      projected.tables.erase(qualified);
      return;
    }
    case IntentKind::kRenameColumn: {
      auto& cols = projected.tables[qualified]["columns"];
      const auto from = in.body.value("column", "");
      if (cols.contains(from)) {
        cols[in.body.value("to", "")] = cols[from];
        cols.erase(from);
      }
      return;
    }
    case IntentKind::kRenameConstraint: {
      auto& cs = projected.tables[qualified]["constraints"];
      const auto from = in.body.value("name", "");
      if (cs.contains(from)) {
        cs[in.body.value("to", "")] = cs[from];
        cs.erase(from);
      }
      return;
    }
    // Handled above, before the tables guard: these are not relations, so
    // schema.table is not their key. Listed so -Werror=switch keeps proving the
    // set is complete.
    case IntentKind::kCreateSchema:
    case IntentKind::kDropSchema:
    case IntentKind::kCreateExtension:
    case IntentKind::kDropExtension:
    case IntentKind::kCreateType:
    case IntentKind::kDropType:
    case IntentKind::kAddEnumValue:
    case IntentKind::kCreateFunction:
    case IntentKind::kDropFunction:
    case IntentKind::kCreateSequence:
    case IntentKind::kDropSequence:
      return;
    case IntentKind::kDropNotNull:
      projected.tables[qualified]["columns"][in.body.value("column", "")]["not_null"] = false;
      return;
    case IntentKind::kCreateMaterializedView:
      projected.tables[in.body.value("schema", "") + "." + in.body.value("name", "")] =
          json{{"exists", true}, {"kind", "materialized_view"}};
      return;
    case IntentKind::kCreateTableAs:
      projected.tables[qualified] =
          json{{"exists", true}, {"kind", "table"}, {"columns", json::object()},
               {"indexes", json::object()}, {"constraints", json::object()}};
      return;
    case IntentKind::kCreatePublication:
    case IntentKind::kAlterPublication:
    case IntentKind::kDropPublication:
    case IntentKind::kCreateSubscription:
    case IntentKind::kAlterSubscription:
    case IntentKind::kDropSubscription:
    case IntentKind::kAlterObject:
    case IntentKind::kImportForeignSchema:
    case IntentKind::kSecurityLabel:
    case IntentKind::kAlterDefaultPrivileges:
    case IntentKind::kCreateObject:
    case IntentKind::kDropObject:
    case IntentKind::kCreateStatistics:
    case IntentKind::kDropStatistics:
    case IntentKind::kCreateRule:
    case IntentKind::kDropRule:
      return;
    case IntentKind::kSetIdentity:
    case IntentKind::kDropExpression:
    case IntentKind::kSetColumnOptions:
    case IntentKind::kSetTableOptions:
    case IntentKind::kSetLogged:
    case IntentKind::kSetTablespace:
    case IntentKind::kSetAccessMethod:
    case IntentKind::kSetReplicaIdentity:
    case IntentKind::kClusterOn:
    case IntentKind::kAlterColumnDefault:
    case IntentKind::kAlterSequence:
    case IntentKind::kAlterSchema:
    case IntentKind::kAlterExtension:
    case IntentKind::kAlterDomain:
    case IntentKind::kAlterFunction:
    case IntentKind::kAlterView:
    case IntentKind::kAlterPolicy:
    case IntentKind::kSetComment:
    case IntentKind::kSetOwner:
      return;
    case IntentKind::kAttachPartition: {
      const auto ch = in.body.value("schema", "") + "." + in.body.value("partition", "");
      projected.tables[qualified]["partitions"].push_back(ch);
      return;
    }
    case IntentKind::kDetachPartition: {
      const auto ch = in.body.value("schema", "") + "." + in.body.value("partition", "");
      auto& parts = projected.tables[qualified]["partitions"];
      for (auto it = parts.begin(); it != parts.end(); ++it) {
        if (it->get<std::string>() == ch) { parts.erase(it); break; }
      }
      return;
    }
    case IntentKind::kAddUniqueConstraint:
    case IntentKind::kAddPrimaryKey:
      projected.tables[qualified]["constraints"][in.body.value("name", "")] =
          json{{"type", in.kind == IntentKind::kAddPrimaryKey ? "p" : "u"},
               {"has_index", true},
               {"depended_on_by", json::array()}};
      for (const auto& c : in.body.value("columns", json::array())) {
        if (in.kind == IntentKind::kAddPrimaryKey) {
          projected.tables[qualified]["columns"][c.get<std::string>()]["not_null"] = true;
        }
      }
      return;
    case IntentKind::kReplaceView: {
      const auto v = in.body.value("schema", "") + "." + in.body.value("name", "");
      projected.tables[v]["exists"] = true;
      if (!projected.tables[v].contains("kind")) projected.tables[v]["kind"] = "view";
      projected.tables[v]["view_definition"] = in.body.value("definition", "");
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

// --- the dependent-view rebuild --------------------------------------------
//
// Shared by alter_column_type and drop_column, which are the two changes
// PostgreSQL refuses outright when a view reads the column.
//
// Measured on 18.6 (spike S13), and both halves matter:
//
//   ALTER TABLE vt ALTER COLUMN a TYPE varchar(100)   -- from varchar(50)
//   ERROR: cannot alter type of a column used by a view or rule
//
// That change is measured as NO rewrite (relfilenode unchanged) and is still
// refused; it succeeds the moment the view is dropped. So the rebuild is not a
// consequence of the change being expensive -- it is a consequence of a view
// existing, and the size reasoning that decides a rewrite does not decide it.
//
// And a naive drop/recreate loses eight things silently -- comment, per-column
// comments, grants, COLUMN-level grants, reloptions, INSTEAD OF triggers, a
// materialized view's indexes, and the owner. The recreate succeeds, nothing
// errors, and an application loses SELECT. That is what makes this painful by
// hand, and it is the whole reason these steps exist.
namespace detail {

// Renders one aclitem -- "grantee=privs/grantor" -- as GRANT statements.
//
// PostgreSQL has no statement that restores an ACL, so the privileges have to
// be reconstructed one letter at a time. An empty grantee is PUBLIC, which is
// the entry most likely to be lost and least likely to be noticed.
// `column` renders a COLUMN-level grant, whose syntax is not the table form
// with a column list appended: it is GRANT <priv> (col) ON <rel> TO <role>, the
// list going after the PRIVILEGE. Getting that backwards produces a syntax
// error that no string comparison against our own output would have caught --
// only running it against PostgreSQL did.
inline std::vector<std::string> grants_from_aclitem(const std::string& item,
                                                    const std::string& on,
                                                    const std::string& column = "") {
  const auto eq = item.find('=');
  if (eq == std::string::npos) return {};
  const auto slash = item.find('/', eq);
  const std::string grantee = item.substr(0, eq);
  const std::string privs =
      item.substr(eq + 1, slash == std::string::npos ? std::string::npos
                                                     : slash - eq - 1);
  static const std::map<char, const char*> kPriv = {
      {'r', "SELECT"},     {'w', "UPDATE"},  {'a', "INSERT"},
      {'d', "DELETE"},     {'D', "TRUNCATE"},{'x', "REFERENCES"},
      {'t', "TRIGGER"}};
  std::vector<std::string> out;
  const std::string who = grantee.empty() ? "PUBLIC" : quote_identifier(grantee);
  for (std::size_t i = 0; i < privs.size(); ++i) {
    const auto it = kPriv.find(privs[i]);
    if (it == kPriv.end()) continue;
    const bool with_grant = i + 1 < privs.size() && privs[i + 1] == '*';
    out.push_back(std::string("GRANT ") + it->second +
                  (column.empty() ? "" : " (" + quote_identifier(column) + ")") +
                  " ON " + on + " TO " + who +
                  (with_grant ? " WITH GRANT OPTION;" : ";"));
  }
  return out;
}

// The views that must be rebuilt for a change to `column`, deepest first.
//
// A view reading a DIFFERENT column of the same table is left alone: it does
// not block the change, and rebuilding it would widen the blast radius for
// nothing. A view that reaches the table only through another view carries no
// column list of its own -- it is dragged in by its parent, which is why the
// level walk exists rather than a single-level lookup.
inline std::vector<std::pair<int, std::string>> views_to_rebuild(
    const json& dependent_views, const std::string& column) {
  // First pass: the views that read the column directly.
  std::set<std::string> affected;
  for (const auto& [name, v] : dependent_views.items()) {
    for (const auto& c : v.value("uses_columns", json::array())) {
      if (c.get<std::string>() == column) affected.insert(name);
    }
  }
  if (affected.empty()) return {};
  // Second pass: close over the view -> view edges. Anything reading something
  // already condemned comes down with it.
  //
  // An earlier version used depth as a proxy -- "anything at or below the
  // shallowest affected level" -- which is wrong, and its own test caught it:
  // two views can share a level and be entirely unrelated, so a view on a
  // column nobody is touching was being dropped and recreated for nothing.
  // Depth orders the rebuild; it does not decide membership.
  for (bool grew = true; grew;) {
    grew = false;
    for (const auto& [name, v] : dependent_views.items()) {
      if (affected.count(name) != 0) continue;
      for (const auto& d : v.value("depends_on", json::array())) {
        if (affected.count(d.get<std::string>()) != 0) {
          affected.insert(name);
          grew = true;
          break;
        }
      }
    }
  }
  std::vector<std::pair<int, std::string>> out;
  for (const auto& name : affected) {
    out.emplace_back(dependent_views[name].value("level", 1), name);
  }
  std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
    if (a.first != b.first) return a.first > b.first;  // deepest first
    return a.second < b.second;                        // then stable by name
  });
  return out;
}

// DROP statements for the stack, deepest first.
inline void emit_view_drops(const json& views,
                            const std::vector<std::pair<int, std::string>>& order,
                            Step& step) {
  for (const auto& [level, name] : order) {
    (void)level;
    const auto& v = views[name];
    const bool matview = v.value("kind", "") == "materialized";
    step.sql.push_back(std::string("DROP ") + (matview ? "MATERIALIZED VIEW " : "VIEW ") +
                       v.value("name", name) + ";");
  }
}

// CREATE statements plus every restore, shallowest first.
inline void emit_view_recreates(
    const json& views, const std::vector<std::pair<int, std::string>>& order,
    Step& step) {
  for (auto it = order.rbegin(); it != order.rend(); ++it) {
    const auto& v = views[it->second];
    const std::string name = v.value("name", it->second);
    const bool matview = v.value("kind", "") == "materialized";
    std::string def = v.value("definition", "");
    // pg_get_viewdef ends the body with a semicolon; keeping it would produce
    // "... ; ;" and, with reloptions, put them after the terminator.
    while (!def.empty() && (def.back() == ';' || def.back() == '\n' ||
                            def.back() == ' ')) {
      def.pop_back();
    }

    std::string opts;
    const auto reloptions = v.value("reloptions", json::array());
    if (!reloptions.empty()) {
      std::vector<std::string> o;
      for (const auto& r : reloptions) o.push_back(r.get<std::string>());
      opts = " WITH (" + join(o, ", ") + ")";
    }
    step.sql.push_back(std::string("CREATE ") +
                       (matview ? "MATERIALIZED VIEW " : "VIEW ") + name +
                       opts + " AS " + def + ";");

    // The owner first: every GRANT below is recorded as granted BY the owner,
    // and restoring privileges before ownership records the wrong grantor.
    const auto owner = v.value("owner", "");
    if (!owner.empty()) {
      step.sql.push_back(std::string("ALTER ") +
                         (matview ? "MATERIALIZED VIEW " : "VIEW ") + name +
                         " OWNER TO " + quote_identifier(owner) + ";");
    }
    // A materialized view's indexes are dropped with it, and the unique one
    // among them is what REFRESH ... CONCURRENTLY requires -- so losing it
    // silently converts every future refresh into a blocking one.
    for (const auto& ix : v.value("indexes", json::array())) {
      step.sql.push_back(ix.get<std::string>() + ";");
    }
    const auto comment = v.value("comment", json());
    if (comment.is_string()) {
      step.sql.push_back(std::string("COMMENT ON ") +
                         (matview ? "MATERIALIZED VIEW " : "VIEW ") + name +
                         " IS " + quote_literal(comment.get<std::string>()) + ";");
    }
    // Bound to a local first. `v.value(...)` returns a TEMPORARY json, and
    // .items() holds a reference into it that dies at the end of the full
    // expression -- the fourth time this exact shape has appeared in this
    // project, and the only reason it is caught is -Wdangling-reference.
    const json column_comments = v.value("column_comments", json::object());
    for (const auto& [col, text] : column_comments.items()) {
      if (!text.is_string()) continue;
      step.sql.push_back("COMMENT ON COLUMN " + name + "." +
                         quote_identifier(col) + " IS " +
                         quote_literal(text.get<std::string>()) + ";");
    }
    for (const auto& acl : v.value("grants", json::array())) {
      for (const auto& g : grants_from_aclitem(acl.get<std::string>(), name)) {
        step.sql.push_back(g);
      }
    }
    for (const auto& cg : v.value("column_grants", json::array())) {
      const auto col = cg.value("column", "");
      for (const auto& acl : cg.value("acl", json::array())) {
        for (const auto& g :
             grants_from_aclitem(acl.get<std::string>(), name, col)) {
          step.sql.push_back(g);
        }
      }
    }
    // An INSTEAD OF trigger is how writes reach a view at all. Losing one does
    // not error on read, so it is found by an INSERT failing in production.
    for (const auto& tg : v.value("triggers", json::array())) {
      step.sql.push_back(tg.get<std::string>() + ";");
    }
  }
}

// Everything the rebuild cannot carry, said before it runs rather than found
// afterwards. Silence here would be the same failure as a hand rebuild.
inline void warn_about_rebuild(
    const json& views, const std::vector<std::pair<int, std::string>>& order,
    const std::string& qualified, Plan& plan) {
  std::vector<std::string> names, matviews;
  for (const auto& [level, name] : order) {
    (void)level;
    names.push_back(name);
    if (views[name].value("kind", "") == "materialized") matviews.push_back(name);
  }
  plan.warnings.push_back(
      "changing " + qualified + " rebuilds " + std::to_string(names.size()) +
      " dependent view(s): " + join(names, ", ") +
      ". They are dropped and recreated in one transaction, so no reader sees "
      "them missing -- but the table holds AccessExclusiveLock for the whole "
      "of it, including any rewrite.");
  if (!matviews.empty()) {
    plan.warnings.push_back(
        "materialized view(s) " + join(matviews, ", ") +
        " are rebuilt WITH DATA, so the transaction also re-runs their queries "
        "in full. On a large one that is the dominant cost of this migration, "
        "and it is paid while the lock is held.");
  }
}

}  // namespace detail

// --- logical replication, and the long tail --------------------------------
//
// Measured (spike S21, 18.6):
//
//   CREATE PUBLICATION                       runs inside a transaction
//   CREATE SUBSCRIPTION (create_slot = true) ERROR: cannot be executed inside
//                                            a transaction block
//   CREATE SUBSCRIPTION WITH (connect=false) runs, and warns the subscription
//                                            is not connected
//   pg_subscription.subconninfo              stores the password IN THE CLEAR
//
// The last one shapes the whole kind. This tool stores every statement it runs
// verbatim in laswell.step.sql -- that is the ledger's point -- so a password
// in the spec would be in the signed specification, in git and in the ledger.
// It is refused at parse time rather than redacted, because a redacted ledger
// entry would no longer be what ran, and that is the one property this tool
// will not trade.
inline void plan_replication(const Intent& in, const Observations& obs,
                             Plan& plan, std::vector<Step>& out) {
  Step step;
  step.kind = in.kind_name;
  struct Emit { std::vector<Step>& o; Step& s; ~Emit() { o.push_back(s); } } emit{out, step};
  step.txn_class = TxnClass::kRequired;
  (void)obs;
  const auto name = detail::quote_identifier(in.body.value("name", ""));

  auto tables_clause = [&](const char* key) {
    std::vector<std::string> ts;
    for (const auto& t : in.body.value(key, json::array())) {
      ts.push_back(t.get<std::string>());
    }
    return detail::join(ts, ", ");
  };

  switch (in.kind) {
    case IntentKind::kCreatePublication: {
      std::string sql = "CREATE PUBLICATION " + name;
      if (in.body.value("all_tables", false)) {
        sql += " FOR ALL TABLES";
      } else if (in.body.contains("tables")) {
        sql += " FOR TABLE " + tables_clause("tables");
      }
      if (in.body.contains("operations")) {
        std::vector<std::string> ops;
        for (const auto& o : in.body["operations"]) ops.push_back(o.get<std::string>());
        sql += " WITH (publish = '" + detail::join(ops, ", ") + "')";
      }
      step.sql.push_back(sql + ";");
      step.lock = "ShareUpdateExclusiveLock on each published table";
      step.why = "measured: a publication is created inside a transaction like "
                 "any other catalog change";
      plan.warnings.push_back(
          "a publication only decides what is SENT. Every table in it must have "
          "a replica identity a subscriber can match rows by -- the default is "
          "the primary key, and a table without one replicates INSERTs and then "
          "fails on the first UPDATE or DELETE. set_replica_identity is the "
          "intent that fixes that, and nothing here checks it.");
      if (in.body.value("all_tables", false)) {
        plan.warnings.push_back(
            "FOR ALL TABLES includes tables created later, which is usually "
            "what is wanted and means every future migration adds to what this "
            "publication sends -- including tables nobody intended to "
            "replicate.");
      }
      return;
    }

    case IntentKind::kAlterPublication: {
      std::vector<std::string> sql;
      if (in.body.contains("add_tables")) {
        sql.push_back("ALTER PUBLICATION " + name + " ADD TABLE " +
                      tables_clause("add_tables") + ";");
      }
      if (in.body.contains("drop_tables")) {
        sql.push_back("ALTER PUBLICATION " + name + " DROP TABLE " +
                      tables_clause("drop_tables") + ";");
      }
      if (in.body.contains("operations")) {
        std::vector<std::string> ops;
        for (const auto& o : in.body["operations"]) ops.push_back(o.get<std::string>());
        sql.push_back("ALTER PUBLICATION " + name + " SET (publish = '" +
                      detail::join(ops, ", ") + "');");
      }
      step.sql = std::move(sql);
      step.lock = "ShareUpdateExclusiveLock on each table added or removed";
      step.why = "catalog-only on this server";
      plan.warnings.push_back(
          "a subscriber does not pick up a publication change by itself: it "
          "needs ALTER SUBSCRIPTION ... REFRESH PUBLICATION, run THERE. Until "
          "that happens the two sides disagree about what is replicated, with "
          "no error on either.");
      return;
    }

    case IntentKind::kDropPublication:
      step.sql.push_back("DROP PUBLICATION " + name + ";");
      step.lock = "ShareUpdateExclusiveLock on the published tables";
      step.why = "the publication stops sending at commit";
      plan.warnings.push_back(
          "any subscriber to this publication stops receiving changes and does "
          "NOT error -- it simply falls behind, silently and permanently, until "
          "someone looks at the subscriber.");
      return;

    case IntentKind::kCreateSubscription: {
      const bool connect = in.body.value("connect", true);
      // The credential is deliberately not in the spec -- see spec.h, where a
      // password is refused rather than redacted. That leaves an out-of-band
      // step, so it is RECORDED as one rather than left as advice: a skill can
      // provision the service file, or open a ticket, or send the mail, and
      // check afterwards whether it worked.
      const auto conn = in.body.value("connection", "");
      const bool by_service = conn.find("service=") != std::string::npos;
      detail::require_out_of_band(
          plan, "credential", in.body.value("name", ""),
          "this server, as the postgres OS user",
          by_service
              ? "the connection service named in \"" + conn +
                    "\" must exist in pg_service.conf with a password for the "
                    "publisher"
              : "the role in \"" + conn +
                    "\" must have a password in ~/.pgpass on this host, or the "
                    "subscription cannot connect -- pg_laswell will not carry "
                    "the credential, because everything it runs is stored "
                    "verbatim in the ledger",
          "SELECT srsubstate FROM pg_subscription_rel, or check that "
          "pg_stat_subscription reports a worker for this subscription",
          /*blocking=*/connect);
      std::vector<std::string> pubs;
      for (const auto& p : in.body.value("publications", json::array())) {
        pubs.push_back(detail::quote_identifier(p.get<std::string>()));
      }
      std::vector<std::string> opts;
      if (!connect) opts.push_back("connect = false");
      if (in.body.contains("enabled")) {
        opts.push_back(std::string("enabled = ") +
                       (in.body.value("enabled", true) ? "true" : "false"));
      }
      if (in.body.contains("slot_name")) {
        opts.push_back("slot_name = " +
                       detail::quote_literal(in.body.value("slot_name", "")));
      }
      step.sql.push_back(
          "CREATE SUBSCRIPTION " + name + " CONNECTION " +
          detail::quote_literal(in.body.value("connection", "")) +
          " PUBLICATION " + detail::join(pubs, ", ") +
          (opts.empty() ? "" : " WITH (" + detail::join(opts, ", ") + ")") + ";");
      if (connect) {
        // Measured: it cannot run inside a transaction block. Same shape as
        // CREATE INDEX CONCURRENTLY and DETACH ... CONCURRENTLY.
        step.txn_class = TxnClass::kForbidden;
        step.own_transaction = true;
        step.lock = "no lock here; it connects to the publisher and creates a "
                    "replication slot THERE";
        step.why =
            "measured on 18.6: CREATE SUBSCRIPTION with create_slot cannot run "
            "inside a transaction block, so this step owns its own and cannot "
            "be rolled back with its neighbours";
        plan.warnings.push_back(
            "this connects to the publisher and creates a replication slot on "
            "IT. The slot then holds WAL on the publisher until this "
            "subscription consumes it -- an inactive slot is the most common "
            "cause of a publisher filling its disk, and nothing on this server "
            "will report it. pg_licht replicationSlots, pointed at the "
            "publisher, is the reading that would.");
        detail::require_out_of_band(
            plan, "monitor", in.body.value("name", ""), "the publisher",
            "the replication slot this creates must be consumed or dropped; "
            "while it exists and is inactive the publisher retains every WAL "
            "segment it has not sent",
            "pg_licht replicationSlots against the publisher, or SELECT "
            "slot_name, active, pg_size_pretty(pg_wal_lsn_diff("
            "pg_current_wal_lsn(), restart_lsn)) FROM pg_replication_slots",
            /*blocking=*/false);
      } else {
        step.lock = "no lock; nothing is contacted";
        step.why =
            "connect = false creates the catalog entry only, so it runs inside "
            "a transaction -- measured, the connecting form does not";
        plan.warnings.push_back(
            "connect = false leaves the subscription NOT connected: the "
            "replication slot must be created on the publisher by hand, then "
            "the subscription enabled and refreshed. PostgreSQL says so as a "
            "NOTICE, which is easy to miss in a migration log.");
      }
      return;
    }

    case IntentKind::kAlterSubscription: {
      std::vector<std::string> sql;
      if (in.body.contains("connection")) {
        sql.push_back("ALTER SUBSCRIPTION " + name + " CONNECTION " +
                      detail::quote_literal(in.body.value("connection", "")) + ";");
      }
      if (in.body.contains("publications")) {
        std::vector<std::string> pubs;
        for (const auto& p : in.body["publications"]) {
          pubs.push_back(detail::quote_identifier(p.get<std::string>()));
        }
        sql.push_back("ALTER SUBSCRIPTION " + name + " SET PUBLICATION " +
                      detail::join(pubs, ", ") + ";");
      }
      if (in.body.value("refresh", false)) {
        sql.push_back("ALTER SUBSCRIPTION " + name + " REFRESH PUBLICATION;");
      }
      if (in.body.contains("enabled")) {
        sql.push_back("ALTER SUBSCRIPTION " + name +
                      (in.body.value("enabled", true) ? " ENABLE;" : " DISABLE;"));
      }
      step.sql = std::move(sql);
      step.lock = "no table lock";
      step.why = "altering a subscription changes what this server asks for";
      if (in.body.contains("enabled") && !in.body.value("enabled", true)) {
        plan.warnings.push_back(
            "disabling a subscription does NOT drop its replication slot, so "
            "the publisher keeps every WAL segment this subscription has not "
            "consumed -- indefinitely, until it is enabled again or the slot is "
            "dropped there. That is a disk-full on the publisher, caused from "
            "here.");
      }
      return;
    }

    case IntentKind::kDropSubscription:
      step.sql.push_back("DROP SUBSCRIPTION " + name + ";");
      step.txn_class = TxnClass::kForbidden;
      step.own_transaction = true;
      step.lock = "no table lock; it contacts the publisher to drop the slot";
      step.why =
          "dropping a subscription that still has a slot must contact the "
          "publisher to remove it, which cannot happen inside a transaction "
          "block";
      plan.warnings.push_back(
          "if the publisher is unreachable this fails, and the fix is to "
          "disable the subscription, SET (slot_name = NONE), then drop -- which "
          "leaves the slot ORPHANED on the publisher, still holding WAL. That "
          "is a deliberate choice with a cost, not a workaround, and it has to "
          "be made on the publisher afterwards.");
      return;

    default: break;
  }
}

// The remaining one-offs. None has a lock to weigh or a scan to avoid; each is
// here because a repository that cannot express it describes a database that
// does not exist, and each carries what its statement does not say.
inline void plan_final_kinds(const Intent& in, const Observations& obs,
                             Plan& plan, std::vector<Step>& out) {
  Step step;
  step.kind = in.kind_name;
  struct Emit { std::vector<Step>& o; Step& s; ~Emit() { o.push_back(s); } } emit{out, step};
  step.txn_class = TxnClass::kRequired;

  const auto ot = in.body.value("object_type", "");
  const auto schema = in.body.value("schema", "");
  const auto name = in.body.value("name", "");

  switch (in.kind) {
    case IntentKind::kAlterObject: {
      const std::string target = schema.empty() ? name : schema + "." + name;
      std::vector<std::string> sql;
      if (in.body.contains("to")) {
        sql.push_back("ALTER " + ot + " " + target + " RENAME TO " +
                      detail::quote_identifier(in.body.value("to", "")) + ";");
      }
      if (in.body.contains("owner")) {
        sql.push_back("ALTER " + ot + " " + target + " OWNER TO " +
                      detail::quote_identifier(in.body.value("owner", "")) + ";");
      }
      if (in.body.contains("set_schema")) {
        sql.push_back("ALTER " + ot + " " + target + " SET SCHEMA " +
                      detail::quote_identifier(in.body.value("set_schema", "")) + ";");
      }
      step.sql = std::move(sql);
      step.lock = "AccessExclusiveLock on the object";
      step.why = "catalog-only";
      return;
    }

    case IntentKind::kCreateTableAs: {
      const auto qualified = in.qualified_table();
      const auto& t = obs.table(qualified);
      if (t.value("exists", false)) {
        step.action = Action::kSatisfied;
        step.why = qualified + " already exists";
        return;
      }
      const bool with_data = in.body.value("with_data", true);
      step.sql.push_back(std::string("CREATE ") +
                         (in.body.value("unlogged", false) ? "UNLOGGED " : "") +
                         "TABLE " + qualified + " AS " +
                         in.body.value("definition", "") +
                         (with_data ? " WITH DATA;" : " WITH NO DATA;"));
      step.sql.push_back("COMMENT ON TABLE " + qualified + " IS " +
                         detail::quote_literal(in.body.value("comment", "")) + ";");
      step.own_transaction = with_data;
      step.lock = with_data
          ? "AccessShareLock on every relation the query reads, for as long as "
            "the query takes"
          : "no lock beyond the catalog";
      step.why = with_data
          ? "the query runs in full at creation, so this costs whatever it "
            "costs and holds read locks throughout"
          : "WITH NO DATA creates the shape and nothing else";
      plan.warnings.push_back(
          "CREATE TABLE AS copies the query's result and its column types, and "
          "nothing else: no primary key, no indexes, no constraints, no "
          "defaults, no identity. " + qualified +
          " is a heap of rows until later intents give it a shape.");
      return;
    }

    case IntentKind::kImportForeignSchema: {
      std::string sql = "IMPORT FOREIGN SCHEMA " +
                        detail::quote_identifier(in.body.value("remote_schema", ""));
      if (in.body.contains("limit_to")) {
        std::vector<std::string> ts;
        for (const auto& t : in.body["limit_to"]) ts.push_back(t.get<std::string>());
        sql += " LIMIT TO (" + detail::join(ts, ", ") + ")";
      } else if (in.body.contains("except")) {
        std::vector<std::string> ts;
        for (const auto& t : in.body["except"]) ts.push_back(t.get<std::string>());
        sql += " EXCEPT (" + detail::join(ts, ", ") + ")";
      }
      sql += " FROM SERVER " + detail::quote_identifier(in.body.value("server", "")) +
             " INTO " + detail::quote_identifier(schema) + ";";
      step.sql.push_back(sql);
      step.own_transaction = true;
      step.lock = "no lock here; it queries the REMOTE server's catalog";
      step.why = "importing contacts the foreign server and creates one foreign "
                 "table per remote table it finds";
      detail::require_out_of_band(
          plan, "reachability", in.body.value("server", ""),
          "the foreign server",
          "the foreign server must be reachable with valid credentials when "
          "this runs, and the remote schema must contain what the spec assumes "
          "-- neither is visible to pg_laswell before it starts",
          "SELECT 1 FROM information_schema.foreign_tables WHERE "
          "foreign_table_schema = " +
              detail::quote_literal(in.body.value("schema", "")),
          /*blocking=*/true);
      plan.warnings.push_back(
          "what this creates depends on the REMOTE schema at the moment it "
          "runs, so the same signed spec produces different objects on "
          "different days. That is the one place in this repository where the "
          "outcome is not determined by the spec, and the ledger will record "
          "the statement rather than the tables it made. Prefer explicit "
          "create_object FOREIGN TABLE intents where the set matters.");
      return;
    }

    case IntentKind::kSecurityLabel: {
      const std::string target = schema.empty() ? name : schema + "." + name;
      const bool removing = !in.body.contains("label");
      step.sql.push_back(
          "SECURITY LABEL FOR " +
          detail::quote_literal(in.body.value("provider", "")) + " ON " + ot +
          " " + target + " IS " +
          (removing ? "NULL" : detail::quote_literal(in.body.value("label", ""))) + ";");
      step.lock = "AccessShareLock";
      step.why = "a security label is metadata this server stores and does not "
                 "interpret";
      plan.warnings.push_back(
          "a security label means nothing without a label provider loaded to "
          "enforce it -- typically an extension such as sepgsql. If none is "
          "loaded the statement fails; if one is, the label changes what that "
          "provider permits, which is not visible in the catalog.");
      return;
    }

    case IntentKind::kAlterDefaultPrivileges: {
      const bool granting = in.body.value("grant", true);
      std::vector<std::string> privs, roles;
      for (const auto& p : in.body.value("privileges", json::array())) {
        privs.push_back(p.get<std::string>());
      }
      for (const auto& r : in.body.value(granting ? "to" : "from", json::array())) {
        const auto rn = r.get<std::string>();
        roles.push_back(rn == "PUBLIC" ? "PUBLIC" : detail::quote_identifier(rn));
      }
      std::string sql = "ALTER DEFAULT PRIVILEGES";
      if (in.body.contains("for_role")) {
        sql += " FOR ROLE " +
               detail::quote_identifier(in.body.value("for_role", ""));
      }
      if (!schema.empty()) sql += " IN SCHEMA " + detail::quote_identifier(schema);
      sql += std::string(granting ? " GRANT " : " REVOKE ") +
             detail::join(privs, ", ") + " ON " + ot +
             (granting ? " TO " : " FROM ") + detail::join(roles, ", ") + ";";
      step.sql.push_back(sql);
      step.lock = "no lock on any table";
      step.why = "this governs objects created LATER and changes nothing that "
                 "exists now";
      plan.warnings.push_back(
          "default privileges apply only to objects created by the role they "
          "are recorded FOR -- by default the role running this statement, not "
          "every role. A table created later by a different role gets nothing "
          "from this, which is the single most common way an ALTER DEFAULT "
          "PRIVILEGES appears not to work. Name for_role deliberately.");
      plan.warnings.push_back(
          "nothing that already exists is changed. Objects created before this "
          "commits keep whatever privileges they have -- a grant intent is what "
          "fixes those.");
      return;
    }

    default: break;
  }
}

// The long tail: object types whose only decisions are existence and what
// depends on them. Two kinds rather than forty, and they still refuse a drop
// that something depends on -- which is the safeguard, not the statement.
inline void plan_generic_object(const Intent& in, const Observations& obs,
                                Plan& plan, std::vector<Step>& out) {
  Step step;
  step.kind = in.kind_name;
  struct Emit { std::vector<Step>& o; Step& s; ~Emit() { o.push_back(s); } } emit{out, step};
  step.txn_class = TxnClass::kRequired;
  (void)obs;

  const auto ot = in.body.value("object_type", "");
  const auto schema = in.body.value("schema", "");
  const auto raw_name = in.body.value("name", "");
  // Some of these name themselves in ways an identifier quote would break --
  // an operator is "+(int,int)", a cast is "(int AS text)" -- so the name is
  // taken as written. It is inside a signed specification, which is the same
  // trust boundary the definition below sits behind.
  const auto name = schema.empty() ? raw_name : schema + "." + raw_name;

  if (in.kind == IntentKind::kCreateObject) {
    step.sql.push_back("CREATE " + ot + " " + name + " " +
                       in.body.value("definition", "") + ";");
    if (in.body.contains("comment")) {
      step.sql.push_back("COMMENT ON " + ot + " " + name + " IS " +
                         detail::quote_literal(in.body.value("comment", "")) + ";");
    }
    step.lock = "no lock on any existing object";
    step.why = ot +
        " has no planning decision beyond whether it is already there: no "
        "lock to weigh, no scan to avoid, no rewrite to predict. It is here so "
        "the repository can express it, not because there is a choice to make";
  } else {
    step.sql.push_back("DROP " + ot + " " + name + ";");
    step.lock = "AccessExclusiveLock on the object";
    step.why = "dropping " + ot + " " + name;
    plan.warnings.push_back(
        "pg_laswell does not read dependants for " + ot +
        " as it does for a type, function or sequence, so this drop is not "
        "pre-checked: PostgreSQL will refuse it if something depends on it, at "
        "execution rather than at plan time. No CASCADE is emitted either way.");
  }
}

// --- materialized views, extended statistics, rules ------------------------
inline void plan_relation_extras(const Intent& in, const Observations& obs,
                                 Plan& plan, std::vector<Step>& out) {
  Step step;
  step.kind = in.kind_name;
  struct Emit { std::vector<Step>& o; Step& s; ~Emit() { o.push_back(s); } } emit{out, step};
  step.txn_class = TxnClass::kRequired;

  const auto schema = in.body.value("schema", "");
  const auto name = in.body.value("name", "");
  const auto qualified_obj = schema + "." + name;
  const auto qualified_tbl = in.qualified_table();

  switch (in.kind) {
    case IntentKind::kCreateMaterializedView: {
      const auto& t = obs.table(qualified_obj);
      if (t.value("exists", false)) {
        step.action = Action::kSatisfied;
        step.why = qualified_obj + " already exists";
        plan.warnings.push_back(
            qualified_obj +
            " already exists, so this is reported satisfied WITHOUT comparing "
            "its query to the spec. replace_view is the intent that changes an "
            "existing one, and it says what a matview rebuild costs.");
        return;
      }
      const bool with_data = in.body.value("with_data", true);
      step.sql.push_back("CREATE MATERIALIZED VIEW " + qualified_obj + " AS " +
                         in.body.value("definition", "") +
                         (with_data ? " WITH DATA;" : " WITH NO DATA;"));
      step.sql.push_back("COMMENT ON MATERIALIZED VIEW " + qualified_obj + " IS " +
                         detail::quote_literal(in.body.value("comment", "")) + ";");
      step.own_transaction = with_data;
      step.lock = with_data
          ? "AccessShareLock on every relation the query reads, for as long as "
            "it takes to run"
          : "no lock beyond the catalog -- nothing is read";
      step.why = with_data
          ? "WITH DATA runs the query in full at creation, so this step costs "
            "whatever that query costs and holds read locks throughout"
          : "WITH NO DATA creates the definition and nothing else, so it is "
            "instant";
      if (!with_data) {
        plan.warnings.push_back(
            qualified_obj +
            " is created WITH NO DATA, so it is unreadable until REFRESH -- a "
            "query against it fails outright rather than returning nothing. "
            "REFRESH is maintenance and not a migration this tool runs, so "
            "something else must do it before anything reads the view.");
      }
      plan.warnings.push_back(
          "a materialized view does not track its sources: " + qualified_obj +
          " is a snapshot, and it is stale from the moment the next write lands "
          "until something refreshes it. Nothing in the database will say so.");
      return;
    }

    case IntentKind::kCreateStatistics: {
      const auto& t = obs.table(qualified_tbl);
      if (!t.value("exists", false)) {
        step.action = Action::kConflict;
        step.why = qualified_tbl + " does not exist";
        plan.conflicts.push_back(step.why);
        return;
      }
      std::vector<std::string> cols;
      for (const auto& c : in.body.value("columns", json::array())) {
        cols.push_back(detail::quote_identifier(c.get<std::string>()));
      }
      const json columns = t.value("columns", json::object());
      for (const auto& c : in.body.value("columns", json::array())) {
        if (!columns.contains(c.get<std::string>())) {
          step.action = Action::kConflict;
          step.why = qualified_tbl + "." + c.get<std::string>() + " does not exist";
          plan.conflicts.push_back(step.why);
          return;
        }
      }
      std::string kinds;
      if (in.body.contains("kinds")) {
        std::vector<std::string> ks;
        for (const auto& k : in.body["kinds"]) ks.push_back(k.get<std::string>());
        kinds = " (" + detail::join(ks, ", ") + ")";
      }
      step.sql.push_back("CREATE STATISTICS " + qualified_obj + kinds + " ON " +
                         detail::join(cols, ", ") + " FROM " + qualified_tbl + ";");
      if (in.body.contains("comment")) {
        step.sql.push_back("COMMENT ON STATISTICS " + qualified_obj + " IS " +
                           detail::quote_literal(in.body.value("comment", "")) + ";");
      }
      step.lock = "ShareUpdateExclusiveLock on " + qualified_tbl +
                  " -- does NOT block reads or writes";
      step.why =
          "creating extended statistics records what to collect; it collects "
          "nothing now and scans nothing";
      plan.warnings.push_back(
          "extended statistics do nothing until the table is ANALYZEd -- until "
          "then the planner's estimates are exactly what they were. ANALYZE is "
          "maintenance and not something this tool schedules, so a plan that "
          "depends on the new estimates should not assume they exist yet.");
      return;
    }

    case IntentKind::kCreateRule: {
      const auto& t = obs.table(qualified_tbl);
      if (!t.value("exists", false)) {
        step.action = Action::kConflict;
        step.why = qualified_tbl + " does not exist";
        plan.conflicts.push_back(step.why);
        return;
      }
      const auto event = in.body.value("event", "");
      std::string sql = "CREATE RULE " + detail::quote_identifier(name) +
                        " AS ON " + event + " TO " + qualified_tbl;
      if (in.body.contains("where")) sql += " WHERE " + in.body.value("where", "");
      // NOTHING is not a command and must not be parenthesised: the
      // parentheses in DO [INSTEAD] (...) are for a command LIST, so
      // "DO INSTEAD (NOTHING)" is a syntax error. Found by the conformance
      // suite, which is the only test that runs what the planner emits.
      auto action = in.body.value("action", "");
      std::string upper = action;
      for (auto& c : upper) c = static_cast<char>(std::toupper(c));
      while (!upper.empty() && upper.back() == ' ') upper.pop_back();
      sql += " DO " + std::string(in.body.value("instead", false) ? "INSTEAD " : "") +
             (upper == "NOTHING" ? "NOTHING" : "(" + action + ")") + ";";
      step.sql.push_back(sql);
      step.lock = "AccessExclusiveLock on " + qualified_tbl;
      step.why = "a rule rewrites queries at parse time, so it is a catalog "
                 "change with no scan";
      // Rules deserve the strongest warning in the tool after RLS: they change
      // what a statement MEANS, invisibly, for everyone.
      plan.warnings.push_back(
          "a rule REWRITES matching queries before they run, for every session "
          "and with nothing in the query text to say so. An " + event +
          " against " + qualified_tbl +
          " will no longer do what it appears to do. Rules interact badly with "
          "RETURNING, with statement-level counts and with triggers, and the "
          "PostgreSQL documentation itself recommends a trigger instead for "
          "everything except a view's _RETURN rule. Consider create_trigger.");
      if (in.body.value("instead", false)) {
        plan.warnings.push_back(
            "DO INSTEAD means the original statement does not run at all. A "
            "caller's INSERT reports success having inserted nothing, unless "
            "the rule's own action happens to do it.");
      }
      return;
    }

    case IntentKind::kDropRule: {
      step.sql.push_back("DROP RULE " + detail::quote_identifier(name) + " ON " +
                         qualified_tbl + ";");
      step.lock = "AccessExclusiveLock on " + qualified_tbl;
      step.why = "dropping a rule restores what the statements it matched "
                 "actually mean";
      return;
    }

    case IntentKind::kDropStatistics: {
      step.sql.push_back("DROP STATISTICS " + qualified_obj + ";");
      step.lock = "AccessExclusiveLock on the statistics object";
      step.why = "the planner's estimates revert to per-column ones at the next "
                 "plan, which may change query plans immediately";
      return;
    }

    default: break;
  }
}

// --- identity, generated columns, storage and physical layout --------------
//
// Measured by relfilenode on 300 000 rows (spike S20, 18.6). Three of these
// rewrite the table and the rest are catalog-only, and predicting that is the
// entire job of this batch:
//
//   SET UNLOGGED / SET LOGGED       REWRITE -- and SET LOGGED writes the whole
//                                   table to WAL as it goes
//   ALTER COLUMN SET EXPRESSION     REWRITE
//   identity add/drop, DROP EXPRESSION, SET STATISTICS / STORAGE /
//   COMPRESSION, storage parameters, REPLICA IDENTITY   no rewrite
inline void plan_physical(const Intent& in, const Observations& obs, Plan& plan,
                          std::vector<Step>& out) {
  const auto qualified = in.qualified_table();
  const auto& t = obs.table(qualified);
  const auto column = in.body.value("column", "");
  const long long size = t.value("size_estimate", 0LL);

  auto emit = [&](std::vector<std::string> sql, const std::string& lock,
                  const std::string& why, bool own = false) {
    Step s;
    s.kind = in.kind_name;
    s.txn_class = TxnClass::kRequired;
    s.sql = std::move(sql);
    s.lock = lock;
    s.why = why;
    s.own_transaction = own;
    out.push_back(std::move(s));
  };
  auto refuse = [&](const std::string& why, const std::string& detail) {
    Step s;
    s.kind = in.kind_name;
    s.action = Action::kConflict;
    s.why = why;
    plan.conflicts.push_back(detail);
    out.push_back(std::move(s));
  };

  if (!t.value("exists", false)) {
    refuse(qualified + " does not exist", qualified + " does not exist");
    return;
  }
  const json columns = t.value("columns", json::object());
  if (!column.empty() && !columns.contains(column)) {
    refuse(qualified + "." + column + " does not exist",
           qualified + "." + column + " does not exist");
    return;
  }

  switch (in.kind) {
    case IntentKind::kSetIdentity: {
      const bool adding = in.body.contains("identity");
      if (adding) {
        // Measured: PostgreSQL refuses outright unless the column is already
        // NOT NULL -- "must be declared NOT NULL before identity can be added".
        // The set_not_null recipe does that scan under a lock the application
        // survives, so it is run first rather than letting the author discover
        // the prerequisite from an error.
        if (!columns[column].value("not_null", false)) {
          Intent nn;
          nn.kind = IntentKind::kSetNotNull;
          nn.kind_name = "set_not_null";
          nn.ordinal = in.ordinal;
          nn.body = json{{"schema", in.body.value("schema", "")},
                         {"table", in.body.value("table", "")},
                         {"column", column}};
          plan_set_not_null(nn, obs, plan, out);
          plan.warnings.push_back(
              qualified + "." + column +
              " must be NOT NULL before an identity can be added -- measured, "
              "PostgreSQL refuses otherwise. The plan runs the NOT VALID check "
              "recipe first, which does that scan under "
              "ShareUpdateExclusiveLock instead of blocking the table.");
        }
        emit({"ALTER TABLE " + qualified + " ALTER COLUMN " +
              detail::quote_identifier(column) + " ADD GENERATED " +
              in.body.value("identity", "") + " AS IDENTITY;"},
             "AccessExclusiveLock on " + qualified + ", but no rewrite",
             "measured: relfilenode unchanged, so this is catalog-only plus a "
             "sequence, whatever the table's size",
             /*own=*/true);
        if (in.body.value("identity", "") == "BY DEFAULT") {
          plan.warnings.push_back(
              "BY DEFAULT lets a caller supply the value, so inserts that pass "
              "one do not advance the sequence -- which is how a sequence ends "
              "up behind its column and every later insert collides. ALWAYS "
              "refuses such a value.");
        }
        // A new identity starts at 1 regardless of what is already there.
        plan.warnings.push_back(
            "the identity sequence starts from 1, not from the largest value "
            "already in " + qualified + "." + column +
            ". If the column holds data, follow this with an alter_sequence "
            "restart above the maximum, or the first insert collides.");
      } else {
        emit({"ALTER TABLE " + qualified + " ALTER COLUMN " +
              detail::quote_identifier(column) + " DROP IDENTITY;"},
             "AccessExclusiveLock on " + qualified + ", no rewrite",
             "measured: catalog-only. The backing sequence goes with it");
      }
      return;
    }

    case IntentKind::kDropExpression:
      emit({"ALTER TABLE " + qualified + " ALTER COLUMN " +
            detail::quote_identifier(column) + " DROP EXPRESSION;"},
           "AccessExclusiveLock on " + qualified + ", no rewrite",
           "measured: catalog-only. The values already computed stay in the "
           "table and simply stop being maintained");
      plan.warnings.push_back(
          qualified + "." + column +
          " keeps the values it has but stops being recomputed, so it becomes "
          "an ordinary column that silently drifts from whatever it was derived "
          "from. Nothing will report the drift.");
      return;

    case IntentKind::kSetColumnOptions: {
      std::vector<std::string> sql;
      const std::string head = "ALTER TABLE " + qualified + " ALTER COLUMN " +
                               detail::quote_identifier(column) + " SET ";
      if (in.body.contains("statistics")) {
        sql.push_back(head + "STATISTICS " +
                      std::to_string(in.body.value("statistics", 100)) + ";");
      }
      if (in.body.contains("storage")) {
        sql.push_back(head + "STORAGE " + in.body.value("storage", "") + ";");
      }
      if (in.body.contains("compression")) {
        sql.push_back(head + "COMPRESSION " + in.body.value("compression", "") + ";");
      }
      emit(std::move(sql), "AccessExclusiveLock on " + qualified + ", no rewrite",
           "measured: all three are catalog-only");
      if (in.body.contains("storage") || in.body.contains("compression")) {
        plan.warnings.push_back(
            "storage and compression apply to values written AFTER this "
            "commits. Everything already in " + qualified +
            " keeps its current representation indefinitely, so the setting "
            "and the table disagree until the rows are rewritten -- which "
            "nothing here does.");
      }
      if (in.body.contains("statistics")) {
        plan.warnings.push_back(
            "a new statistics target does nothing until the column is analysed. "
            "Run ANALYZE afterwards, outside this migration: it is maintenance, "
            "and not something this tool schedules.");
      }
      return;
    }

    case IntentKind::kSetTableOptions: {
      std::vector<std::string> sql;
      const json options = in.body.value("options", json::object());
      if (!options.empty()) {
        std::vector<std::string> pairs;
        for (const auto& [k, v] : options.items()) {
          pairs.push_back(k + " = " + (v.is_string() ? v.get<std::string>() : v.dump()));
        }
        sql.push_back("ALTER TABLE " + qualified + " SET (" +
                      detail::join(pairs, ", ") + ");");
      }
      if (in.body.contains("reset")) {
        std::vector<std::string> names;
        for (const auto& r : in.body["reset"]) names.push_back(r.get<std::string>());
        sql.push_back("ALTER TABLE " + qualified + " RESET (" +
                      detail::join(names, ", ") + ");");
      }
      emit(std::move(sql), "AccessExclusiveLock on " + qualified + ", no rewrite",
           "measured: setting a storage parameter such as fillfactor is "
           "catalog-only; it changes how FUTURE writes lay out pages and "
           "reorganises nothing already there");
      return;
    }

    case IntentKind::kSetLogged: {
      const bool logged = in.body.value("logged", true);
      emit({"ALTER TABLE " + qualified + (logged ? " SET LOGGED;" : " SET UNLOGGED;")},
           "AccessExclusiveLock on " + qualified + " for the whole rewrite",
           std::string("measured: this REWRITES the table -- relfilenode "
                       "changes in both directions -- so ") +
               detail::human_bytes(size) +
               " is copied under an exclusive lock" +
               (logged ? ", and every byte of it is written to WAL as it goes"
                       : ""),
           /*own=*/true);
      if (!logged) {
        plan.warnings.push_back(
            "UNLOGGED means " + qualified +
            " is not written to WAL: it is not replicated to any standby, it is "
            "not in any physical backup taken from one, and its contents are "
            "TRUNCATED after a crash. That is a durability decision, not a "
            "performance setting.");
      } else {
        plan.warnings.push_back(
            "SET LOGGED writes the entire table to WAL as it rewrites it, so a "
            "replica must receive " + detail::human_bytes(size) +
            " of WAL and archiving must absorb it. On a large table that is "
            "the dominant cost, and it lands on the replication link rather "
            "than on this connection.");
      }
      return;
    }

    case IntentKind::kSetTablespace:
      emit({"ALTER TABLE " + qualified + " SET TABLESPACE " +
            detail::quote_identifier(in.body.value("tablespace", "")) + ";"},
           "AccessExclusiveLock on " + qualified + " for the whole move",
           "moving a table copies every page to the new location under an "
           "exclusive lock: " + detail::human_bytes(size) + " to write",
           /*own=*/true);
      plan.warnings.push_back(
          "the move needs " + detail::human_bytes(size) +
          " free in the destination tablespace WHILE the source still holds "
          "it -- both at once, since the old files are removed only at commit. "
          "pg_laswell cannot see either filesystem's free space; that is a "
          "check to make before starting, not one it can make for you.");
      detail::require_out_of_band(
          plan, "capacity", in.body.value("tablespace", ""),
          "the database host's filesystem",
          "at least " + detail::human_bytes(size) +
              " must be free in the destination tablespace at the same time as "
              "the source still holds its copy",
          "df on the tablespace directory from pg_tablespace_location(), or "
          "pg_licht hostCapacity",
          /*blocking=*/true);
      return;

    case IntentKind::kSetAccessMethod:
      emit({"ALTER TABLE " + qualified + " SET ACCESS METHOD " +
            detail::quote_identifier(in.body.value("method", "")) + ";"},
           "AccessExclusiveLock on " + qualified + " for the whole rewrite",
           "changing access method rewrites the table in the new method's "
           "format: " + detail::human_bytes(size) + " under an exclusive lock",
           /*own=*/true);
      plan.warnings.push_back(
          "measured only for a no-op change (heap to heap, which does not "
          "rewrite). A real change of access method does rewrite, and this plan "
          "assumes it will rather than claiming a measurement it does not have.");
      return;

    case IntentKind::kSetReplicaIdentity: {
      const auto form = in.body.value("identity", "DEFAULT");
      std::string sql = "ALTER TABLE " + qualified + " REPLICA IDENTITY " + form;
      if (form == "USING INDEX") {
        sql += " " + detail::quote_identifier(in.body.value("index", ""));
      }
      emit({sql + ";"}, "AccessExclusiveLock on " + qualified + ", no rewrite",
           "measured: catalog-only whatever the table's size");
      // This is the quietest dangerous change in the tool: nothing local
      // changes at all, and a subscriber starts behaving differently.
      if (form == "NOTHING") {
        plan.warnings.push_back(
            "REPLICA IDENTITY NOTHING means UPDATE and DELETE on " + qualified +
            " are replicated with no way to identify the row, so a logical "
            "subscriber cannot apply them -- it errors, or with "
            "publish_via_partition_root it silently diverges. Nothing on THIS "
            "server will report it.");
      } else if (form == "FULL") {
        plan.warnings.push_back(
            "REPLICA IDENTITY FULL puts every column of the old row into WAL "
            "for each UPDATE and DELETE, and a subscriber matches rows by "
            "scanning. It is correct and it is expensive on both sides, "
            "proportionally to the row's width.");
      } else {
        plan.warnings.push_back(
            "replica identity decides what logical replication can identify a "
            "row by. Changing it changes what subscribers receive, with no "
            "error and nothing visible on this server -- check the subscriber, "
            "not this database.");
      }
      return;
    }

    case IntentKind::kClusterOn: {
      const bool setting = in.body.contains("index");
      emit({"ALTER TABLE " + qualified +
            (setting ? " CLUSTER ON " +
                           detail::quote_identifier(in.body.value("index", "")) + ";"
                     : " SET WITHOUT CLUSTER;")},
           "AccessExclusiveLock on " + qualified + ", no rewrite",
           "this only RECORDS which index a future CLUSTER would use; it does "
           "not reorder anything now");
      if (setting) {
        plan.warnings.push_back(
            "CLUSTER ON reorders nothing by itself -- it marks the index for a "
            "later CLUSTER command, which is maintenance and is not a "
            "migration this tool runs. Nothing about " + qualified +
            "'s physical order changes at this commit.");
      }
      return;
    }

    default: break;
  }
}

// --- the ALTER forms for objects we can already create ---------------------
//
// Measured (spike S19, 18.6). One of these is a recipe and the rest are
// statements, and knowing which is the whole job:
//
//   ALTER DOMAIN ADD CONSTRAINT   37.377 ms on 1.5M values -- it SCANS
//   ... NOT VALID                  0.502 ms
//   ... VALIDATE afterwards       37.360 ms
//
// So alter_domain splits, exactly as add_check_constraint does. The blast
// radius is wider than a table constraint's: the scan covers every column of
// that type in every table, not one table, and grows with each new use of the
// domain.
//
//   ALTER POLICY                  AccessExclusiveLock, same as CREATE POLICY
//   ALTER COLUMN SET DEFAULT      relfilenode unchanged -- catalog-only
namespace detail {
// Defined below with the object planners; declared here because alter_function
// needs the same signature rendering drop_function does, and the two must not
// disagree about what identifies a function.
inline std::string function_signature(const Intent& in);
}  // namespace detail

inline void plan_alter_misc(const Intent& in, const Observations& obs,
                            Plan& plan, std::vector<Step>& out) {
  const auto schema = in.body.value("schema", "");
  const auto name = in.body.value("name", "");
  const auto qualified_obj = schema + "." + name;
  const auto qualified_tbl = in.qualified_table();

  auto emit = [&](TxnClass klass, std::vector<std::string> sql,
                  const std::string& lock, const std::string& why, bool own) {
    Step s;
    s.kind = in.kind_name;
    s.txn_class = klass;
    s.sql = std::move(sql);
    s.lock = lock;
    s.why = why;
    s.own_transaction = own;
    out.push_back(std::move(s));
  };
  auto verdict = [&](Action a, const std::string& why) {
    Step s;
    s.kind = in.kind_name;
    s.action = a;
    s.why = why;
    out.push_back(std::move(s));
  };
  auto refuse = [&](const std::string& why, const std::string& detail) {
    verdict(Action::kConflict, why);
    plan.conflicts.push_back(detail);
  };

  switch (in.kind) {
    case IntentKind::kAlterColumnDefault: {
      const auto& t = obs.table(qualified_tbl);
      const auto column = in.body.value("column", "");
      if (!t.value("exists", false)) {
        refuse(qualified_tbl + " does not exist", qualified_tbl + " does not exist");
        return;
      }
      const json columns = t.value("columns", json::object());
      if (!columns.contains(column)) {
        refuse(qualified_tbl + "." + column + " does not exist",
               qualified_tbl + "." + column + " does not exist");
        return;
      }
      const bool setting = in.body.contains("default");
      emit(TxnClass::kRequired,
           {"ALTER TABLE " + qualified_tbl + " ALTER COLUMN " +
            detail::quote_identifier(column) +
            (setting ? " SET DEFAULT " + in.body.value("default", "")
                     : " DROP DEFAULT") + ";"},
           "AccessExclusiveLock on " + qualified_tbl + ", but no scan and no rewrite",
           "measured on 18.6: relfilenode is unchanged, so this is catalog-only "
           "whatever the table's size",
           /*own=*/false);
      plan.warnings.push_back(
          "a default applies only to rows inserted AFTER this commits. Existing "
          "rows keep whatever they have, including nulls -- if they should carry "
          "the new value, that is a backfill intent, and it is not implied by "
          "this one.");
      return;
    }

    case IntentKind::kDropNotNull: {
      const auto& t = obs.table(qualified_tbl);
      const auto column = in.body.value("column", "");
      const json columns = t.value("columns", json::object());
      if (!t.value("exists", false) || !columns.contains(column)) {
        refuse(qualified_tbl + "." + column + " does not exist",
               qualified_tbl + "." + column + " does not exist");
        return;
      }
      if (!columns[column].value("not_null", false)) {
        verdict(Action::kSatisfied, qualified_tbl + "." + column + " is already nullable");
        return;
      }
      emit(TxnClass::kRequired,
           {"ALTER TABLE " + qualified_tbl + " ALTER COLUMN " +
            detail::quote_identifier(column) + " DROP NOT NULL;"},
           "AccessExclusiveLock on " + qualified_tbl + ", briefly and with no scan",
           "removing NOT NULL needs no verification -- there is nothing to check "
           "when the constraint is being relaxed",
           /*own=*/false);
      plan.warnings.push_back(
          "this widens what " + qualified_tbl + "." + column +
          " may contain, so it is a change to the DATA's shape and not only to "
          "what is enforced. Anything reading the column that has never had to "
          "handle a null now does.");
      return;
    }

    case IntentKind::kAlterSequence: {
      const auto& o = obs.object("sequence:" + qualified_obj);
      if (!o.value("exists", false)) {
        refuse("sequence " + qualified_obj + " does not exist",
               "sequence " + qualified_obj + " does not exist");
        return;
      }
      std::vector<std::string> sql;
      if (in.body.contains("to")) {
        sql.push_back("ALTER SEQUENCE " + qualified_obj + " RENAME TO " +
                      detail::quote_identifier(in.body.value("to", "")) + ";");
      }
      std::string alter;
      if (in.body.contains("increment")) {
        alter += " INCREMENT BY " + std::to_string(in.body.value("increment", 1));
      }
      if (in.body.contains("restart")) {
        alter += " RESTART WITH " + std::to_string(in.body.value("restart", 1));
      }
      if (in.body.contains("owned_by")) {
        alter += " OWNED BY " + in.body.value("owned_by", "");
      }
      if (!alter.empty()) sql.push_back("ALTER SEQUENCE " + qualified_obj + alter + ";");
      emit(TxnClass::kRequired, std::move(sql),
           "AccessExclusiveLock on the sequence only",
           "altering a sequence touches no table", /*own=*/false);
      if (in.body.contains("restart")) {
        plan.warnings.push_back(
            "RESTART sets the sequence back without checking what is already in "
            "the column it feeds. If rows exist at or above " +
            std::to_string(in.body.value("restart", 1)) +
            ", the next inserts collide with them -- and only a unique "
            "constraint will notice.");
      }
      return;
    }

    case IntentKind::kAlterSchema: {
      const auto& o = obs.object("schema:" + schema);
      if (!o.value("exists", false)) {
        refuse("schema " + schema + " does not exist", "schema " + schema + " does not exist");
        return;
      }
      std::vector<std::string> sql;
      if (in.body.contains("to")) {
        sql.push_back("ALTER SCHEMA " + detail::quote_identifier(schema) +
                      " RENAME TO " +
                      detail::quote_identifier(in.body.value("to", "")) + ";");
      }
      if (in.body.contains("owner")) {
        sql.push_back("ALTER SCHEMA " + detail::quote_identifier(schema) +
                      " OWNER TO " +
                      detail::quote_identifier(in.body.value("owner", "")) + ";");
      }
      emit(TxnClass::kRequired, std::move(sql), "AccessExclusiveLock on the schema",
           "catalog-only", /*own=*/false);
      if (in.body.contains("to")) {
        plan.warnings.push_back(
            "renaming schema " + schema +
            " moves every object inside it. Nothing in the database breaks -- "
            "dependencies are held by OID -- but every application search_path, "
            "every qualified query and every grant written against the old name "
            "stops matching, and none of that is visible from here.");
      }
      return;
    }

    case IntentKind::kAlterExtension: {
      const auto ext = in.body.value("name", "");
      const auto& o = obs.object("extension:" + ext);
      if (!o.value("exists", false)) {
        refuse("extension " + ext + " is not installed",
               "extension " + ext + " is not installed, so there is nothing to "
               "alter. create_extension is the intent for installing it.");
        return;
      }
      std::vector<std::string> sql;
      if (in.body.contains("version")) {
        const auto want = in.body.value("version", "");
        if (o.value("version", "") == want) {
          verdict(Action::kSatisfied, ext + " is already at version " + want);
          return;
        }
        sql.push_back("ALTER EXTENSION " + detail::quote_identifier(ext) +
                      " UPDATE TO " + detail::quote_literal(want) + ";");
        plan.warnings.push_back(
            "updating extension " + ext + " from " + o.value("version", "?") +
            " to " + want +
            " runs the extension's own migration scripts. What they do is not "
            "visible to pg_laswell, is not in this plan, and is generally NOT "
            "reversible -- there is usually no downgrade script. The ledger "
            "will record that the update ran, not what it did.");
      }
      if (in.body.contains("schema")) {
        sql.push_back("ALTER EXTENSION " + detail::quote_identifier(ext) +
                      " SET SCHEMA " +
                      detail::quote_identifier(in.body.value("schema", "")) + ";");
      }
      emit(TxnClass::kRequired, std::move(sql),
           "AccessExclusiveLock on every object the extension owns",
           "an extension update rewrites its own objects", /*own=*/true);
      return;
    }

    case IntentKind::kAlterDomain: {
      const auto& o = obs.object("type:" + qualified_obj);
      if (!o.value("exists", false)) {
        refuse("domain " + qualified_obj + " does not exist",
               "domain " + qualified_obj + " does not exist");
        return;
      }
      if (o.value("type_kind", "") != "domain") {
        refuse(qualified_obj + " is not a domain",
               qualified_obj + " is a " + o.value("type_kind", "type") +
                   ", not a domain. alter_domain applies only to domains.");
        return;
      }
      if (in.body.contains("add_check")) {
        const auto cname = in.body.value("constraint_name", "");
        // The recipe, for the same measured reason add_check_constraint has
        // one -- and it matters more here: the scan covers every column of this
        // type in every table, so it grows with each use of the domain.
        emit(TxnClass::kRequired,
             {"ALTER DOMAIN " + qualified_obj + " ADD CONSTRAINT " +
              detail::quote_identifier(cname) + " CHECK (" +
              in.body.value("add_check", "") + ") NOT VALID;"},
             "AccessExclusiveLock on the domain, briefly; NOT VALID means no scan",
             "step 1 of 2: measured on 18.6, adding the constraint NOT VALID "
             "took 0.5ms against 37ms for the same constraint validated -- and "
             "that 37ms was 1.5M values across two tables, growing with every "
             "column of this type anywhere in the database",
             /*own=*/true);
        emit(TxnClass::kRequired,
             {"ALTER DOMAIN " + qualified_obj + " VALIDATE CONSTRAINT " +
              detail::quote_identifier(cname) + ";"},
             "AccessExclusiveLock on the domain while every column of this type "
             "is scanned",
             "step 2 of 2: the scan happens here, in its own transaction, so "
             "step 1's lock is not held across it",
             /*own=*/true);
        plan.warnings.push_back(
            "validating a domain constraint reads every column of type " +
            qualified_obj +
            " in every table that has one. Unlike a table constraint, the cost "
            "is not bounded by one table and pg_laswell cannot size it from "
            "here -- pg_licht listTableSizes over the tables using this domain "
            "is the reading that would.");
        return;
      }
      std::vector<std::string> sql;
      if (in.body.contains("drop_constraint")) {
        sql.push_back("ALTER DOMAIN " + qualified_obj + " DROP CONSTRAINT " +
                      detail::quote_identifier(in.body.value("drop_constraint", "")) + ";");
      }
      if (in.body.contains("not_null")) {
        sql.push_back("ALTER DOMAIN " + qualified_obj +
                      (in.body.value("not_null", false) ? " SET NOT NULL" : " DROP NOT NULL") + ";");
      }
      if (in.body.contains("default")) {
        sql.push_back("ALTER DOMAIN " + qualified_obj + " SET DEFAULT " +
                      in.body.value("default", "") + ";");
      }
      emit(TxnClass::kRequired, std::move(sql),
           "AccessExclusiveLock on the domain",
           "a domain change reaches every column of that type", /*own=*/false);
      if (in.body.value("not_null", false)) {
        plan.warnings.push_back(
            "SET NOT NULL on a domain scans every column of type " + qualified_obj +
            " and fails on the first null anywhere. There is no NOT VALID form "
            "for it, so the scan cannot be deferred the way a check can.");
      }
      return;
    }

    case IntentKind::kAlterFunction: {
      const auto& o = obs.object("function:" + qualified_obj);
      if (!o.value("exists", false)) {
        refuse("function " + qualified_obj + " does not exist",
               "function " + qualified_obj + " does not exist");
        return;
      }
      const auto signature = detail::function_signature(in);
      std::vector<std::string> sql;
      if (in.body.contains("to")) {
        sql.push_back("ALTER FUNCTION " + signature + " RENAME TO " +
                      detail::quote_identifier(in.body.value("to", "")) + ";");
      }
      if (in.body.contains("owner")) {
        sql.push_back("ALTER FUNCTION " + signature + " OWNER TO " +
                      detail::quote_identifier(in.body.value("owner", "")) + ";");
      }
      if (in.body.contains("search_path")) {
        sql.push_back("ALTER FUNCTION " + signature + " SET search_path = " +
                      in.body.value("search_path", "") + ";");
      }
      if (in.body.contains("volatility")) {
        sql.push_back("ALTER FUNCTION " + signature + " " +
                      in.body.value("volatility", "") + ";");
      }
      emit(TxnClass::kRequired, std::move(sql), "no lock on any table",
           "altering a function's properties is catalog-only", /*own=*/false);
      if (in.body.contains("volatility")) {
        plan.warnings.push_back(
            "changing " + signature + "'s volatility changes how the PLANNER may "
            "use it -- an IMMUTABLE function can be folded into an index "
            "expression and cached, a VOLATILE one cannot. Declaring a function "
            "more immutable than it is produces wrong answers from indexes "
            "built over it, and nothing will report that.");
      }
      return;
    }

    case IntentKind::kAlterView: {
      const auto& t = obs.table(qualified_obj);
      if (!t.value("exists", false)) {
        refuse(qualified_obj + " does not exist", qualified_obj + " does not exist");
        return;
      }
      const bool matview = t.value("kind", "") == "materialized_view";
      const std::string what = matview ? "MATERIALIZED VIEW " : "VIEW ";
      std::vector<std::string> sql;
      if (in.body.contains("to")) {
        sql.push_back("ALTER " + what + qualified_obj + " RENAME TO " +
                      detail::quote_identifier(in.body.value("to", "")) + ";");
      }
      if (in.body.contains("owner")) {
        sql.push_back("ALTER " + what + qualified_obj + " OWNER TO " +
                      detail::quote_identifier(in.body.value("owner", "")) + ";");
      }
      if (in.body.contains("options")) {
        std::vector<std::string> opts;
        for (const auto& o : in.body["options"]) opts.push_back(o.get<std::string>());
        sql.push_back("ALTER " + what + qualified_obj + " SET (" +
                      detail::join(opts, ", ") + ");");
      }
      emit(TxnClass::kRequired, std::move(sql),
           "AccessExclusiveLock on " + qualified_obj,
           "catalog-only; the view's definition is untouched", /*own=*/false);
      return;
    }

    case IntentKind::kAlterPolicy: {
      const auto& t = obs.table(qualified_tbl);
      const json policies = t.value("policies", json::object());
      if (!policies.contains(name)) {
        refuse("no policy named " + name + " on " + qualified_tbl,
               "no policy named " + name + " on " + qualified_tbl +
                   ". create_policy is the intent for a new one.");
        return;
      }
      std::string sql = "ALTER POLICY " + detail::quote_identifier(name) +
                        " ON " + qualified_tbl;
      if (in.body.contains("roles")) {
        std::vector<std::string> roles;
        for (const auto& r : in.body["roles"]) {
          roles.push_back(detail::quote_identifier(r.get<std::string>()));
        }
        sql += " TO " + detail::join(roles, ", ");
      }
      if (in.body.contains("using")) sql += " USING (" + in.body.value("using", "") + ")";
      if (in.body.contains("check")) {
        sql += " WITH CHECK (" + in.body.value("check", "") + ")";
      }
      emit(TxnClass::kRequired, {sql + ";"},
           "AccessExclusiveLock on " + qualified_tbl +
               " -- measured, the same lock CREATE POLICY takes",
           "changing a policy in place keeps its name and its place in the "
           "permissive/restrictive set, which dropping and recreating would not",
           /*own=*/false);
      plan.warnings.push_back(
          "this changes what rows the application sees the moment it commits, "
          "for sessions already connected. And you will probably not see it: an "
          "owner bypasses row-level security unless FORCE is set, and a "
          "superuser bypasses it regardless -- check the result as the "
          "application role, with SET ROLE.");
      return;
    }

    case IntentKind::kSetComment: {
      const auto ot = in.body.value("object_type", "");
      std::string target = schema.empty() ? detail::quote_identifier(name)
                                          : schema + "." + name;
      std::string stmt;
      if (ot == "COLUMN") {
        stmt = "COMMENT ON COLUMN " + qualified_tbl + "." +
               detail::quote_identifier(name) + " IS ";
      } else if (ot == "TRIGGER" || ot == "POLICY" || ot == "CONSTRAINT") {
        stmt = "COMMENT ON " + ot + " " + detail::quote_identifier(name) +
               " ON " + qualified_tbl + " IS ";
      } else if (ot == "SCHEMA" || ot == "EXTENSION") {
        stmt = "COMMENT ON " + ot + " " + detail::quote_identifier(name) + " IS ";
      } else {
        stmt = "COMMENT ON " + ot + " " + target + " IS ";
      }
      emit(TxnClass::kRequired,
           {stmt + detail::quote_literal(in.body.value("comment", "")) + ";"},
           "AccessShareLock -- a comment blocks nothing",
           "documentation is a change to schema state like any other, and this "
           "is the intent that changes one without recreating the object",
           /*own=*/false);
      return;
    }

    case IntentKind::kSetOwner: {
      const auto ot = in.body.value("object_type", "");
      const std::string target = schema.empty() ? detail::quote_identifier(name)
                                                : schema + "." + name;
      emit(TxnClass::kRequired,
           {"ALTER " + ot + " " + target + " OWNER TO " +
            detail::quote_identifier(in.body.value("owner", "")) + ";"},
           "AccessExclusiveLock on " + target,
           "catalog-only", /*own=*/false);
      plan.warnings.push_back(
          "changing the owner of " + target +
          " changes who bypasses row-level security on it, who its SECURITY "
          "DEFINER functions run as, and which role future grants are recorded "
          "as coming from. It is a privilege change, not bookkeeping.");
      return;
    }

    default: break;
  }
}

// --- schemas, extensions, types, functions, triggers, sequences ------------
//
// The object kinds that had no intent at all, so a repository could not
// describe a database from nothing. Each carries the same safeguards as the
// rest of the tool rather than being a thin wrapper (spike S18, 18.6):
//
//   * every drop refuses when something depends on the object, naming the
//     dependant that PostgreSQL would have named, and never emits CASCADE;
//   * ALTER TYPE ... ADD VALUE is IRREVERSIBLE -- "dropping an enum value is
//     not implemented" -- and the new label cannot be USED until the adding
//     transaction commits ("unsafe use of new value"), so the step forces a
//     boundary;
//   * CREATE OR REPLACE FUNCTION keeps the comment, grants, owner AND THE OID,
//     so dependants survive -- but it cannot change the return type, and the
//     drop-and-recreate that can loses all four;
//   * CREATE TRIGGER takes ShareRowExclusiveLock: it blocks writes, not reads.
namespace detail {

// "public.f(integer, text)" from the structured arguments. A function's
// identity is its argument types, so this is what DROP and REPLACE both need.
inline std::string function_signature(const Intent& in) {
  std::vector<std::string> types;
  for (const auto& a : in.body.value("arguments", json::array())) {
    types.push_back(a.value("type", ""));
  }
  return in.body.value("schema", "") + "." + in.body.value("name", "") + "(" +
         join(types, ", ") + ")";
}

}  // namespace detail

inline void plan_object(const Intent& in, const Observations& obs, Plan& plan,
                        std::vector<Step>& out) {
  Step step;
  step.kind = in.kind_name;
  struct Emit { std::vector<Step>& o; Step& s; ~Emit() { o.push_back(s); } } emit{out, step};
  step.txn_class = TxnClass::kRequired;

  const auto schema = in.body.value("schema", "");
  const auto name = in.body.value("name", "");
  const auto qualified = schema + "." + name;

  // Every drop in this family shares one refusal, because PostgreSQL shares one
  // behaviour: it names the dependants and stops. Doing the same beforehand
  // turns a failed migration into a plan that was never started.
  auto refuse_if_depended_on = [&](const json& o, const std::string& what) {
    const auto deps = o.value("depended_on_by", json::array());
    if (deps.empty()) return false;
    std::vector<std::string> names;
    for (const auto& d : deps) names.push_back(d.get<std::string>());
    step.action = Action::kConflict;
    step.why = what + " is depended on by " + std::to_string(names.size()) +
               " object(s)";
    plan.conflicts.push_back(
        "cannot drop " + what + ": " + detail::join(names, ", ") +
        " depend" + (names.size() == 1 ? "s" : "") +
        " on it. PostgreSQL would refuse this and suggest CASCADE; pg_laswell "
        "will not emit CASCADE, because it drops objects the spec never named. "
        "Remove them in earlier intents.");
    return true;
  };

  switch (in.kind) {
    case IntentKind::kCreateSchema: {
      const auto& o = obs.object("schema:" + schema);
      if (o.value("exists", false)) {
        step.action = Action::kSatisfied;
        step.why = "schema " + schema + " already exists";
        return;
      }
      step.sql.push_back("CREATE SCHEMA " + detail::quote_identifier(schema) + ";");
      if (in.body.contains("owner")) {
        step.sql.push_back("ALTER SCHEMA " + detail::quote_identifier(schema) +
                           " OWNER TO " +
                           detail::quote_identifier(in.body.value("owner", "")) + ";");
      }
      step.sql.push_back("COMMENT ON SCHEMA " + detail::quote_identifier(schema) +
                         " IS " + detail::quote_literal(in.body.value("comment", "")) + ";");
      step.lock = "no lock on any existing object";
      step.why = "creating a schema locks nothing and is instant";
      return;
    }

    case IntentKind::kDropSchema: {
      const auto& o = obs.object("schema:" + schema);
      if (!o.value("exists", false)) {
        step.action = Action::kSatisfied;
        step.why = "schema " + schema + " does not exist";
        return;
      }
      const auto contains = o.value("contains", json::array());
      if (!contains.empty()) {
        std::vector<std::string> names;
        for (const auto& c : contains) names.push_back(c.get<std::string>());
        step.action = Action::kConflict;
        step.why = "schema " + schema + " is not empty";
        plan.conflicts.push_back(
            "cannot drop schema " + schema + ": it still contains " +
            std::to_string(names.size()) + " object(s) -- " +
            detail::join(names, ", ") +
            ". Dropping a schema with CASCADE is the widest blast radius in "
            "this tool and pg_laswell will not do it; drop what is inside in "
            "earlier intents, where each one is named and reviewed.");
        return;
      }
      step.sql.push_back("DROP SCHEMA " + detail::quote_identifier(schema) + ";");
      step.lock = "AccessExclusiveLock on the schema";
      step.why = "the schema is empty, so this removes only the schema itself";
      return;
    }

    case IntentKind::kCreateExtension: {
      const auto ext = in.body.value("name", "");
      const auto& o = obs.object("extension:" + ext);
      if (o.value("exists", false)) {
        step.action = Action::kSatisfied;
        step.why = "extension " + ext + " is already installed" +
                   (o.value("version", json()).is_string()
                        ? " at version " + o.value("version", "")
                        : "");
        return;
      }
      // Refuse a name the server cannot satisfy, rather than emitting a
      // statement that fails: the packages an extension needs are installed by
      // an administrator on the host, not by a migration.
      if (o.contains("available") && !o.value("available", true)) {
        step.action = Action::kConflict;
        step.why = ext + " is not available on this server";
        plan.conflicts.push_back(
            "extension \"" + ext + "\" is not in pg_available_extensions on "
            "this server, so CREATE EXTENSION would fail. Its files are "
            "installed on the host by a package, not by a migration -- this is "
            "an infrastructure prerequisite, not something the spec can fix.");
        detail::require_out_of_band(
            plan, "package", ext, "the database host",
            "the extension's files must be installed by a package -- typically "
            "postgresql-<version>-" + ext + " or a vendor build -- before any "
            "migration can create it",
            "SELECT 1 FROM pg_available_extensions WHERE name = " +
                detail::quote_literal(ext),
            /*blocking=*/true);
        return;
      }
      std::string sql = "CREATE EXTENSION " + detail::quote_identifier(ext);
      if (in.body.contains("schema")) {
        sql += " SCHEMA " + detail::quote_identifier(in.body.value("schema", ""));
      }
      if (in.body.contains("version")) {
        sql += " VERSION " + detail::quote_literal(in.body.value("version", ""));
      }
      step.sql.push_back(sql + ";");
      step.lock = "no lock on any existing object, but see the warning";
      step.why = "installing an extension creates every object it defines, in "
                 "one transaction";
      if (!in.body.contains("schema")) {
        plan.warnings.push_back(
            "create_extension for \"" + ext +
            "\" does not name a schema, so its objects land wherever "
            "search_path points when this runs -- measured, that is public by "
            "default. Where an extension lives affects every later query's "
            "search_path and every dump and restore, and moving it afterwards "
            "is far harder than choosing now. Name the schema.");
      }
      if (o.value("default_version", json()).is_string()) {
        plan.warnings.push_back(
            "this server would install \"" + ext + "\" at version " +
            o.value("default_version", "") +
            (in.body.contains("version") ? ", and the spec asks for " +
                                               in.body.value("version", "")
                                         : ", and the spec does not pin one") +
            ". An unpinned version means two databases can end up with "
            "different behaviour from the same repository.");
      }
      return;
    }

    case IntentKind::kDropExtension: {
      const auto ext = in.body.value("name", "");
      const auto& o = obs.object("extension:" + ext);
      if (!o.value("exists", false)) {
        step.action = Action::kSatisfied;
        step.why = "extension " + ext + " is not installed";
        return;
      }
      if (refuse_if_depended_on(o, "extension " + ext)) return;
      step.sql.push_back("DROP EXTENSION " + detail::quote_identifier(ext) + ";");
      step.lock = "AccessExclusiveLock on every object the extension owns";
      step.why = "nothing outside the extension depends on it";
      plan.warnings.push_back(
          "dropping extension \"" + ext +
          "\" removes every function, type, operator and index method it "
          "defines. Any index using an operator class from it goes too, and "
          "queries written against it stop parsing.");
      return;
    }

    case IntentKind::kCreateType: {
      const auto& o = obs.object("type:" + qualified);
      const auto tk = in.body.value("type_kind", "");
      if (o.value("exists", false)) {
        step.action = Action::kSatisfied;
        step.why = "type " + qualified + " already exists as a " +
                   o.value("type_kind", std::string("type"));
        if (o.value("type_kind", "") != tk) {
          step.action = Action::kConflict;
          plan.conflicts.push_back(
              "type " + qualified + " exists as a " + o.value("type_kind", "") +
              ", and the spec declares a " + tk + ". Those are different types "
              "with one name; drop the existing one in an earlier intent if it "
              "is really meant to be replaced.");
        }
        return;
      }
      if (tk == "enum") {
        std::vector<std::string> labels;
        for (const auto& l : in.body["labels"]) {
          labels.push_back(detail::quote_literal(l.get<std::string>()));
        }
        step.sql.push_back("CREATE TYPE " + qualified + " AS ENUM (" +
                           detail::join(labels, ", ") + ");");
        plan.warnings.push_back(
            "an enum's labels can be ADDED later but never removed -- measured: "
            "\"dropping an enum value is not implemented\". Every label in " +
            qualified + " is permanent for the life of the type, so a "
            "misspelling is repaired only by recreating the type and every "
            "column that uses it. A CHECK constraint over text is the "
            "reversible alternative.");
      } else if (tk == "domain") {
        std::string sql = "CREATE DOMAIN " + qualified + " AS " +
                          in.body.value("base", "");
        if (in.body.value("not_null", false)) sql += " NOT NULL";
        if (in.body.contains("check")) {
          sql += " CHECK (" + in.body.value("check", "") + ")";
        }
        step.sql.push_back(sql + ";");
      } else {
        std::vector<std::string> attrs;
        for (const auto& a : in.body["attributes"]) {
          attrs.push_back(detail::quote_identifier(a.value("name", "")) + " " +
                          a.value("type", ""));
        }
        step.sql.push_back("CREATE TYPE " + qualified + " AS (" +
                           detail::join(attrs, ", ") + ");");
      }
      step.sql.push_back("COMMENT ON " +
                         std::string(tk == "domain" ? "DOMAIN " : "TYPE ") +
                         qualified + " IS " +
                         detail::quote_literal(in.body.value("comment", "")) + ";");
      step.lock = "no lock on any existing object";
      step.why = "creating a type locks nothing";
      return;
    }

    case IntentKind::kDropType: {
      const auto& o = obs.object("type:" + qualified);
      if (!o.value("exists", false)) {
        step.action = Action::kSatisfied;
        step.why = "type " + qualified + " does not exist";
        return;
      }
      if (refuse_if_depended_on(o, "type " + qualified)) return;
      step.sql.push_back("DROP TYPE " + qualified + ";");
      step.lock = "AccessExclusiveLock on the type";
      step.why = "nothing uses " + qualified;
      return;
    }

    case IntentKind::kAddEnumValue: {
      const auto& o = obs.object("type:" + qualified);
      const auto value = in.body.value("value", "");
      if (!o.value("exists", false)) {
        step.action = Action::kConflict;
        step.why = "type " + qualified + " does not exist";
        plan.conflicts.push_back(step.why);
        return;
      }
      for (const auto& l : o.value("enum_labels", json::array())) {
        if (l.get<std::string>() == value) {
          step.action = Action::kSatisfied;
          step.why = qualified + " already has the label " + value;
          return;
        }
      }
      std::string sql = "ALTER TYPE " + qualified + " ADD VALUE " +
                        detail::quote_literal(value);
      if (in.body.contains("before")) {
        sql += " BEFORE " + detail::quote_literal(in.body.value("before", ""));
      } else if (in.body.contains("after")) {
        sql += " AFTER " + detail::quote_literal(in.body.value("after", ""));
      }
      step.sql.push_back(sql + ";");
      // Measured: the new label cannot be USED until this transaction commits
      // -- "unsafe use of new value". Forcing the boundary here is what stops a
      // later intent in the same group from failing on it.
      step.own_transaction = true;
      step.lock = "no table lock; the type's row is updated";
      step.why =
          "added in its own transaction because PostgreSQL refuses to USE a new "
          "enum label until the transaction that added it has committed "
          "(\"unsafe use of new value\") -- so anything backfilling with " +
          value + " must come after this commits, which this boundary "
          "guarantees";
      plan.warnings.push_back(
          "adding \"" + value + "\" to " + qualified +
          " is IRREVERSIBLE: PostgreSQL has no way to remove an enum label "
          "(\"dropping an enum value is not implemented\"). There is no revert "
          "for this step, in this tool or by hand, short of recreating the type "
          "and every column that uses it.");
      return;
    }

    case IntentKind::kCreateFunction: {
      const auto signature = detail::function_signature(in);
      const auto& o = obs.object("function:" + qualified);
      std::vector<std::string> args;
      for (const auto& a : in.body.value("arguments", json::array())) {
        args.push_back((a.contains("name")
                            ? detail::quote_identifier(a.value("name", "")) + " "
                            : "") + a.value("type", ""));
      }
      const auto returns = in.body.value("returns", "");
      const bool exists = o.value("exists", false);
      // CREATE OR REPLACE keeps the comment, grants, owner AND the OID, so
      // dependent views survive -- measured. It cannot change the return type,
      // which is the one case that forces the destructive path.
      const bool return_changes =
          exists && o.value("returns", "") != returns;
      if (return_changes) {
        step.action = Action::kConflict;
        step.why = signature + " changes its return type from " +
                   o.value("returns", "") + " to " + returns;
        plan.conflicts.push_back(
            "cannot replace " + signature + ": its return type would change "
            "from " + o.value("returns", "") + " to " + returns +
            ", and PostgreSQL refuses that (\"cannot change return type of "
            "existing function\"). The only way through is DROP then CREATE, "
            "which loses the function's comment, its grants and its owner, and "
            "breaks every view that depends on it -- so it belongs in explicit "
            "drop_function and create_function intents, not hidden inside a "
            "replace.");
        return;
      }
      const auto routine = in.body.value("routine_kind", "FUNCTION");
      std::string sql = "CREATE OR REPLACE " + routine + " " + qualified + "(" +
                        detail::join(args, ", ") +
                        (routine == "PROCEDURE" ? ")" : ")\n  RETURNS " + returns) +
                        "\n  LANGUAGE " + in.body.value("language", "");
      if (in.body.contains("volatility")) sql += "\n  " + in.body.value("volatility", "");
      if (in.body.value("strict", false)) sql += "\n  STRICT";
      if (in.body.value("security_definer", false)) sql += "\n  SECURITY DEFINER";
      sql += "\nAS $laswell$" + in.body.value("body", "") + "$laswell$;";
      step.sql.push_back(sql);
      step.sql.push_back("COMMENT ON " + routine + " " + signature + " IS " +
                         detail::quote_literal(in.body.value("comment", "")) + ";");
      step.lock = "no lock on any table";
      step.why = exists
          ? "CREATE OR REPLACE keeps the comment, grants, owner and the "
            "function's OID, so every dependent view survives -- measured; a "
            "drop and recreate would lose all four"
          : "creating a function locks nothing";
      if (in.body.value("security_definer", false)) {
        plan.warnings.push_back(
            signature + " is SECURITY DEFINER: it runs with the OWNER's "
            "privileges, not the caller's, so it bypasses whatever the caller "
            "is otherwise denied -- including row-level security. Set an "
            "explicit search_path inside the body, or a caller can change what "
            "the function resolves.");
      }
      return;
    }

    case IntentKind::kDropFunction: {
      const auto signature = detail::function_signature(in);
      const auto& o = obs.object("function:" + qualified);
      if (!o.value("exists", false)) {
        step.action = Action::kSatisfied;
        step.why = "no function " + signature;
        return;
      }
      if (refuse_if_depended_on(o, "function " + signature)) return;
      step.sql.push_back("DROP " + in.body.value("routine_kind", "FUNCTION") +
                         " " + signature + ";");
      step.lock = "AccessExclusiveLock on the function";
      step.why = "nothing depends on " + signature;
      return;
    }

    case IntentKind::kCreateSequence: {
      const auto& o = obs.object("sequence:" + qualified);
      if (o.value("exists", false)) {
        step.action = Action::kSatisfied;
        step.why = "sequence " + qualified + " already exists";
        return;
      }
      std::string sql = "CREATE SEQUENCE " + qualified;
      if (in.body.contains("increment")) {
        sql += " INCREMENT BY " + std::to_string(in.body.value("increment", 1));
      }
      if (in.body.contains("start")) {
        sql += " START WITH " + std::to_string(in.body.value("start", 1));
      }
      if (in.body.contains("owned_by")) {
        sql += " OWNED BY " + in.body.value("owned_by", "");
      }
      step.sql.push_back(sql + ";");
      step.sql.push_back("COMMENT ON SEQUENCE " + qualified + " IS " +
                         detail::quote_literal(in.body.value("comment", "")) + ";");
      step.lock = "no lock on any existing object";
      step.why = "creating a sequence locks nothing";
      if (!in.body.contains("owned_by")) {
        plan.warnings.push_back(
            "sequence " + qualified +
            " is not OWNED BY a column, so it survives every table that uses "
            "it and is not dropped with them. That is occasionally what is "
            "wanted and usually an oversight.");
      }
      return;
    }

    case IntentKind::kDropSequence: {
      const auto& o = obs.object("sequence:" + qualified);
      if (!o.value("exists", false)) {
        step.action = Action::kSatisfied;
        step.why = "sequence " + qualified + " does not exist";
        return;
      }
      if (refuse_if_depended_on(o, "sequence " + qualified)) return;
      step.sql.push_back("DROP SEQUENCE " + qualified + ";");
      step.lock = "AccessExclusiveLock on the sequence";
      step.why = "nothing depends on " + qualified;
      return;
    }

    case IntentKind::kDropView: {
      const auto& t = obs.table(qualified);
      if (!t.value("exists", false)) {
        step.action = Action::kSatisfied;
        step.why = qualified + " does not exist";
        return;
      }
      const auto kind = t.value("kind", "");
      if (kind != "view" && kind != "materialized_view") {
        step.action = Action::kConflict;
        step.why = qualified + " is a " + kind + ", not a view";
        plan.conflicts.push_back(
            qualified + " is a " + kind +
            ". drop_view will not drop a table; drop_table is the intent that "
            "says so out loud, and warns about the data.");
        return;
      }
      const json dependent = t.value("dependent_views", json::object());
      if (!dependent.empty()) {
        std::vector<std::string> names;
        for (const auto& [n, v] : dependent.items()) { (void)v; names.push_back(n); }
        step.action = Action::kConflict;
        step.why = qualified + " has views stacked on it";
        plan.conflicts.push_back(
            "cannot drop " + qualified + ": " + detail::join(names, ", ") +
            " read it. Drop them first, in earlier intents. CASCADE would take "
            "the whole stack, which is measured to leave zero views standing.");
        return;
      }
      step.sql.push_back(std::string("DROP ") +
                         (kind == "materialized_view" ? "MATERIALIZED VIEW "
                                                      : "VIEW ") +
                         qualified + ";");
      step.lock = "AccessExclusiveLock on " + qualified;
      step.why = "nothing reads " + qualified;
      if (kind == "materialized_view") {
        plan.warnings.push_back(
            qualified +
            " is a materialized view, so this discards its stored data as well "
            "as its definition. Recreating it re-runs the query in full.");
      }
      return;
    }

    default: break;
  }
}

// --- row security, policies, triggers, grants ------------------------------
//
// These emit one statement each, and they belong here by the second test: every
// one is a change to schema state, applied once. They are also the changes
// whose EFFECT is least visible from the session that makes them, which is
// where the planning value is.
//
// Locks, measured on 18.6:
//   ALTER TABLE ... DISABLE TRIGGER   ShareRowExclusiveLock  (blocks writes,
//                                                             not reads)
//   ENABLE ROW LEVEL SECURITY         AccessExclusiveLock
//   CREATE POLICY                     AccessExclusiveLock
//   GRANT                             AccessShareLock        (cheap)
inline void plan_security(const Intent& in, const Observations& obs, Plan& plan,
                          std::vector<Step>& out) {
  Step step;
  step.kind = in.kind_name;
  struct Emit { std::vector<Step>& o; Step& s; ~Emit() { o.push_back(s); } } emit{out, step};

  const auto qualified = in.qualified_table();
  const auto& t = obs.table(qualified);
  // A grant or revoke may name something that is not a relation at all, so the
  // table guard applies to the kinds that really are table-scoped.
  const bool table_scoped =
      in.kind != IntentKind::kGrant && in.kind != IntentKind::kRevoke;
  if (table_scoped && !t.value("exists", false)) {
    step.action = Action::kConflict;
    step.why = qualified + " does not exist";
    plan.conflicts.push_back(step.why);
    return;
  }
  step.txn_class = TxnClass::kRequired;

  switch (in.kind) {
    case IntentKind::kSetRowSecurity: {
      const bool want = in.body.value("enabled", false);
      const bool force = in.body.value("force", false);
      const json rls = t.value("row_security", json::object());
      if (rls.value("enabled", false) == want &&
          rls.value("forced", false) == force) {
        step.action = Action::kSatisfied;
        step.why = "row security on " + qualified + " is already " +
                   (want ? "enabled" : "disabled") +
                   (force ? " and forced" : "");
        return;
      }
      step.sql.push_back("ALTER TABLE " + qualified +
                         (want ? " ENABLE" : " DISABLE") +
                         " ROW LEVEL SECURITY;");
      if (want) {
        step.sql.push_back("ALTER TABLE " + qualified +
                           (force ? " FORCE" : " NO FORCE") +
                           " ROW LEVEL SECURITY;");
      }
      step.lock = "AccessExclusiveLock on " + qualified;
      step.why = std::string(want ? "enabling" : "disabling") +
                 " row-level security is a catalog change, and its effect is "
                 "immediate for every session already connected";

      // The measured trap, and the reason this kind is worth having. Enabling
      // RLS with no policy is not a no-op: it is a default deny.
      if (want) {
        const json policies = t.value("policies", json::object());
        if (policies.empty()) {
          plan.warnings.push_back(
              "enabling row-level security on " + qualified +
              " with NO POLICY hides every row from every non-owner. Measured "
              "on 18.6: an application role reading the table went from 1000 "
              "rows to 0, with no error -- the query succeeds and returns "
              "nothing. Create the policies in EARLIER intents than this one.");
        }
        plan.warnings.push_back(
            "you will probably not be able to see this working. Measured: a "
            "table's OWNER bypasses row-level security unless FORCE is also "
            "set" + std::string(force ? " (this plan sets it)" : "") +
            ", and a SUPERUSER bypasses it whatever either flag says. "
            "Verifying an RLS change from a superuser session shows no change "
            "at all, however wrong the policy is. Check it as the application "
            "role, with SET ROLE.");
      }
      step.detail["enabled"] = want;
      step.detail["forced"] = force;
      return;
    }

    case IntentKind::kCreatePolicy: {
      const auto name = in.body.value("name", "");
      const json policies = t.value("policies", json::object());
      if (policies.contains(name)) {
        step.action = Action::kSatisfied;
        step.why = "policy " + name + " already exists on " + qualified;
        return;
      }
      std::string sql = "CREATE POLICY " + detail::quote_identifier(name) +
                        " ON " + qualified;
      if (in.body.contains("command")) sql += " FOR " + in.body.value("command", "");
      if (in.body.contains("roles")) {
        std::vector<std::string> roles;
        for (const auto& r : in.body["roles"]) {
          roles.push_back(detail::quote_identifier(r.get<std::string>()));
        }
        sql += " TO " + detail::join(roles, ", ");
      }
      if (in.body.contains("using")) sql += " USING (" + in.body.value("using", "") + ")";
      if (in.body.contains("check")) {
        sql += " WITH CHECK (" + in.body.value("check", "") + ")";
      }
      step.sql.push_back(sql + ";");
      step.lock = "AccessExclusiveLock on " + qualified;
      step.why = "adding a policy is a catalog change, and it takes effect for "
                 "sessions already connected";
      step.detail["policy"] = name;
      if (!t.value("row_security", json::object()).value("enabled", false)) {
        plan.warnings.push_back(
            "policy \"" + name + "\" is being created on " + qualified +
            ", which does not have row-level security ENABLED. The policy is "
            "stored and does nothing at all until it is. That is the safe "
            "order to write it in -- policies first, then set_row_security -- "
            "but only if the enabling intent actually follows.");
      }
      if (!in.body.contains("check") && in.body.contains("command") &&
          in.body.value("command", "") != "SELECT") {
        plan.warnings.push_back(
            "policy \"" + name + "\" has USING but no WITH CHECK. USING filters "
            "the rows a statement may SEE; WITH CHECK constrains the rows it "
            "may WRITE. With only USING, a role can insert or update a row "
            "into a state its own policy would not have let it read back.");
      }
      return;
    }

    case IntentKind::kDropPolicy: {
      const auto name = in.body.value("name", "");
      const json policies = t.value("policies", json::object());
      if (!policies.contains(name)) {
        step.action = Action::kSatisfied;
        step.why = "no policy named " + name + " on " + qualified;
        return;
      }
      step.sql.push_back("DROP POLICY " + detail::quote_identifier(name) +
                         " ON " + qualified + ";");
      step.lock = "AccessExclusiveLock on " + qualified;
      step.why = "dropping a policy is a catalog change";
      step.detail["policy"] = name;
      if (policies.size() == 1 &&
          t.value("row_security", json::object()).value("enabled", false)) {
        plan.warnings.push_back(
            "\"" + name + "\" is the LAST policy on " + qualified +
            " and row-level security stays enabled, so afterwards every "
            "non-owner sees zero rows -- silently, with no error. If the "
            "intention is to stop filtering, disable row security in the same "
            "migration.");
      }
      return;
    }

    case IntentKind::kSetTriggerState: {
      const auto name = in.body.value("trigger", "");
      const bool want = in.body.value("enabled", false);
      const json triggers = t.value("triggers", json::object());
      if (!triggers.contains(name)) {
        step.action = Action::kConflict;
        step.why = "no trigger named " + name + " on " + qualified;
        plan.conflicts.push_back(step.why);
        return;
      }
      if (triggers[name].value("enabled", true) == want) {
        step.action = Action::kSatisfied;
        step.why = "trigger " + name + " is already " +
                   (want ? "enabled" : "disabled");
        return;
      }
      step.sql.push_back("ALTER TABLE " + qualified +
                         (want ? " ENABLE TRIGGER " : " DISABLE TRIGGER ") +
                         detail::quote_identifier(name) + ";");
      step.lock = "ShareRowExclusiveLock on " + qualified +
                  " -- blocks writes, not reads";
      step.why =
          "measured on 18.6: this takes ShareRowExclusiveLock rather than the "
          "AccessExclusiveLock most ALTER TABLE forms need, so readers are "
          "unaffected";
      step.detail["trigger"] = name;
      if (!want) {
        plan.warnings.push_back(
            "while trigger \"" + name + "\" is disabled, whatever it maintains "
            "stops being maintained -- an audit row not written, a denormalised "
            "column not updated, a total not adjusted. Nothing records the gap, "
            "and re-enabling does not backfill it. If rows change in between, "
            "they will need repairing explicitly.");
      }
      return;
    }

    case IntentKind::kCreateTrigger: {
      const auto name = in.body.value("name", "");
      const json triggers = t.value("triggers", json::object());
      if (triggers.contains(name)) {
        step.action = Action::kSatisfied;
        step.why = "trigger " + name + " already exists on " + qualified;
        return;
      }
      std::vector<std::string> events;
      for (const auto& e : in.body.value("events", json::array())) {
        events.push_back(e.get<std::string>());
      }
      std::string sql = "CREATE TRIGGER " + detail::quote_identifier(name) +
                        " " + in.body.value("timing", "") + " " +
                        detail::join(events, " OR ") + " ON " + qualified +
                        " FOR EACH " + in.body.value("for_each", "ROW");
      if (in.body.contains("when")) sql += " WHEN (" + in.body.value("when", "") + ")";
      sql += " EXECUTE FUNCTION " + in.body.value("function", "") + ";";
      step.sql.push_back(sql);
      // Measured: not AccessExclusiveLock. Readers are unaffected.
      step.lock = "ShareRowExclusiveLock on " + qualified +
                  " -- blocks writes, not reads";
      step.why =
          "creating a trigger takes ShareRowExclusiveLock rather than the "
          "AccessExclusiveLock most ALTER TABLE forms need, so reads continue "
          "throughout";
      step.detail["trigger"] = name;
      plan.warnings.push_back(
          "trigger \"" + name + "\" starts firing at commit, for every row "
          "changed from then on -- and for none of the rows already there. If "
          "it maintains something derived, the existing rows need a backfill "
          "intent to match.");
      return;
    }

    case IntentKind::kDropTrigger: {
      const auto name = in.body.value("name", "");
      const json triggers = t.value("triggers", json::object());
      if (!triggers.contains(name)) {
        step.action = Action::kSatisfied;
        step.why = "no trigger named " + name + " on " + qualified;
        return;
      }
      step.sql.push_back("DROP TRIGGER " + detail::quote_identifier(name) +
                         " ON " + qualified + ";");
      step.lock = "AccessExclusiveLock on " + qualified;
      step.why = "dropping a trigger is a catalog change";
      step.detail["trigger"] = name;
      plan.warnings.push_back(
          "whatever trigger \"" + name + "\" maintained stops being "
          "maintained at commit, permanently. Unlike disabling it, this cannot "
          "be undone by re-enabling: the definition is gone and must be "
          "recreated from the spec.");
      return;
    }

    default: break;  // grant / revoke below
  }

  // grant / revoke
  const bool granting = in.kind == IntentKind::kGrant;
  const auto object_type = in.body.value("object_type", "TABLE");
  std::vector<std::string> privs;
  for (const auto& pv : in.body.value("privileges", json::array())) {
    privs.push_back(pv.get<std::string>());
  }
  std::vector<std::string> roles;
  for (const auto& r : in.body.value(granting ? "to" : "from", json::array())) {
    const auto name = r.get<std::string>();
    roles.push_back(name == "PUBLIC" ? "PUBLIC" : detail::quote_identifier(name));
  }
  std::string cols;
  if (in.body.contains("columns")) {
    std::vector<std::string> cs;
    for (const auto& cn : in.body["columns"]) {
      cs.push_back(detail::quote_identifier(cn.get<std::string>()));
    }
    // The column list goes after the PRIVILEGE, not after the relation. The
    // other order is a syntax error -- found once already, by running it.
    cols = " (" + detail::join(cs, ", ") + ")";
  }
  // What the privileges are ON. A table grant names schema.table; every other
  // object type names itself, and ALL ... IN SCHEMA is a third shape again.
  std::string target;
  if (object_type == "TABLE") {
    target = in.body.value("all_in_schema", false)
                 ? "ALL TABLES IN SCHEMA " +
                       detail::quote_identifier(in.body.value("schema", ""))
                 : "TABLE " + qualified;
  } else if (object_type == "FUNCTION") {
    target = "FUNCTION " + detail::function_signature(in);
  } else if (object_type == "SCHEMA" || object_type == "DATABASE") {
    target = object_type + " " +
             detail::quote_identifier(in.body.value("name", ""));
  } else {
    target = object_type + " " + in.body.value("schema", "") + "." +
             in.body.value("name", "");
  }
  step.sql.push_back(std::string(granting ? "GRANT " : "REVOKE ") +
                     detail::join(privs, ", ") + cols +
                     " ON " + target + (granting ? " TO " : " FROM ") +
                     detail::join(roles, ", ") + ";");
  step.lock = "AccessShareLock on " + target +
              " -- measured; a privilege change does not block anything";
  if (in.body.value("all_in_schema", false)) {
    plan.warnings.push_back(
        "ALL TABLES IN SCHEMA applies to the tables that exist RIGHT NOW. A "
        "table created afterwards gets nothing, and the repository will not "
        "notice -- alter_default_privileges is what governs future objects, "
        "and this intent is not it.");
  }
  step.why = std::string(granting ? "granting" : "revoking") +
             " takes only AccessShareLock, so this is safe on a busy table at "
             "any size";
  step.detail["privileges"] = privs;
  if (!granting) {
    plan.warnings.push_back(
        "a revoke takes effect for sessions that are ALREADY CONNECTED, on "
        "their next statement. An application holding a pool of connections "
        "starts failing immediately, not at its next deploy.");
  }
}

// --- delete_rows -----------------------------------------------------------
//
// A retention purge, paced exactly as a backfill is and for the same reason: a
// single DELETE over a retention window holds locks for as long as it takes and
// is the same outage a single UPDATE would be. The executor's paced runner is
// generic over the statement -- $1 is the cursor, $2 the batch size, and the
// key comes back in RETURNING -- so this is a planner change and nothing else.
//
// It is a migration by both tests: it changes state, and it is applied once.
inline void plan_delete_rows(const Intent& in, const Observations& obs,
                             const ExecutorConfig& cfg, Plan& plan,
                             std::vector<Step>& out) {
  Step step;
  step.kind = in.kind_name;
  struct Emit { std::vector<Step>& o; Step& s; ~Emit() { o.push_back(s); } } emit{out, step};

  const auto qualified = in.qualified_table();
  const auto& t = obs.table(qualified);
  const auto key = in.body.value("key", "");
  const auto where = in.body.value("where", "");

  if (!t.value("exists", false)) {
    step.action = Action::kConflict;
    step.why = qualified + " does not exist";
    plan.conflicts.push_back(step.why);
    return;
  }
  const json columns = t.value("columns", json::object());
  if (!columns.contains(key)) {
    step.action = Action::kConflict;
    step.why = qualified + "." + key + " does not exist";
    plan.conflicts.push_back(
        step.why + ", so there is nothing to walk the table by.");
    return;
  }

  // A delete that removes a parent row a foreign key points at fails per row,
  // mid-batch, after some batches have already committed. Saying so first is
  // the difference between a migration that stops understandably and one that
  // stops half-done.
  const auto referenced = t.value("referenced_by", json::array());
  if (!referenced.empty()) {
    std::vector<std::string> names;
    for (const auto& r : referenced) names.push_back(r.get<std::string>());
    plan.warnings.push_back(
        qualified + " is referenced by " + detail::join(names, ", ") +
        ". Any row still referenced will fail its batch, and because this is "
        "paced, earlier batches have already COMMITTED by then -- the purge "
        "stops part-done rather than rolling back. Delete the children first, "
        "in an earlier intent, or confirm the constraint cascades.");
  }

  const auto rows = t.value("reltuples", 0LL);
  step.txn_class = TxnClass::kOwnTxnPerBatch;
  step.sql.push_back(
      "WITH batch AS (\n"
      "  SELECT " + qualified + "." + key + "\n"
      "    FROM " + qualified + "\n"
      "   WHERE " + qualified + "." + key + " > $1 AND (" + where + ")\n"
      "   ORDER BY " + qualified + "." + key + "\n"
      "   LIMIT $2\n"
      "   FOR UPDATE\n"
      ")\n"
      "DELETE FROM " + qualified + "\n"
      " USING batch AS b\n"
      " WHERE " + qualified + "." + key + " = b." + key + "\n"
      "RETURNING " + qualified + "." + key + ";");

  step.lock = "RowExclusiveLock on " + qualified +
              " -- no table-level exclusive lock at any point";
  step.why =
      "deleting " + std::to_string(rows) +
      " estimated rows in batches of " + std::to_string(cfg.batch_rows) +
      ", committing on a lock waiter, on " + std::to_string(cfg.commit_interval_ms) +
      "ms elapsed, or on " + std::to_string(cfg.batch_cap_rows) +
      " rows -- whichever comes first. A single DELETE over the same predicate "
      "would hold its locks for the whole of it";
  step.detail["qualified"] = qualified;
  step.detail["key"] = key;
  step.detail["where"] = where;
  step.detail["batch_rows"] = cfg.batch_rows;
  step.detail["commit_interval_ms"] = cfg.commit_interval_ms;
  step.detail["batch_cap_rows"] = cfg.batch_cap_rows;
  step.detail["rows_estimated"] = rows;
  if (in.body.contains("verify_remaining")) {
    step.detail["verify_remaining"] = in.body["verify_remaining"];
  }

  // The space is not returned to the operating system, and an operator who
  // expects it to be will go looking for a bug that is not there.
  plan.warnings.push_back(
      "a delete does not shrink " + qualified +
      " on disk: the rows become dead tuples and the space is reused by future "
      "inserts, not returned to the filesystem. Autovacuum will reclaim it for "
      "reuse; only VACUUM FULL or pg_repack returns it, and neither is a "
      "migration. pg_licht tableBloat shows what is actually there afterwards.");
}

// --- create_table / drop_table ---------------------------------------------
//
// These emit one statement whatever the database looks like, so the original
// rule -- a kind must make a real decision -- excluded them. That was the wrong
// instrument. The second test decides it: a migration is a change to schema
// state applied exactly once, and creating a table is the clearest example
// there is. A repository that cannot express the tables it depends on describes
// a database that does not exist.
inline void plan_create_table(const Intent& in, const Observations& obs,
                              Plan& plan, std::vector<Step>& out) {
  Step step;
  step.kind = in.kind_name;
  struct Emit { std::vector<Step>& o; Step& s; ~Emit() { o.push_back(s); } } emit{out, step};

  const auto qualified = in.qualified_table();
  const auto& t = obs.table(qualified);

  if (t.value("exists", false)) {
    // Deliberately NOT compared column by column. A table that exists with a
    // different shape is a conflict the author has to look at, and quietly
    // reporting "satisfied" over a table that differs would be the worst
    // possible answer -- it is how a migration silently does nothing.
    step.action = Action::kSatisfied;
    step.why = qualified + " already exists";
    plan.warnings.push_back(
        qualified +
        " already exists, so create_table is reported satisfied WITHOUT "
        "comparing its columns to the spec. This kind proves absence, not "
        "shape: if the existing table might differ, check it, or express the "
        "difference as add_column and alter_column_type intents which do "
        "compare.");
    return;
  }

  std::vector<std::string> defs;
  std::vector<std::string> comments;
  for (const auto& col : in.body.value("columns", json::array())) {
    const auto name = col.value("name", "");
    std::string d = detail::quote_identifier(name) + " " + col.value("type", "");
    if (col.contains("default")) d += " DEFAULT " + col.value("default", "");
    if (!col.value("nullable", true)) d += " NOT NULL";
    defs.push_back(d);
    comments.push_back("COMMENT ON COLUMN " + qualified + "." +
                       detail::quote_identifier(name) + " IS " +
                       detail::quote_literal(col.value("comment", "")) + ";");
  }
  if (in.body.contains("primary_key")) {
    std::vector<std::string> pk;
    for (const auto& c : in.body["primary_key"]) {
      pk.push_back(detail::quote_identifier(c.get<std::string>()));
    }
    // Inline, and only here. On an EMPTY table there is no scan to avoid and
    // nothing to lock out, so the two-step add_primary_key recipe would be
    // machinery for nothing.
    defs.push_back("PRIMARY KEY (" + detail::join(pk, ", ") + ")");
  }

  std::string sql = std::string("CREATE ") +
                    (in.body.value("unlogged", false) ? "UNLOGGED " : "") +
                    "TABLE " + qualified + " (\n  " + detail::join(defs, ",\n  ") +
                    "\n)";
  if (in.body.contains("partition_by")) {
    sql += " PARTITION BY " + in.body.value("partition_by", "");
  }
  step.sql.push_back(sql + ";");
  step.sql.push_back("COMMENT ON TABLE " + qualified + " IS " +
                     detail::quote_literal(in.body.value("comment", "")) + ";");
  for (auto& c : comments) step.sql.push_back(std::move(c));

  step.txn_class = TxnClass::kRequired;
  step.lock = "no lock on any existing object -- the table does not exist yet";
  step.why =
      "creating a table locks nothing and scans nothing, whatever else is "
      "happening; the risk of this intent is what it OMITS, not what it does";
  step.detail["columns"] = static_cast<int>(defs.size());
  if (in.body.value("unlogged", false)) {
    plan.warnings.push_back(
        qualified +
        " is UNLOGGED: it is not written to WAL, so it is not replicated to "
        "any standby and its contents do not survive a crash. That is a "
        "durability decision, not a performance setting.");
  }
}

inline void plan_drop_table(const Intent& in, const Observations& obs,
                            Plan& plan, std::vector<Step>& out) {
  Step step;
  step.kind = in.kind_name;
  struct Emit { std::vector<Step>& o; Step& s; ~Emit() { o.push_back(s); } } emit{out, step};

  const auto qualified = in.qualified_table();
  const auto& t = obs.table(qualified);

  if (!t.value("exists", false)) {
    step.action = Action::kSatisfied;
    step.why = qualified + " does not exist";
    return;
  }

  // Both kinds of dependant, because PostgreSQL refuses for either and the
  // catalog shows both beforehand (measured, S17).
  std::vector<std::string> blockers;
  const json blocking_views = t.value("dependent_views", json::object());
  for (const auto& [name, v] : blocking_views.items()) {
    (void)v;
    blockers.push_back("view " + name);
  }
  for (const auto& fk : t.value("referenced_by", json::array())) {
    blockers.push_back("foreign key " + fk.get<std::string>());
  }
  if (!blockers.empty()) {
    step.action = Action::kConflict;
    step.why = qualified + " has " + std::to_string(blockers.size()) +
               " dependant(s)";
    plan.conflicts.push_back(
        "cannot drop " + qualified + ": " + detail::join(blockers, ", ") +
        " depend on it. Remove them in earlier intents, where each removal is "
        "written down and reviewed. pg_laswell will not emit CASCADE, which "
        "would drop every one of those objects without the spec ever naming "
        "them.");
    return;
  }

  step.txn_class = TxnClass::kRequired;
  step.sql.push_back("DROP TABLE " + qualified + ";");
  step.lock = "AccessExclusiveLock on " + qualified;
  step.why =
      "nothing depends on " + qualified +
      ", so the drop is a brief catalog change whatever the table's size";
  plan.warnings.push_back(
      "dropping " + qualified + " is irreversible. " +
      detail::human_bytes(t.value("size_estimate", 0LL)) +
      " and roughly " + std::to_string(t.value("reltuples", 0LL)) +
      " rows go at commit, and no revert recovers them. If the data may be "
      "wanted, copy it out in an earlier intent.");
}

// --- rename_table / rename_column / rename_constraint ----------------------
//
// Measured (spike S17, 18.6): a rename needs NONE of the S13 view machinery.
// Dependencies are held by OID, so the catalog repairs itself -- after
// renaming a column, the dependent view's definition became
// "SELECT id, value AS amount FROM t" on its own, views stacked on it stayed
// queryable, and the index, CHECK and foreign-key definitions all followed.
// The lock is AccessExclusiveLock on the table alone; no view is locked.
//
// What does NOT change is the part people expect to. The view keeps its own
// output column name, so after renaming amount to value the view still exposes
// a column called "amount". That is good for compatibility and it means the
// rename does not reach anything reading through a view -- half-done, from the
// application's point of view, and silently. Saying so is most of the value
// this intent kind adds.
inline void plan_rename(const Intent& in, const Observations& obs, Plan& plan,
                        std::vector<Step>& out) {
  Step step;
  step.kind = in.kind_name;
  struct Emit { std::vector<Step>& o; Step& s; ~Emit() { o.push_back(s); } } emit{out, step};

  const auto qualified = in.qualified_table();
  const auto& t = obs.table(qualified);
  const auto to = in.body.value("to", "");

  if (!t.value("exists", false)) {
    step.action = Action::kConflict;
    step.why = qualified + " does not exist";
    plan.conflicts.push_back(step.why);
    return;
  }

  step.txn_class = TxnClass::kRequired;
  step.lock = "AccessExclusiveLock on " + qualified +
              " only -- dependent views are not locked and not rebuilt";
  step.detail["to"] = to;

  const json views = t.value("dependent_views", json::object());

  if (in.kind == IntentKind::kRenameTable) {
    if (in.table() == to) {
      step.action = Action::kSatisfied;
      step.why = qualified + " is already named " + to;
      return;
    }
    step.sql.push_back("ALTER TABLE " + qualified + " RENAME TO " +
                       detail::quote_identifier(to) + ";");
    step.why =
        "renaming a table is catalog-only, and every index, constraint and "
        "dependent view follows it by OID -- measured, not assumed";
    if (!views.empty()) {
      plan.warnings.push_back(
          std::to_string(views.size()) + " view(s) read " + qualified +
          " and will follow the rename automatically -- their stored "
          "definitions are rewritten to name " + to +
          ". Nothing in the database breaks. What breaks is anything holding "
          "the OLD name as text: application SQL, a search_path-dependent "
          "script, a dashboard query.");
    }
    return;
  }

  if (in.kind == IntentKind::kRenameColumn) {
    const auto column = in.body.value("column", "");
    const json columns = t.value("columns", json::object());
    if (columns.contains(to) && !columns.contains(column)) {
      step.action = Action::kSatisfied;
      step.why = qualified + "." + to + " already exists and " + column +
                 " does not, so the rename has been applied";
      return;
    }
    if (!columns.contains(column)) {
      step.action = Action::kConflict;
      step.why = qualified + "." + column + " does not exist";
      plan.conflicts.push_back(step.why);
      return;
    }
    if (columns.contains(to)) {
      step.action = Action::kConflict;
      step.why = qualified + "." + to + " already exists";
      plan.conflicts.push_back(
          "cannot rename " + qualified + "." + column + " to " + to +
          ": a column of that name is already there. Drop or rename it first, "
          "in an earlier intent.");
      return;
    }
    step.sql.push_back("ALTER TABLE " + qualified + " RENAME COLUMN " +
                       detail::quote_identifier(column) + " TO " +
                       detail::quote_identifier(to) + ";");
    step.why =
        "renaming a column is catalog-only; indexes, constraints and view "
        "definitions follow it by OID with no rebuild";
    step.detail["column"] = column;

    // The measured surprise, and the reason this is worth an intent kind at
    // all rather than a psql call.
    std::vector<std::string> reading;
    for (const auto& [name, v] : views.items()) {
      for (const auto& c : v.value("uses_columns", json::array())) {
        if (c.get<std::string>() == column) reading.push_back(name);
      }
    }
    if (!reading.empty()) {
      plan.warnings.push_back(
          detail::join(reading, ", ") + " read " + qualified + "." + column +
          " and will follow the rename, but each KEEPS ITS OWN OUTPUT NAME: "
          "the view definition becomes \"" + to + " AS " + column +
          "\" and anything selecting through the view still sees \"" + column +
          "\". Measured on 18.6. So this rename does not reach the "
          "application at all unless those views are changed too, with "
          "replace_view intents -- and nothing will error to tell you.");
    }
    return;
  }

  // rename_constraint
  const auto name = in.body.value("name", "");
  const json constraints = t.value("constraints", json::object());
  if (constraints.contains(to) && !constraints.contains(name)) {
    step.action = Action::kSatisfied;
    step.why = to + " already exists on " + qualified + " and " + name +
               " does not, so the rename has been applied";
    return;
  }
  if (!constraints.contains(name)) {
    step.action = Action::kConflict;
    step.why = "no constraint named " + name + " on " + qualified;
    plan.conflicts.push_back(step.why);
    return;
  }
  if (constraints.contains(to)) {
    step.action = Action::kConflict;
    step.why = to + " already exists on " + qualified;
    plan.conflicts.push_back("cannot rename " + name + " to " + to + " on " +
                             qualified + ": that name is taken.");
    return;
  }
  step.sql.push_back("ALTER TABLE " + qualified + " RENAME CONSTRAINT " +
                     detail::quote_identifier(name) + " TO " +
                     detail::quote_identifier(to) + ";");
  step.why = "renaming a constraint is catalog-only";
  step.detail["constraint"] = name;
  if (constraints[name].value("has_index", false)) {
    plan.warnings.push_back(
        "renaming constraint \"" + name + "\" also renames its backing index "
        "to \"" + to +
        "\" -- measured on 18.6. Anything naming the old index, a dashboard or "
        "an alert or a REINDEX script, stops matching, and PostgreSQL says "
        "nothing about it.");
  }
}

// --- attach_partition / detach_partition -----------------------------------
//
// Partition rotation is how time-series data is actually retired, and both
// halves carry a trap (spike S16, 18.6).
//
// ATTACH validates the candidate against the new bounds unless an existing
// CHECK constraint already proves it cannot violate them. Measured on 2M rows
// with the index build taken out of the comparison: 98.393 ms without a
// matching CHECK, 0.914 ms with one. 108x, and it scales with the partition.
//
// The index build is the reason that comparison needs care: any of the parent's
// partitioned indexes without a match on the candidate is BUILT during ATTACH,
// under AccessExclusiveLock on the candidate. A first measurement that left
// that in read as though the CHECK bought nothing.
//
// And a DEFAULT partition must be scanned on every ATTACH, to prove it holds no
// row belonging to the new bounds. That cost belongs to the default, so no
// CHECK on the candidate can avoid it: 165 ms with a 2M-row default present.
namespace detail {

// The same bounds rendered as a CHECK. This is the entire reason bounds are
// structured in the spec rather than given as a raw FOR VALUES clause: they
// have to come out in two syntaxes, and deriving the second from the first
// would mean parsing SQL in a header that must stay pure.
inline std::string bounds_as_check(const Intent& in, const std::string& key) {
  if (in.body.contains("from")) {
    return key + " >= " + in.body.value("from", "") + " AND " + key + " < " +
           in.body.value("to", "");
  }
  if (in.body.contains("values")) {
    std::vector<std::string> vs;
    for (const auto& v : in.body["values"]) vs.push_back(v.get<std::string>());
    return key + " IN (" + join(vs, ", ") + ")";
  }
  return {};  // DEFAULT has no bound to prove
}

inline std::string bounds_as_for_values(const Intent& in) {
  if (in.body.contains("from")) {
    return "FOR VALUES FROM (" + in.body.value("from", "") + ") TO (" +
           in.body.value("to", "") + ")";
  }
  if (in.body.contains("values")) {
    std::vector<std::string> vs;
    for (const auto& v : in.body["values"]) vs.push_back(v.get<std::string>());
    return "FOR VALUES IN (" + join(vs, ", ") + ")";
  }
  return "DEFAULT";
}

// "RANGE (at)" -> "at". Only a single-column key is handled; a composite one is
// refused rather than half-understood, because a CHECK derived from the first
// column alone would not prove what ATTACH needs and the scan would happen
// anyway -- silently, which is the worst of both.
inline std::string single_partition_key(const std::string& partkeydef) {
  const auto open = partkeydef.find('(');
  const auto close = partkeydef.rfind(')');
  if (open == std::string::npos || close == std::string::npos || close < open) {
    return {};
  }
  const std::string inner = partkeydef.substr(open + 1, close - open - 1);
  if (inner.find(',') != std::string::npos) return {};
  return inner;
}

}  // namespace detail

inline void plan_attach_partition(const Intent& in, const Observations& obs,
                                  Plan& plan, std::vector<Step>& out) {
  const auto parent = in.qualified_table();
  const auto& p = obs.table(parent);
  const auto child =
      in.body.value("schema", "") + "." + in.body.value("partition", "");
  const auto& c = obs.table(child);

  auto fail = [&](const std::string& why, const std::string& detail) {
    Step s;
    s.kind = in.kind_name;
    s.action = Action::kConflict;
    s.why = why;
    plan.conflicts.push_back(detail);
    out.push_back(std::move(s));
  };

  if (!p.value("exists", false)) {
    fail(parent + " does not exist", parent + " does not exist");
    return;
  }
  if (p.value("kind", "") != "partitioned_table") {
    fail(parent + " is not partitioned",
         parent + " is a " + p.value("kind", std::string("relation")) +
             ", not a partitioned table, so nothing can be attached to it.");
    return;
  }
  if (!c.value("exists", false)) {
    fail(child + " does not exist",
         child + " does not exist. attach_partition adopts an existing table; "
                 "create it in an earlier intent.");
    return;
  }
  for (const auto& already : p.value("partitions", json::array())) {
    if (already.get<std::string>() == child) {
      Step s;
      s.kind = in.kind_name;
      s.action = Action::kSatisfied;
      s.why = child + " is already a partition of " + parent;
      out.push_back(std::move(s));
      return;
    }
  }

  const auto key = detail::single_partition_key(p.value("partition_key", ""));
  const auto check_expr = key.empty() ? std::string()
                                      : detail::bounds_as_check(in, key);
  const auto check_name = in.body.value("partition", "") + "_laswell_bound";

  auto emit = [&](TxnClass klass, std::string sql, const std::string& lock,
                  const std::string& why, bool own) {
    Step s;
    s.kind = in.kind_name;
    s.txn_class = klass;
    s.sql.push_back(std::move(sql));
    s.lock = lock;
    s.why = why;
    s.own_transaction = own;
    out.push_back(std::move(s));
  };

  // Step 0: the parent's partitioned indexes the candidate does not match.
  //
  // ATTACH builds one on the candidate for each unmatched parent index, under
  // AccessExclusiveLock on the candidate -- and a pre-existing MATCHING index
  // is attached to the parent's instead, costing nothing. Measured: leaving
  // this in a comparison made a validated CHECK look like it bought nothing,
  // because the index build was the whole cost.
  //
  // Matched structurally -- method, columns, uniqueness, predicate -- not by
  // name and not by presence. A child index over the wrong columns satisfies
  // nothing, and reporting it as a match would be worse than saying nothing.
  auto shape_of = [](const json& ix) {
    std::vector<std::string> cols;
    for (const auto& col : ix.value("columns", json::array())) {
      cols.push_back(col.get<std::string>());
    }
    return ix.value("method", std::string("btree")) + "(" + detail::join(cols, ",") +
           ")" + (ix.value("is_unique", false) ? " unique" : "") +
           " where " + ix.value("predicate", std::string());
  };
  const json parent_indexes = p.value("indexes", json::object());
  const json child_indexes = c.value("indexes", json::object());
  std::set<std::string> child_shapes;
  for (const auto& [cname, cix] : child_indexes.items()) {
    (void)cname;
    child_shapes.insert(shape_of(cix));
  }
  std::vector<std::string> unmatched;
  for (const auto& [iname, pix] : parent_indexes.items()) {
    if (child_shapes.count(shape_of(pix)) == 0) unmatched.push_back(iname);
  }
  if (!unmatched.empty()) {
    plan.warnings.push_back(
        child + " has no index matching " + std::to_string(unmatched.size()) +
        " of " + parent + "'s partitioned index(es): " +
        detail::join(unmatched, ", ") +
        ". ATTACH builds each one on the candidate under AccessExclusiveLock, "
        "and on a large partition that is the dominant cost of the whole "
        "operation. Build them first with create_index intents on " + child +
        ": a matching index is ATTACHED to the parent's rather than rebuilt.");
  }

  // Steps 1 and 2: prove the bounds before ATTACH has to.
  if (key.empty()) {
    plan.warnings.push_back(
        parent + " has a composite or expression partition key (" +
        p.value("partition_key", std::string("unknown")) +
        "), which pg_laswell will not reduce to a CHECK. ATTACH therefore "
        "scans " + child +
        " in full under AccessExclusiveLock. Refusing to guess here is "
        "deliberate: a CHECK over part of the key would not prove what ATTACH "
        "needs, and the scan would happen anyway without anyone being told.");
  } else if (!check_expr.empty()) {
    emit(TxnClass::kRequired,
         "ALTER TABLE " + child + " ADD CONSTRAINT " + check_name + " CHECK (" +
             check_expr + ") NOT VALID;",
         "ShareRowExclusiveLock on " + child + ", briefly; NOT VALID means no scan",
         "step 1 of 4: the CHECK is what lets ATTACH skip its scan, and adding "
         "it NOT VALID costs nothing",
         /*own=*/true);
    emit(TxnClass::kRequired,
         "ALTER TABLE " + child + " VALIDATE CONSTRAINT " + check_name + ";",
         "ShareUpdateExclusiveLock on " + child + " -- does NOT block reads or writes",
         "step 2 of 4: the scan happens here instead, under a lock the "
         "application survives. Measured: ATTACH without this took 98ms on 2M "
         "rows against 0.9ms with it",
         /*own=*/true);
  }

  emit(TxnClass::kRequired,
       "ALTER TABLE " + parent + " ATTACH PARTITION " + child + " " +
           detail::bounds_as_for_values(in) + ";",
       "ShareUpdateExclusiveLock on " + parent +
           " -- other partitions keep serving -- and AccessExclusiveLock on " +
           child,
       check_expr.empty()
           ? "step 3 of 4: attaching, with a full validation scan under the lock"
           : "step 3 of 4: attaching, now a catalog change because the "
             "validated CHECK already proves the bounds",
       /*own=*/true);

  // Step 4: the CHECK is redundant once the partition bound enforces the same
  // thing, and a redundant constraint costs time on every insert forever.
  if (!check_expr.empty()) {
    emit(TxnClass::kRequired,
         "ALTER TABLE " + child + " DROP CONSTRAINT " + check_name + ";",
         "AccessExclusiveLock on " + child + ", briefly",
         "step 4 of 4: the partition bound now enforces what the CHECK did, "
         "and PostgreSQL evaluates both on every insert if it is left behind",
         /*own=*/true);
  }

  const auto deflt = p.value("default_partition", json());
  if (deflt.is_string()) {
    plan.warnings.push_back(
        parent + " has a DEFAULT partition (" + deflt.get<std::string>() +
        "), which ATTACH scans in full to prove it holds no row belonging to "
        "the new bounds. No CHECK on " + child +
        " avoids that -- the cost belongs to the default. Measured at 165ms "
        "with a 2M-row default against 0.9ms without one, and it grows with "
        "the default's size.");
  }
}

inline void plan_detach_partition(const Intent& in, const Observations& obs,
                                  Plan& plan, std::vector<Step>& out) {
  Step step;
  step.kind = in.kind_name;
  struct Emit { std::vector<Step>& o; Step& s; ~Emit() { o.push_back(s); } } emit{out, step};

  const auto parent = in.qualified_table();
  const auto& p = obs.table(parent);
  const auto child =
      in.body.value("schema", "") + "." + in.body.value("partition", "");

  if (!p.value("exists", false)) {
    step.action = Action::kConflict;
    step.why = parent + " does not exist";
    plan.conflicts.push_back(step.why);
    return;
  }

  // A partition left half-detached by an interrupted DETACH CONCURRENTLY is
  // still in pg_inherits and still reports relispartition; only
  // inhdetachpending says the cluster is mid-operation. Finishing it is the
  // only legal next move -- a fresh DETACH is refused.
  for (const auto& pending : p.value("detach_pending", json::array())) {
    if (pending.get<std::string>() != child) continue;
    step.txn_class = TxnClass::kForbidden;
    step.own_transaction = true;
    step.sql.push_back("ALTER TABLE " + parent + " DETACH PARTITION " + child +
                       " FINALIZE;");
    step.lock = "ShareUpdateExclusiveLock on " + parent;
    step.why =
        child + " is mid-detach: a previous DETACH CONCURRENTLY was "
                "interrupted, leaving it in pg_inherits with inhdetachpending "
                "set. FINALIZE is the only legal next step; a fresh DETACH is "
                "refused";
    step.detail["method"] = "finalize";
    return;
  }

  bool attached = false;
  for (const auto& already : p.value("partitions", json::array())) {
    if (already.get<std::string>() == child) attached = true;
  }
  if (!attached) {
    step.action = Action::kSatisfied;
    step.why = child + " is not a partition of " + parent;
    return;
  }

  const long long size = obs.table(child).value("size_estimate", 0LL);
  const int waiters = p.value("lock_waiters", 0);
  const bool concurrent_available = obs.server_version >= 140000;
  const bool small_and_quiet = size < (64LL << 20) && waiters == 0;

  if (concurrent_available && !small_and_quiet) {
    step.txn_class = TxnClass::kForbidden;
    step.own_transaction = true;
    step.sql.push_back("ALTER TABLE " + parent + " DETACH PARTITION " + child +
                       " CONCURRENTLY;");
    step.lock = "ShareUpdateExclusiveLock on " + parent +
                ", so the rest of the table keeps serving";
    step.why =
        "size " + detail::human_bytes(size) +
        (waiters > 0 ? " with " + std::to_string(waiters) + " lock waiter(s)"
                     : "") +
        ": a plain DETACH takes AccessExclusiveLock on the PARENT as well as "
        "the partition, which blocks every query against every partition. "
        "CONCURRENTLY does not -- and it cannot run inside a transaction "
        "block, which is why this step owns its own";
    step.detail["method"] = "concurrent";
    plan.warnings.push_back(
        "DETACH ... CONCURRENTLY on " + child +
        " cannot be rolled back with the rest of a transaction, and an "
        "interruption leaves the partition half-detached (inhdetachpending). "
        "That state is recoverable -- re-running this intent emits FINALIZE -- "
        "but until it is finished the partition is neither in nor out.");
    return;
  }

  step.txn_class = TxnClass::kRequired;
  step.own_transaction = true;
  step.sql.push_back("ALTER TABLE " + parent + " DETACH PARTITION " + child + ";");
  step.lock = "AccessExclusiveLock on " + parent + " AND on " + child;
  step.why =
      concurrent_available
          ? "size " + detail::human_bytes(size) +
                " < 64 MiB and nothing queued: a plain detach is a brief "
                "catalog change, and CONCURRENTLY costs an extra transaction "
                "and a recoverable-but-awkward intermediate state for nothing"
          : "PostgreSQL " + std::to_string(obs.server_version) +
                " has no DETACH ... CONCURRENTLY (it arrived in 14), so the "
                "exclusive lock on the parent is unavoidable -- it blocks "
                "every query against every partition while it is held";
  step.detail["method"] = "plain";
  if (!concurrent_available) {
    plan.warnings.push_back(
        "this server is older than PostgreSQL 14, so detaching " + child +
        " takes AccessExclusiveLock on " + parent +
        " and blocks queries against EVERY partition, not just this one.");
  }
}

// --- add_unique_constraint / add_primary_key -------------------------------
//
// Both are backed by a unique index, so both are planned through one, and the
// decision is which route builds it (spike S15, 18.6).
//
// Plain ADD CONSTRAINT ... UNIQUE takes AccessExclusiveLock AND ShareLock and
// builds the index while holding them. The two-step route -- CREATE UNIQUE
// INDEX CONCURRENTLY, then ADD CONSTRAINT ... USING INDEX -- still takes
// AccessExclusiveLock. Stated precisely, because the usual claim is wrong: it
// does not avoid the lock, it avoids the index BUILD under the lock. Same lock
// level, duration different by orders of magnitude.
//
// Two measured behaviours shape the emitted SQL. USING INDEX renames the index
// to the constraint name, so the index is named after the constraint from the
// start and the rename becomes a no-op rather than a surprise. And ADD PRIMARY
// KEY sets attnotnull itself, verifying it with a full scan under the exclusive
// lock: 75ms on 2M rows nullable versus 0.6ms already NOT NULL. So a primary
// key over a nullable column runs the set_not_null recipe first -- which exists
// precisely to do that scan under a lock that does not block the application.
inline void plan_unique_like(const Intent& in, const Observations& obs,
                             Plan& plan, std::vector<Step>& out) {
  const bool primary = in.kind == IntentKind::kAddPrimaryKey;
  const auto qualified = in.qualified_table();
  const auto& t = obs.table(qualified);
  const auto name = in.body.value("name", "");
  std::vector<std::string> columns;
  for (const auto& c : in.body.value("columns", json::array())) {
    columns.push_back(c.get<std::string>());
  }

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
  const json cols = t.value("columns", json::object());
  for (const auto& c : columns) {
    if (!cols.contains(c)) {
      fail(qualified + "." + c + " does not exist",
           qualified + "." + c + " does not exist, so no constraint can cover it");
      return;
    }
  }

  const json constraints = t.value("constraints", json::object());
  if (constraints.contains(name)) {
    const auto type = constraints[name].value("type", "");
    if (type == (primary ? "p" : "u")) {
      Step s;
      s.kind = in.kind_name;
      s.action = Action::kSatisfied;
      s.why = name + " is already present on " + qualified;
      out.push_back(std::move(s));
      return;
    }
    fail(name + " exists as a different constraint type",
         name + " already exists on " + qualified + " as contype '" + type +
             "', not as " + (primary ? "a primary key" : "a unique constraint") +
             ". Drop it in an earlier intent if it is to be replaced.");
    return;
  }
  if (primary) {
    for (const auto& [cname, c] : constraints.items()) {
      if (c.value("type", "") == "p") {
        fail(qualified + " already has a primary key",
             qualified + " already has primary key \"" + cname +
                 "\". A table has at most one; drop that constraint in an "
                 "earlier intent before adding another.");
        return;
      }
    }
  }

  // A primary key sets NOT NULL itself, and pays for it with a scan under
  // AccessExclusiveLock. Run the recipe that does that scan under a lock the
  // application survives, rather than letting ADD PRIMARY KEY do it the
  // expensive way.
  if (primary) {
    for (const auto& c : columns) {
      if (cols[c].value("not_null", false)) continue;
      Intent nn;
      nn.kind = IntentKind::kSetNotNull;
      nn.kind_name = "set_not_null";
      nn.ordinal = in.ordinal;
      nn.body = json{{"schema", in.body.value("schema", "")},
                     {"table", in.body.value("table", "")},
                     {"column", c}};
      plan_set_not_null(nn, obs, plan, out);
      plan.warnings.push_back(
          qualified + "." + c +
          " is nullable, and a primary key makes it NOT NULL either way. The "
          "plan does that first through the NOT VALID check recipe, because "
          "letting ADD PRIMARY KEY do it costs a full scan under "
          "AccessExclusiveLock -- measured at 75ms on 2M rows against 0.6ms "
          "when the column is already NOT NULL, and it grows with the table.");
    }
  }

  // An existing valid unique index over exactly these columns is the whole
  // build, already paid for.
  std::string backing;
  const json indexes = t.value("indexes", json::object());
  for (const auto& [iname, ix] : indexes.items()) {
    if (!ix.value("is_valid", false) || !ix.value("is_unique", false)) continue;
    if (ix.value("has_expressions", false)) continue;
    if (!ix.value("predicate", std::string()).empty()) continue;
    std::vector<std::string> icols;
    for (const auto& c : ix.value("columns", json::array())) {
      icols.push_back(c.get<std::string>());
    }
    if (icols == columns) { backing = iname; break; }
  }

  const long long size = t.value("size_estimate", 0LL);
  const int waiters = t.value("lock_waiters", 0);
  const bool small_and_quiet = size < (64LL << 20) && waiters == 0;

  auto emit = [&](TxnClass klass, std::vector<std::string> sql,
                  const std::string& lock, const std::string& why, bool own) {
    Step s;
    s.kind = in.kind_name;
    s.txn_class = klass;
    s.sql = std::move(sql);
    s.lock = lock;
    s.why = why;
    s.own_transaction = own;
    s.detail["constraint"] = name;
    out.push_back(std::move(s));
  };

  const std::string kind_sql = primary ? "PRIMARY KEY" : "UNIQUE";
  const std::string quoted_cols = detail::join(columns, ", ");

  if (!backing.empty()) {
    if (backing != name) {
      plan.warnings.push_back(
          "ADD CONSTRAINT ... USING INDEX renames the index: \"" + backing +
          "\" becomes \"" + name +
          "\". Anything naming the old index -- a dashboard, an alert, a "
          "REINDEX script -- stops matching, and PostgreSQL reports it only as "
          "a NOTICE.");
    }
    emit(TxnClass::kRequired,
         {"ALTER TABLE " + qualified + " ADD CONSTRAINT " + name + " " +
          kind_sql + " USING INDEX " + backing + ";"},
         "AccessExclusiveLock on " + qualified + ", but with no index build "
         "and no scan under it",
         "index \"" + backing + "\" already covers (" + quoted_cols +
             ") and is valid and unique, so the constraint is a catalog change "
             "over a build that is already paid for",
         /*own=*/true);
    return;
  }

  if (small_and_quiet) {
    emit(TxnClass::kOptional,
         {"ALTER TABLE " + qualified + " ADD CONSTRAINT " + name + " " +
          kind_sql + " (" + quoted_cols + ");"},
         "AccessExclusiveLock and ShareLock on " + qualified +
             " while the index builds",
         "size " + detail::human_bytes(size) +
             " < 64 MiB and no lock waiters, so one statement is briefer than "
             "a concurrent build plus a second exclusive lock",
         /*own=*/false);
    return;
  }

  // The index is named after the constraint deliberately: USING INDEX renames
  // it to that name anyway, so naming it so from the start turns a surprise
  // into a no-op.
  emit(TxnClass::kForbidden,
       {"CREATE UNIQUE INDEX CONCURRENTLY " + name + " ON " + qualified +
        " (" + quoted_cols + ");"},
       "ShareUpdateExclusiveLock -- does NOT block reads or writes",
       "size " + detail::human_bytes(size) +
           (waiters > 0 ? " with " + std::to_string(waiters) + " lock waiter(s)"
                        : "") +
           ": step 1 of 2, build the index without blocking the application. "
           "It is named after the constraint because USING INDEX would rename "
           "it to that anyway",
       /*own=*/true);
  emit(TxnClass::kRequired,
       {"ALTER TABLE " + qualified + " ADD CONSTRAINT " + name + " " +
        kind_sql + " USING INDEX " + name + ";"},
       "AccessExclusiveLock on " + qualified + ", briefly and with no scan",
       "step 2 of 2: the lock is the same LEVEL a plain ADD CONSTRAINT takes, "
       "and that is the point -- what the concurrent build removed is the "
       "index build held under it, not the lock itself",
       /*own=*/true);
  plan.warnings.push_back(
      "there is no NOT VALID form for a unique constraint, so the index build "
      "IS the validation. If (" + quoted_cols + ") on " + qualified +
      " holds duplicates the concurrent build fails and leaves an INVALID "
      "index named \"" + name +
      "\"; USING INDEX then refuses it, so the job stops rather than creating "
      "a constraint nothing checked. Clear it with a drop_index intent before "
      "retrying.");
}

// --- replace_view ----------------------------------------------------------
//
// The counterpart to the S13 rebuild, and deliberately a separate path because
// the two cost wildly different amounts (spike S14, 18.6).
//
// ADD COLUMN is never blocked by a view -- only ALTER COLUMN ... TYPE and DROP
// COLUMN are. But the new column is invisible to every existing view, INCLUDING
// one written as SELECT *: the star is expanded when the view is created and
// the column list is stored, so a later ADD COLUMN never reaches it. "Add it
// and the views pick it up" is false, and false silently.
//
// Exposing it through CREATE OR REPLACE VIEW costs ONE loss rather than eight:
// comment, per-column comments, grants, column-level grants, INSTEAD OF
// triggers, owner and dependent objects all survive. Only reloptions are reset,
// because the WITH clause replaces the list wholesale -- so a view loses
// security_barrier without an error, which is a security property downgraded in
// silence. That one is restored explicitly.
//
// CREATE OR REPLACE can only APPEND columns; reorder, remove, retype and rename
// are all refused. And there is no CREATE OR REPLACE MATERIALIZED VIEW at all,
// so a materialized view falls back to drop-and-recreate under the full
// eight-way restore.
inline void plan_replace_view(const Intent& in, const Observations& obs,
                              Plan& plan, std::vector<Step>& out) {
  Step step;
  step.kind = in.kind_name;
  struct Emit { std::vector<Step>& o; Step& s; ~Emit() { o.push_back(s); } } emit{out, step};

  const auto schema = in.body.value("schema", "");
  const auto name = in.body.value("name", "");
  const auto qualified = schema + "." + name;
  const auto& v = obs.table(qualified);
  const auto definition = in.body.value("definition", "");
  const auto kind = v.value("kind", "");

  step.txn_class = TxnClass::kRequired;
  step.detail["view"] = qualified;

  if (!v.value("exists", false)) {
    step.sql.push_back("CREATE VIEW " + qualified + " AS " + definition + ";");
    if (in.body.contains("comment")) {
      step.sql.push_back("COMMENT ON VIEW " + qualified + " IS " +
                         detail::quote_literal(in.body.value("comment", "")) + ";");
    }
    step.lock = "no lock on any existing object";
    step.why = qualified + " does not exist, so this creates it";
    step.detail["method"] = "create";
    return;
  }
  if (kind != "view" && kind != "materialized_view") {
    step.action = Action::kConflict;
    step.why = qualified + " is a " + kind + ", not a view";
    plan.conflicts.push_back(
        qualified + " is a " + kind +
        ". replace_view will not turn a table into a view: that is a data loss "
        "dressed as a definition change.");
    return;
  }

  if (kind == "view") {
    step.sql.push_back("CREATE OR REPLACE VIEW " + qualified + " AS " +
                       definition + ";");
    // The one thing CREATE OR REPLACE resets. Re-applied unconditionally when
    // the view had any, because the failure is silent: no error, and a view
    // that was a security barrier quietly stops being one.
    const auto reloptions = v.value("reloptions", json::array());
    if (!reloptions.empty()) {
      std::vector<std::string> o;
      for (const auto& r : reloptions) o.push_back(r.get<std::string>());
      step.sql.push_back("ALTER VIEW " + qualified + " SET (" +
                         detail::join(o, ", ") + ");");
      plan.warnings.push_back(
          "CREATE OR REPLACE VIEW resets a view's options, so " + qualified +
          " would silently lose " + detail::join(o, ", ") +
          ". The plan re-applies them in the same transaction; a hand-written "
          "replace would not, and nothing would report it.");
    }
    step.lock = "AccessExclusiveLock on " + qualified + " only, briefly";
    step.why =
        "CREATE OR REPLACE keeps the comment, grants, column grants, INSTEAD "
        "OF triggers, owner and every dependent object -- so this is the cheap "
        "path, and it is available because the definition only appends columns";
    step.detail["method"] = "replace";
    // Said plainly, because the failure arrives as a confusing message about a
    // column name when what the author actually did was reorder a SELECT list.
    plan.warnings.push_back(
        "CREATE OR REPLACE VIEW can only APPEND columns to " + qualified +
        ". Reordering, removing, retyping or renaming an existing column is "
        "refused by PostgreSQL; such a change needs the view dropped and "
        "rebuilt, and every dependent object with it.");
    return;
  }

  // A materialized view has no replace form -- measured, it is a syntax error
  // -- so it falls back to the destructive path and pays the S13 restore.
  step.own_transaction = true;
  step.sql.push_back("DROP MATERIALIZED VIEW " + qualified + ";");
  step.sql.push_back("CREATE MATERIALIZED VIEW " + qualified + " AS " +
                     definition + ";");
  const auto owner = v.value("owner", "");
  if (!owner.empty()) {
    step.sql.push_back("ALTER MATERIALIZED VIEW " + qualified + " OWNER TO " +
                       detail::quote_identifier(owner) + ";");
  }
  step.lock = "AccessExclusiveLock on " + qualified;
  step.why =
      "PostgreSQL has no CREATE OR REPLACE MATERIALIZED VIEW, so this is a "
      "drop and recreate: the query is re-run in full and everything attached "
      "to the old object has to be put back";
  step.detail["method"] = "drop_and_recreate";
  plan.warnings.push_back(
      qualified +
      " is a materialized view, which has no replace form. It is dropped and "
      "recreated WITH DATA, so its query re-runs in full inside the "
      "transaction -- and its indexes, comments and grants go with it. Restate "
      "them in later intents; pg_laswell restores the owner only, because "
      "anything else here would be it inventing objects the spec never named.");
  return;
}

// --- alter_column_type / drop_column ---------------------------------------

namespace detail {

// Splits "character varying(50)" into {"character varying", 50, -1}.
struct TypeShape {
  std::string base;
  long first = -1;   // length, or numeric precision
  long second = -1;  // numeric scale
};

inline TypeShape type_shape(const std::string& t) {
  TypeShape out;
  const auto open = t.find('(');
  if (open == std::string::npos) {
    out.base = t;
    return out;
  }
  out.base = t.substr(0, open);
  while (!out.base.empty() && out.base.back() == ' ') out.base.pop_back();
  const auto close = t.find(')', open);
  const std::string mods =
      t.substr(open + 1, close == std::string::npos ? std::string::npos
                                                    : close - open - 1);
  const auto comma = mods.find(',');
  try {
    out.first = std::stol(mods.substr(0, comma));
    if (comma != std::string::npos) out.second = std::stol(mods.substr(comma + 1));
  } catch (const std::exception&) {
    out.first = -1;  // a type modifier we do not understand is not a claim
  }
  return out;
}

// Whether the change provably avoids rewriting the table.
//
// Only the cases MEASURED on 18.6 return true; everything else is treated as a
// rewrite. The asymmetry is the whole rule: relaxing or dropping a type
// modifier is free, adding or tightening one rewrites, because every existing
// value must be checked against the new limit. So varchar(50) -> varchar(100)
// and varchar(50) -> text are free, and text -> varchar(200) is not -- which is
// the pair that makes a "same family, therefore cheap" shortcut wrong.
//
// Erring towards "rewrite" is deliberate. Reporting a rewrite that does not
// happen costs a cautious plan; reporting none where one occurs is an
// unplanned outage under AccessExclusiveLock.
inline bool provably_no_rewrite(const std::string& from, const std::string& to) {
  const auto a = type_shape(from), b = type_shape(to);
  // Dropping a length limit: varchar(n) -> text.
  if ((a.base == "character varying" || a.base == "character") &&
      b.base == "text") {
    return true;
  }
  if (a.base != b.base) return false;
  if (b.first == -1) return a.first != -1;   // dropping the modifier entirely
  if (a.first == -1) return false;           // adding one: every value checked
  if (a.second != b.second) return false;    // a scale change re-encodes
  return b.first >= a.first;                 // widening only
}

}  // namespace detail

// The change PostgreSQL refuses outright when a view reads the column, so the
// planner's job is the rebuild around it rather than the statement itself.
inline void plan_alter_column_type(const Intent& in, const Observations& obs,
                                   Plan& plan, std::vector<Step>& out) {
  Step step;
  step.kind = in.kind_name;
  struct Emit { std::vector<Step>& o; Step& s; ~Emit() { o.push_back(s); } } emit{out, step};

  const auto qualified = in.qualified_table();
  const auto& t = obs.table(qualified);
  const auto column = in.body.value("column", "");
  const auto target = in.body.value("type", "");

  if (!t.value("exists", false)) {
    step.action = Action::kConflict;
    step.why = qualified + " does not exist";
    plan.conflicts.push_back(step.why);
    return;
  }
  const json columns = t.value("columns", json::object());
  if (!columns.contains(column)) {
    step.action = Action::kConflict;
    step.why = qualified + "." + column + " does not exist";
    plan.conflicts.push_back(
        step.why + ". A type change names a column that must already be there; "
                   "add_column is the intent for one that is not.");
    return;
  }
  const auto current = columns[column].value("type", "");
  if (current == target) {
    step.action = Action::kSatisfied;
    step.why = qualified + "." + column + " is already " + target;
    return;
  }

  const json views = t.value("dependent_views", json::object());
  const auto rebuild = detail::views_to_rebuild(views, column);
  const bool free_change = detail::provably_no_rewrite(current, target);

  step.txn_class = TxnClass::kRequired;
  // Its own transaction, always. The drops, the change and the recreates have
  // to commit together or a reader finds the views missing -- and a plan that
  // let them merge into a neighbouring group would make that window depend on
  // what happened to be next in the spec.
  step.own_transaction = true;

  detail::emit_view_drops(views, rebuild, step);
  step.sql.push_back("ALTER TABLE " + qualified + " ALTER COLUMN " +
                     detail::quote_identifier(column) + " TYPE " + target +
                     (in.body.contains("using")
                          ? " USING " + in.body.value("using", "")
                          : "") +
                     ";");
  detail::emit_view_recreates(views, rebuild, step);

  step.lock = "AccessExclusiveLock on " + qualified +
              (rebuild.empty() ? "" : " and on every view rebuilt with it");
  if (free_change) {
    step.why = current + " -> " + target +
               " relaxes the type modifier, which PostgreSQL applies without "
               "rewriting the table (measured on 18.6 by relfilenode)";
  } else {
    step.why = current + " -> " + target +
               " is not a provable widening, so assume PostgreSQL rewrites the "
               "whole table: " + detail::human_bytes(t.value("size_estimate", 0LL)) +
               " under AccessExclusiveLock, during which every reader and "
               "writer queues";
    plan.warnings.push_back(
        "alter_column_type on " + qualified + "." + column + " (" + current +
        " -> " + target +
        ") is assumed to rewrite the table. Only a relaxed type modifier is "
        "provably free; this project measures rather than guesses, and the "
        "measured no-rewrite cases are varchar(n)->varchar(m>n), "
        "varchar(n)->text, numeric(p,s)->numeric(p2>=p,s) and a widened "
        "timestamp precision.");
  }
  if (!rebuild.empty()) {
    detail::warn_about_rebuild(views, rebuild, qualified + "." + column, plan);
    step.why += "; " + std::to_string(rebuild.size()) +
                " dependent view(s) block the change and are rebuilt around it";
  }
  step.detail["column"] = column;
  step.detail["from"] = current;
  step.detail["to"] = target;
  step.detail["rewrite"] = free_change ? "no" : "assumed";
  json rebuilt = json::array();
  for (const auto& [level, name] : rebuild) {
    rebuilt.push_back(json{{"view", name}, {"level", level}});
  }
  step.detail["views_rebuilt"] = rebuilt;
}

// Catalog-only in PostgreSQL and therefore fast -- and irreversible, which is
// the part worth saying out loud.
inline void plan_drop_column(const Intent& in, const Observations& obs,
                             Plan& plan, std::vector<Step>& out) {
  Step step;
  step.kind = in.kind_name;
  struct Emit { std::vector<Step>& o; Step& s; ~Emit() { o.push_back(s); } } emit{out, step};

  const auto qualified = in.qualified_table();
  const auto& t = obs.table(qualified);
  const auto column = in.body.value("column", "");

  if (!t.value("exists", false)) {
    step.action = Action::kConflict;
    step.why = qualified + " does not exist";
    plan.conflicts.push_back(step.why);
    return;
  }
  const json columns = t.value("columns", json::object());
  if (!columns.contains(column)) {
    step.action = Action::kSatisfied;
    step.why = "no column " + column + " on " + qualified;
    return;
  }

  const json views = t.value("dependent_views", json::object());
  const auto rebuild = detail::views_to_rebuild(views, column);

  step.txn_class = TxnClass::kRequired;
  step.own_transaction = true;
  detail::emit_view_drops(views, rebuild, step);
  step.sql.push_back("ALTER TABLE " + qualified + " DROP COLUMN " +
                     detail::quote_identifier(column) + ";");
  detail::emit_view_recreates(views, rebuild, step);

  step.lock = "AccessExclusiveLock on " + qualified;
  step.why =
      "dropping a column is catalog-only in PostgreSQL -- the data is not "
      "reclaimed, the attribute is marked dropped -- so the statement is fast "
      "whatever the table's size";
  if (!rebuild.empty()) {
    // The views must be rebuilt WITHOUT the column, and their stored
    // definitions still name it. This is the one case the recipe cannot carry.
    step.action = Action::kConflict;
    std::vector<std::string> names;
    for (const auto& [level, name] : rebuild) { (void)level; names.push_back(name); }
    step.why = column + " is read by " + detail::join(names, ", ");
    plan.conflicts.push_back(
        "cannot drop " + qualified + "." + column + ": it is read by " +
        detail::join(names, ", ") +
        ". Unlike a type change, the recorded view definitions cannot be "
        "replayed here -- they still select the column being removed, so "
        "recreating them verbatim would fail. Change or drop those views in "
        "earlier intents, where the new definition is written down and "
        "reviewable. pg_laswell will not guess at a rewritten view body, and "
        "will not emit CASCADE: measured on 18.6, DROP ... CASCADE under a "
        "three-level stack left zero views standing.");
    step.sql.clear();
    return;
  }
  plan.warnings.push_back(
      "dropping " + qualified + "." + column +
      " is irreversible: the values are gone at commit and no revert can "
      "recover them. If they may be needed, capture them in an earlier intent "
      "first.");
  step.detail["column"] = column;
  step.detail["type"] = columns[column].value("type", "");
}

// --- drop_constraint -------------------------------------------------------
//
// The statement is always the same -- there is no concurrent variant and the
// lock is AccessExclusiveLock in every case -- and for a while that argued this
// kind did not belong here at all.
//
// That was wrong, for two reasons. A repository has to be able to express the
// whole change: a constraint dropped OUTSIDE pg_laswell is unsigned, absent
// from the ledger and missing from the dependency graph, so the repository then
// describes a database that does not exist. And there IS a decision, just not
// about which statement to emit -- whether the drop can succeed at all, what
// else it takes with it, and how far the lock reaches.
inline void plan_drop_constraint(const Intent& in, const Observations& obs,
                                 Plan& plan, std::vector<Step>& out) {
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

  const json constraints = t.value("constraints", json::object());
  if (!constraints.contains(name)) {
    step.action = Action::kSatisfied;
    step.why = "no constraint named " + name + " on " + qualified;
    return;
  }
  const auto& c = constraints[name];
  const auto type = c.value("type", "");

  // PostgreSQL refuses to drop a unique constraint a foreign key depends on,
  // and the dependency is visible here through the shared index. Refusing in
  // the planner, naming the dependent, beats failing at execution with a
  // message that suggests CASCADE.
  const auto dependents = c.value("depended_on_by", json::array());
  if (!dependents.empty()) {
    std::vector<std::string> names;
    for (const auto& d : dependents) names.push_back(d.get<std::string>());
    step.action = Action::kConflict;
    step.why = name + " is depended on by " + detail::join(names, ", ");
    plan.conflicts.push_back(
        "\"" + name + "\" on " + qualified + " cannot be dropped: " +
        detail::join(names, ", ") +
        " depends on its index. Drop the dependent constraint first, in an "
        "earlier intent. pg_laswell will not emit CASCADE, because CASCADE "
        "drops objects the spec never named and a signed change must not do "
        "that.");
    return;
  }

  step.txn_class = TxnClass::kRequired;
  step.sql.push_back("ALTER TABLE " + qualified + " DROP CONSTRAINT " + name + ";");

  // The lock reaches further than the statement reads. Measured on 18.6:
  // dropping a foreign key takes AccessExclusiveLock on the REFERENCED table
  // too -- a stronger lock on the parent than adding the constraint takes, and
  // on a table the statement never names.
  if (type == "f") {
    const auto parent = c.value("references", "");
    step.lock = "AccessExclusiveLock on " + qualified + " AND on " + parent +
                ", which this statement never names";
    step.why = "dropping a foreign key; note the second lock -- it is stronger "
               "on the parent than ADDING the constraint takes, and " + parent +
               " may be the busier table";
  } else {
    step.lock = "AccessExclusiveLock on " + qualified;
    step.why = "dropping a " + std::string(
                   type == "c" ? "check" : type == "u" ? "unique" :
                   type == "x" ? "exclusion" : type == "n" ? "not-null" :
                   type == "p" ? "primary key" : "") +
               " constraint; there is no concurrent variant, so the lock is "
               "brief but exclusive";
  }

  // What goes with it, said before rather than discovered after.
  if (c.value("has_index", false)) {
    plan.warnings.push_back(
        "dropping \"" + name + "\" also drops its index " +
        c.value("index", std::string("(unnamed)")) +
        ", so every lookup that index served becomes a scan. pg_licht "
        "evaluateIndex with hide will say what still plans against it.");
  }
  if (type == "n") {
    plan.warnings.push_back(
        "\"" + name + "\" is a NOT NULL constraint: dropping it makes " +
        qualified + "." + c.value("column", std::string("that column")) +
        " nullable again, which is a change to what the data may contain and "
        "not merely to what is enforced.");
  }
  if (type == "p") {
    plan.warnings.push_back(
        "\"" + name + "\" is the PRIMARY KEY of " + qualified +
        ". Dropping it removes the row identity anything else may rely on, "
        "including a backfill's keyset walk.");
  }

  step.detail["constraint"] = name;
  step.detail["constraint_type"] = type;
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
  const int maintenance_mb = plan.budget.contains("maintenanceWorkMem")
                                 ? plan.budget["maintenanceWorkMem"].value("perStepMb", 0)
                                 : 0;

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
      case IntentKind::kDropConstraint:
        plan_drop_constraint(in, projected, plan, emitted); break;
      case IntentKind::kAlterColumnType:
        plan_alter_column_type(in, projected, plan, emitted); break;
      case IntentKind::kDropColumn:
        plan_drop_column(in, projected, plan, emitted); break;
      case IntentKind::kReplaceView:
        plan_replace_view(in, projected, plan, emitted); break;
      case IntentKind::kAddUniqueConstraint:
      case IntentKind::kAddPrimaryKey:
        plan_unique_like(in, projected, plan, emitted); break;
      case IntentKind::kAttachPartition:
        plan_attach_partition(in, projected, plan, emitted); break;
      case IntentKind::kDetachPartition:
        plan_detach_partition(in, projected, plan, emitted); break;
      case IntentKind::kRenameTable:
      case IntentKind::kRenameColumn:
      case IntentKind::kRenameConstraint:
        plan_rename(in, projected, plan, emitted); break;
      case IntentKind::kCreateTable:
        plan_create_table(in, projected, plan, emitted); break;
      case IntentKind::kDropTable:
        plan_drop_table(in, projected, plan, emitted); break;
      case IntentKind::kDeleteRows:
        plan_delete_rows(in, projected, cfg, plan, emitted); break;
      case IntentKind::kCreatePublication:
      case IntentKind::kAlterPublication:
      case IntentKind::kDropPublication:
      case IntentKind::kCreateSubscription:
      case IntentKind::kAlterSubscription:
      case IntentKind::kDropSubscription:
        plan_replication(in, projected, plan, emitted); break;
      case IntentKind::kCreateObject:
      case IntentKind::kDropObject:
        plan_generic_object(in, projected, plan, emitted); break;
      case IntentKind::kAlterObject:
      case IntentKind::kCreateTableAs:
      case IntentKind::kImportForeignSchema:
      case IntentKind::kSecurityLabel:
      case IntentKind::kAlterDefaultPrivileges:
        plan_final_kinds(in, projected, plan, emitted); break;
      case IntentKind::kCreateMaterializedView:
      case IntentKind::kCreateStatistics:
      case IntentKind::kDropStatistics:
      case IntentKind::kCreateRule:
      case IntentKind::kDropRule:
        plan_relation_extras(in, projected, plan, emitted); break;
      case IntentKind::kSetIdentity:
      case IntentKind::kDropExpression:
      case IntentKind::kSetColumnOptions:
      case IntentKind::kSetTableOptions:
      case IntentKind::kSetLogged:
      case IntentKind::kSetTablespace:
      case IntentKind::kSetAccessMethod:
      case IntentKind::kSetReplicaIdentity:
      case IntentKind::kClusterOn:
        plan_physical(in, projected, plan, emitted); break;
      case IntentKind::kAlterColumnDefault:
      case IntentKind::kDropNotNull:
      case IntentKind::kAlterSequence:
      case IntentKind::kAlterSchema:
      case IntentKind::kAlterExtension:
      case IntentKind::kAlterDomain:
      case IntentKind::kAlterFunction:
      case IntentKind::kAlterView:
      case IntentKind::kAlterPolicy:
      case IntentKind::kSetComment:
      case IntentKind::kSetOwner:
        plan_alter_misc(in, projected, plan, emitted); break;
      case IntentKind::kCreateSchema:
      case IntentKind::kDropSchema:
      case IntentKind::kCreateExtension:
      case IntentKind::kDropExtension:
      case IntentKind::kCreateType:
      case IntentKind::kDropType:
      case IntentKind::kAddEnumValue:
      case IntentKind::kCreateFunction:
      case IntentKind::kDropFunction:
      case IntentKind::kCreateSequence:
      case IntentKind::kDropSequence:
      case IntentKind::kDropView:
        plan_object(in, projected, plan, emitted); break;
      case IntentKind::kCreateTrigger:
      case IntentKind::kDropTrigger:
      case IntentKind::kSetRowSecurity:
      case IntentKind::kCreatePolicy:
      case IntentKind::kDropPolicy:
      case IntentKind::kSetTriggerState:
      case IntentKind::kGrant:
      case IntentKind::kRevoke:
        plan_security(in, projected, plan, emitted); break;
    }
    if (!emitted.empty()) project(in, emitted.front(), projected);

    for (auto& step : emitted) {
      step.ordinal = ordinal++;
      // The operations PostgreSQL documents as using maintenance_work_mem:
      // CREATE INDEX, VACUUM, and ALTER TABLE ADD FOREIGN KEY -- which is the
      // validation step add_foreign_key already emits. Marked here so the
      // executor applies it and the plan shows what it chose; a step that does
      // not use it must not carry it, or the number becomes decoration.
      if (maintenance_mb > 0 && step.action == Action::kApply) {
        const auto sql = detail::join(step.sql, " ");
        if (sql.find("CREATE INDEX") != std::string::npos ||
            sql.find("CREATE UNIQUE INDEX") != std::string::npos ||
            sql.find("VALIDATE CONSTRAINT") != std::string::npos ||
            sql.find("ALTER COLUMN") != std::string::npos) {
          step.detail["maintenance_work_mem_mb"] = maintenance_mb;
          step.why += "; maintenance_work_mem raised to " +
                      std::to_string(maintenance_mb) +
                      "MB for this step (the configured ceiling divided by "
                      "max_concurrent_jobs)";
        }
      }
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
  // Prerequisites last and set apart, because they are the only lines here
  // that ask somebody to go and DO something -- often on another machine --
  // rather than to know something.
  if (!prerequisites.empty()) {
    out += "\n-- before this can run ------------------------------------------\n";
    for (const auto& p : prerequisites) {
      out += std::string(p.value("blocking", false) ? " REQUIRED  " : " advised  ") +
             p.value("kind", "") + ": " + p.value("target", "") + "\n" +
             "    where:  " + p.value("where", "") + "\n" +
             "    needs:  " + p.value("requirement", "") + "\n" +
             "    check:  " + p.value("verify", "") + "\n";
    }
  }
  return out;
}

}  // namespace pglaswell
