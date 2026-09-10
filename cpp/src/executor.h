#pragma once

// The executor: runs a plan, paced by contention.
//
// Three connections per job, and the third earns its place twice:
//
//   worker        owns every transaction; commits explicitly
//   observer      short READ ONLY transactions, one per tick (see jobs.h)
//   coordination  holds the advisory locks and writes job/step rows
//
// A ledger write on the worker would be rolled back by a worker rollback, so a
// failure would erase its own record. An advisory lock on the worker would be
// dropped by a worker reconnect. Different requirements, different connections.
//
// The one exception is laswell.backfill_cursor, which is written BY THE WORKER
// inside the same transaction as the data it describes. Cursor and data must be
// atomically consistent or a crash produces re-applied or skipped rows. Putting
// every ledger write on one connection is the natural instinct and it is wrong
// for exactly this table.

#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "catalog.h"  // Catalog::stream_copy, shared with the dry run
#include "jobs.h"
#include "ledger.h"
#include "planner.h"
#include "session.h"

namespace pglaswell {

namespace detail {

inline long long steady_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// SQLSTATE 55P03, lock_not_available. libpqxx declares no class for it, so the
// code is the only handle -- type where libpqxx has a class, SQLSTATE where it
// does not.
inline bool is_lock_timeout(const pqxx::sql_error& e) {
  return std::string(e.sqlstate()) == "55P03";
}

// SQLSTATE 57014, query_canceled. Raised BOTH by an explicit cancel and by
// statement_timeout, and libpqxx has no class for either, so the two cannot be
// told apart from the error alone -- the worker must consult its own
// cancel_requested flag. Reporting every 57014 as "we cancelled you" would
// mislabel every statement timeout as deliberate.
inline bool is_query_canceled(const pqxx::sql_error& e) {
  return std::string(e.sqlstate()) == "57014";
}

inline std::string strip_semicolon(const std::string& s) {
  auto end = s.find_last_not_of(" \n\t\r");
  if (end == std::string::npos) return {};
  if (s[end] == ';') {
    if (end == 0) return {};
    end = s.find_last_not_of(" \n\t\r", end - 1);
    if (end == std::string::npos) return {};
  }
  return s.substr(0, end + 1);
}

}  // namespace detail

class Executor {
 public:
  Executor(ConnConfig cfg, std::shared_ptr<Job> job, Ledger* ledger)
      : cfg_(std::move(cfg)), job_(std::move(job)), ledger_(ledger) {}

  void run() {
    try {
      execute();
    } catch (const std::exception& e) {
      fail(json{{"error", e.what()},
                {"hint", "See laswell.step for the statement that failed."}});
    }
    {
      std::lock_guard<std::mutex> lock(job_->m);
      job_->finished = std::chrono::steady_clock::now();
      job_->has_finished = true;
    }
    // Release the worker connection pointer before it is destroyed, so the
    // observer can never call cancel_query() on a dangling connection.
    {
      std::lock_guard<std::mutex> lock(job_->cancel_m);
      job_->worker_conn = nullptr;
    }
  }

 private:
  void execute() {
    WriteSession worker(cfg_);
    {
      std::lock_guard<std::mutex> lock(job_->cancel_m);
      job_->worker_conn = &worker.connection();
    }
    job_->pacing.worker_pid = worker.backend_pid();
    job_->state = JobState::kRunning;

    const auto& steps = job_->plan["steps"];
    int current_group = -1;
    bool group_open = false;

    for (const auto& step : steps) {
      if (job_->pacing.cancel_stop.load()) {
        if (group_open) worker.rollback();
        cancelled();
        return;
      }

      const int ordinal = step.value("ordinal", 0);
      const int group = step.value("txnGroup", 0);
      const auto kind = step.value("kind", "");
      const auto txn_class = step.value("txnClass", "");
      const auto action = step.value("action", "apply");

      if (action == "satisfied") {
        // Recorded with the observation that justified it, so a later reader
        // can tell "we did not need to" from "we forgot to".
        record_step(ordinal, step, "skipped_satisfied", 0, json::object());
        continue;
      }
      if (action != "apply") continue;

      const bool needs_own = txn_class == "txn_forbidden" ||
                             txn_class == "own_txn_per_batch";
      if (group != current_group || needs_own) {
        if (group_open) {
          worker.commit();
          group_open = false;
        }
        current_group = group;
      }

      if (txn_class == "own_txn_per_batch") {
        run_backfill(worker, ordinal, step);
        continue;
      }
      if (txn_class == "txn_forbidden") {
        run_nontransactional(worker, ordinal, step);
        continue;
      }
      if (kind == "verify_index_valid") {
        // Runs in its own read, not inside the group, because the CIC that
        // preceded it committed outside any transaction.
        verify_index_valid(worker, ordinal, step);
        continue;
      }
      // COPY carries a payload after its statement, so it cannot go through
      // exec() with the rest. It still belongs to the current transaction
      // group -- COPY is transactional, and rolls back with everything else.
      if (kind == "copy_rows" && step.value("detail", json::object())
                                     .contains("copy_rows")) {
        if (!group_open) {
          worker.begin(app_name(ordinal));
          group_open = true;
        }
        run_copy(worker, ordinal, step);
        continue;
      }

      if (!group_open) {
        worker.begin(app_name(ordinal));
        group_open = true;
      }
      run_in_transaction(worker, ordinal, step);
    }

    if (group_open) worker.commit();
    succeed();
  }

