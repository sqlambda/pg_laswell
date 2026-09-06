#pragma once

// Connections and units of work.
//
// ReadSession is pg_licht's Session, near enough verbatim: READ ONLY, always
// rolls back, never commits. WriteSession is its counterpart and has no
// equivalent there, because pg_licht never requests a strong lock and never
// writes. Every difference between the two is deliberate and commented.

#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#if defined(__GNUC__) && !defined(__clang__)
// GCC-only false positive from std::variant inside pqxx headers; Clang has no
// such warning group, and with -Werror an unguarded pragma would hard-fail
// there on "unknown warning group".
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
#include <pqxx/pqxx>
#if defined(__GNUC__) && !defined(__clang__)
#  pragma GCC diagnostic pop
#endif

#include "config.h"

namespace pglaswell {

// pqxx 7.9 renamed exec_params to exec(sql, pqxx::params). Selected at compile
// time so both spellings build from one source.
template <typename T, typename P>
inline pqxx::result pqxx_exec(T& txn, const std::string& sql, P&& params) {
#if PQXX_VERSION_MAJOR > 7 || (PQXX_VERSION_MAJOR == 7 && PQXX_VERSION_MINOR >= 9)
  return txn.exec(sql, std::forward<P>(params));
#else
  return txn.exec_params(sql, std::forward<P>(params));
#endif
}

// A keyed pool of IDLE connections, with a reaper.
//
// The map holds only idle connections: take() erases the entry and transfers
// ownership, release() puts it back. That is the whole safety argument -- a
// connection being used by a caller is not in the map, so the reaper cannot
// see it, let alone close it. No in-use flag, no race.
class ConnectionCache {
 public:
  explicit ConnectionCache(std::chrono::seconds idle_ttl = std::chrono::seconds(60))
      : idle_ttl_(idle_ttl),
        // Joined, not detached. A detached thread racing PQfinish against
        // process exit is the classic source of an intermittent crash at
        // shutdown, and it is the kind that only ever reproduces in CI.
        reaper_([this] { reap(); }) {}

  ~ConnectionCache() {
    {
      std::lock_guard<std::mutex> lock(m_);
      stop_ = true;
    }
    cv_.notify_all();
    if (reaper_.joinable()) reaper_.join();
  }

  ConnectionCache(const ConnectionCache&) = delete;
  ConnectionCache& operator=(const ConnectionCache&) = delete;

  std::unique_ptr<pqxx::connection> take(const std::string& name) {
    std::lock_guard<std::mutex> lock(m_);
    const auto it = idle_.find(name);
    if (it == idle_.end()) return nullptr;
    auto conn = std::move(it->second.conn);
    idle_.erase(it);
    return conn;
  }

  void release(const std::string& name, std::unique_ptr<pqxx::connection> conn) {
    if (!conn) return;
    // A closed connection is a corpse, not a cache entry: keeping it would
    // hand the next caller a guaranteed failure that looks like a flake.
    if (!conn->is_open()) return;
    std::lock_guard<std::mutex> lock(m_);
    idle_[name] = Entry{std::move(conn), std::chrono::steady_clock::now()};
  }

  std::size_t idle_count() const {
    std::lock_guard<std::mutex> lock(m_);
    return idle_.size();
  }

 private:
  struct Entry {
    std::unique_ptr<pqxx::connection> conn;
    std::chrono::steady_clock::time_point since;
  };

  void reap() {
    std::unique_lock<std::mutex> lock(m_);
    while (!stop_) {
      cv_.wait_for(lock, idle_ttl_, [this] { return stop_; });
      if (stop_) break;
      const auto now = std::chrono::steady_clock::now();
      for (auto it = idle_.begin(); it != idle_.end();) {
        it = (now - it->second.since >= idle_ttl_) ? idle_.erase(it)
                                                   : std::next(it);
      }
    }
  }

