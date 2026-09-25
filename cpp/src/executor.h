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

// ONE BATCH of a paced step, and the only implementation of it.
//
// The executor calls this, and so do the tests. That is deliberate: the
// conformance harness already carried the comment "a batch statement that only
// works inside the executor is a batch statement nobody can check", and it had
// its own copy of the loop to say so. Two copies became three when a batch grew
// a second statement, and two of them broke. One function cannot drift from
// itself.
struct BatchOutcome {
  // True when a batch only advanced past finished groups without doing work. The
  // walk is NOT over: reading considered == 0 as "finished" here would stop at
  // the first run of 64 empty tenants and report success.
  bool skipped_only = false;
  // Rows CONSIDERED, not rows changed. The walk ends when a batch considers
  // nothing; a batch that considered rows and changed none is still progress,
  // and reading it as "finished" is how a merge that matched nothing reported
  // success having merged nothing.
  long long considered = 0;
  std::string cursor;
};

// WHERE a grouped walk has got to: which value of the group column, and how far
// into it. JSON rather than a delimiter, because a delimiter has to be a byte no
// key contains and there is no such byte -- and because `last_key` is read by
// people looking at a stuck job, so ["3","5571"] beats an escaped blob.
//
// An empty string means "not started". A group with an empty key means "this
// group, from the beginning", which is what advancing to the next group leaves
// behind.
struct GroupCursor {
  std::string group;
  std::string key;

  std::string encode() const {
    return json::array({group, key}).dump();
  }

  // Tolerant on purpose: a cursor that cannot be parsed must not be read as
  // "start of some group", because that would silently redo or skip a group and
  // look like success. Anything unparseable is reported as such by the caller.
  static bool decode(const std::string& text, GroupCursor& out) {
    // "0" is resume_cursor's sentinel for "nothing to resume from", and also
    // what it returns when it rejects a stale cursor. An encoded group cursor is
    // always a JSON array, so the two cannot be confused -- which is why this
    // accepts the sentinel by name and still refuses anything else it cannot
    // read. Refusing is the point: a cursor read as "the start of some group"
    // when it means something else would silently redo or skip a whole group.
    if (text.empty() || text == "0") return true;  // not started
    json j;
    try {
      j = json::parse(text);
    } catch (const std::exception&) {
      return false;
    }
    if (!j.is_array() || j.size() != 2 || !j[0].is_string() || !j[1].is_string()) {
      return false;
    }
    out.group = j[0].get<std::string>();
    out.key = j[1].get<std::string>();
    return true;
  }
};

// `apply_sql` empty means the one-statement form, where the mutation's own
// RETURNING both applies and advances. Otherwise `select_sql` selects and LOCKS
// the batch's keys and `apply_sql` names exactly those keys -- the form Citus
// can route, and the only form that can bound a batch by bytes as well as rows,
// because the keys must be in hand before the mutation is sent.
inline BatchOutcome run_paced_batch(pqxx::work& txn,
                                    const std::string& select_sql,
                                    const std::string& apply_sql,
                                    const std::string& cursor, int batch,
                                    long long batch_bytes) {
  BatchOutcome out;
  out.cursor = cursor;
  if (apply_sql.empty()) {
    const auto r = txn.exec(select_sql, pqxx::params{cursor, batch});
    out.considered = static_cast<long long>(r.size());
    for (const auto& row : r) out.cursor = row[0].as<std::string>();
    return out;
  }

  // The lock taken here is what makes the two statements safe as one: nothing
  // can change these rows before the apply below, because both run in this
  // transaction.
  const auto sel = txn.exec(select_sql, pqxx::params{cursor, batch});
  std::vector<std::string> keys;
  keys.reserve(static_cast<std::size_t>(sel.size()));
  long long bytes = 0;
  for (const auto& row : sel) {
    auto v = row[0].as<std::string>();
    // Stop between keys rather than mid-array, so an applied batch is always
    // one that both the planner's LIMIT and this budget allowed. Never zero
    // keys: a single oversized key still makes progress, where stopping at zero
    // would spin forever on it.
    if (!keys.empty() && bytes + static_cast<long long>(v.size()) > batch_bytes) {
      break;
    }
    bytes += static_cast<long long>(v.size());
    keys.push_back(std::move(v));
  }
  if (keys.empty()) return out;

  // A PostgreSQL array literal, quoted element by element so a text key
  // containing a comma, a brace or a quote survives. Paired with the
  // ::<keytype>[] cast the planner emitted.
  std::string literal = "{";
  for (std::size_t i = 0; i < keys.size(); ++i) {
    if (i != 0) literal += ',';
    literal += '"';
    for (const char c : keys[i]) {
      if (c == '"' || c == '\\') literal += '\\';
      literal += c;
    }
    literal += '"';
  }
  literal += "}";

  const auto r = txn.exec(apply_sql, pqxx::params{literal});
  // The cursor advances over what was CONSIDERED -- the selection -- not over
  // what the mutation returned.
  out.cursor = keys.back();
  out.considered = static_cast<long long>(r.size());
  if (out.considered == 0) out.considered = static_cast<long long>(keys.size());
  return out;
}