  std::string app_name(int ordinal) const {
    return "pg_laswell/" + job_->job_id + "/step-" + std::to_string(ordinal);
  }

  void run_in_transaction(WriteSession& w, int ordinal, const json& step) {
    const auto started = detail::steady_ms();
    long long rows = 0;
    for (const auto& raw : step.value("sql", json::array())) {
      const auto stmt = detail::strip_semicolon(raw.get<std::string>());
      if (stmt.empty()) continue;
      try {
        const auto r = w.txn().exec(stmt);
        rows += r.affected_rows();
      } catch (const pqxx::sql_error& e) {
        // A lock timeout is a retryable OUTCOME, not a broken migration: "we
        // did not get the lock this second" and "this migration is wrong" are
        // different facts and must not share a state.
        if (detail::is_lock_timeout(e)) {
          record_step(ordinal, step, "lock_not_acquired", 0,
                      json{{"sqlstate", "55P03"},
                           {"note",
                            "the lock was not available within lock_timeout; "
                            "nothing was applied. Retry when the table is "
                            "quieter -- pg_licht currentLocks names the holder."}});
          throw std::runtime_error(
              "step " + std::to_string(ordinal) +
              " could not acquire its lock within lock_timeout");
        }
        record_step(ordinal, step, "failed", 0,
                    json{{"sqlstate", e.sqlstate()}, {"error", e.what()}});
        throw;
      }
    }
    record_step(ordinal, step, "succeeded", rows,
                json{{"elapsedMs", detail::steady_ms() - started}});
  }

  // COPY ... FROM STDIN.
  //
  // The one step whose work is not entirely in its SQL: the statement opens the
  // stream and the rows follow it over the protocol. Both halves are recorded
  // -- the statement in laswell.step.sql, the row count in the step's result --
  // because "what ran" for a COPY is genuinely two things.
  //
  // pqxx::stream_to is the only sanctioned way to send COPY data through
  // libpqxx 7.10; connection::raw_connection() and write_copy_line() are
  // private, reachable only through internal gate classes. It builds the
  // statement itself, which is why the planner emits exactly the form probed
  // out of it -- COPY t(cols) FROM STDIN, no WITH clause -- and why spec.h
  // refuses ON_ERROR and friends rather than accepting keys that cannot travel.
  void run_copy(WriteSession& w, int ordinal, const json& step) {
    const auto started = detail::steady_ms();
    long long written = 0;
    try {
      // Catalog::stream_copy, the same call the dry run makes. This had its
      // own copy of the streaming loop, which is how the two came to quote
      // column names slightly differently while both being correct only
      // because require_identifier restricts them to [a-z0-9_].
      Catalog::stream_copy(w, step.value("detail", json::object()), written);
    } catch (const pqxx::sql_error& e) {
      record_step(ordinal, step, "failed", 0,
                  json{{"sqlstate", e.sqlstate()},
                       {"error", e.what()},
                       {"rowsSent", written},
                       {"note",
                        "a COPY is all-or-nothing within its transaction: none "
                        "of the rows above were kept, whatever the row number "
                        "in the error says"}});
      throw;
    }
    record_step(ordinal, step, "succeeded", written,
                json{{"elapsedMs", detail::steady_ms() - started},
                     {"rowsCopied", written}});
  }

