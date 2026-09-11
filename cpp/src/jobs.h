#pragma once

// Jobs, pacing state, and the process-wide observer.
//
// CONCURRENCY, stated once so it can be audited in one paragraph:
//
//   The stdio loop stays single-threaded. Jobs run on their own threads. The
//   only shared state is JobRegistry, guarded by one mutex, and EVERY read out
//   of it returns a SNAPSHOT COPY -- never a reference or pointer into live job
//   state. The lock is held for the duration of the copy and never while
//   writing to stdout.
//
// That is what PGLASWELL_SANITIZER=THREAD is protecting, and why THREAD is a
// gate for this project rather than the belt-and-braces it is for pg_licht.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "config.h"
#include "session.h"

namespace pglaswell {

using json = nlohmann::json;

enum class JobState {
  kPlanned,
  kRunning,
  kThrottled,
  kPausedContention,
  kSucceeded,
  kFailed,
  kCancelled,
  kAbortedContention
};

inline const char* to_string(JobState s) {
  switch (s) {
    case JobState::kPlanned: return "planned";
    case JobState::kRunning: return "running";
    case JobState::kThrottled: return "throttled";
    case JobState::kPausedContention: return "paused_contention";
    case JobState::kSucceeded: return "succeeded";
    case JobState::kFailed: return "failed";
    case JobState::kCancelled: return "cancelled";
    case JobState::kAbortedContention: return "aborted_contention";
  }
  return "unknown";
}

inline bool is_terminal(JobState s) {
  return s == JobState::kSucceeded || s == JobState::kFailed ||
         s == JobState::kCancelled || s == JobState::kAbortedContention;
}

// Why a paced backfill committed. The histogram of these is the observable
// proof that pacing works, and it is what the deterministic test asserts on.
enum class CommitReason { kInterval, kLockWaiter, kBatchCap, kFinal };

inline const char* to_string(CommitReason r) {
  switch (r) {
    case CommitReason::kInterval: return "interval";
    case CommitReason::kLockWaiter: return "lock_waiter";
    case CommitReason::kBatchCap: return "batch_cap";
    case CommitReason::kFinal: return "final";
  }
  return "unknown";
}

// What the observer tells the worker, and what the worker tells the observer.
//
// Atomics, not a condition variable. The worker is not WAITING, it is WORKING;
// it reads these at the only point where it can act on them -- the boundary
// between batches. A condvar would add a lock the worker must take on every
// batch to learn nothing 99% of the time.
struct PacingState {
  std::atomic<int> worker_pid{0};
  std::atomic<int> direct_waiters{0};
  std::atomic<int> transitive_waiters{0};
  std::atomic<int> blocking_waiter_pid{0};
  std::atomic<long long> oldest_wait_ms{0};
  std::atomic<long long> inflicted_blocked_ms{0};
  std::atomic<int> contention_ticks{0};   // consecutive ticks above pause_waiters
  std::atomic<int> quiet_ticks{0};        // consecutive ticks at or below resume
  std::atomic<bool> cancel_requested{false};
  std::atomic<bool> cancel_stop{false};   // operator cancellation, not pacing
  std::atomic<bool> throttled{false};
  std::atomic<bool> paused{false};
  std::atomic<long long> paused_since_ms{0};
  // CIC telemetry, from pg_stat_progress_create_index.
  std::atomic<int> cic_lockers_total{0};
  std::atomic<int> cic_lockers_done{0};
  std::atomic<int> cic_current_locker_pid{0};
  std::atomic<long long> cic_waiting_ms{0};
};

struct Job {
  std::string job_id;
  std::string spec_id;
  std::string spec_digest;
  std::string signer_key_id;
  std::string connection;
  json plan;
  std::string plan_digest;
  long long lock_key = 0;

  std::atomic<JobState> state{JobState::kPlanned};
  PacingState pacing;

  std::mutex m;                 // guards everything below
  json steps = json::array();   // per-step progress
  json backfill = json::object();
  json error;
  std::vector<std::string> warnings;
  std::chrono::steady_clock::time_point started;
  std::chrono::steady_clock::time_point finished;
  bool has_finished = false;

