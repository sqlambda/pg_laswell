// pg_laswell test suite.
//
// One file, as in pg_licht, grouped by banner comments.
//
// One deliberate difference from pg_licht's suite: its main() refuses to run at
// all without DATABASE_URL, because every one of its tests needs a database.
// That is not true here. canonical.h, trust.h, spec.h and planner.h are pure by
// construction -- planner.h must not even include pqxx -- and their tests are
// the ones most worth running on a machine with no PostgreSQL. So the pure
// tests always run, and the database fixture skips when DATABASE_URL is absent.
//
// A skip that nobody notices is a test that silently stopped running, so CI
// sets PGLASWELL_REQUIRE_DATABASE=1, which turns that skip into a failure.
// Same pattern as pg_licht's PGLICHT_REQUIRE_HYPOPG.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <set>
#include <sstream>
#include <string>
#include <thread>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#if defined(__GNUC__) && !defined(__clang__)
// GCC-only false positive from std::variant inside pqxx headers; Clang does not
// have this warning group at all, and with -Werror active an unguarded pragma
// would hard-fail there on "unknown warning group".
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
#include <pqxx/pqxx>
#if defined(__GNUC__) && !defined(__clang__)
#  pragma GCC diagnostic pop
#endif

#include "server.h"

using pglaswell::json;
using pglaswell::McpServer;

namespace {

json rpc(McpServer& s, const json& request) {
  auto r = s.handle_request(request);
  EXPECT_TRUE(r.has_value()) << "expected a response for " << request.dump();
  return r.value_or(json::object());
}

json initialize(McpServer& s, const char* protocol = "2025-06-18") {
  return rpc(s, json{{"jsonrpc", "2.0"},
                     {"id", 1},
                     {"method", "initialize"},
                     {"params", {{"protocolVersion", protocol}}}});
}

}  // namespace

// --- initialize and protocol negotiation ---------------------------------

TEST(Initialize, ReportsServerNameAndVersion) {
  McpServer s;
  const json r = initialize(s);
  ASSERT_TRUE(r.contains("result")) << r.dump();
  EXPECT_EQ(r["result"]["serverInfo"]["name"], "pg-laswell");
  EXPECT_EQ(r["result"]["serverInfo"]["version"], PGLASWELL_VERSION);
}

TEST(Initialize, DeclaresToolsCapability) {
  McpServer s;
  const json r = initialize(s);
  EXPECT_TRUE(r["result"]["capabilities"].contains("tools"));
}

TEST(Initialize, CarriesInstructionsNamingPgLicht) {
  // The instructions are how a model learns the two servers are a pair. If
  // this ever stops mentioning the sibling, an agent has to guess.
  McpServer s;
  const json r = initialize(s);
  const auto text = r["result"]["instructions"].get<std::string>();
  EXPECT_NE(text.find("pg_licht"), std::string::npos) << text;
  EXPECT_NE(text.find("planMigration"), std::string::npos) << text;
}

TEST(Initialize, EchoesASupportedProtocol) {
  for (const auto& p : pglaswell::supported_protocols()) {
    McpServer s;
    const json r = initialize(s, p.c_str());
    EXPECT_EQ(r["result"]["protocolVersion"], p);
  }
}

TEST(Initialize, AnUnknownProtocolFallsBackToTheNewestRatherThanFailing) {
  // Refusing would make every future revision a hard incompatibility with an
  // already-installed binary.
  McpServer s;
  const json r = initialize(s, "2099-01-01");
  EXPECT_EQ(r["result"]["protocolVersion"],
            pglaswell::supported_protocols().back());
}

TEST(Initialize, AMissingProtocolVersionIsNotAnError) {
  McpServer s;
  const json r = rpc(s, json{{"jsonrpc", "2.0"},
                             {"id", 1},
                             {"method", "initialize"},
                             {"params", json::object()}});
  EXPECT_TRUE(r.contains("result")) << r.dump();
}

// --- JSON-RPC framing -----------------------------------------------------

TEST(Rpc, ANotificationGetsNoResponseAtAll) {
  // Replying to a notification is a protocol violation that some clients
  // tolerate and others treat as a desync.
  McpServer s;
  auto r = s.handle_request(
      json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});
  EXPECT_FALSE(r.has_value());
}