  // CREATE INDEX CONCURRENTLY and DROP INDEX CONCURRENTLY.
  void run_nontransactional(WriteSession& w, int ordinal, const json& step) {
    const auto started = detail::steady_ms();
    // A concurrent build on a large table legitimately runs for hours, so the
    // statement timeout has to come off. It must be a SESSION-level SET with an
    // explicit RESET, never SET LOCAL: measured 2026-09-05 (spike S6), SET
    // LOCAL is silently a no-op on a nontransaction -- PostgreSQL warns and
    // libpqxx does not raise, so the timeout would appear set and would not be.
    // maintenance_work_mem, when the planner decided this step uses it. Nested
    // inside the statement_timeout guard and for the same reason: SET LOCAL is
    // silently a no-op on a nontransaction (spike S6), so both must be
    // session-level SETs with explicit RESETs, which with_session_setting does.
    const int mwm = step.value("detail", json::object())
                        .value("maintenance_work_mem_mb", 0);
    try {
      w.with_session_setting("statement_timeout", "0", [&] {
        return w.with_session_setting(
            "maintenance_work_mem",
            mwm > 0 ? std::to_string(mwm) + "MB" : std::string(),
            [&] {
        for (const auto& raw : step.value("sql", json::array())) {
          const auto stmt = detail::strip_semicolon(raw.get<std::string>());
          if (stmt.empty()) continue;
          w.exec_nontransactional(stmt);
        }
        return 0;
            });
      });
    } catch (const pqxx::sql_error& e) {
      record_step(ordinal, step, "failed", 0,
                  json{{"sqlstate", e.sqlstate()},
                       {"error", e.what()},
                       {"note",
                        "a failed CREATE INDEX CONCURRENTLY leaves an INVALID "
                        "index that still appears in pg_indexes. Re-planning "
                        "will emit DROP INDEX CONCURRENTLY before rebuilding."}});
      throw;
    }
    record_step(ordinal, step, "succeeded", 0,
                json{{"elapsedMs", detail::steady_ms() - started}});
  }

  // The step that makes a concurrent build honest.
  //
  // A CIC can return WITHOUT ERROR and leave an invalid index: measured
  // 2026-09-05 (spike S5), the failed index appears in pg_indexes like any
  // other, with indisvalid = false. Reporting success because the statement
  // returned would be reporting a failure that looks like a success.
  void verify_index_valid(WriteSession& w, int ordinal, const json& step) {
    const auto index = index_name_for(ordinal);
    if (index.second.empty()) {
      record_step(ordinal, step, "succeeded", 0,
                  json{{"note", "no index to verify"}});
      return;
    }
    w.begin(app_name(ordinal));
    const auto r = pqxx_exec(
        w.txn(),
        "SELECT i.indisvalid, i.indisready FROM pg_index i"
        "  JOIN pg_class c ON c.oid = i.indexrelid"
        "  JOIN pg_namespace n ON n.oid = c.relnamespace"
        " WHERE c.relname = $1 AND n.nspname = $2",
        pqxx::params{index.second, index.first});
    const bool present = !r.empty();
    const bool valid = present && r[0][0].as<bool>();
    w.commit();

    if (!valid) {
      record_step(ordinal, step, "failed", 0,
                  json{{"index", index.first + "." + index.second},
                       {"present", present},
                       {"indisvalid", valid},
                       {"error",
                        present ? "the index exists but is INVALID: the "
                                  "concurrent build did not complete"
                                : "the index does not exist after the build"},
                       {"hint",
                        "Re-plan: pg_laswell will emit DROP INDEX CONCURRENTLY "
                        "before rebuilding. Never a plain DROP INDEX -- that "
                        "takes AccessExclusiveLock, which is what the "
                        "concurrent build was chosen to avoid."}});
      throw std::runtime_error("index " + index.first + "." + index.second +
                               " is invalid after a concurrent build");
    }
    record_step(ordinal, step, "succeeded", 0, json{{"indisvalid", true}});
  }

  std::pair<std::string, std::string> index_name_for(int ordinal) const {
    // The verify step follows its build; look back for the create_index step.
    for (int i = ordinal - 1; i >= 0; --i) {
      for (const auto& s : job_->plan["steps"]) {
        if (s.value("ordinal", -1) != i) continue;
        if (s.value("kind", "") != "create_index") continue;
        return {s["detail"].value("schema", ""), s["detail"].value("index", "")};
      }
    }
    return {};
  }

