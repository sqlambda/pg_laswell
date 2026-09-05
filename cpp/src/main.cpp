// pg_laswell_mcp — entry point.
//
// Argument parsing, configuration resolution, one try/catch. Everything else
// lives in the headers. Hand-rolled arg loop rather than a CLI library, for the
// same reason there is no logging framework: the dependency list is the thing
// an operator has to install, and five options do not justify a line in it.

#include <sys/stat.h>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>

#include "config.h"
#include "server.h"
#include "session.h"
#include "tools.h"

namespace {

// Reads an --args value: literal JSON, @file, or @- for stdin.
std::string read_args(const std::string& spec) {
  if (spec.empty() || spec[0] != '@') return spec;
  if (spec == "@-") {
    return std::string(std::istreambuf_iterator<char>(std::cin),
                       std::istreambuf_iterator<char>());
  }
  std::ifstream in(spec.substr(1));
  if (!in) throw std::runtime_error("cannot read " + spec.substr(1));
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}

std::string default_config_path() {
  if (const char* home = std::getenv("HOME")) {
    return std::string(home) + "/.config/pg_laswell/laswell.ini";
  }
  return {};
}

bool file_exists(const std::string& path) {
  struct stat st {};
  return ::stat(path.c_str(), &st) == 0;
}

void usage(const char* argv0) {
  std::cerr
      << "pg_laswell_mcp " << PGLASWELL_VERSION << "\n"
      << "Lock-aware PostgreSQL migration executor, speaking MCP over stdio.\n\n"
      << "Usage: " << argv0 << " [options] [conninfo]\n\n"
      << "  -c, --config <file>  connection registry (INI)\n"
      << "      --call <tool>    run one tool and exit; JSON on stdout\n"
      << "      --args <json>    arguments for --call; @file reads a file,\n"
      << "                       @- reads stdin\n"
      << "  -h, --help           this message\n"
      << "  -V, --version        print the version and exit\n\n"
      << "--call exits 0 when the tool answered and 1 when it reported an\n"
      << "error, so a pipeline can gate on it. Without it the process speaks\n"
      << "MCP over stdio and exits 0 whatever the tools reported.\n\n"
      << "Configuration is resolved in this order:\n"
      << "  1. --config <file>\n"
      << "  2. $PGLASWELL_CONFIG\n"
      << "  3. ~/.config/pg_laswell/laswell.ini, if it exists\n"
      << "  4. $DATABASE_URL\n"
      << "  5. the first non-option argument\n\n"
      << "The executor requires a direct connection: see the POOLERS section\n"
      << "of man pg_laswell_mcp before pointing this at PgBouncer.\n";
}

}  // namespace

