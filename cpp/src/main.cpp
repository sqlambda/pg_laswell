// pg_laswell_mcp — entry point.
//
// Argument parsing, configuration resolution, one try/catch. Everything else
// lives in the headers. Hand-rolled arg loop rather than a CLI library, for the
// same reason there is no logging framework: the dependency list is the thing
// an operator has to install, and five options do not justify a line in it.

#include <sys/stat.h>

#include <cstdlib>
#include <iostream>
#include <string>

#include "config.h"
#include "server.h"
#include "session.h"
#include "tools.h"

namespace {

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
      << "  -h, --help           this message\n"
      << "  -V, --version        print the version and exit\n\n"
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
  if (config_path.empty() && db_url.empty()) {
    usage(argv[0]);
    return 1;
  }

  try {
    // Nothing connects here. Reachability is a property of the moment a call is
    // made, not of startup: a registry of twenty databases must not be
    // unusable because one of them is behind a VPN that happens to be down.
    const std::string app = std::string("pg-laswell/") + PGLASWELL_VERSION;
    pglaswell::ToolContext ctx{
        config_path.empty() ? pglaswell::Registry::from_url(db_url, app)
                            : pglaswell::Registry::from_ini(config_path, app),
        nullptr};
    pglaswell::ConnectionCache cache;
    ctx.cache = &cache;

    pglaswell::McpServer server(pglaswell::make_tools(ctx));
    server.run();
  } catch (const std::exception& e) {
    std::cerr << "Fatal: " << e.what() << std::endl;
    return 1;
  }
  return 0;
}
