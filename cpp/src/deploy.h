#pragma once

// Unattended deployment: apply everything a repository has pending, in order,
// and exit with a code.
//
// WHY THIS EXISTS, AND WHY IT IS NOT A SECOND SURFACE.
//
// The MCP server was the only way to drive a migration, which quietly made an
// agent a *requirement* for deploying rather than a convenience for authoring
// one. That is the wrong dependency for a tool whose job runs on a release
// pipeline at four in the morning. Nothing about the decisions needs an agent,
// and the reason is worth stating precisely, because the opposite is the
// natural assumption:
//
//   PACING is measured, not decided. The executor commits on a lock waiter, on
//   an interval or on a row cap, and the observer sets `throttled` from the
//   transitive waiter count and cancels a batch that starves one past
//   max_waiter_wait_ms. No caller is consulted; there is nothing to consult
//   about.
//
//   ORDERING is declared, not decided. `depends_on` is a field of the signed
//   spec -- business ordering that no measurement can derive, written down by
//   the author and covered by the signature.
//
//   CONCURRENCY is already conservative. repository.h's parallel_subsets()
//   denies concurrency on a shared relation, which is decidable, AND on
//   incomplete evidence, which is the case that matters: an opaque trigger
//   function can write any table at all, so a migration whose relation list
//   cannot be proven complete is never grouped with another. The grouping a
//   repository reports is therefore already the safe one.
//
// So the agent was never load-bearing for safety. Its role in listMigrations is
// to override the grouping UPWARD -- to say "these two are independent, I know
// the business fact the catalog cannot show you" -- which is an optimisation,
// not a correctness requirement. This runner simply honours what was derived,
// which is the conservative choice by construction, and advisory locks still
// refuse an unsafe overlap whatever anybody decided.
//
// What it therefore does NOT do, deliberately: it never widens a group, never
// reorders a level, and never runs an `undecided` pair together. Where the
// repository declined to prove independence, this waits.
//
// It is not a general CLI. There are no per-tool subcommands and no second
// argument surface to keep in step: it drives the same tools.h entry points the
// MCP server drives, in the one sequence a deployment needs.

#include <chrono>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "tools.h"

namespace pglaswell {

struct DeployOptions {
  std::string repo;
  bool dry_run = false;      // plan everything, apply nothing
  bool status_only = false;  // report what is pending, change nothing
  int poll_ms = 500;
  std::ostream* out = &std::cout;
};

// Exit codes, chosen so a pipeline can branch on them rather than parse text.
enum class DeployResult {
  kOk = 0,            // nothing pending, or everything applied
  kRefused = 1,       // a plan was refused, a spec untrusted, a job failed
  kRepoProblem = 2,   // drift, an unreadable spec, a broken dependency
  kConfigProblem = 3  // no connection, no repository, nothing to work with
};

namespace detail {

inline bool is_terminal(const std::string& state) {
  return state == "succeeded" || state == "failed" || state == "cancelled" ||
         state == "interrupted" || state == "aborted_contention";
}

inline json read_spec_file(const std::string& path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("cannot read " + path);
  return json::parse(in);
}

}  // namespace detail

class Deployment {
 public:
  Deployment(ToolContext& ctx, DeployOptions opts)
      : ctx_(ctx), opts_(std::move(opts)) {}