int main(int argc, char* argv[]) {
  std::string config_path;
  std::string db_url;
  std::string call_tool;
  std::string call_args = "{}";

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      usage(argv[0]);
      return 0;
    }
    if (arg == "-V" || arg == "--version") {
      std::cout << PGLASWELL_VERSION << "\n";
      return 0;
    }
    if (arg == "--call") {
      if (i + 1 >= argc) {
        std::cerr << "--call requires a tool name\n";
        return 1;
      }
      call_tool = argv[++i];
      continue;
    }
    if (arg == "--args") {
      if (i + 1 >= argc) {
        std::cerr << "--args requires a value\n";
        return 1;
      }
      call_args = argv[++i];
      continue;
    }
    if (arg == "-c" || arg == "--config") {
      if (i + 1 >= argc) {
        std::cerr << arg << " requires a file argument\n";
        return 1;
      }
      config_path = argv[++i];
      continue;
    }
    if (!arg.empty() && arg[0] == '-') {
      std::cerr << "Unknown option: " << arg << "\n";
      usage(argv[0]);
      return 1;
    }
    db_url = arg;
  }

  if (config_path.empty()) {
    if (const char* env = std::getenv("PGLASWELL_CONFIG")) config_path = env;
  }
  if (db_url.empty()) {
    if (const char* env = std::getenv("DATABASE_URL")) db_url = env;
  }

  if (config_path.empty() && db_url.empty()) {
    const auto fallback = default_config_path();
    if (!fallback.empty() && file_exists(fallback)) config_path = fallback;
  }
  // A missing connection is fatal for the stdio server and NOT fatal for
  // --call, and the asymmetry is deliberate.
  //
  // getSpecDigest touches no database -- that is the whole point of it, since
  // the machine that signs a specification is deliberately not the machine
  // that can reach production. Refusing here made the signing workflow the man
  // page documents impossible to follow on the machine it is meant for. The
  // tools that DO need a connection already fail well: ToolContext::connection
  // says "no connection is configured; pass a conninfo argument, set
  // DATABASE_URL, or use --config", which is strictly better than usage text.
  //
  // The server keeps the startup refusal because a long-lived MCP server with
  // no database is a misconfiguration an operator wants to hear about at
  // startup rather than at the first tool call.
  if (config_path.empty() && db_url.empty() && call_tool.empty()) {
    usage(argv[0]);
    return 1;
  }

  try {
    // Nothing connects here. Reachability is a property of the moment a call is
    // made, not of startup: a registry of twenty databases must not be
    // unusable because one of them is behind a VPN that happens to be down.
    const std::string app = std::string("pg-laswell/") + PGLASWELL_VERSION;
    pglaswell::ToolContext ctx{
        !config_path.empty() ? pglaswell::Registry::from_ini(config_path, app)
        : !db_url.empty()    ? pglaswell::Registry::from_url(db_url, app)
                             : pglaswell::Registry(),
        nullptr};
    pglaswell::ConnectionCache cache;
    ctx.cache = &cache;

    pglaswell::JobRegistry jobs;
    ctx.jobs = &jobs;

    // No connection means no observer, and nothing is lost: the observer exists
    // to watch running jobs, and a job cannot start without a connection. Held
    // in an optional rather than constructed against a placeholder ConnConfig,
    // because a placeholder conninfo is an empty string and libpq reads that as
    // "connect to the local socket with defaults" -- an observer thread quietly
    // connecting to whatever happens to be on this machine.
    std::optional<pglaswell::Observer> observer;
    if (!ctx.registry.default_name().empty()) {
      const auto& first = ctx.registry.get(ctx.registry.default_name());
      observer.emplace(first, &jobs, first.executor.observer_tick_ms);
      ctx.observer = &*observer;
    }

    pglaswell::McpServer server(pglaswell::make_tools(ctx));

    // One tool, one answer, an exit code a pipeline can gate on.
    //
    // This exists because CI is the one caller that is neither an agent nor a
    // person: it wants "is anything pending, and has anything been edited since
    // it was applied" to fail a build, and an agent in a pipeline is expensive,
    // nondeterministic and often not permitted. It is deliberately NOT a CLI --
    // there are no subcommands, no argument parsing per tool, and no second
    // surface to keep in step with the MCP one. The tool set is the same tool
    // set; only the framing differs.
    if (!call_tool.empty()) {
      pglaswell::json args;
      try {
        args = pglaswell::json::parse(read_args(call_args));
      } catch (const std::exception& e) {
        std::cerr << "--args: " << e.what() << "\n";
        return 1;
      }
      if (!args.is_object()) {
        std::cerr << "--args must be a JSON object\n";
        return 1;
      }
      const auto response = server.handle_request(
          pglaswell::json{{"jsonrpc", "2.0"},
                          {"id", 1},
                          {"method", "tools/call"},
                          {"params", {{"name", call_tool}, {"arguments", args}}}});
      if (!response) {
        std::cerr << "no response\n";
        return 1;
      }
      if (response->contains("error")) {
        std::cout << (*response)["error"].dump() << std::endl;
        return 1;
      }
      const auto& result = (*response)["result"];
      const auto text = result["content"][0]["text"].get<std::string>();
      std::cout << text << std::endl;

      // What counts as failure for a pipeline, stated explicitly because the
      // MCP notion of isError is narrower than the CI one. A plan that
      // correctly refuses is a successful CALL and an unsuccessful OUTCOME, and
      // a build must fail on the second.
      //
      //   isError            the tool threw
      //   "error"            the project's uniform failure payload; every tool
      //                      that fails puts it at the TOP level, and nested
      //                      ones (a failed job, an unreadable spec) are data
      //   ok: false          a plan was refused
      //   accepted: false    a spec was not trusted, or a job was not started
      //   problems: [...]    a repository has something wrong with it -- which
      //                      is exactly the listMigrations-as-a-CI-gate case:
      //                      a spec edited after it was applied must fail a
      //                      build, not merely be mentioned
      const auto payload = pglaswell::json::parse(text);
      const bool failed =
          result.value("isError", false) || payload.contains("error") ||
          !payload.value("ok", true) || !payload.value("accepted", true) ||
          !payload.value("problems", pglaswell::json::array()).empty();
      return failed ? 1 : 0;
    }

    server.run();

    // Joined, never abandoned. A worker thread racing PQfinish against static
    // destruction is the classic intermittent crash at shutdown.
    if (observer) observer->stop();
    jobs.join_all();
  } catch (const std::exception& e) {
    std::cerr << "Fatal: " << e.what() << std::endl;
    return 1;
  }
  return 0;
}