  // The paced backfill.
  void run_backfill(WriteSession& w, int ordinal, const json& step) {
    const auto& e = cfg_.executor;
    const auto detail_json = step.value("detail", json::object());
    const auto sql = detail::strip_semicolon(step["sql"][0].get<std::string>());
    const auto key_column = detail_json.value("key", "id");

    resume_qualified_ = detail_json.value("qualified", "");
    resume_key_ = key_column;
    resume_where_ = step["detail"].value("where", "");
    std::string cursor = resume_cursor(ordinal);
    long long rows_done = 0;      // includes the open transaction
    long long rows_committed = 0; // survives a crash
    int commits = 0;
    std::map<std::string, int> reasons{
        {"interval", 0}, {"lock_waiter", 0}, {"batch_cap", 0}, {"final", 0}};

    // Invariants are evaluated BEFORE the first batch and again after the
    // last, on the worker connection. Both readings are stored, so a failure
    // says what the value was and what it became rather than merely that
    // something changed.
    //
    // They are not a substitute for verify_remaining: that asks "is the work
    // finished", this asks "did the work break something it should not have".
    // A backfill can complete every row and still halve the revenue total.
    json invariants_before = json::object();
    const auto invariants = detail_json.value("assert_invariants", json::array());
    if (!invariants.empty()) {
      w.begin(app_name(ordinal));
      for (const auto& inv : invariants) {
        invariants_before[inv.value("name", "")] =
            scalar(w, inv.value("query", ""));
      }
      w.commit();
    }

    const auto started = detail::steady_ms();
    bool done = false;

    while (!done) {
      if (job_->pacing.cancel_stop.load()) {
        cancelled();
        return;
      }
      wait_while_paused(ordinal);
      if (job_->pacing.cancel_stop.load()) {
        cancelled();
        return;
      }

      w.begin(app_name(ordinal));
      const auto txn_started = detail::steady_ms();
      long long rows_this_txn = 0;
      CommitReason reason = CommitReason::kInterval;

      while (true) {
        // batch_rows bounds how long ONE STATEMENT runs, and therefore
        // worst-case waiter latency. The commit triggers below bound how long
        // ONE TRANSACTION runs, and therefore how long locks accumulate.
        // Conflating them is the design error to avoid.
        const int batch = job_->pacing.throttled.load()
                              ? std::max(1, e.batch_rows / 2)
                              : e.batch_rows;
        long long affected = 0;
        try {
          const auto r = pqxx_exec(w.txn(), sql, pqxx::params{cursor, batch});
          affected = static_cast<long long>(r.size());
          for (const auto& row : r) {
            cursor = row[0].as<std::string>();
          }
        } catch (const pqxx::sql_error& ex) {
          if (detail::is_query_canceled(ex) &&
              job_->pacing.cancel_requested.exchange(false)) {
            // OUR cancel, not a statement timeout. The flag is what tells them
            // apart; the SQLSTATE cannot.
            w.rollback();
            reasons["lock_waiter"] += 1;
            add_warning("a batch was cancelled after a waiter was blocked "
                        "longer than max_waiter_wait_ms");
            break;
          }
          record_step(ordinal, step, "failed", rows_done,
                      json{{"sqlstate", ex.sqlstate()},
                           {"error", ex.what()},
                           {"cursor", cursor},
                           {"rowsDone", rows_done}});
          throw;
        }

        rows_this_txn += affected;
        rows_done += affected;
        // Published after every BATCH, not only after every commit. With a
        // long commit interval an entire backfill can run in one transaction,
        // and an agent polling jobStatus would otherwise see zero progress
        // throughout -- indistinguishable from a stuck job.
        publish_backfill(ordinal, rows_done, rows_committed, commits, cursor,
                         reasons, detail_json, "in_flight");
        if (affected == 0) {
          done = true;
          reason = CommitReason::kFinal;
          break;
        }

        // Priority order: a waiter first, then the interval, then the row cap.
        if (job_->pacing.direct_waiters.load() > 0) {
          reason = CommitReason::kLockWaiter;
          break;
        }
        if (detail::steady_ms() - txn_started >= e.commit_interval_ms) {
          reason = CommitReason::kInterval;
          break;
        }
        if (rows_this_txn >= e.batch_cap_rows) {
          reason = CommitReason::kBatchCap;
          break;
        }
      }

      if (w.in_transaction()) {
        // The cursor rides in the SAME transaction as the data it describes.
        // This is why backfill_cursor is written by the worker while job and
        // step rows are written by the coordination connection: cursor and data
        // must be atomically consistent, or a crash produces re-applied or
        // skipped rows.
        pqxx_exec(w.txn(),
                  "INSERT INTO laswell.backfill_cursor"
                  "  (job_id, ordinal, last_key, rows_done, commits)"
                  "  VALUES ($1::uuid, $2, $3, $4, 1)"
                  "  ON CONFLICT (job_id, ordinal) DO UPDATE"
                  "  SET last_key = EXCLUDED.last_key,"
                  "      rows_done = EXCLUDED.rows_done,"
                  "      commits = laswell.backfill_cursor.commits + 1,"
                  "      updated_at = now()",
                  pqxx::params{job_->job_id, ordinal, cursor, rows_done});
        w.commit();
        ++commits;
        rows_committed = rows_done;
        reasons[to_string(reason)] += 1;
      }

      publish_backfill(ordinal, rows_done, rows_committed, commits, cursor,
                       reasons, detail_json, to_string(reason));
    }

    json d{{"elapsedMs", detail::steady_ms() - started},
           {"rowsDone", rows_done},
           {"commits", commits},
           {"commitReasons", reasons},
           {"finalCursor", cursor},
           {"key", key_column}};
    // What this step DID, and deliberately not what bloat resulted.
    //
    // rows updated is not dead tuples. Autovacuum runs during the backfill and
    // removes some as it goes -- which is partly the point of pacing into short
    // transactions, since each commit makes the previous row versions
    // removable. HOT updates do not bloat indexes at all when the updated
    // column is unindexed and there is fillfactor room. And other workload
    // contributes to the same counter. Reporting rows_affected as bloat would
    // be a confidently wrong number, which is the one thing this tool must
    // never produce.
    d["bloatNote"] =
        "this updated " + std::to_string(rows_done) +
        " rows, and each update leaves a dead row version behind. How many "
        "remain is NOT derivable from that figure -- autovacuum reclaims some "
        "during the run, HOT updates may not bloat indexes at all, and other "
        "workload contributes too. Read pg_licht tableBloat or "
        "tableStats.n_dead_tup for the actual state.";

    // Completeness, checked on the WORKER connection. An imported snapshot on
    // another connection cannot see this transaction's rows (spike S1), so
    // there is nowhere else this check could run.
    if (detail_json.contains("verify_remaining")) {
      w.begin(app_name(ordinal));
      const auto q =
          "SELECT count(*) FROM " + step["detail"].value("qualified", "") ;
      (void)q;
      const auto remaining = w.txn().exec(
          "SELECT count(*) FROM " + qualified_for(step) + " WHERE " +
          detail_json["verify_remaining"].get<std::string>());
      const auto n = remaining[0][0].as<long long>();
      w.commit();
      d["verifyRemaining"] = n;
      if (n != 0) {
        d["error"] = "verify_remaining still selects " + std::to_string(n) +
                     " rows after the backfill completed";
        record_step(ordinal, step, "failed", rows_done, d);
        throw std::runtime_error(d["error"].get<std::string>());
      }
    }

    if (!invariants.empty()) {
      json after = json::object();
      json broken = json::array();
      w.begin(app_name(ordinal));
      for (const auto& inv : invariants) {
        const auto name = inv.value("name", "");
        const auto now = scalar(w, inv.value("query", ""));
        after[name] = now;
        if (now != invariants_before.value(name, json())) {
          broken.push_back(json{{"name", name},
                                {"query", inv.value("query", "")},
                                {"before", invariants_before.value(name, json())},
                                {"after", now}});
        }
      }
      w.commit();
      d["invariantsBefore"] = invariants_before;
      d["invariantsAfter"] = after;
      if (!broken.empty()) {
        d["brokenInvariants"] = broken;
        d["error"] = "the backfill changed something an invariant said it "
                     "must not";
        record_step(ordinal, step, "failed", rows_done, d);
        throw std::runtime_error(
            "invariant \"" + broken[0]["name"].get<std::string>() +
            "\" went from " + broken[0]["before"].dump() + " to " +
            broken[0]["after"].dump());
      }
    }

    record_step(ordinal, step, "succeeded", rows_done, d);
  }

