// Renders one plan from a spec and a fixed set of observations, and prints it.
//
// The counterpart to PostgreSQL's pg_regress: cpp/test/plans/*.spec.json is the
// input, cpp/test/plans/expected/*.out is the transcript, and plan-tests.sh
// diffs them. What that buys is what it buys PostgreSQL -- a behaviour change
// arrives as a diff somebody reads, rather than as a substring assertion
// somebody wrote once and nobody revisits.
//
// It takes OBSERVATIONS FROM A FILE rather than from a database, which is not a
// shortcut: plan_migration is a pure function of (spec, observations), and this
// is the only test that exercises that directly. It also makes the output
// deterministic by construction -- no table sizes drifting with autovacuum, no
// database to set up, nothing to skip when one is absent.
#include <fstream>
#include <iostream>
#include <sstream>

#include "planner.h"
#include "spec.h"

namespace {

pglaswell::json read_json(const char* path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error(std::string("cannot read ") + path);
  return pglaswell::json::parse(in);
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc < 3) {
    std::cerr << "usage: render_plan <spec.json> <observations.json>\n";
    return 2;
  }
  try {
    const auto spec_doc = read_json(argv[1]);
    const auto obs_doc = read_json(argv[2]);

    pglaswell::Observations obs;
    obs.server_version = obs_doc.value("serverVersion", 180006);
    obs.tables = obs_doc.value("tables", pglaswell::json::object());
    obs.objects = obs_doc.value("objects", pglaswell::json::object());
    obs.server = obs_doc.value("server", pglaswell::json::object());

    pglaswell::ExecutorConfig cfg;
    if (obs_doc.contains("executor")) {
      const auto& e = obs_doc["executor"];
      cfg.batch_rows = e.value("batch_rows", cfg.batch_rows);
      cfg.commit_interval_ms = e.value("commit_interval_ms", cfg.commit_interval_ms);
      cfg.batch_cap_rows = e.value("batch_cap_rows", cfg.batch_cap_rows);
      cfg.max_concurrent_jobs = e.value("max_concurrent_jobs", cfg.max_concurrent_jobs);
      cfg.maintenance_work_mem_mb =
          e.value("maintenance_work_mem_mb", cfg.maintenance_work_mem_mb);
      cfg.host_vcpus = e.value("host_vcpus", cfg.host_vcpus);
    }

    // A parse failure is OUTPUT, not a crash -- the same reason pg_regress puts
    // ERROR lines in its expected files. A refusal is a behaviour worth
    // reviewing, and it changes more often than a success does.
    try {
      const auto spec = pglaswell::parse_spec(spec_doc);
      std::cout << pglaswell::plan_migration(spec, obs, cfg).render();
    } catch (const pglaswell::SpecError& e) {
      std::cout << "SPEC REFUSED: " << e.what() << "\n"
                << "        hint: " << e.hint() << "\n";
    }
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "render_plan: " << e.what() << "\n";
    return 1;
  }
}