  DeployResult run() {
    auto& out = *opts_.out;

    const auto listing = list_migrations(ctx_, json{{"directory", opts_.repo}});
    if (listing.contains("error")) {
      out << "error: " << listing["error"].get<std::string>() << "\n";
      if (listing.contains("hint")) out << "  " << listing["hint"].get<std::string>() << "\n";
      return DeployResult::kConfigProblem;
    }

    // A repository problem is not a migration failure and must not be reported
    // as one. A spec edited after it was applied means the database no longer
    // matches the file that claims to describe it -- there is nothing to apply
    // and nothing to retry, and a pipeline should stop rather than proceed.
    const auto problems = listing.value("problems", json::array());
    if (!problems.empty()) {
      out << "repository problems, nothing applied:\n";
      for (const auto& p : problems) out << "  - " << p.get<std::string>() << "\n";
      return DeployResult::kRepoProblem;
    }

    std::map<std::string, json> by_id;
    int pending = 0, held = 0, elsewhere = 0;
    for (const auto& m : listing.value("migrations", json::array())) {
      by_id[m.value("specId", "")] = m;
      const auto status = m.value("status", "");
      if (status == "pending") ++pending;
      if (status == "held_for_release") ++held;
      if (status == "wrong_environment") ++elsewhere;
    }

    report_plan(out, listing, pending);
    // Held and not-for-here are reported and are NOT failures. A deployment
    // that skips them has done its job: the first is waiting on an approval
    // this database has not been given, the second belongs to another
    // database. Exiting non-zero on either would make a correct run look like
    // a broken one, and a pipeline would learn to ignore the code.
    for (const auto& m : listing.value("migrations", json::array())) {
      const auto status = m.value("status", "");
      if (status == "held_for_release" || status == "wrong_environment") {
        out << "  " << m.value("specId", "") << ": " << status;
        if (m.contains("error")) {
          out << " -- " << m["error"].get<std::string>();
        }
        out << "\n";
      }
    }
    if (held || elsewhere) {
      out << "  (" << held << " held, " << elsewhere
          << " for another environment)\n";
    }
    if (opts_.status_only || pending == 0) return DeployResult::kOk;

    // Levels run in order; within a level, groups run one after another,
    // because parallel_subsets() puts two migrations in DIFFERENT groups
    // exactly when it could not prove they may share a moment. Within one
    // group, everything may run at once.
    for (const auto& level : listing.value("order", json::array())) {
      for (const auto& group : level.value("groups", json::array())) {
        std::vector<std::string> ids;
        for (const auto& s : group.value("specs", json::array())) {
          const auto id = s.get<std::string>();
          if (by_id.count(id) && by_id[id].value("status", "") == "pending") {
            ids.push_back(id);
          }
        }
        if (ids.empty()) continue;
        const auto r = run_group(out, ids, by_id);
        if (r != DeployResult::kOk) return r;
      }
    }

    out << (opts_.dry_run ? "dry run complete: every pending migration planned\n"
                          : "applied " + std::to_string(pending) + " migration(s)\n");
    return DeployResult::kOk;
  }

 private:
  void report_plan(std::ostream& out, const json& listing, int pending) const {
    out << "repository " << opts_.repo << ": " << pending << " pending\n";
    int level_no = 0;
    for (const auto& level : listing.value("order", json::array())) {
      ++level_no;
      for (const auto& group : level.value("groups", json::array())) {
        const auto specs = group.value("specs", json::array());
        if (specs.size() > 1) {
          out << "  level " << level_no << ": " << specs.size()
              << " concurrent (" << join_specs(specs) << ")\n";
        } else if (!specs.empty()) {
          out << "  level " << level_no << ": " << specs[0].get<std::string>() << "\n";
        }
      }
      // Named, not hidden. This is the one place a human or an agent could do
      // better than this runner, and saying so is the honest report.
      for (const auto& u : level.value("undecided", json::array())) {
        out << "    serialised: " << u.get<std::string>() << "\n";
      }
    }
  }

  static std::string join_specs(const json& specs) {
    std::string s;
    for (const auto& x : specs) {
      if (!s.empty()) s += ", ";
      s += x.get<std::string>();
    }
    return s;
  }

  DeployResult run_group(std::ostream& out, const std::vector<std::string>& ids,
                         std::map<std::string, json>& by_id) {
    // Never more at once than the connection budget allows. tools.h refuses a
    // start past max_concurrent_jobs, which is right for a caller that asked
    // for one too many; here it would be a self-inflicted failure, so the
    // runner waits for a slot instead of asking for one it cannot have.
    const int cap = ctx_.registry.get(ctx_.registry.default_name())
                        .executor.max_concurrent_jobs;

    std::vector<std::pair<std::string, std::string>> running;  // id -> jobId
    std::size_t next = 0;
    while (next < ids.size() || !running.empty()) {
      while (next < ids.size() && static_cast<int>(running.size()) < cap) {
        const auto& id = ids[next++];
        json spec;
        try {
          spec = detail::read_spec_file(by_id[id].value("path", ""));
        } catch (const std::exception& e) {
          out << "  " << id << ": " << e.what() << "\n";
          return DeployResult::kRepoProblem;
        }

        if (opts_.dry_run) {
          const auto plan = plan_migration_tool(ctx_, json{{"spec", spec}});
          if (!report_plan_outcome(out, id, plan)) return DeployResult::kRefused;
          continue;
        }

        const auto started = start_migration(ctx_, json{{"spec", spec}});
        if (!started.value("accepted", false)) {
          // `accepted` is false for two different reasons and they read very
          // differently to whoever is looking at a failed deployment: the plan
          // was refused, or the specification was not trusted. On a refusal
          // startMigration returns the WHOLE plan, conflicts included, so the
          // reason is already here and only has to be printed. Reporting a bare
          // "not accepted" -- which this did until a real repository was pointed
          // at it -- throws away the one thing this tool exists to say.
          report_not_accepted(out, id, started);
          drain(out, running);
          return DeployResult::kRefused;
        }
        out << "  " << id << ": started (" << started.value("jobId", "") << ")\n";
        running.emplace_back(id, started.value("jobId", ""));
      }

      if (running.empty()) continue;
      std::this_thread::sleep_for(std::chrono::milliseconds(opts_.poll_ms));

      for (auto it = running.begin(); it != running.end();) {
        const auto st = snapshot(it->second);
        const auto state = st.value("state", "");
        if (!detail::is_terminal(state)) {
          ++it;
          continue;
        }
        out << "  " << it->first << ": " << state << "\n";
        if (state != "succeeded") {
          report_failure(out, st);
          running.erase(it);
          drain(out, running);
          return DeployResult::kRefused;
        }
        it = running.erase(it);
      }
    }
    return DeployResult::kOk;
  }

