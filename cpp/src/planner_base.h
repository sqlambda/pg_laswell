#pragma once

// The planner's shared vocabulary: TxnClass, Action, Step, Plan, and the small
// rendering helpers every planner family uses.
//
// Split out of planner.h so the per-family planner headers -- planner_dml.h is
// the first -- can be separate translation-unit-sized files without each one
// dragging in the whole planner. planner.h includes this and remains the single
// entry point: plan_migration() lives there and nowhere else.
//
// THIS HEADER MUST NOT INCLUDE PQXX, for the same reason planner.h must not:
// purity is what makes "deterministic" testable. planner_purity_check.cpp
// asserts it for the whole include graph.

#include <algorithm>
#include <cctype>
#include <cstdio>
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

// A raw "schema.table" observation key, rendered for SQL as "schema"."table".
//
// The two forms have to stay separate and this is the seam between them.
// `Intent::qualified_table()` is the key the observations map, the repository's
// concurrency grouping and the executor's advisory locks all agree on, so it
// must stay exactly as it is; SQL needs the same name quoted. Passing the key
// straight into a statement is what broke on a schema or table whose name is a
// reserved word -- measured on 18.6, "CREATE TABLE user.order" and even
// "SELECT ... FROM user.order" are syntax errors, while every identifier
// PostgreSQL hands back is safe once quoted.
//
// Splitting on the first dot is exact rather than approximate: require_identifier
// permits only [a-z0-9_], so neither half can contain one.
inline std::string quote_qualified(const std::string& qualified) {
  const auto dot = qualified.find('.');
  if (dot == std::string::npos) return quote_identifier(qualified);
  return quote_identifier(qualified.substr(0, dot)) + "." +
         quote_identifier(qualified.substr(dot + 1));
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

}  // namespace pglaswell