  mutable std::mutex m_;
  std::condition_variable cv_;
  std::map<std::string, Entry> idle_;
  std::chrono::seconds idle_ttl_;
  bool stop_ = false;
  std::thread reaper_;
};

// A read-only unit of work. Guarded per transaction rather than per session,
// which is what makes the guard hold behind a pooler in transaction mode.
class ReadSession {
 public:
  explicit ReadSession(const ConnConfig& cfg,
                       std::optional<int> timeout_ms = std::nullopt,
                       ConnectionCache* cache = nullptr,
                       std::optional<int> lock_timeout_ms = std::nullopt)
      : cache_(cache), name_(cfg.name) {
    // One-shot retry on a cached connection that died while idle. Safe here
    // precisely because nothing of the caller's has run yet: only BEGIN, and
    // there is nothing to replay.
    for (int attempt = 0; attempt < 2; ++attempt) {
      conn_ = (attempt == 0 && cache_ != nullptr) ? cache_->take(name_) : nullptr;
      if (!conn_) conn_ = std::make_unique<pqxx::connection>(cfg.conninfo);
      try {
        txn_.emplace(*conn_);
        break;
      } catch (const std::exception&) {
        conn_.reset();
        if (attempt == 1) throw;  // a fresh connection failed too: real error
      }
    }

    const int timeout = timeout_ms.value_or(cfg.statement_timeout_ms);
    // The whole setup rides one round trip. PQexec accepts semicolon-separated
    // statements and returns the last result, so the read-only guard, the
    // timeout and the recovery probe cost one round trip together rather than
    // three. Every interpolated value is an integer this process computed, so
    // none of it can carry a quote or a semicolon.
    std::string setup = "SET TRANSACTION READ ONLY";
    if (timeout > 0) {  // `timeout` is already a plain int here
      setup += "; SET LOCAL statement_timeout = " + std::to_string(timeout);
    }
    // A read that can queue behind a strong lock is not a safe read. Some
    // catalog functions -- pg_get_indexdef() among them, measured 2026-09-05 --
    // open the relation and therefore take AccessShareLock, so an observation
    // of a table being ALTERed would wait behind that ALTER rather than
    // reporting it. Bounding it here turns a hang into a reading.
    //
    // Read once through has_value() rather than twice through value_or().
    // std::optional leaves its payload uninitialised when empty, and the
    // optimiser is entitled to compile value_or() as a branchless select that
    // loads the payload before discarding it -- harmless at the machine level,
    // and a "conditional jump depends on uninitialised value(s)" under
    // Valgrind. It appeared here after unrelated header changes shifted
    // inlining, which is exactly how much that formulation is worth relying
    // on. This form reads the payload only when there is one.
    const int lock_timeout = lock_timeout_ms.has_value() ? *lock_timeout_ms : 0;
    if (lock_timeout > 0) {
      setup += "; SET LOCAL lock_timeout = " + std::to_string(lock_timeout);
    }
    setup += "; SELECT pg_is_in_recovery()";
    lock_timeout_ms_ = lock_timeout;
    const auto r = txn_->exec(setup);
    if (!r.empty() && !r[0][0].is_null()) in_recovery_ = r[0][0].as<bool>();
  }

  ~ReadSession() {
    // A destructor must not throw, and a connection already gone is not an
    // error worth reporting: the transaction dies with it either way.
    try {
      if (txn_) txn_->abort();
    } catch (...) {
    }
    txn_.reset();  // before releasing, so what returns to the cache is idle
    if (cache_ && conn_) cache_->release(name_, std::move(conn_));
  }

  ReadSession(const ReadSession&) = delete;
  ReadSession& operator=(const ReadSession&) = delete;

  pqxx::work& txn() { return *txn_; }

  // Starts a fresh transaction on the same connection. Needed after a failed
  // statement, which aborts the surrounding transaction and leaves every later
  // statement failing with "current transaction is aborted".
  void renew() {
    txn_.reset();
    txn_.emplace(*conn_);
    std::string setup = "SET TRANSACTION READ ONLY";
    if (lock_timeout_ms_ > 0) {
      setup += "; SET LOCAL lock_timeout = " + std::to_string(lock_timeout_ms_);
    }
    txn_->exec(setup);
  }