  std::thread worker;
  // The worker's connection, so the observer can cancel a statement on it.
  // Guarded by cancel_m rather than m: the observer must be able to reach it
  // without contending on the mutex the worker holds while updating progress.
  std::mutex cancel_m;
  pqxx::connection* worker_conn = nullptr;
};

// The process-wide registry. One mutex, snapshot reads.
// A process-wide ceiling on how many MIGRATION STATEMENTS are in flight at
// once, across every job this process is running.
//
// It answers a different question from max_concurrent_jobs, which is why both
// exist. Jobs are how much work is UNDERWAY; operations are how much of it is
// touching the server at this instant. Today a job runs its steps one at a
// time, so the two numbers meet -- but they are not the same claim, and only
// one of them is what an operator means by "how hard is this thing leaning on
// my database right now".
//
// Zero means no ceiling, the same way host_vcpus and maintenance_work_mem_mb
// mean "not declared" rather than "zero of them". A setting that changed
// behaviour the moment the field existed would be a setting nobody chose.
//
// WHAT IT DOES NOT GATE, deliberately: the observer, the ledger, and the
// coordination connection. Those are how the tool watches and records itself,
// and starving them would disable the contention breaker exactly when the
// server is busiest -- which is the one moment it is for.
//
// THE HAZARD, written down because it is invisible from either side. A job can
// hold a transaction open while it waits here for a slot, and the job holding
// the slot can be waiting on a row lock that the first job's transaction owns.
// PostgreSQL cannot see that cycle: half of it is a condition variable in this
// process. It does not hang -- lock_timeout bounds the statement holding the
// slot, and the observer cancels one that starves a waiter past
// max_waiter_wait_ms -- so it resolves as a timeout rather than a deadlock.
// Set this below max_concurrent_jobs and that trade is what you are buying.
class OperationGate {
 public:
  // Raising the limit wakes whoever is waiting; lowering it does not interrupt
  // work already in flight, which would mean abandoning a statement mid-run.
  void configure(int limit) {
    {
      std::lock_guard<std::mutex> lock(m_);
      limit_ = limit;
    }
    cv_.notify_all();
  }

  int limit() const {
    std::lock_guard<std::mutex> lock(m_);
    return limit_;
  }

  // The most that were ever in flight at once. The gate measuring itself,
  // which is worth exactly what that is worth -- but it is the only vantage
  // point that sees every job, and a test can assert on it.
  int peak() const {
    std::lock_guard<std::mutex> lock(m_);
    return peak_;
  }

  int in_flight() const {
    std::lock_guard<std::mutex> lock(m_);
    return in_flight_;
  }

  class Slot {
   public:
    Slot() = default;
    explicit Slot(OperationGate* g) : gate_(g) {}
    Slot(Slot&& o) noexcept : gate_(o.gate_) { o.gate_ = nullptr; }
    Slot& operator=(Slot&& o) noexcept {
      if (this != &o) { release(); gate_ = o.gate_; o.gate_ = nullptr; }
      return *this;
    }
    Slot(const Slot&) = delete;
    Slot& operator=(const Slot&) = delete;
    ~Slot() { release(); }
    bool held() const { return gate_ != nullptr; }

   private:
    void release() {
      if (gate_ == nullptr) return;
      {
        std::lock_guard<std::mutex> lock(gate_->m_);
        --gate_->in_flight_;
      }
      gate_->cv_.notify_one();
      gate_ = nullptr;
    }
    OperationGate* gate_ = nullptr;
  };

  // Waits up to `wait_ms` for room. Bounded rather than indefinite so a caller
  // can re-check whether its job was cancelled: a thread parked forever in
  // here would ignore cancelJob, and a cancel that does not cancel is worse
  // than a queue that is slow.
  Slot try_acquire_for(int wait_ms) {
    std::unique_lock<std::mutex> lock(m_);
    if (limit_ <= 0) return Slot();  // no ceiling: nothing to hold
    if (!cv_.wait_for(lock, std::chrono::milliseconds(wait_ms),
                      [this] { return limit_ <= 0 || in_flight_ < limit_; })) {
      return Slot();
    }
    if (limit_ <= 0) return Slot();
    ++in_flight_;
    if (in_flight_ > peak_) peak_ = in_flight_;
    return Slot(this);
  }

  // True when a caller must keep waiting: a ceiling is set and it is full.
  bool would_block() const {
    std::lock_guard<std::mutex> lock(m_);
    return limit_ > 0 && in_flight_ >= limit_;
  }

 private:
  mutable std::mutex m_;
  std::condition_variable cv_;
  int limit_ = 0;
  int in_flight_ = 0;
  int peak_ = 0;
};

class JobRegistry {
 public:
  std::shared_ptr<Job> create(const std::string& job_id) {
    auto j = std::make_shared<Job>();
    j->job_id = job_id;
    j->started = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(m_);
    jobs_[job_id] = j;
    return j;
  }