  // One value, as text. Text rather than a typed read because an invariant may
  // be any scalar -- a count, a sum, a checksum -- and comparing the rendered
  // form is exactly what "unchanged" means for all of them. numeric in
  // particular has no lossless C++ type.
  static json scalar(WriteSession& w, const std::string& query) {
    const auto r = w.txn().exec(query);
    if (r.empty() || r[0].size() == 0 || r[0][0].is_null()) return json();
    return r[0][0].as<std::string>();
  }

  static std::string qualified_for(const json& step) {
    return step["detail"].value("qualified", "");
  }

  void wait_while_paused(int ordinal) {
    if (!job_->pacing.paused.load()) {
      if (job_->state.load() == JobState::kPausedContention) {
        job_->state = JobState::kRunning;
      }
      return;
    }
    job_->state = JobState::kPausedContention;
    const auto paused_at = detail::steady_ms();
    while (job_->pacing.paused.load() && !job_->pacing.cancel_stop.load()) {
      if (detail::steady_ms() - paused_at >
          static_cast<long long>(cfg_.executor.max_pause_s) * 1000) {
        // The cursor is committed, so aborting loses nothing and the job is
        // resumable. Continuing to hold a place in a pile-up would not be.
        job_->state = JobState::kAbortedContention;
        add_warning("aborted after " + std::to_string(cfg_.executor.max_pause_s) +
                    "s paused for contention; the cursor is committed, so a "
                    "new job for the same spec digest resumes from it");
        throw std::runtime_error("aborted: paused for contention too long");
      }
      std::this_thread::sleep_for(
          std::chrono::milliseconds(cfg_.executor.observer_tick_ms));
    }
    if (!job_->pacing.cancel_stop.load()) job_->state = JobState::kRunning;
    (void)ordinal;
  }