  bool in_recovery() const { return in_recovery_.value_or(false); }
  // Free: comes from the libpq startup handshake, no round trip. Per
  // connection, which matters when a registry spans server versions.
  int server_version() const { return conn_->server_version(); }
  int backend_pid() const { return conn_->backendpid(); }

 private:
  ConnectionCache* cache_ = nullptr;
  std::string name_;
  std::unique_ptr<pqxx::connection> conn_;
  std::optional<pqxx::work> txn_;
  std::optional<bool> in_recovery_;
  int lock_timeout_ms_ = 0;
};

// The writing counterpart. Owns its transaction explicitly: nothing here
// commits unless the caller says so.
class WriteSession {
 public:
  explicit WriteSession(const ConnConfig& cfg)
      : conn_(std::make_unique<pqxx::connection>(cfg.conninfo)),
        cfg_(cfg),
        backend_pid_(conn_->backendpid()) {
    if (in_recovery()) {
      // Cheap to check, easy to forget, and the alternative is a confusing
      // read-only error several statements later.
      throw std::runtime_error(
          "connection \"" + cfg.name +
          "\" is a standby (pg_is_in_recovery() is true); pg_laswell will not "
          "apply a migration to a replica");
    }
  }

  WriteSession(const WriteSession&) = delete;
  WriteSession& operator=(const WriteSession&) = delete;

  ~WriteSession() {
    try {
      if (txn_) txn_->abort();
    } catch (...) {
    }
    txn_.reset();
  }

  int backend_pid() const { return backend_pid_; }
  int server_version() const { return conn_->server_version(); }
  pqxx::connection& connection() { return *conn_; }

  bool in_recovery() {
    pqxx::nontransaction tx(*conn_);
    const auto r = tx.exec("SELECT pg_is_in_recovery()");
    return !r.empty() && !r[0][0].is_null() && r[0][0].as<bool>();
  }

  // Opens a transaction with the guards every write must carry.
  //
  // lock_timeout is the single most important setting here, and pg_licht has
  // no equivalent because it never requests a strong lock. Without it an
  // ALTER TABLE queues behind one long transaction, and everything arriving
  // afterwards queues behind the ALTER -- which is how a millisecond
  // operation becomes an outage.
  //
  // idle_in_transaction_session_timeout is a self-guard: if this process
  // hangs, the server tears the transaction down rather than holding locks
  // indefinitely. The tool must not be able to hurt the database by
  // malfunctioning quietly.
  void begin(const std::string& application_name) {
    if (txn_) throw std::runtime_error("WriteSession: a transaction is open");
    nontxn_.reset();
    txn_.emplace(*conn_);

    const int idle_timeout = cfg_.executor.commit_interval_ms * 3;
    std::string setup =
        "SET LOCAL lock_timeout = " + std::to_string(cfg_.executor.lock_timeout_ms) +
        "; SET LOCAL idle_in_transaction_session_timeout = " +
        std::to_string(idle_timeout);
    if (cfg_.statement_timeout_ms > 0) {
      setup += "; SET LOCAL statement_timeout = " +
               std::to_string(cfg_.statement_timeout_ms);
    }
    // application_name carries the job id, so an operator running pg_licht's
    // currentActivity sees which job a backend belongs to without
    // cross-referencing anything.
    //
    // Quoted rather than bound, and that is forced rather than chosen: a bound
    // parameter uses the extended query protocol, which permits exactly ONE
    // statement, so binding here would split this single round trip into two.
    // txn_->quote() is libpqxx's own escaping and keeps one escaping path, the
    // same trade pg_licht makes for literals that must appear in SQL text.
    setup += "; SET LOCAL application_name = " + txn_->quote(application_name);
    setup += "; SELECT pg_backend_pid()";

    const auto r = txn_->exec(setup);

    // The pooler invariant, enforced continuously rather than detected once.
    // Measured 2026-09-05 (spike S7): comparing pg_backend_pid() across two
    // transactions at startup has false negatives at both ends of the
    // pool-size range, so startup detection is not achievable. Re-reading it
    // here is free -- it rides this same round trip -- and it fires at the
    // moment the connection is actually multiplexed, which is the only moment
    // it matters.
    if (!r.empty() && !r[0][0].is_null()) {
      const int now = r[0][0].as<int>();
      if (now != backend_pid_) {
        throw std::runtime_error(
            "the backend pid changed from " + std::to_string(backend_pid_) +
            " to " + std::to_string(now) +
            " between transactions, which means this connection is being "
            "multiplexed -- almost certainly a transaction-mode pooler. "
            "pg_laswell's executor requires a direct connection; see the "
            "POOLERS section of man pg_laswell_mcp.");
      }
    }
  }

