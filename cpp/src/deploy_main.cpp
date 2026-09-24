// pg_laswell -- apply a migration repository, unattended.
//
// The deployment half of the pair. pg_laswell_mcp is for AUTHORING a migration:
// an agent plans one, reads the warnings, argues with the reasoning, and
// decides. This binary is for APPLYING one on a release pipeline, where there
// is nobody to ask and nothing to decide -- see the rationale at the top of
// deploy.h for why there is nothing to decide, which is the part that is not
// obvious.
//
// Two binaries rather than a subcommand on one, because the audiences are
// disjoint: a server deployment should not have to install, configure or reason
// about an MCP server it will never speak to, and an agent has no use for a
// blocking run loop. They share every line that matters -- spec parsing,
// planning, the executor, the ledger -- and differ only in who is asking.

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>

#include "deploy.h"
#include "jobs.h"
#include "tools.h"

namespace {

void usage(const char* argv0) {
  std::cerr <<
      "pg_laswell " PGLASWELL_VERSION " -- apply a migration repository\n"
      "\n"
      "Usage: " << argv0 << " --repo DIR [options] [conninfo]\n"
      "\n"
      "  --repo DIR         the directory of signed specifications; repeat it\n"
      "                     to apply several as one repository\n"
      "  --manifest FILE    a manifest naming the sources this deployment is\n"
      "                     made of; `complete: true` in it asserts they are\n"
      "                     ALL of it, so the drift check answers for every\n"
      "                     epoch rather than only the ones on disk\n"
      "  --status           report what is pending and exit; change nothing\n"
      "  --dry-run          plan every pending migration; apply nothing. Each is\n"
      "                     planned against the database as it is NOW, so in a\n"
      "                     repository not yet applied, a specification needing\n"
      "                     what an earlier PENDING one creates is refused.\n"
      "  --dry-run=chain    rehearse the pending migrations in order, each on top\n"
      "                     of the ones before it, then roll everything back.\n"
      "                     Holds every lock until the end: for an empty database\n"
      "                     or a restored copy, not for production.\n"
      "  -c, --config FILE  configuration file (or PGLASWELL_CONFIG)\n"
      "  -V, --version      print the version and exit\n"
      "  -h, --help         this text\n"
      "\n"
      "The connection comes from the argument, DATABASE_URL, or --config.\n"
      "\n"
      "Exit codes, so a pipeline can branch without parsing output:\n"
      "  0  nothing pending, or everything applied\n"
      "  1  a plan was refused, a specification was untrusted, or a job failed\n"
      "  2  a repository problem: drift, an unreadable spec, a broken depends_on\n"
      "  3  a configuration problem: no connection, or no repository\n"
      "\n"
      "For authoring migrations, and for an agent to read the reasoning behind\n"
      "a plan, use pg_laswell_mcp(1).\n";
}

}  // namespace

int main(int argc, char* argv[]) {
  pglaswell::DeployOptions opts;
  std::string config_path, db_url;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") { usage(argv[0]); return 0; }
    if (arg == "-V" || arg == "--version") {
      std::cout << "pg_laswell " PGLASWELL_VERSION "\n" << pglaswell::module_banner();
      return 0;
    }
    if (arg == "--status")  { opts.status_only = true; continue; }
    if (arg == "--dry-run") { opts.dry_run = true;     continue; }
    if (arg == "--dry-run=chain") {
      opts.dry_run = true;
      opts.chain = true;
      continue;
    }
    if (arg == "--repo") {
      if (i + 1 >= argc) { std::cerr << "--repo requires a directory\n"; return 3; }
      // Repeatable: a project per directory, applied as one repository.
      opts.repos.push_back(argv[++i]);
      continue;
    }
    if (arg == "--manifest") {
      if (i + 1 >= argc) { std::cerr << "--manifest requires a file\n"; return 3; }
      opts.manifest = argv[++i];
      continue;
    }
    if (arg == "-c" || arg == "--config") {
      if (i + 1 >= argc) { std::cerr << arg << " requires a file argument\n"; return 3; }
      config_path = argv[++i];
      continue;
    }
    if (!arg.empty() && arg[0] == '-') {
      std::cerr << "Unknown option: " << arg << "\n";
      usage(argv[0]);
      return 3;
    }
    db_url = arg;
  }

  if (config_path.empty()) {
    if (const char* env = std::getenv("PGLASWELL_CONFIG")) config_path = env;
  }
  if (db_url.empty()) {
    if (const char* env = std::getenv("DATABASE_URL")) db_url = env;
  }
  if (opts.repos.empty() && opts.repo.empty() && opts.manifest.empty()) {
    std::cerr << "--repo is required: there is nothing to apply without one\n";
    return 3;
  }
  // Unlike the stdio server, every path through this binary needs a database:
  // there is no getSpecDigest here, so a missing connection is fatal at startup
  // rather than at the first call.
  if (config_path.empty() && db_url.empty()) {
    std::cerr << "no connection configured: pass a conninfo, set DATABASE_URL, "
                 "or use --config\n";
    return 3;
  }

  try {
    const std::string app = std::string("pg-laswell/") + PGLASWELL_VERSION;
    pglaswell::ToolContext ctx{
        !config_path.empty() ? pglaswell::Registry::from_ini(config_path, app)
                             : pglaswell::Registry::from_url(db_url, app),
        nullptr};
    pglaswell::ConnectionCache cache;
    ctx.cache = &cache;

    pglaswell::JobRegistry jobs;
    ctx.jobs = &jobs;

    // The observer is what makes pacing adaptive rather than scheduled: it
    // measures transitive waiters and sets `throttled`, and cancels a batch
    // that has starved one past max_waiter_wait_ms. Without it this binary
    // would still be correct and would no longer be the tool it claims to be.
    // One per connection, created on demand: a specification may name its own
    // connection, and a job must be watched in the database it runs in.
    pglaswell::ObserverPool observers(&jobs);
    ctx.observers = &observers;

    pglaswell::Deployment run(ctx, opts);
    const auto result = run.run();

    // Joined, never abandoned: a worker thread racing PQfinish against static
    // destruction is the classic intermittent crash at shutdown.
    observers.stop_all();
    jobs.join_all();
    return static_cast<int>(result);
  } catch (const std::exception& e) {
    std::cerr << "Fatal: " << e.what() << "\n";
    return 3;
  }
}