TEST(Rpc, AnUnknownNotificationGetsNoResponseEither) {
  McpServer s;
  auto r = s.handle_request(
      json{{"jsonrpc", "2.0"}, {"method", "notifications/somethingNew"}});
  EXPECT_FALSE(r.has_value());
}

TEST(Rpc, AnUnknownMethodWithAnIdIsMethodNotFound) {
  McpServer s;
  const json r = rpc(s, json{{"jsonrpc", "2.0"},
                             {"id", 7},
                             {"method", "tools/nonesuch"}});
  ASSERT_TRUE(r.contains("error")) << r.dump();
  EXPECT_EQ(r["error"]["code"], pglaswell::kMethodNotFound);
  EXPECT_EQ(r["id"], 7);
}

TEST(Rpc, PingAnswersEmpty) {
  McpServer s;
  const json r = rpc(s, json{{"jsonrpc", "2.0"}, {"id", 2}, {"method", "ping"}});
  EXPECT_TRUE(r["result"].is_object());
  EXPECT_TRUE(r["result"].empty());
}

TEST(Rpc, ARequestWithoutAMethodIsInvalid) {
  McpServer s;
  const json r = rpc(s, json{{"jsonrpc", "2.0"}, {"id", 3}});
  ASSERT_TRUE(r.contains("error")) << r.dump();
  EXPECT_EQ(r["error"]["code"], pglaswell::kInvalidRequest);
}

TEST(Rpc, MalformedInputOnTheStreamIsAParseErrorAndDoesNotKillTheLoop) {
  // The loop must survive a bad line: an MCP client that sends one garbled
  // frame should not need to restart the server.
  McpServer s;
  std::istringstream in(
      "{ this is not json\n"
      "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}\n");
  std::ostringstream out;
  s.run(in, out);

  std::istringstream lines(out.str());
  std::string first, second;
  ASSERT_TRUE(std::getline(lines, first));
  ASSERT_TRUE(std::getline(lines, second)) << "loop stopped after the bad line";
  EXPECT_EQ(json::parse(first)["error"]["code"], pglaswell::kParseError);
  EXPECT_TRUE(json::parse(second).contains("result"));
}

TEST(Rpc, BlankLinesAreIgnored) {
  McpServer s;
  std::istringstream in("\n\n{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}\n");
  std::ostringstream out;
  s.run(in, out);
  // Bind the string before iterating it. out.str() returns a temporary, so
  // out.str().begin() and out.str().end() would be iterators into two
  // different, already-destroyed objects -- which is exactly how this test
  // first failed, and what Valgrind reported as several million errors.
  const std::string written = out.str();
  EXPECT_EQ(std::count(written.begin(), written.end(), '\n'), 1);
}

// --- the tool registry ----------------------------------------------------

TEST(Tools, ListIsDerivedEntirelyFromToolDefs) {
  // The whole point of the single-vector design: nothing can appear in
  // tools/list that is not a ToolDef, and nothing can be dispatched that is
  // not listed.
  McpServer s;
  initialize(s);
  const json r = rpc(s, json{{"jsonrpc", "2.0"}, {"id", 4}, {"method", "tools/list"}});
  ASSERT_TRUE(r["result"]["tools"].is_array()) << r.dump();
  EXPECT_EQ(r["result"]["tools"].size(), pglaswell::tool_defs().size());
}

TEST(Tools, NamesAreUniqueAndCamelCase) {
  std::set<std::string> seen;
  for (const auto& t : pglaswell::tool_defs()) {
    const std::string name = t.name;
    ASSERT_FALSE(name.empty());
    EXPECT_TRUE(seen.insert(name).second) << "duplicate tool name: " << name;
    EXPECT_GE(name[0], 'a');
    EXPECT_LE(name[0], 'z') << name << " must start lower-case";
    EXPECT_EQ(name.find('_'), std::string::npos)
        << name << " must be camelCase on the wire, not snake_case";
  }
}

TEST(Tools, EveryToolHasADescriptionAndSchemas) {
  for (const auto& t : pglaswell::tool_defs()) {
    EXPECT_NE(t.description, nullptr) << t.name;
    EXPECT_STRNE(t.description, "") << t.name;
    ASSERT_NE(t.input_schema, nullptr) << t.name;
    EXPECT_TRUE(t.input_schema().is_object()) << t.name;
    ASSERT_NE(t.invoke, nullptr) << t.name;
  }
}