  pqxx::work& txn() {
    if (!txn_) throw std::runtime_error("WriteSession: no transaction is open");
    return *txn_;
  }

  void commit() {
    if (!txn_) throw std::runtime_error("WriteSession: no transaction to commit");
    txn_->commit();
    txn_.reset();
  }

  void rollback() {
    if (!txn_) return;
    txn_->abort();
    txn_.reset();
  }

  bool in_transaction() const { return txn_.has_value(); }

  // A nontransaction on this connection, for statements that must run outside
  // any transaction and whose result is needed -- session advisory locks, which
  // are released at COMMIT if taken with pg_advisory_xact_lock and must
  // therefore be taken outside one.
  pqxx::nontransaction& exec_nontransactional_txn() {
    if (txn_) {
      throw std::runtime_error(
          "exec_nontransactional_txn called while a transaction is open");
    }
    nontxn_.emplace(*conn_);
    return *nontxn_;
  }

  // For statements that cannot run inside a transaction block at all --
  // CREATE INDEX CONCURRENTLY and DROP INDEX CONCURRENTLY.
  //
  // Note for anyone adding a GUC around such a statement: SET LOCAL is
  // SILENTLY A NO-OP on a nontransaction. PostgreSQL emits a warning that
  // libpqxx does not raise, so the setting appears to take and does not
  // (verified 2026-09-05, spike S6). Use a session-level SET with an explicit
  // RESET, as with_session_setting() below does.
  pqxx::result exec_nontransactional(const std::string& sql) {
    if (txn_) {
      throw std::runtime_error(
          "exec_nontransactional called while a transaction is open; the "
          "statement would fail with 'cannot run inside a transaction block'");
    }
    pqxx::nontransaction tx(*conn_);
    return tx.exec(sql);
  }

  // Applies a session-level GUC, runs `body`, and resets it -- including on
  // an exception path, which is why this is a scope guard rather than three
  // statements at the call site. This is the one place the project knowingly
  // uses session state, and it is licensed only because the executor requires
  // a direct connection.
  template <typename Body>
  auto with_session_setting(const std::string& name, const std::string& value,
                            Body&& body) {
    struct Reset {
      pqxx::connection* c;
      std::string n;
      ~Reset() {
        try {
          pqxx::nontransaction tx(*c);
          tx.exec("RESET " + n);
        } catch (...) {
        }
      }
    };
    // An empty value means "leave this setting alone and just run the body",
    // so a caller with an optional setting does not need two code paths --
    // and, more to the point, does not accidentally set the GUC to the empty
    // string, which is an error for every numeric one.
    if (value.empty()) return body();
    {
      pqxx::nontransaction tx(*conn_);
      // The GUC name is from a fixed internal set, never from a spec; the
      // value is bound.
      pqxx_exec(tx, "SELECT set_config($1, $2, false)",
                pqxx::params{name, value});
    }
    Reset guard{conn_.get(), name};
    return body();
  }

 private:
  std::unique_ptr<pqxx::connection> conn_;
  ConnConfig cfg_;
  std::optional<pqxx::work> txn_;
  std::optional<pqxx::nontransaction> nontxn_;
  int backend_pid_ = 0;
};

}  // namespace pglaswell