  // Set before resume_cursor runs, so the staleness check knows what the
  // backfill is actually looking for.
  std::string resume_qualified_;
  std::string resume_key_;
  std::string resume_where_;

  bool cursor_is_stale(ReadSession& r, const std::string& from) {
    if (resume_qualified_.empty() || resume_where_.empty()) return false;
    try {
      const auto q = "SELECT EXISTS (SELECT 1 FROM " + resume_qualified_ +
                     " WHERE " + resume_key_ + " <= $1::bigint AND (" +
                     resume_where_ + "))";
      const auto res = pqxx_exec(r.txn(), q, pqxx::params{from});
      return !res.empty() && res[0][0].as<bool>();
    } catch (const std::exception&) {
      // If the check cannot run, do not resume on a cursor we could not
      // validate: starting over is slow, and starting wrong is silent.
      return true;
    }
  }

  std::string resume_cursor(int ordinal) {
    // A prior UNFINISHED job for the same spec digest resumes from its cursor.
    // A digest mismatch is never resumed: restarting a half-done backfill is
    // usually harmless and sometimes a double-apply, and the tool cannot tell
    // which.
    //
    // The state filter is load-bearing, and its absence was a real bug. A
    // SUCCEEDED job's cursor sits at the end of the table, so resuming from it
    // makes the next run skip everything and report success. That is exactly
    // the failure verify_remaining exists to catch, and it did -- but a
    // completeness check should be the second line of defence, not the first.
    //
    // Only an interrupted, failed, cancelled or contention-aborted job leaves a
    // cursor worth resuming. After a success the backfill's own predicate is
    // the idempotence mechanism: a re-run matches nothing and costs one scan.
    try {
      ReadSession r(cfg_, std::nullopt, nullptr, 2000);
      const auto res = pqxx_exec(
          r.txn(),
          "SELECT c.last_key FROM laswell.backfill_cursor c"
          "  JOIN laswell.job j ON j.job_id = c.job_id"
          "  JOIN laswell.migration m ON m.migration_id = j.migration_id"
          " WHERE m.spec_digest = $1 AND c.ordinal = $2"
          "   AND (j.finished_at IS NULL"
          "        OR j.state IN ('failed','cancelled','interrupted',"
          "                       'aborted_contention'))"
          " ORDER BY c.updated_at DESC LIMIT 1",
          pqxx::params{job_->spec_digest, ordinal});
      if (!res.empty() && !res[0][0].is_null()) {
        const auto from = res[0][0].as<std::string>();
        // A cursor is a claim: "everything at or below this key is done". Check
        // it before trusting it. EXISTS rather than COUNT, so it stops at the
        // first counterexample.
        //
        // Without this, a stale cursor makes the run skip everything and only
        // verify_remaining notices, at the very end. That is a real failure
        // mode -- it happened -- and converting a late failure into an early
        // self-correction is worth one bounded existence check per resume.
        if (cursor_is_stale(r, from)) {
          add_warning("ignoring the recorded cursor for step " +
                      std::to_string(ordinal) + " at key " + from +
                      ": rows at or below it still match the backfill's filter, "
                      "so the cursor does not describe this table. Starting "
                      "from the beginning.");
          return "0";
        }
        add_warning("resuming step " + std::to_string(ordinal) + " from key " +
                    from + " (a prior job for this spec digest did not finish)");
        return from;
      }
    } catch (const std::exception&) {
      // No prior cursor, or the ledger is unreadable: start from the beginning.
    }
    return "0";
  }

