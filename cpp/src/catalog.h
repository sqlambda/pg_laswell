#pragma once

// Observation: everything the planner measures, gathered from the catalog.
//
// This is the only file that talks to a database on the planning path, and
// that is deliberate: planner.h is a pure function of (Spec, Observations), so
// plan determinism can be tested against a fixed struct with no server in the
// loop. Anything the planner needs must arrive through Observations.
//
// The JSON is built in SQL rather than in C++ -- one SELECT JSONB_BUILD_OBJECT
// per concern, one row, one column, parsed on arrival. It removes essentially
// all marshalling code, and it is pg_licht's dominant pattern for the same
// reason.

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "observations.h"
#include "session.h"

namespace pglaswell {

using json = nlohmann::json;


namespace detail {

// One statement, all the per-table structure and statistics the planner reads.
//
// size_estimate is relpages*8192 -- free, and only as fresh as estimated_from
// says. The measured size (pg_table_size) is NOT taken here: it opens the
// relation with AccessShareLock, so during a rewrite it queues behind the
// ALTER TABLE. Escalation to it is a separate, conditional call below, which
// keeps the lock hazard in one place with one reason.
inline const char* kTableObservationSql = R"SQL(
WITH target AS (
  SELECT c.oid, c.relname, c.relkind, c.relispartition, c.reltuples, c.relpages,
         n.nspname
    FROM pg_class c
    JOIN pg_namespace n ON n.oid = c.relnamespace
   WHERE n.nspname = $1 AND c.relname = $2
)
SELECT COALESCE(
  (SELECT JSONB_BUILD_OBJECT(
     'exists', true,
     'kind', CASE t.relkind WHEN 'r' THEN 'table'
                            WHEN 'p' THEN 'partitioned_table'
                            WHEN 'm' THEN 'materialized_view'
                            WHEN 'v' THEN 'view'
                            ELSE t.relkind::text END,
     'is_partition', t.relispartition,
     'reltuples', GREATEST(t.reltuples, 0)::bigint,
     'size_estimate', (t.relpages::bigint * 8192),
     'estimated_from', (SELECT GREATEST(s.last_vacuum, s.last_autovacuum,
                                        s.last_analyze, s.last_autoanalyze)
                          FROM pg_stat_all_tables s WHERE s.relid = t.oid),
     'stats', (SELECT JSONB_BUILD_OBJECT(
                 'n_live_tup', s.n_live_tup, 'n_dead_tup', s.n_dead_tup,
                 'n_mod_since_analyze', s.n_mod_since_analyze,
                 'seq_scan', s.seq_scan, 'idx_scan', s.idx_scan,
                 'n_tup_ins', s.n_tup_ins, 'n_tup_upd', s.n_tup_upd,
                 'n_tup_del', s.n_tup_del)
                 FROM pg_stat_all_tables s WHERE s.relid = t.oid),
     'columns', COALESCE((SELECT JSONB_OBJECT_AGG(a.attname, JSONB_BUILD_OBJECT(
                   'type_oid', a.atttypid::bigint,
                   'type', format_type(a.atttypid, a.atttypmod),
                   'not_null', a.attnotnull,
                   'has_default', a.atthasdef))
                   FROM pg_attribute a
                  WHERE a.attrelid = t.oid AND a.attnum > 0 AND NOT a.attisdropped),
                 '{}'::jsonb),
     'indexes', COALESCE((SELECT JSONB_OBJECT_AGG(ic.relname, JSONB_BUILD_OBJECT(
                   'is_valid', i.indisvalid,
                   'is_ready', i.indisready,
                   'is_unique', i.indisunique,
                   'is_primary', i.indisprimary,
                   'definition', pg_get_indexdef(i.indexrelid),
                   'leading_column', (SELECT a.attname FROM pg_attribute a
                                       WHERE a.attrelid = t.oid
                                         AND a.attnum = i.indkey[0])))
                   FROM pg_index i JOIN pg_class ic ON ic.oid = i.indexrelid
                  WHERE i.indrelid = t.oid),
                 '{}'::jsonb),
     'lock_waiters', (SELECT COUNT(*) FROM pg_locks l
                       WHERE l.relation = t.oid AND NOT l.granted)
   ) FROM target t),
  JSONB_BUILD_OBJECT('exists', false))
)SQL";

// Server-wide readings: the connection budget (spike S12) and how busy the
// server is right now. The application's OWN pool limit is the ceiling that
// actually matters and is invisible from here, so it is configuration; what
// this reports is the server's, plus enough to say so honestly.
inline const char* kServerObservationSql = R"SQL(
SELECT JSONB_BUILD_OBJECT(
  'max_connections', current_setting('max_connections')::int,
  'reserved_connections',
      current_setting('superuser_reserved_connections')::int
      + COALESCE(NULLIF(current_setting('reserved_connections', true), '')::int, 0),
  'current_backends', (SELECT COUNT(*) FROM pg_stat_activity),
  'active_backends', (SELECT COUNT(*) FROM pg_stat_activity WHERE state = 'active'),
  'ungranted_locks', (SELECT COUNT(*) FROM pg_locks WHERE NOT granted),
  'oldest_xact_age_s', (SELECT ROUND(EXTRACT(EPOCH FROM now() - MIN(xact_start))::numeric, 1)
                          FROM pg_stat_activity
                         WHERE xact_start IS NOT NULL AND pid <> pg_backend_pid()),
  'is_in_recovery', pg_is_in_recovery(),
  'now', now()
)
)SQL";