  std::shared_ptr<Job> find(const std::string& job_id) const {
    std::lock_guard<std::mutex> lock(m_);
    const auto it = jobs_.find(job_id);
    return it == jobs_.end() ? nullptr : it->second;
  }

  std::vector<std::shared_ptr<Job>> all() const {
    std::lock_guard<std::mutex> lock(m_);
    std::vector<std::shared_ptr<Job>> out;
    out.reserve(jobs_.size());
    for (const auto& [id, j] : jobs_) out.push_back(j);
    return out;
  }

  int running_count() const {
    int n = 0;
    for (const auto& j : all()) {
      if (!is_terminal(j->state.load())) ++n;
    }
    return n;
  }

  // The process-wide statement ceiling. One per registry rather than one per
  // job, because "how much is in flight" is a question about the server, and
  // every job in this process is leaning on the same one.
  OperationGate& operations() { return operations_; }
  const OperationGate& operations() const { return operations_; }

  // Joins every worker thread. Called before the process exits, so a thread is
  // never racing PQfinish against static destruction -- the same reasoning
  // that makes ConnectionCache's reaper joined rather than detached.
  void join_all() {
    for (const auto& j : all()) {
      if (j->worker.joinable()) j->worker.join();
    }
  }

 private:
  mutable std::mutex m_;
  OperationGate operations_;
  std::map<std::string, std::shared_ptr<Job>> jobs_;
};

namespace detail {

// Tier 1. A plain shared-memory scan with no pg_blocking_pids() at all: if
// nothing anywhere is waiting for a lock, nothing can be waiting on us. On a
// healthy server this answers every tick and costs almost nothing.
inline const char* kAnyWaitersSql =
    "SELECT count(*) FROM pg_locks WHERE NOT granted";

// Tiers 2 and 3, in one statement, for every worker pid at once.
//
// The DIRECT count drives the commit trigger. The TRANSITIVE count drives the
// contention breaker, and the two are not the same: measured 2026-09-05 (spike
// S11), a direct count sees 1 waiter where the real pile-up is 6, because
// pg_blocking_pids() returns direct blockers only and second-order waiters are
// invisible to it.
//
// Note the direction: pg_blocking_pids(x) answers "who blocks x", and pacing
// needs the inverse -- "is anyone blocked by me" -- so the scan is over
// ungranted locks, asking each waiter whether we are among its blockers.
// Reaching for pg_blocking_pids(worker_pid) is the natural mistake and gives
// the exactly-backwards answer.
inline const char* kObserverSql = R"SQL(
WITH RECURSIVE
  targets AS (SELECT unnest($1::int[]) AS pid),
  direct AS (
    SELECT t.pid AS worker, l.pid AS waiter, l.waitstart
      FROM targets t
      JOIN pg_locks l ON NOT l.granted AND t.pid = ANY(pg_blocking_pids(l.pid))
  ),
  chain(worker, pid, depth, path) AS (
      SELECT t.pid, t.pid, 0, ARRAY[t.pid] FROM targets t
    UNION ALL
      SELECT c.worker, l.pid, c.depth + 1, c.path || l.pid
        FROM chain c
        JOIN pg_locks l ON NOT l.granted AND c.pid = ANY(pg_blocking_pids(l.pid))
       WHERE NOT l.pid = ANY(c.path) AND c.depth < 16
  )
SELECT COALESCE(JSONB_OBJECT_AGG(x.worker::text, JSONB_BUILD_OBJECT(
         'direct', x.direct,
         'transitive', x.transitive,
         'blockingWaiterPid', x.blocking_pid,
         'oldestWaitMs', x.oldest_ms,
         'inflictedBlockedMs', x.inflicted_ms,
         'cic', x.cic)), '{}'::jsonb)
  FROM (
    SELECT t.pid AS worker,
           (SELECT count(DISTINCT d.waiter) FROM direct d WHERE d.worker = t.pid) AS direct,
           (SELECT count(DISTINCT c.pid) FROM chain c
             WHERE c.worker = t.pid AND c.depth > 0) AS transitive,
           (SELECT d.waiter FROM direct d WHERE d.worker = t.pid
             ORDER BY d.waitstart LIMIT 1) AS blocking_pid,
           COALESCE((SELECT round(extract(epoch FROM now() - min(d.waitstart)) * 1000)
                       FROM direct d WHERE d.worker = t.pid), 0) AS oldest_ms,
           COALESCE((SELECT round(sum(extract(epoch FROM now() - d.waitstart)) * 1000)
                       FROM direct d WHERE d.worker = t.pid), 0) AS inflicted_ms,
           (SELECT JSONB_BUILD_OBJECT(
                     'phase', ci.phase,
                     'blocksDone', ci.blocks_done, 'blocksTotal', ci.blocks_total,
                     'lockersTotal', ci.lockers_total, 'lockersDone', ci.lockers_done,
                     'currentLockerPid', ci.current_locker_pid)
              FROM pg_stat_progress_create_index ci WHERE ci.pid = t.pid) AS cic
      FROM targets t) AS x
)SQL";

}  // namespace detail