// An empty position is a real NULL, not an empty string: the parameter takes the
// column's own type and '' is not a bigint.
inline std::optional<std::string> maybe(const std::string& v) {
  if (v.empty()) return std::nullopt;
  return v;
}

// ONE BATCH of a grouped walk, and the only implementation of it.
//
// A grouped walk iterates the values of one column and keyset-walks the key
// inside each. Three statements, and the order is the algorithm: which group
// values remain, which keys remain inside one of them, and the change to exactly
// those keys, still restricted to that group.
//
// Two reasons a table is walked this way, and core does not need to know which:
//
//   - its only proof of uniqueness is a unique index on (group, key), so the key
//     is unique WITHIN a group and not across the table -- the primary key of any
//     table whose tenant column comes first, on plain PostgreSQL as much as
//     anywhere;
//   - a module's reading says row-locking batches on this table must be confined
//     to one value of a column. Citus is the case that exists: FOR UPDATE on a
//     distributed table is refused unless an equality on the distribution column
//     makes the query single-shard.
//
// Returns considered == 0 only when no group with work in it remains, which is
// the walk's one end condition. See BatchOutcome::skipped_only for the other way
// a batch can do no work.
inline BatchOutcome run_grouped_batch(pqxx::work& txn,
                                             const std::string& groups_sql,
                                             const std::string& select_sql,
                                             const std::string& apply_sql,
                                             const GroupCursor& from, int batch,
                                             long long batch_bytes) {
  BatchOutcome out;
  GroupCursor at = from;
  out.cursor = at.encode();

  // Up to this many groups are looked at in one batch before giving up the
  // transaction. A group whose rows are all done contributes no work, and a
  // cluster can hold many such groups, so a batch that could only skip ONE
  // would take a transaction per finished tenant.
  constexpr int kGroupsPerBatch = 64;

  for (int looked = 0; looked < kGroupsPerBatch; ++looked) {
    if (at.group.empty()) {
      // `groups_sql` carries the predicate too, so a distribution value with no
      // matching rows never becomes a group at all.
      const auto g = txn.exec(groups_sql,
                               pqxx::params{maybe(at.group), 1});
      if (g.empty()) {  // no values left: the walk is finished
        out.cursor = at.encode();
        return out;
      }
      at.group = g[0][0].as<std::string>();
      at.key.clear();
    }

    const auto sel = txn.exec(select_sql,
                               pqxx::params{at.group, maybe(at.key), batch});
    if (sel.empty()) {
      // This group is done. Advance past it and look at the next one: `groups_sql`
      // is keyset-walked on the group, so an empty key with a group set means
      // "everything in this group is done", and the next iteration asks for the
      // next group after it.
      const auto g = txn.exec(groups_sql,
                               pqxx::params{maybe(at.group), 1});
      if (g.empty()) {
        out.cursor = at.encode();
        return out;  // finished
      }
      at.group = g[0][0].as<std::string>();
      at.key.clear();
      continue;
    }

    std::vector<std::string> keys;
    keys.reserve(static_cast<std::size_t>(sel.size()));
    long long bytes = 0;
    for (const auto& row : sel) {
      auto v = row[0].as<std::string>();
      if (!keys.empty() && bytes + static_cast<long long>(v.size()) > batch_bytes) {
        break;
      }
      bytes += static_cast<long long>(v.size());
      keys.push_back(std::move(v));
    }

    std::string literal = "{";
    for (std::size_t i = 0; i < keys.size(); ++i) {
      if (i != 0) literal += ',';
      literal += '"';
      for (const char c : keys[i]) {
        if (c == '"' || c == '\\') literal += '\\';
        literal += c;
      }
      literal += '"';
    }
    literal += "}";

    const auto r = txn.exec(apply_sql, pqxx::params{at.group, literal});
    at.key = keys.back();
    out.cursor = at.encode();
    out.considered = static_cast<long long>(r.size());
    if (out.considered == 0) out.considered = static_cast<long long>(keys.size());
    return out;
  }

  // Ran out of group-skipping budget without finding work. Not finished -- the
  // cursor moved, so the next batch resumes from where this one stopped rather
  // than starting over.
  out.considered = 0;
  out.cursor = at.encode();
  out.skipped_only = true;
  return out;
}