inline std::string strip_trailing_semicolon(const std::string& s) {
  auto end = s.find_last_not_of(" \n\t\r");
  if (end == std::string::npos) return {};
  if (s[end] == ';') {
    end = s.find_last_not_of(" \n\t\r", end == 0 ? 0 : end - 1);
    if (end == std::string::npos) return {};
  }
  return s.substr(0, end + 1);
}

}  // namespace detail

// How stale an estimate may be before it is worth paying AccessShareLock for a
// measured size. One hour, and only when the estimate is near a decision
// threshold -- see escalate_size below.
inline constexpr int kEstimateStalenessSeconds = 3600;

// Observation must never queue behind the very lock it is trying to report.
//
// pg_get_indexdef() opens the index relation and so takes AccessShareLock:
// measured 2026-09-05 on 18.6, it blocks outright behind an AccessExclusiveLock,
// while plain pg_class/pg_index/pg_attribute column reads and format_type() do
// not. Without a bound, observing a table mid-ALTER would hang for the length
// of the ALTER -- the planner's own measurement blocking on the thing it is
// planning around, which is the exact hazard documented for pg_table_size().
inline constexpr int kObservationLockTimeoutMs = 2000;

// SQLSTATE 55P03, lock_not_available. Matched by code because libpqxx declares
// no exception class for it -- the same rule the rest of this codebase follows:
// type where libpqxx has a class, SQLSTATE where it does not.
inline bool is_lock_not_available(const pqxx::sql_error& e) {
  return std::string(e.sqlstate()) == "55P03";
}

class Catalog {
 public:
  explicit Catalog(const ConnConfig& cfg, ConnectionCache* cache = nullptr)
      : cfg_(cfg), cache_(cache) {}

  // Gathers everything the planner needs for the tables a spec names.
  Observations observe(const std::vector<std::string>& schemas,
                       const std::vector<std::string>& tables) {
    if (schemas.size() != tables.size()) {
      throw std::runtime_error("observe: schema/table lists differ in length");
    }
    ReadSession s(cfg_, std::nullopt, cache_, kObservationLockTimeoutMs);
    Observations obs;
    obs.server_version = s.server_version();

    const auto server = s.txn().exec(detail::kServerObservationSql);
    if (!server.empty() && !server[0][0].is_null()) {
      obs.server = json::parse(server[0][0].as<std::string>());
    }
    obs.gathered_at = obs.server.value("now", json());

    for (std::size_t i = 0; i < schemas.size(); ++i) {
      const auto qualified = schemas[i] + "." + tables[i];
      if (obs.tables.contains(qualified)) continue;
      try {
        const auto r = pqxx_exec(s.txn(), detail::kTableObservationSql,
                                 pqxx::params{schemas[i], tables[i]});
        if (!r.empty() && !r[0][0].is_null()) {
          obs.tables[qualified] = json::parse(r[0][0].as<std::string>());
        }
      } catch (const pqxx::sql_error& e) {
        if (!is_lock_not_available(e)) throw;
        // Something holds a strong lock on this table right now. That is not a
        // failure to report -- it is the single most important thing to report,
        // because planning a migration against a table already under
        // AccessExclusiveLock is exactly what must not happen.
        obs.tables[qualified] = json{
            {"exists", true},
            {"observation_blocked", true},
            {"blocked_reason",
             "could not read " + qualified + " within " +
                 std::to_string(kObservationLockTimeoutMs) +
                 "ms: something holds a lock that conflicts with AccessShareLock "
                 "(an ALTER TABLE, a plain CREATE INDEX, a VACUUM FULL, or a "
                 "LOCK TABLE). Use pg_licht currentLocks to see what, and plan "
                 "again once it has finished."}};
        // The transaction is aborted after a failed statement, so a fresh one
        // is needed for whatever tables remain.
        s.txn().abort();
        s.renew();
      }
    }
    return obs;
  }