  // Never walk away from a job that is still running. Returning early left the
  // worker mid-migration while the process tore down around it -- the ledger
  // would have been correct and the deployment log would have been silent about
  // a migration that was still writing. Whatever went wrong elsewhere, what is
  // already in flight gets watched to its end and reported.
  void drain(std::ostream& out,
             std::vector<std::pair<std::string, std::string>>& running) {
    if (running.empty()) return;
    out << "  waiting for " << running.size()
        << " migration(s) already running before stopping\n";
    while (!running.empty()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(opts_.poll_ms));
      for (auto it = running.begin(); it != running.end();) {
        const auto st = snapshot(it->second);
        const auto state = st.value("state", "");
        if (!detail::is_terminal(state)) { ++it; continue; }
        out << "  " << it->first << ": " << state << "\n";
        if (state != "succeeded") report_failure(out, st);
        it = running.erase(it);
      }
    }
  }

  void report_not_accepted(std::ostream& out, const std::string& id,
                           const json& started) const {
    const auto conflicts = started.value("conflicts", json::array());
    if (!conflicts.empty()) {
      out << "  " << id << ": REFUSED\n";
      for (const auto& c : conflicts) {
        out << "      " << c.get<std::string>() << "\n";
      }
      return;
    }
    out << "  " << id << ": not accepted\n";
    for (const char* k : {"error", "hint"}) {
      if (started.contains(k)) {
        out << "      " << started[k].get<std::string>() << "\n";
      }
    }
    // A trust failure says which gate refused, and that is the whole answer.
    for (const char* gate : {"clientTrust", "databaseTrust"}) {
      if (started.contains(gate) && started[gate].is_object() &&
          !started[gate].value("accepted", true)) {
        out << "      " << gate << ": "
            << started[gate].value("reason", "refused") << "\n";
      }
    }
  }

  // job_status answers about EVERY job it knows and puts the state inside
  // `jobs`, not at the top level -- it was built for an agent asking "what is
  // happening", not for a caller waiting on one id. Reading `state` off the
  // envelope silently yields "", which is never terminal, so the runner applied
  // a migration and then polled forever. Found by pointing it at a real
  // repository; unit-testable now that it is one function.
  json snapshot(const std::string& job_id) {
    const auto st = job_status(ctx_, json{{"jobId", job_id}});
    for (const auto& j : st.value("jobs", json::array())) {
      if (j.value("jobId", "") == job_id) return j;
    }
    return json::object();
  }

  bool report_plan_outcome(std::ostream& out, const std::string& id,
                           const json& plan) const {
    if (!plan.value("ok", false)) {
      out << "  " << id << ": REFUSED\n";
      for (const auto& c : plan.value("conflicts", json::array())) {
        out << "      " << c.get<std::string>() << "\n";
      }
      return false;
    }
    out << "  " << id << ": " << plan.value("steps", json::array()).size()
        << " step(s), plan " << plan.value("planDigest", "").substr(0, 12) << "\n";
    for (const auto& w : plan.value("warnings", json::array())) {
      out << "      warning: " << w.get<std::string>() << "\n";
    }
    return true;
  }

  static void report_failure(std::ostream& out, const json& st) {
    // The ledger has the statement; this says where to look rather than
    // reprinting it, because a deployment log is not the place a failure is
    // diagnosed and pretending otherwise encourages reading the wrong thing.
    if (st.contains("error")) {
      out << "      " << st["error"].get<std::string>() << "\n";
    }
    for (const auto& s : st.value("steps", json::array())) {
      if (s.value("state", "") == "failed") {
        out << "      step " << s.value("ordinal", 0) << " "
            << s.value("kind", "") << ": " << s.value("detail", json::object())
                                                 .value("error", "") << "\n";
      }
    }
    out << "      laswell.step in the target database has the statement and "
           "its result\n";
  }

  ToolContext& ctx_;
  DeployOptions opts_;
};

}  // namespace pglaswell