TEST(Tools, EveryLongRunningToolReturnsAJobId) {
  // A long-running tool that returned results inline would block the stdio
  // loop for the length of a migration, which is the one thing the job
  // registry exists to prevent.
  for (const auto& t : pglaswell::tool_defs()) {
    if (!t.hints.long_running) continue;
    ASSERT_NE(t.output_schema, nullptr) << t.name;
    const json schema = t.output_schema();
    EXPECT_TRUE(schema.dump().find("jobId") != std::string::npos)
        << t.name << " is long-running but its output schema has no jobId";
  }
}

TEST(Tools, CallingAnUnknownToolIsMethodNotFound) {
  McpServer s;
  initialize(s);
  const json r = rpc(s, json{{"jsonrpc", "2.0"},
                             {"id", 5},
                             {"method", "tools/call"},
                             {"params", {{"name", "nosuchTool"}}}});
  ASSERT_TRUE(r.contains("error")) << r.dump();
  EXPECT_EQ(r["error"]["code"], pglaswell::kMethodNotFound);
}

TEST(Tools, CallWithoutANameIsInvalidParams) {
  McpServer s;
  initialize(s);
  const json r = rpc(s, json{{"jsonrpc", "2.0"},
                             {"id", 6},
                             {"method", "tools/call"},
                             {"params", json::object()}});
  ASSERT_TRUE(r.contains("error")) << r.dump();
  EXPECT_EQ(r["error"]["code"], pglaswell::kInvalidParams);
}

// --- the database fixture -------------------------------------------------
//
// Empty through phase 1; it exists now so that the skip-vs-require policy is
// settled before the first test needs a server, rather than being invented
// under pressure when one does.

class DatabaseTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const char* url = std::getenv("DATABASE_URL");
    if (url == nullptr || *url == '\0') {
      if (std::getenv("PGLASWELL_REQUIRE_DATABASE") != nullptr) {
        FAIL() << "DATABASE_URL is unset and PGLASWELL_REQUIRE_DATABASE=1. "
                  "Set DATABASE_URL, e.g. "
                  "DATABASE_URL=\"port=5555 dbname=postgres\".";
      }
      GTEST_SKIP() << "DATABASE_URL is unset; skipping the database tests. "
                      "Set PGLASWELL_REQUIRE_DATABASE=1 to make this a failure.";
    }
    url_ = url;
  }
  std::string url_;
};

TEST_F(DatabaseTest, FixtureResolvesAConnectionString) {
  EXPECT_FALSE(url_.empty());
}

// --- S6: cancelling a worker's statement from the observer thread ---------
//
// The escalation path depends on this: when a backend has been blocked behind
// one of our batch statements for longer than kMaxWaiterWaitMs, the observer
// thread cancels that statement rather than waiting for it to finish. libpqxx
// documents cancel_query() as callable from another thread, with the caveat
// that it is the caller's job not to cancel the wrong query. These tests are
// what makes that caveat concrete, and they run under TSAN in CI.

