#pragma once
// SIGINT and SIGTERM, for a process that may be running migrations.
//
// Both binaries used to exit at once. A job's row in the ledger is closed by
// the process that runs it, so a job stopped that way stayed `running` for
// good (found in the field: two hours later, with a backend pid that belonged
// to no session, on a server that had since been restarted).
//
// The first signal is recorded and nothing else: what to do about it is the
// caller's, outside the handler. The handler then puts the default action
// back, so a second signal ends the process at once -- for whoever does not
// want to wait for a batch to commit. What that leaves in the ledger is set
// right when the next run starts (ledger.h, reconcile_dead_jobs).

#include <atomic>
#include <csignal>

#include <signal.h>

#include "jobs.h"

namespace pglaswell::signals {

inline std::atomic<int> g_received{0};

extern "C" inline void pglaswell_on_signal(int sig) {
  g_received.store(sig);
  struct sigaction dfl {};
  dfl.sa_handler = SIG_DFL;
  sigemptyset(&dfl.sa_mask);
  sigaction(sig, &dfl, nullptr);
}

inline void install() {
  struct sigaction sa {};
  sa.sa_handler = pglaswell_on_signal;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART;
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);
}

inline int received() { return g_received.load(); }

// Ask every job of this process to stop, as cancelJob does, and to stop NOW
// where it is inside one long statement: a walk commits the batch it is in and
// its cursor, and anything else is cancelled by the observer (jobs.h, tick).
// Returns how many jobs were asked.
inline int stop_jobs(JobRegistry& jobs) {
  int asked = 0;
  for (const auto& j : jobs.all()) {
    if (is_terminal(j->state.load())) continue;
    j->pacing.stop_now = true;
    j->pacing.cancel_stop = true;
    ++asked;
  }
  return asked;
}

}  // namespace pglaswell::signals
