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