  // Proves a whole plan actually works, in a transaction that never commits.
  //
  // This is the property the project was conceived around: almost all
  // PostgreSQL DDL is transactional, so the earlier steps of a plan can be
  // APPLIED, the later steps checked against the schema they produce, and the
  // whole thing rolled back. Statement-by-statement checking cannot do this --
  // a backfill referencing a column that step 0 adds does not parse until step
  // 0 has run, and that is the ordinary case, not an edge one.
  //
  // Three honest costs, all bounded and all reported:
  //
  //  - It takes the real locks. An ALTER TABLE here takes AccessExclusiveLock,
  //    briefly, and rolls it back. lock_timeout bounds the attempt so a dry run
  //    can never queue behind anything, and it is the same lock the migration
  //    would take anyway -- but it IS a side effect of planning, so it is
  //    opt-outable and the result says whether it ran.
  //  - CREATE INDEX CONCURRENTLY cannot run inside a transaction block, so it
  //    is skipped and reported as unverified rather than silently passed.
  //  - The backfill is EXPLAINed, never executed. ANALYZE is never used.
  struct DryRun {
    bool ran = false;
    std::vector<std::string> problems;
    std::vector<int> unverified_steps;  // txn_forbidden: could not be included
    std::string skipped_reason;
  };

  DryRun dry_run(const std::vector<std::pair<int, std::vector<std::string>>>& steps,
                 const std::vector<bool>& txn_forbidden, int server_version) {
    DryRun out;
    WriteSession w(cfg_);
    // A dry run must never queue: it holds strong locks, and a planning call
    // that blocks the application is worse than one that declines to check.
    ConnConfig probe = cfg_;
    probe.executor.lock_timeout_ms = kObservationLockTimeoutMs;
    WriteSession session(probe);

    try {
      session.begin("pg_laswell/dry-run (rolled back)");
    } catch (const std::exception& e) {
      out.skipped_reason = std::string("could not open a dry-run transaction: ") + e.what();
      return out;
    }

    out.ran = true;
    for (std::size_t i = 0; i < steps.size(); ++i) {
      if (txn_forbidden[i]) {
        out.unverified_steps.push_back(steps[i].first);
        continue;
      }
      for (const auto& raw : steps[i].second) {
        const auto stmt = detail::strip_trailing_semicolon(raw);
        if (stmt.empty()) continue;
        const auto head = stmt.substr(0, stmt.find_first_of(" \n"));
        const bool is_query =
            stmt.rfind("WITH", 0) == 0 || head == "UPDATE" || head == "INSERT" ||
            head == "SELECT" || head == "DELETE" || head == "MERGE";
        try {
          if (is_query) {
            // GENERIC_PLAN (PG16+) plans a statement with $1/$2 placeholders
            // without binding values. On older servers PREPARE catches the
            // same class of error. Never ANALYZE: EXPLAIN must not execute the
            // thing being planned.
            if (server_version >= 160000) {
              session.txn().exec("EXPLAIN (GENERIC_PLAN, FORMAT JSON) " + stmt);
            } else {
              session.txn().exec("PREPARE laswell_dry AS " + stmt);
              session.txn().exec("DEALLOCATE laswell_dry");
            }
          } else {
            session.txn().exec(stmt);  // real DDL, rolled back below
          }
        } catch (const pqxx::sql_error& e) {
          out.problems.push_back("step " + std::to_string(steps[i].first) + ": " +
                                 e.what());
          session.rollback();
          return out;
        }
      }
    }
    // Always. Nothing a dry run does is ever committed.
    session.rollback();
    return out;
  }

  // Escalates one table to a measured size.
  //
  // Kept out of observe() and out of the planner entirely. pg_table_size() is
  // not a physical read -- it stat()s one file per segment -- but it opens the
  // relation with AccessShareLock, so on a table being rewritten by an ALTER
  // TABLE it waits behind AccessExclusiveLock. Paying that unconditionally
  // would mean the planner's own measurement could block on the thing it is
  // trying to plan around.
  void escalate_size(Observations& obs, const std::string& schema,
                     const std::string& table) {
    ReadSession s(cfg_, std::nullopt, cache_);
    const auto r = pqxx_exec(
        s.txn(),
        "SELECT JSONB_BUILD_OBJECT("
        "  'size_measured', pg_table_size(c.oid),"
        "  'indexes_size', pg_indexes_size(c.oid),"
        "  'total_size', pg_total_relation_size(c.oid))"
        "  FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace"
        " WHERE n.nspname = $1 AND c.relname = $2",
        pqxx::params{schema, table});
    const auto qualified = schema + "." + table;
    if (!r.empty() && !r[0][0].is_null() && obs.tables.contains(qualified)) {
      const auto measured = json::parse(r[0][0].as<std::string>());
      for (auto it = measured.begin(); it != measured.end(); ++it) {
        obs.tables[qualified][it.key()] = it.value();
      }
    }
  }

 private:
  ConnConfig cfg_;
  ConnectionCache* cache_ = nullptr;
};

}  // namespace pglaswell