// One observer thread for the whole process, not one per job.
//
// Per-job observers would issue a query per job per tick -- O(jobs) load on the
// server for a monitoring function, which is the wrong direction when the whole
// premise is "do not hurt the database". This collects every running worker's
// pid into one array and issues one query per tick.
class Observer {
 public:
  Observer(ConnConfig cfg, JobRegistry* registry, int tick_ms)
      : cfg_(std::move(cfg)), registry_(registry), tick_ms_(tick_ms) {}

  ~Observer() { stop(); }

  void start() {
    if (thread_.joinable()) return;
    stop_ = false;
    thread_ = std::thread([this] { loop(); });
  }

  void stop() {
    {
      std::lock_guard<std::mutex> lock(m_);
      stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
  }

  // Runs one observation cycle. Exposed so a test can step the observer
  // deterministically instead of racing its timer.
  void tick() {
    std::vector<std::shared_ptr<Job>> live;
    std::vector<int> pids;
    for (const auto& j : registry_->all()) {
      if (is_terminal(j->state.load())) continue;
      const int pid = j->pacing.worker_pid.load();
      if (pid <= 0) continue;
      live.push_back(j);
      pids.push_back(pid);
    }
    if (live.empty()) return;

    try {
      if (!conn_ || !conn_->is_open()) {
        conn_ = std::make_unique<pqxx::connection>(cfg_.conninfo);
      }
      // A short READ ONLY transaction per tick, never a long one. Measured
      // 2026-09-05 (spike S9): pg_locks stays live inside a long transaction,
      // but CUMULATIVE STATISTICS FREEZE at stats_fetch_consistency=cache, so a
      // long-lived observer would report stale table statistics forever while
      // its lock readings looked correct. It would also hold back xmin for
      // nothing.
      pqxx::work txn(*conn_);
      txn.exec("SET TRANSACTION READ ONLY");

      const auto any = txn.exec(detail::kAnyWaitersSql);
      const bool anyone_waiting =
          !any.empty() && any[0][0].as<long long>() > 0;
      if (!anyone_waiting) {
        for (const auto& j : live) clear_contention(*j);
        txn.commit();
        return;
      }

      std::string array = "{";
      for (std::size_t i = 0; i < pids.size(); ++i) {
        if (i != 0) array += ",";
        array += std::to_string(pids[i]);
      }
      array += "}";

      const auto r = pqxx_exec(txn, detail::kObserverSql, pqxx::params{array});
      json by_pid = json::object();
      if (!r.empty() && !r[0][0].is_null()) {
        by_pid = json::parse(r[0][0].as<std::string>());
      }
      txn.commit();

      for (const auto& j : live) {
        const auto key = std::to_string(j->pacing.worker_pid.load());
        if (!by_pid.contains(key)) {
          clear_contention(*j);
          continue;
        }
        apply(*j, by_pid[key]);
      }
    } catch (const std::exception&) {
      // A failed observation is not a failed migration. The worker keeps its
      // time-based commit trigger, which alone already bounds waiter latency;
      // the lock-waiter trigger is an optimisation on top of it.
      conn_.reset();
    }
  }

 private:
  void clear_contention(Job& j) {
    j.pacing.direct_waiters = 0;
    j.pacing.transitive_waiters = 0;
    j.pacing.blocking_waiter_pid = 0;
    j.pacing.oldest_wait_ms = 0;
    j.pacing.inflicted_blocked_ms = 0;
    j.pacing.contention_ticks = 0;
    bump_quiet(j);
  }

  void bump_quiet(Job& j) {
    const auto& cfg = cfg_.executor;
    if (j.pacing.transitive_waiters.load() <= cfg.resume_waiters) {
      const int q = j.pacing.quiet_ticks.fetch_add(1) + 1;
      // Hysteresis, and the resume condition is the transitive count FALLING --
      // never "we committed". Measured 2026-09-05 (spike S11): after the worker
      // commits, waiters can remain, because the backend ahead of them in the
      // queue has taken its lock. Resuming on our own commit would resume
      // straight into the pile-up we made.
      if (q >= cfg.contention_dwell_ticks) {
        j.pacing.paused = false;
        j.pacing.throttled = false;
      }
    } else {
      j.pacing.quiet_ticks = 0;
    }
  }

  void apply(Job& j, const json& r) {
    const auto& cfg = cfg_.executor;
    const int direct = r.value("direct", 0);
    const int transitive = r.value("transitive", 0);

    j.pacing.direct_waiters = direct;
    j.pacing.transitive_waiters = transitive;
    j.pacing.blocking_waiter_pid = r.value("blockingWaiterPid", 0);
    j.pacing.oldest_wait_ms = r.value("oldestWaitMs", 0LL);
    j.pacing.inflicted_blocked_ms = r.value("inflictedBlockedMs", 0LL);

    if (r.contains("cic") && r["cic"].is_object()) {
      j.pacing.cic_lockers_total = r["cic"].value("lockersTotal", 0);
      j.pacing.cic_lockers_done = r["cic"].value("lockersDone", 0);
      j.pacing.cic_current_locker_pid = r["cic"].value("currentLockerPid", 0);
    }

    // The ladder, on the TRANSITIVE count. Throttle before pause, deliberately:
    // pausing a backfill that is 80% done and mildly contending is worse than
    // slowing it down. Harm is quadratic in block duration (spike S12), so
    // reducing it continuously beats stopping late.
    j.pacing.throttled = transitive >= cfg.throttle_waiters;

    if (transitive >= cfg.pause_waiters) {
      const int n = j.pacing.contention_ticks.fetch_add(1) + 1;
      j.pacing.quiet_ticks = 0;
      if (n >= cfg.contention_dwell_ticks && !j.pacing.paused.load()) {
        j.pacing.paused = true;
        j.pacing.paused_since_ms = now_ms();
      }
    } else {
      j.pacing.contention_ticks = 0;
      bump_quiet(j);
    }

    // Escalation: one waiter starving behind a single batch statement is not
    // something the next commit boundary will fix in time.
    if (j.pacing.oldest_wait_ms.load() > cfg.max_waiter_wait_ms) {
      request_cancel(j);
    }
  }

  void request_cancel(Job& j) {
    if (j.pacing.cancel_requested.exchange(true)) return;
    std::lock_guard<std::mutex> lock(j.cancel_m);
    if (j.worker_conn == nullptr) return;
    try {
      // Verified callable from another thread, and TSAN-clean, on 2026-09-05
      // (spike S6). libpqxx documents it as safe cross-thread with the caveat
      // that the caller must not cancel the wrong query -- which is why
      // cancel_requested is set first and the worker consults it: SQLSTATE
      // 57014 is raised by statement_timeout too, and reporting every 57014 as
      // "we cancelled you" would mislabel every timeout as deliberate.
      j.worker_conn->cancel_query();
    } catch (...) {
    }
  }

  static long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }

  void loop() {
    std::unique_lock<std::mutex> lock(m_);
    while (!stop_) {
      cv_.wait_for(lock, std::chrono::milliseconds(tick_ms_),
                   [this] { return stop_; });
      if (stop_) break;
      lock.unlock();
      tick();
      lock.lock();
    }
  }

  ConnConfig cfg_;
  JobRegistry* registry_;
  int tick_ms_;
  std::unique_ptr<pqxx::connection> conn_;
  mutable std::mutex m_;
  std::condition_variable cv_;
  bool stop_ = false;
  std::thread thread_;
};

// A job id. Random rather than sequential: a ledger row whose id reveals how
// many migrations preceded it is a small information leak, and more usefully a
// random id cannot be guessed by a caller poking at jobStatus.
inline std::string new_job_id() {
  static std::mt19937_64 rng{std::random_device{}()};
  static std::mutex m;
  std::uint64_t a, b;
  {
    std::lock_guard<std::mutex> lock(m);
    a = rng();
    b = rng();
  }
  char buf[40];
  std::snprintf(buf, sizeof(buf), "%08x-%04x-4%03x-%04x-%012llx",
                static_cast<unsigned>(a >> 32),
                static_cast<unsigned>((a >> 16) & 0xFFFF),
                static_cast<unsigned>(a & 0xFFF),
                static_cast<unsigned>(0x8000 | ((b >> 48) & 0x3FFF)),
                static_cast<unsigned long long>(b & 0xFFFFFFFFFFFFULL));
  return buf;
}

}  // namespace pglaswell