class Executor {
 public:
  Executor(ConnConfig cfg, std::shared_ptr<Job> job, Ledger* ledger,
           OperationGate* operations = nullptr)
      : cfg_(std::move(cfg)),
        job_(std::move(job)),
        ledger_(ledger),
        operations_(operations) {}

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
    // Held for as long as the group's transaction is open, and taken before it
    // opens. Declared out here so committing the group releases it.
    OperationGate::Slot group_slot;

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
          group_slot = OperationGate::Slot();  // released with the transaction
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
          group_slot = operation_slot();
          worker.begin(app_name(ordinal));
          group_open = true;
        }
        run_copy(worker, ordinal, step);
        continue;
      }

      if (!group_open) {
        group_slot = operation_slot();
        worker.begin(app_name(ordinal));
        group_open = true;
      }
      run_in_transaction(worker, ordinal, step);
    }

    if (group_open) {
      worker.commit();
      group_slot = OperationGate::Slot();
    }
    succeed();
  }

  std::string app_name(int ordinal) const {
    return "pg_laswell/" + job_->job_id + "/step-" + std::to_string(ordinal);
  }

  // A slot, taken BEFORE a transaction opens and held until it closes.
  //
  // Never while one is open, and the reason is not a preference. begin() sets
  // idle_in_transaction_session_timeout to commit_interval_ms * 3 as a
  // self-guard -- "the tool must not be able to hurt the database by
  // malfunctioning quietly", session.h -- and a worker parked on this gate
  // with a transaction open is precisely that malfunction: holding locks while
  // doing nothing. The first version of this queued per statement, inside the
  // group, and the server did exactly what it is configured to do:
  //
  //   FATAL: terminating connection due to idle-in-transaction timeout
  //
  // Both jobs died and 3450 of 4000 rows went unwritten. The guard was right.
  // So the ceiling counts transactions rather than individual statements: a
  // group of DDL holds one slot for its whole transaction, a paced batch holds
  // one for its batch, and a CREATE INDEX CONCURRENTLY -- which runs in no
  // transaction at all -- holds one for the statement. That is a tighter bound
  // than counting statements, never a looser one, which is the direction an
  // upper bound is allowed to be wrong in.
  //
  // Waits in bounded steps so a cancelled job still cancels rather than
  // queueing for permission to do work nobody wants any more.
  OperationGate::Slot operation_slot() {
    if (operations_ == nullptr || operations_->limit() <= 0) {
      return OperationGate::Slot();
    }
    for (;;) {
      auto slot = operations_->try_acquire_for(50);
      if (slot.held()) return slot;
      // Re-read rather than trusting the first look: the ceiling can be raised
      // while a job waits, and an unheld slot returned because there is no
      // ceiling any more is correct, while one returned because the gate
      // happened to be momentarily free is a statement running uncounted.
      if (operations_->limit() <= 0) return OperationGate::Slot();
      if (job_->pacing.cancel_stop.load()) return OperationGate::Slot();
    }
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
  // libpqxx 8.0.2, the pinned version: connection::raw_connection() and
  // write_copy_line() are private, reachable only through internal gate
  // classes, and release_raw_connection() gives up the whole connection, so it
  // cannot serve a COPY inside a transaction. It builds the
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
          // CREATE INDEX CONCURRENTLY is the longest single statement this
          // tool issues and the one most worth counting: it is also the one
          // that takes parallel workers on the server side.
          const auto slot = operation_slot();
          w.exec_nontransactional(stmt);
        }
        return 0;
            });
      });
    } catch (const pqxx::sql_error& e) {
      json failed{{"sqlstate", e.sqlstate()}, {"error", e.what()}};
      // What a failure here leaves behind depends on what failed. The note
      // used to be about CREATE INDEX CONCURRENTLY whatever the step was, and
      // told the reader of a failed shard rebalance to look for an invalid
      // index. A step that knows its own aftermath says it in on_failure.
      const auto own = step.value("detail", json::object()).value("on_failure", "");
      bool concurrent_build = false;
      for (const auto& raw : step.value("sql", json::array())) {
        const auto sql = raw.get<std::string>();
        if (sql.rfind("CREATE INDEX CONCURRENTLY", 0) == 0 ||
            sql.rfind("CREATE UNIQUE INDEX CONCURRENTLY", 0) == 0) {
          concurrent_build = true;
        }
      }
      if (!own.empty()) {
        failed["note"] = own;
      } else if (concurrent_build) {
        failed["note"] =
            "a failed CREATE INDEX CONCURRENTLY leaves an INVALID index that "
            "still appears in pg_indexes. Re-planning will emit DROP INDEX "
            "CONCURRENTLY before rebuilding.";
      }
      record_step(ordinal, step, "failed", 0, failed);
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
    const auto r = w.txn().exec("SELECT i.indisvalid, i.indisready FROM pg_index i"
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
    // Two statements: select and lock the batch's keys, then apply to exactly
    // those keys. The CTE form could not be routed by Citus, and a single
    // statement's LIMIT can only bound a batch by rows -- accumulating to a byte
    // budget needs the keys in hand before the mutation is sent.
    const auto batch_mode = detail_json.value("batch_mode", "");
    const bool two_statement =
        batch_mode == "two_statement" && step["sql"].size() > 1;
    // A grouped walk: three statements, and a cursor that is a pair. The
    // a module's kind asks for it, but the loop is core's -- a module declares
    // the mode, it does not bring its own executor.
    const bool grouped =
        batch_mode == "grouped" && step["sql"].size() > 2;
    const auto apply_sql =
        (two_statement || grouped)
            ? detail::strip_semicolon(
                  step["sql"][grouped ? 2 : 1].get<std::string>())
            : std::string();
    const auto confined_select_sql =
        grouped ? detail::strip_semicolon(step["sql"][1].get<std::string>())
                       : std::string();
    const auto key_column = detail_json.value("key", "id");

    resume_qualified_ = detail_json.value("qualified", "");
    resume_key_ = key_column;
    resume_where_ = step["detail"].value("where", "");
    // Non-empty only for a grouped walk, whose cursor is a pair and whose
    // staleness check therefore has a different shape. See cursor_is_stale.
    resume_group_ = detail_json.value("group_column", "");
    std::string cursor = resume_cursor(ordinal);
    // Recorded so a resume is VISIBLE. Without it a retry that resumed and one
    // that silently started over were indistinguishable -- the predicate hides
    // rows already done, so both finish with the same count -- and that is
    // exactly how a staleness check that could not read its own cursor went
    // unnoticed: every grouped retry restarted from the top.
    const std::string resumed_from = cursor;
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

      // Before the batch transaction opens, never inside it.
      const auto batch_slot = operation_slot();
      if (job_->pacing.cancel_stop.load()) {
        cancelled();
        return;
      }
      w.begin(app_name(ordinal));
      const auto txn_started = detail::steady_ms();
      long long rows_this_txn = 0;
      CommitReason reason = CommitReason::kInterval;
      bool skipped_groups_only = false;

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
          BatchOutcome outcome;
          if (grouped) {
            GroupCursor at;
            if (!GroupCursor::decode(cursor, at)) {
              // A cursor that will not parse must stop the step, not be read as
              // the start of some group: that would silently redo or skip a
              // whole group and look like success.
              record_step(ordinal, step, "failed", rows_done,
                          json{{"error",
                                "the recorded cursor \"" + cursor +
                                    "\" is not a grouped-walk cursor, so where "
                                    "this walk had got to cannot be known"},
                               {"hint",
                                "Cancel the job and start a new one; the rows "
                                "already done are still done, and the predicate "
                                "excludes them."}});
              w.rollback();
              throw std::runtime_error(
                  "a grouped walk could not read its own cursor: \"" +
                  cursor + "\"");
            }
            outcome = run_grouped_batch(w.txn(), sql, confined_select_sql,
                                               apply_sql, at, batch,
                                               e.batch_bytes);
          } else {
            outcome = run_paced_batch(w.txn(), sql, apply_sql, cursor, batch,
                                      e.batch_bytes);
          }
          affected = outcome.considered;
          cursor = outcome.cursor;
          // A batch that only advanced past finished groups did no work and is
          // not the end of the walk. Treated as progress so the loop continues,
          // and not counted as rows.
          if (affected == 0 && outcome.skipped_only) {
            skipped_groups_only = true;
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
        if (affected == 0 && !skipped_groups_only) {
          done = true;
          reason = CommitReason::kFinal;
          break;
        }
        if (skipped_groups_only) {
          // Groups were skipped, none had work, and the walk is NOT over. Commit
          // what the cursor now says and come back: treating this as finished
          // would stop at the first run of finished tenants and report success.
          skipped_groups_only = false;
          reason = CommitReason::kInterval;
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
        w.txn().exec("INSERT INTO laswell.backfill_cursor"
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
           {"resumedFrom", resumed_from == "0" ? json(nullptr) : json(resumed_from)},
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
  std::string resume_group_;

  bool cursor_is_stale(ReadSession& r, const std::string& from) {
    if (resume_qualified_.empty() || resume_where_.empty()) return false;
    try {
      // A grouped cursor is a pair, so "everything at or below it is
      // done" means: every earlier distribution value, and within the current
      // one every key up to the recorded one. The check asks whether any row in
      // that region still matches the predicate -- the same question as below,
      // over a region with two edges instead of one.
      if (!resume_group_.empty()) {
        GroupCursor at;
        if (!GroupCursor::decode(from, at)) return true;  // unreadable: start over
        if (at.group.empty()) return false;               // nothing claimed yet
        const auto rel = resume_qualified_;
        const auto d = detail::quote_identifier(resume_group_);
        const auto k = detail::quote_identifier(resume_key_);
        if (at.key.empty()) {
          const auto res = r.txn().exec("SELECT EXISTS (SELECT 1 FROM " + rel + " WHERE " + d +
                  " < $1 AND (" + resume_where_ + "))",
              pqxx::params{at.group});
          return !res.empty() && res[0][0].as<bool>();
        }
        const auto res = r.txn().exec("SELECT EXISTS (SELECT 1 FROM " + rel + " WHERE (" + d + " < $1 OR (" +
                d + " = $1 AND " + k + " <= $2)) AND (" + resume_where_ + "))",
            pqxx::params{at.group, at.key});
        return !res.empty() && res[0][0].as<bool>();
      }
      // No cast on the parameter. It was `$1::bigint`, which made the check
      // throw for every text or uuid key -- and a check that throws is read as
      // "stale", so a backfill keyed on anything but an integer could never
      // resume, silently. The comparison fixes the type from the column.
      const auto q = "SELECT EXISTS (SELECT 1 FROM " + resume_qualified_ +
                     " WHERE " + resume_key_ + " <= $1 AND (" +
                     resume_where_ + "))";
      const auto res = r.txn().exec(q, pqxx::params{from});
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
      const auto res = r.txn().exec("SELECT c.last_key FROM laswell.backfill_cursor c"
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
  OperationGate* operations_ = nullptr;  // null: no ceiling
};

}  // namespace pglaswell