  void publish_backfill(int ordinal, long long rows_done, long long rows_committed,
                        int commits, const std::string& cursor,
                        const std::map<std::string, int>& reasons,
                        const json& detail_json, const std::string& last_reason) {
    json b{{"ordinal", ordinal},
           {"rowsDone", std::to_string(rows_done)},
           // The distinction that matters on a crash: only committed rows
           // survive, and only the committed cursor is resumable.
           {"rowsCommitted", std::to_string(rows_committed)},
           {"rowsEstimated", std::to_string(detail_json.value("rows_estimated", 0LL))},
           {"cursor", cursor},
           {"commits", commits},
           {"lastCommitReason", last_reason},
           {"commitReasons", reasons}};
    const auto est = detail_json.value("rows_estimated", 0LL);
    if (est > 0) {
      b["percent"] = std::min(100.0, 100.0 * static_cast<double>(rows_done) /
                                         static_cast<double>(est));
    }
    std::lock_guard<std::mutex> lock(job_->m);
    job_->backfill = std::move(b);
  }

  void record_step(int ordinal, const json& step, const std::string& state,
                   long long rows, const json& extra) {
    json entry{{"ordinal", ordinal},
               {"kind", step.value("kind", "")},
               {"txnGroup", step.value("txnGroup", 0)},
               {"txnClass", step.value("txnClass", "")},
               {"state", state},
               {"why", step.value("why", "")},
               {"rowsAffected", rows},
               {"detail", extra}};
    {
      std::lock_guard<std::mutex> lock(job_->m);
      job_->steps.push_back(entry);
    }
    if (ledger_ == nullptr) return;
    try {
      ledger_->record_step(job_->job_id, ordinal, step, state, rows, extra);
    } catch (const std::exception&) {
      // The ledger write is on the coordination connection precisely so a
      // worker rollback cannot erase it; if it fails anyway, the in-memory
      // status still carries the step.
    }
  }

  void add_warning(const std::string& w) {
    std::lock_guard<std::mutex> lock(job_->m);
    job_->warnings.push_back(w);
  }

  // THE LEDGER FIRST, THEN THE STATE, for all three terminal transitions.
  //
  // A caller polls jobStatus until the state is terminal and then reads the
  // ledger -- that is the documented shape, and it is what the deployment
  // runner and every agent does. Flipping the state first opens a window where
  // jobStatus says "succeeded" and laswell.migration still says the job is
  // running, so the two answers disagree and which one a caller gets depends on
  // timing it cannot see. Found on CI, where a repository scan taken straight
  // after a successful wait reported in_progress; it had never lost the race on
  // a developer's machine, which is exactly why it survived.
  //
  // Ordered this way, a terminal state MEANS "durably recorded as terminal".
  void succeed() {
    if (ledger_) ledger_->finish_job(job_->job_id, "succeeded", json());
    job_->state = JobState::kSucceeded;
  }

  void cancelled() {
    if (ledger_) ledger_->finish_job(job_->job_id, "cancelled", json());
    job_->state = JobState::kCancelled;
  }

  void fail(const json& error) {
    // The state this job is ending in, decided before it is published: an
    // abort for contention keeps its own state rather than being overwritten
    // with a generic failure.
    const auto ending = job_->state.load() == JobState::kAbortedContention
                            ? JobState::kAbortedContention
                            : JobState::kFailed;
    {
      std::lock_guard<std::mutex> lock(job_->m);
      job_->error = error;
    }
    if (ledger_) {
      ledger_->finish_job(job_->job_id, to_string(ending), error);
    }
    job_->state = ending;
  }

  ConnConfig cfg_;
  std::shared_ptr<Job> job_;
  Ledger* ledger_;
};

}  // namespace pglaswell