TEST_F(DatabaseTest, CancelQueryFromAnotherThreadStopsALongStatement) {
  pqxx::connection worker(url_);
  const int worker_pid = worker.backendpid();
  ASSERT_GT(worker_pid, 0);

  std::atomic<bool> threw_sql_error{false};
  std::atomic<bool> finished{false};
  std::string sqlstate;

  const auto started = std::chrono::steady_clock::now();
  std::thread runner([&] {
    try {
      pqxx::work txn(worker);
      // Long enough that a pass here cannot be the statement simply finishing.
      txn.exec("SELECT pg_sleep(30)");
      txn.commit();
    } catch (const pqxx::sql_error& e) {
      threw_sql_error = true;
      sqlstate = e.sqlstate();
    } catch (const std::exception&) {
      // Any other exception leaves threw_sql_error false and fails below.
    }
    finished = true;
  });

  // Do not cancel until the statement is provably in flight. Cancelling before
  // the query is sent would cancel nothing and the test would pass for the
  // wrong reason -- this is exactly the "wrong query" hazard the libpqxx
  // documentation warns about, and the observer has the same obligation.
  {
    pqxx::connection watcher(url_);
    bool running = false;
    for (int i = 0; i < 200 && !running; ++i) {
      pqxx::nontransaction tx(watcher);
      const auto r = tx.exec(
          "SELECT count(*) FROM pg_stat_activity "
          " WHERE pid = " + std::to_string(worker_pid) +
          "   AND state = 'active' AND query LIKE '%pg_sleep%'");
      running = !r.empty() && r[0][0].as<int>() > 0;
      if (!running) std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    ASSERT_TRUE(running) << "the sleep never became visible in pg_stat_activity";
  }

  worker.cancel_query();
  runner.join();
  const auto elapsed = std::chrono::steady_clock::now() - started;

  EXPECT_TRUE(finished);
  ASSERT_TRUE(threw_sql_error) << "cancel_query() did not interrupt the statement";
  // 57014 is query_canceled. libpqxx has no exception class for it -- see the
  // next test -- so the SQLSTATE string is the only handle.
  EXPECT_EQ(sqlstate, "57014");
  EXPECT_LT(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count(), 20)
      << "the statement ran to completion instead of being cancelled";
}

TEST_F(DatabaseTest, ACancelledQueryHasNoDedicatedExceptionClassSoMatchSqlstate) {
  // pg_licht's hard-won lesson is that insufficient_privilege must be matched
  // by exception TYPE, because libpqxx leaves sqlstate() empty on that class.
  // Cancellation is the mirror image: libpqxx declares no query_canceled
  // class, so a cancelled statement arrives as a plain sql_error and the type
  // tells you nothing. Type where a class exists, SQLSTATE where it does not.
  //
  // The consequence for the executor, recorded here because it is easy to miss:
  // 57014 is ALSO what statement_timeout raises. The two cannot be told apart
  // from the error alone, so the worker must consult its own
  // PacingState::cancel_requested_ flag to know which happened. An executor
  // that reported every 57014 as "we cancelled you" would silently mislabel
  // every statement-timeout as deliberate.
  pqxx::connection conn(url_);
  bool caught_plain_sql_error = false;
  try {
    pqxx::work txn(conn);
    txn.exec("SET LOCAL statement_timeout = 100");
    txn.exec("SELECT pg_sleep(5)");
    txn.commit();
  } catch (const pqxx::sql_error& e) {
    caught_plain_sql_error = true;
    EXPECT_EQ(std::string(e.sqlstate()), "57014")
        << "statement_timeout should also surface as 57014";
  }
  EXPECT_TRUE(caught_plain_sql_error);
}

TEST_F(DatabaseTest, SetLocalIsSilentlyANoOpOnANontransaction) {
  // Found by writing the test above wrongly, and kept because the executor is
  // going to walk into it: CREATE INDEX CONCURRENTLY cannot run inside a
  // transaction block, so it runs on a pqxx::nontransaction -- and SET LOCAL
  // there is not an error, it simply does nothing. PostgreSQL emits a warning
  // ("SET LOCAL can only be used in transaction blocks") that libpqxx does not
  // raise, so the timeout would appear to be set and would not be.
  //
  // Consequence: the CIC step must use a session-level SET with an explicit
  // RESET afterwards, never SET LOCAL. That is the one place this project
  // knowingly uses session state, and it is licensed only because the executor
  // requires a direct connection.
  pqxx::connection conn(url_);
  pqxx::nontransaction tx(conn);
  tx.exec("SET LOCAL statement_timeout = 100");
  const auto after_set_local =
      tx.exec("SELECT current_setting('statement_timeout')")[0][0]
          .as<std::string>();
  EXPECT_EQ(after_set_local, "0")
      << "SET LOCAL unexpectedly took effect outside a transaction block; "
         "if this ever changes, the CIC step's session-level SET can be "
         "simplified";

  // A session-level SET, by contrast, does take effect here.
  tx.exec("SET statement_timeout = 100");
  const auto after_set =
      tx.exec("SELECT current_setting('statement_timeout')")[0][0]
          .as<std::string>();
  EXPECT_EQ(after_set, "100ms");
  tx.exec("RESET statement_timeout");
}
