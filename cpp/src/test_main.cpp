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

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
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

#include "canonical.h"
#include "config.h"
#include "server.h"
#include "spec.h"
#include "trust.h"

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

// --- canonicalisation (RFC 8785) -----------------------------------------

TEST(Canonical, SortsObjectKeys) {
  EXPECT_EQ(pglaswell::canonicalize(json::parse(R"({"b":1,"a":2})")),
            R"({"a":2,"b":1})");
}

TEST(Canonical, EmitsNoInsignificantWhitespace) {
  EXPECT_EQ(pglaswell::canonicalize(json::parse("{ \"a\" : [ 1 , 2 ] }")),
            R"({"a":[1,2]})");
}

TEST(Canonical, PreservesArrayOrder) {
  // Arrays are ordered data; only object members are sorted.
  EXPECT_EQ(pglaswell::canonicalize(json::parse(R"([3,1,2])")), "[3,1,2]");
}

TEST(Canonical, EscapesOnlyWhatItMust) {
  const json v = json::parse(R"({"k":"a\"b\\c\nd\te"})");
  EXPECT_EQ(pglaswell::canonicalize(v), "{\"k\":\"a\\\"b\\\\c\\nd\\te\"}");
}

TEST(Canonical, LeavesNonAsciiAsLiteralUtf8) {
  // JCS does NOT \u-escape non-ASCII. Escaping it would still round-trip as
  // JSON but would produce different bytes from every other implementation.
  const json v = json::parse(R"({"k":"café"})");
  EXPECT_EQ(pglaswell::canonicalize(v), "{\"k\":\"café\"}");
}

TEST(Canonical, EscapesControlCharactersAsFourHexDigits) {
  json v;
  v["k"] = std::string("a\x01" "b");
  EXPECT_EQ(pglaswell::canonicalize(v), "{\"k\":\"a\\u0001b\"}");
}

TEST(Canonical, SortsKeysByUtf16CodeUnitsNotBytes) {
  // The case that separates a correct implementation from a byte-sorting one.
  // U+FF3A (fullwidth Z, three UTF-8 bytes, one UTF-16 unit 0xFF3A) versus
  // U+10000 (four UTF-8 bytes, surrogate pair starting 0xD800). In byte order
  // the four-byte sequence sorts FIRST (0xF0 < 0xEF); in UTF-16 order it sorts
  // SECOND, because 0xD800 < 0xFF3A.
  json v;
  v["\xEF\xBC\xBA"] = 1;          // U+FF3A
  v["\xF0\x90\x80\x80"] = 2;      // U+10000
  const auto out = pglaswell::canonicalize(v);
  EXPECT_LT(out.find("\xF0\x90\x80\x80"), out.find("\xEF\xBC\xBA"))
      << "keys were sorted by bytes rather than by UTF-16 code units: " << out;
}

TEST(Canonical, ReformattingTheDocumentDoesNotChangeTheDigest) {
  // THE property the whole signature scheme rests on. If this ever fails,
  // every signed spec in existence stops verifying.
  const json a = json::parse(R"({
      "laswell_spec_version": 1,
      "id": "0001-x",
      "intents": [ {"kind": "add_column", "column": "c"} ]
  })");
  const json b = json::parse(
      R"({"intents":[{"column":"c","kind":"add_column"}],)"
      R"("id":"0001-x","laswell_spec_version":1})");
  EXPECT_EQ(pglaswell::canonicalize(a), pglaswell::canonicalize(b));
  EXPECT_EQ(pglaswell::digest_hex(a), pglaswell::digest_hex(b));
}

TEST(Canonical, AnyDifferenceAParserCanSeeChangesTheDigest) {
  EXPECT_NE(pglaswell::digest_hex(json::parse(R"({"a":1})")),
            pglaswell::digest_hex(json::parse(R"({"a":2})")));
  EXPECT_NE(pglaswell::digest_hex(json::parse(R"({"a":1})")),
            pglaswell::digest_hex(json::parse(R"({"a":"1"})")));
  EXPECT_NE(pglaswell::digest_hex(json::parse(R"([1,2])")),
            pglaswell::digest_hex(json::parse(R"([2,1])")));
}

TEST(Canonical, AFloatIsRefusedAndNamesItsLocation) {
  try {
    pglaswell::canonicalize(json::parse(R"({"outer":{"inner":1.5}})"));
    FAIL() << "a float should be refused";
  } catch (const std::runtime_error& e) {
    const std::string what = e.what();
    EXPECT_NE(what.find("floating-point"), std::string::npos) << what;
    EXPECT_NE(what.find("/outer/inner"), std::string::npos)
        << "the error must name the offending path: " << what;
  }
}

TEST(Canonical, AnIntegerBeyondTwoToTheFiftyThreeIsRefused) {
  // Beyond 2^53 a JSON number stops round-tripping through the double most
  // clients parse it into, so it would not survive the trip to an agent.
  EXPECT_THROW(pglaswell::canonicalize(json::parse(R"({"n":9007199254740993})")),
               std::runtime_error);
  EXPECT_NO_THROW(pglaswell::canonicalize(json::parse(R"({"n":9007199254740991})")));
}

TEST(Canonical, Sha256MatchesTheKnownEmptyStringDigest) {
  EXPECT_EQ(pglaswell::to_hex(pglaswell::sha256("")),
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST(Canonical, MatchesTheRfc8785StringEscapingVector) {
  // The string half of the RFC 8785 worked example. Its number half exercises
  // ECMAScript Number::toString, which this implementation refuses outright
  // rather than implements -- see the header comment in canonical.h.
  const json v = json::parse(
      "{\"string\":\"\\u20ac$\\u000F\\u000aA'\\u0042\\u0022"
      "\\u005c\\\\\\\"\\/\"}");
  EXPECT_EQ(pglaswell::canonicalize(v),
            "{\"string\":\""
            "\xE2\x82\xAC"          // U+20AC, literal UTF-8, never \u-escaped
            "$"
            "\\u000f"               // control chars take the \u00XX form
            "\\n"                   // ... except the five with short forms
            "A'B"
            "\\\""                  // a quote is escaped
            "\\\\\\\\"              // two backslashes, each escaped
            "\\\""
            "/"                     // a solidus is NOT escaped
            "\"}");
}

TEST(Canonical, MalformedUtf8IsRefusedRatherThanGuessedAt) {
  // A canonicalizer that repaired broken UTF-8 would produce bytes depending
  // on which implementation did the repairing, which defeats the purpose.
  json v;
  v[std::string("k\xC3")] = 1;  // truncated two-byte sequence in a KEY
  EXPECT_THROW(pglaswell::canonicalize(v), std::runtime_error);
}

// --- trust: Ed25519 verification -----------------------------------------

namespace {

// An Ed25519 keypair generated once per run rather than checked in. A test
// private key in a repository is the sort of thing that gets copied into
// something that matters.
struct TestKey {
  std::vector<unsigned char> pub;
  pglaswell::detail::PkeyPtr pkey;
  std::string key_id;
};

TestKey& test_key() {
  static TestKey k = [] {
    TestKey out;
    EVP_PKEY* raw = nullptr;
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
    EVP_PKEY_keygen_init(ctx);
    EVP_PKEY_keygen(ctx, &raw);
    EVP_PKEY_CTX_free(ctx);
    out.pkey.reset(raw);
    std::size_t len = 32;
    out.pub.resize(32);
    EVP_PKEY_get_raw_public_key(raw, out.pub.data(), &len);
    out.key_id = pglaswell::key_id_for(out.pub);
    return out;
  }();
  return k;
}

std::vector<unsigned char> sign(const std::string& message) {
  pglaswell::detail::MdCtxPtr ctx(EVP_MD_CTX_new());
  EVP_DigestSignInit(ctx.get(), nullptr, nullptr, nullptr, test_key().pkey.get());
  std::size_t siglen = 0;
  EVP_DigestSign(ctx.get(), nullptr, &siglen,
                 reinterpret_cast<const unsigned char*>(message.data()),
                 message.size());
  std::vector<unsigned char> sig(siglen);
  EVP_DigestSign(ctx.get(), sig.data(), &siglen,
                 reinterpret_cast<const unsigned char*>(message.data()),
                 message.size());
  sig.resize(siglen);
  return sig;
}

std::string base64(const std::vector<unsigned char>& in) {
  static const char* kAlpha =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for (std::size_t i = 0; i < in.size(); i += 3) {
    const unsigned int b0 = in[i];
    const unsigned int b1 = (i + 1 < in.size()) ? in[i + 1] : 0u;
    const unsigned int b2 = (i + 2 < in.size()) ? in[i + 2] : 0u;
    const unsigned int t = (b0 << 16) | (b1 << 8) | b2;
    out += kAlpha[(t >> 18) & 0x3Fu];
    out += kAlpha[(t >> 12) & 0x3Fu];
    out += (i + 1 < in.size()) ? kAlpha[(t >> 6) & 0x3Fu] : '=';
    out += (i + 2 < in.size()) ? kAlpha[t & 0x3Fu] : '=';
  }
  return out;
}

pglaswell::TrustPolicy policy_trusting_test_key() {
  pglaswell::TrustPolicy p;
  pglaswell::TrustedKey k;
  k.key_id = test_key().key_id;
  k.label = "suite";
  k.public_key = test_key().pub;
  p.keys[k.key_id] = k;
  p.accept.push_back(k.key_id);
  return p;
}

}  // namespace

TEST(Trust, AValidSignatureVerifies) {
  const std::string msg = "the canonical bytes";
  EXPECT_TRUE(pglaswell::verify_ed25519(test_key().pub, msg, sign(msg)));
}

TEST(Trust, ABitFlippedSignatureIsRejected) {
  const std::string msg = "the canonical bytes";
  auto sig = sign(msg);
  sig[10] = static_cast<unsigned char>(sig[10] ^ 0x01);
  EXPECT_FALSE(pglaswell::verify_ed25519(test_key().pub, msg, sig));
}

TEST(Trust, ASignatureOverDifferentBytesIsRejected) {
  EXPECT_FALSE(pglaswell::verify_ed25519(test_key().pub, "other bytes",
                                         sign("the canonical bytes")));
}

TEST(Trust, AWrongLengthKeyOrSignatureIsRejectedNotCrashed) {
  const std::string msg = "x";
  EXPECT_FALSE(pglaswell::verify_ed25519({1, 2, 3}, msg, sign(msg)));
  EXPECT_FALSE(pglaswell::verify_ed25519(test_key().pub, msg, {1, 2, 3}));
}

TEST(Trust, KeyIdIsTheContentAddressOfTheKey) {
  const auto id = pglaswell::key_id_for(test_key().pub);
  EXPECT_EQ(id.rfind("ed25519:", 0), 0u);
  EXPECT_EQ(id.size(), 8u + 16u);
  auto other = test_key().pub;
  other[0] = static_cast<unsigned char>(other[0] ^ 0xFF);
  EXPECT_NE(pglaswell::key_id_for(other), id);
}

TEST(Trust, PolicyVerifiesASpecSignedByAnAcceptedKey) {
  const std::string bytes = "canonical bytes";
  const json sigs = json::array({{{"key_id", test_key().key_id},
                                  {"algorithm", "ed25519"},
                                  {"signature", base64(sign(bytes))}}});
  const auto r =
      pglaswell::verify_against_policy(policy_trusting_test_key(), bytes, sigs);
  ASSERT_TRUE(r.verified) << r.reason;
  EXPECT_EQ(r.key_id, test_key().key_id);
  EXPECT_EQ(r.label, "suite");
}

TEST(Trust, PolicyRejectsASignatureOverDifferentCanonicalBytes) {
  // The tamper case: the spec was edited after signing.
  const json sigs = json::array({{{"key_id", test_key().key_id},
                                  {"algorithm", "ed25519"},
                                  {"signature", base64(sign("original"))}}});
  const auto r = pglaswell::verify_against_policy(policy_trusting_test_key(),
                                                  "tampered", sigs);
  EXPECT_FALSE(r.verified);
  EXPECT_NE(r.reason.find("does not verify"), std::string::npos) << r.reason;
}

TEST(Trust, PolicyRejectsASignatureFromAKeyItDoesNotAccept) {
  pglaswell::TrustPolicy policy;  // accepts nothing
  const json sigs = json::array({{{"key_id", test_key().key_id},
                                  {"algorithm", "ed25519"},
                                  {"signature", "AAAA"}}});
  const auto r = pglaswell::verify_against_policy(policy, "canonical", sigs);
  EXPECT_FALSE(r.verified);
  EXPECT_NE(r.reason.find("accepts"), std::string::npos) << r.reason;
}

TEST(Trust, PolicyRefusesAKeyIdThatDoesNotMatchItsOwnKeyBytes) {
  // The attack this closes: a config files an attacker's key under a trusted
  // key id. Every later check would pass without this.
  pglaswell::TrustPolicy policy;
  pglaswell::TrustedKey k;
  k.key_id = "ed25519:0000000000000000";  // not the content address
  k.label = "impostor";
  k.public_key = test_key().pub;
  policy.keys[k.key_id] = k;
  policy.accept.push_back(k.key_id);

  const json sigs = json::array({{{"key_id", k.key_id},
                                  {"algorithm", "ed25519"},
                                  {"signature", "AAAA"}}});
  const auto r = pglaswell::verify_against_policy(policy, "canonical", sigs);
  EXPECT_FALSE(r.verified);
  EXPECT_NE(r.reason.find("does not match its own public key"), std::string::npos)
      << r.reason;
}

TEST(Trust, AnUnsupportedAlgorithmIsRefusedRatherThanIgnored) {
  pglaswell::TrustPolicy policy;
  const json sigs = json::array(
      {{{"key_id", "x"}, {"algorithm", "rsa"}, {"signature", "AAAA"}}});
  const auto r = pglaswell::verify_against_policy(policy, "b", sigs);
  EXPECT_FALSE(r.verified);
  EXPECT_NE(r.reason.find("ed25519 only"), std::string::npos) << r.reason;
}

TEST(Trust, AnUnsignedSpecIsRefused) {
  pglaswell::TrustPolicy policy;
  const auto r = pglaswell::verify_against_policy(policy, "b", json::array());
  EXPECT_FALSE(r.verified);
  EXPECT_NE(r.reason.find("no signatures"), std::string::npos) << r.reason;
}

TEST(Trust, Base64DecoderIsStrictAboutInvalidInput) {
  // A lenient decoder would silently accept a corrupted key and produce a key
  // id that simply never matches -- much harder to read than "not base64".
  EXPECT_THROW(pglaswell::Registry::base64_decode("AA*A"), std::runtime_error);
  EXPECT_THROW(pglaswell::Registry::base64_decode("AAA"), std::runtime_error);
  EXPECT_THROW(pglaswell::Registry::base64_decode("A=AA"), std::runtime_error);
  EXPECT_EQ(pglaswell::Registry::base64_decode("AAAA").size(), 3u);
  EXPECT_EQ(pglaswell::Registry::base64_decode("AAA=").size(), 2u);
  EXPECT_EQ(pglaswell::Registry::base64_decode("AA==").size(), 1u);
}

TEST(Trust, Base64RoundTripsThroughTheDecoder) {
  EXPECT_EQ(pglaswell::Registry::base64_decode(base64(test_key().pub)),
            test_key().pub);
}

// --- spec parsing ---------------------------------------------------------

namespace {

json minimal_spec() {
  return json::parse(R"({
    "laswell_spec_version": 1,
    "id": "0001-orders-fulfilment-region",
    "description": "Route orders by warehouse region without a join at read time.",
    "target": { "database": "shop_prod", "min_server_version": 150000 },
    "intents": [
      { "kind": "add_column", "schema": "shop", "table": "orders",
        "column": "fulfilment_region", "type": "text", "nullable": true,
        "comment": "Denormalised warehouse.region." },
      { "kind": "backfill", "schema": "shop", "table": "orders", "key": "id",
        "set": { "fulfilment_region": "w.region" },
        "from": "shop.warehouse AS w",
        "where": "orders.warehouse_id = w.id AND orders.fulfilment_region IS NULL",
        "verify_remaining": "fulfilment_region IS NULL" },
      { "kind": "create_index", "schema": "shop", "table": "orders",
        "name": "orders_open_by_region_idx",
        "columns": ["fulfilment_region", "created_at"],
        "unique": false, "method": "btree", "where": "status = 'open'",
        "comment": "Serves the fulfilment queue." }
    ]
  })");
}

std::string spec_error(const json& doc) {
  try {
    pglaswell::parse_spec(doc);
  } catch (const pglaswell::SpecError& e) {
    return std::string(e.what()) + " || " + e.hint();
  }
  return "<no error>";
}

}  // namespace

TEST(Spec, ParsesTheWorkedExample) {
  const auto s = pglaswell::parse_spec(minimal_spec());
  EXPECT_EQ(s.id, "0001-orders-fulfilment-region");
  EXPECT_EQ(s.target_database, "shop_prod");
  EXPECT_EQ(s.min_server_version, 150000);
  ASSERT_EQ(s.intents.size(), 3u);
  EXPECT_EQ(s.intents[0].kind, pglaswell::IntentKind::kAddColumn);
  EXPECT_EQ(s.intents[1].kind, pglaswell::IntentKind::kBackfill);
  EXPECT_EQ(s.intents[2].kind, pglaswell::IntentKind::kCreateIndex);
  EXPECT_EQ(s.intents[2].qualified_table(), "shop.orders");
  EXPECT_EQ(s.digest.size(), 64u);
}

TEST(Spec, TheSignedProjectionExcludesSignaturesAndNothingElse) {
  json doc = minimal_spec();
  const auto before = pglaswell::parse_spec(doc).digest;
  doc["signatures"] = json::array({{{"key_id", "ed25519:abc"},
                                    {"signature", "AAAA"}}});
  const auto after = pglaswell::parse_spec(doc).digest;
  // Adding a signature must not change what the signature covers, or signing
  // would be impossible: the act of attaching one would invalidate it.
  EXPECT_EQ(before, after);
  EXPECT_FALSE(pglaswell::parse_spec(doc).signed_projection.contains("signatures"));
}

TEST(Spec, ReformattingTheDocumentDoesNotChangeTheDigest) {
  const auto a = pglaswell::parse_spec(minimal_spec());
  const auto b = pglaswell::parse_spec(json::parse(minimal_spec().dump(4)));
  EXPECT_EQ(a.digest, b.digest);
}

TEST(Spec, AnUnknownTopLevelKeyIsAFatalParseError) {
  // The signature-stripping defence. With a blocklist an attacker appends a
  // key the canonicalizer skips and the parser honours.
  json doc = minimal_spec();
  doc["extra_instructions"] = "drop everything";
  const auto err = spec_error(doc);
  EXPECT_NE(err.find("unknown top-level key"), std::string::npos) << err;
  EXPECT_NE(err.find("extra_instructions"), std::string::npos) << err;
}

TEST(Spec, AnUnknownIntentKindRefusesTheWholeSpecAndListsWhatIsSupported) {
  json doc = minimal_spec();
  doc["intents"][1]["kind"] = "add_check_constraint";
  const auto err = spec_error(doc);
  EXPECT_NE(err.find("add_check_constraint"), std::string::npos) << err;
  EXPECT_NE(err.find("add_column"), std::string::npos)
      << "the error must list the kinds this binary supports: " << err;
  // And nothing is planned: parse_spec throws, so no partial spec escapes.
  EXPECT_THROW(pglaswell::parse_spec(doc), pglaswell::SpecError);
}

TEST(Spec, KnownKindsAreExactlyTheImplementedKinds) {
  // The governing invariant. Every kind in the table must parse a
  // representative body; a kind that is listed but unimplemented would be
  // accepted and then silently skipped.
  const json bodies = json::parse(R"({
    "add_column":   {"kind":"add_column","schema":"s","table":"t","column":"c",
                     "type":"text","nullable":true,"comment":"c"},
    "backfill":     {"kind":"backfill","schema":"s","table":"t","key":"id",
                     "set":{"c":"1"},"where":"true"},
    "create_index": {"kind":"create_index","schema":"s","table":"t","name":"i",
                     "columns":["c"],"comment":"c"}
  })");
  for (const auto& [name, kind] : pglaswell::intent_kinds()) {
    (void)kind;
    ASSERT_TRUE(bodies.contains(name))
        << name << " is a known kind with no representative body here, which "
                   "means it may be listed but not implemented";
    json doc = minimal_spec();
    doc["intents"] = json::array({bodies[name]});
    EXPECT_NO_THROW(pglaswell::parse_spec(doc)) << name;
  }
  EXPECT_EQ(bodies.size(), pglaswell::intent_kinds().size());
}

TEST(Spec, AnIntentThatCreatesAnObjectMustCarryAComment) {
  for (const char* kind : {"add_column", "create_index"}) {
    json doc = minimal_spec();
    for (auto& intent : doc["intents"]) {
      if (intent["kind"] == kind) intent.erase("comment");
    }
    const auto err = spec_error(doc);
    EXPECT_NE(err.find("comment"), std::string::npos) << kind << ": " << err;
  }
}

TEST(Spec, AddColumnMustStateNullabilityExplicitly) {
  json doc = minimal_spec();
  doc["intents"][0].erase("nullable");
  const auto err = spec_error(doc);
  EXPECT_NE(err.find("nullable"), std::string::npos) << err;
  EXPECT_NE(err.find("risk class"), std::string::npos)
      << "the hint should say why there is no default: " << err;
}

TEST(Spec, AnUnfilteredBackfillIsRefusedWithAWayToOptIn) {
  json doc = minimal_spec();
  doc["intents"][1].erase("where");
  const auto err = spec_error(doc);
  EXPECT_NE(err.find("rewrites every row"), std::string::npos) << err;
  EXPECT_NE(err.find("\"true\""), std::string::npos)
      << "the hint must say how to opt in deliberately: " << err;
  // ... and opting in explicitly works.
  doc["intents"][1]["where"] = "true";
  EXPECT_NO_THROW(pglaswell::parse_spec(doc));
}

TEST(Spec, AnUnknownKeyInsideAnIntentIsRefused) {
  json doc = minimal_spec();
  doc["intents"][0]["cascade"] = true;
  const auto err = spec_error(doc);
  EXPECT_NE(err.find("cascade"), std::string::npos) << err;
  EXPECT_NE(err.find("Accepted keys"), std::string::npos) << err;
}

TEST(Spec, IdentifiersThatWouldNeedQuotingAreRefused) {
  for (const char* bad : {"Orders", "orders;drop", "order s", "1st", ""}) {
    json doc = minimal_spec();
    doc["intents"][0]["table"] = bad;
    EXPECT_THROW(pglaswell::parse_spec(doc), pglaswell::SpecError)
        << "accepted a table name that needs quoting: " << bad;
  }
}

TEST(Spec, AnOverlongIdentifierIsRefusedBecausePostgresWouldTruncateIt) {
  json doc = minimal_spec();
  doc["intents"][0]["column"] = std::string(64, 'a');
  const auto err = spec_error(doc);
  EXPECT_NE(err.find("63 bytes"), std::string::npos) << err;
}

TEST(Spec, AWrongSpecVersionIsRefused) {
  json doc = minimal_spec();
  doc["laswell_spec_version"] = 2;
  const auto err = spec_error(doc);
  EXPECT_NE(err.find("not supported by this binary"), std::string::npos) << err;
}

TEST(Spec, ADescriptionIsRequiredBecauseJsonHasNoComments) {
  json doc = minimal_spec();
  doc.erase("description");
  EXPECT_THROW(pglaswell::parse_spec(doc), pglaswell::SpecError);
}

TEST(Spec, AFloatAnywhereInASpecIsRefused) {
  json doc = minimal_spec();
  doc["intents"][0]["type"] = "numeric";
  doc["target"]["min_server_version"] = 150000;
  doc["rationale"] = "fine";
  // A float reaches canonicalisation, which refuses it.
  doc["intents"] = json::array({json::parse(R"({
      "kind":"add_column","schema":"s","table":"t","column":"c",
      "type":"numeric","nullable":true,"comment":"c"})")});
  doc["intents"][0]["scale"] = 2.5;
  EXPECT_THROW(pglaswell::parse_spec(doc), std::exception);
}

TEST(Spec, SignaturesSurviveParsingUnchanged) {
  json doc = minimal_spec();
  doc["signatures"] = json::array({{{"key_id", "ed25519:abc"},
                                    {"algorithm", "ed25519"},
                                    {"signature", "AAAA"}}});
  const auto s = pglaswell::parse_spec(doc);
  ASSERT_EQ(s.signatures.size(), 1u);
  EXPECT_EQ(s.signatures[0]["key_id"], "ed25519:abc");
}

TEST(Spec, EndToEndSignThenVerifyOverTheCanonicalBytes) {
  // The whole gate-1 path: parse, canonicalise, sign those exact bytes,
  // attach, re-parse, verify. Reformatting between the two must not matter.
  auto s = pglaswell::parse_spec(minimal_spec());
  json doc = minimal_spec();
  doc["signatures"] = json::array({{{"key_id", test_key().key_id},
                                    {"algorithm", "ed25519"},
                                    {"signature", base64(sign(s.canonical_bytes))}}});

  const auto reparsed = pglaswell::parse_spec(json::parse(doc.dump(2)));
  const auto check = pglaswell::verify_against_policy(
      policy_trusting_test_key(), reparsed.canonical_bytes, reparsed.signatures);
  ASSERT_TRUE(check.verified) << check.reason;
  EXPECT_EQ(check.key_id, test_key().key_id);
}

TEST(Spec, TamperingWithAnIntentBreaksTheSignature) {
  auto s = pglaswell::parse_spec(minimal_spec());
  json doc = minimal_spec();
  doc["signatures"] = json::array({{{"key_id", test_key().key_id},
                                    {"algorithm", "ed25519"},
                                    {"signature", base64(sign(s.canonical_bytes))}}});
  doc["intents"][2]["name"] = "orders_something_else_idx";

  const auto tampered = pglaswell::parse_spec(doc);
  const auto check = pglaswell::verify_against_policy(
      policy_trusting_test_key(), tampered.canonical_bytes, tampered.signatures);
  EXPECT_FALSE(check.verified);
  EXPECT_NE(check.reason.find("does not verify"), std::string::npos)
      << check.reason;
}

// --- configuration --------------------------------------------------------

namespace {

class TempIni {
 public:
  explicit TempIni(const std::string& body, mode_t mode = 0600) {
    path_ = "/tmp/laswell_test_" + std::to_string(::getpid()) + "_" +
            std::to_string(counter_++) + ".ini";
    std::ofstream out(path_);
    out << body;
    out.close();
    ::chmod(path_.c_str(), mode);
  }
  ~TempIni() { ::unlink(path_.c_str()); }
  TempIni(const TempIni&) = delete;
  TempIni& operator=(const TempIni&) = delete;
  const std::string& path() const { return path_; }

 private:
  std::string path_;
  static int counter_;
};
int TempIni::counter_ = 0;

std::string config_error(const std::string& body, mode_t mode = 0600) {
  TempIni ini(body, mode);
  try {
    pglaswell::Registry::from_ini(ini.path(), "test");
  } catch (const std::exception& e) {
    return e.what();
  }
  return "<no error>";
}

}  // namespace

TEST(Config, ParsesConnectionsExecutorAndTrust) {
  TempIni ini(R"(
[executor]
batch_rows = 50
commit_interval_ms = 100
observer_tick_ms = 25

[trust]
accept = ed25519:1111111111111111

[key ed25519:1111111111111111]
label      = daniel-dev
public_key = AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=

[shop_dev]
host   = localhost
port   = 5555
dbname = shop
)");
  const auto r = pglaswell::Registry::from_ini(ini.path(), "pg-laswell/test");
  EXPECT_EQ(r.default_name(), "shop_dev");
  EXPECT_EQ(r.executor().batch_rows, 50);
  EXPECT_EQ(r.get("shop_dev").executor.batch_rows, 50);
  EXPECT_NE(r.get("shop_dev").conninfo.find("dbname=shop"), std::string::npos);
  EXPECT_NE(r.get("shop_dev").conninfo.find("application_name=pg-laswell/test"),
            std::string::npos);
  EXPECT_TRUE(r.trust().accepts("ed25519:1111111111111111"));
  EXPECT_EQ(r.trust().keys.at("ed25519:1111111111111111").label, "daniel-dev");
}

TEST(Config, RefusesAGroupOrWorldWritableFile) {
  // pg_licht refuses a group/world-READABLE config because it may hold a
  // password. This one holds only public keys, so readability is fine -- but
  // it holds POLICY, and a policy anyone can rewrite is not a policy.
  const auto err = config_error("[a]\nhost = x\n", 0622);
  EXPECT_NE(err.find("writable"), std::string::npos) << err;
  EXPECT_NE(err.find("chmod go-w"), std::string::npos) << err;
}

TEST(Config, AGroupReadableFileIsFineUnlikePgLicht) {
  TempIni ini("[a]\nhost = x\n", 0644);
  EXPECT_NO_THROW(pglaswell::Registry::from_ini(ini.path(), "t"));
}

TEST(Config, ErrorsNameTheFileAndLineAndTheOffendingKey) {
  const auto err = config_error("[a]\nhost = x\n[a]\nhost = y\n");
  EXPECT_NE(err.find(":3"), std::string::npos) << err;
  EXPECT_NE(err.find("duplicate section"), std::string::npos) << err;

  const auto err2 = config_error("host = x\n");
  EXPECT_NE(err2.find(":1"), std::string::npos) << err2;
  EXPECT_NE(err2.find("outside any section"), std::string::npos) << err2;

  const auto err3 = config_error("[a]\nhost\n");
  EXPECT_NE(err3.find(":2"), std::string::npos) << err3;
  EXPECT_NE(err3.find("key = value"), std::string::npos) << err3;
}

TEST(Config, NothingIsSilentlyDefaulted) {
  const auto err = config_error("[executor]\nbatch_rows = lots\n[a]\nhost=x\n");
  EXPECT_NE(err.find("batch_rows"), std::string::npos) << err;
  EXPECT_NE(err.find("positive integer"), std::string::npos) << err;

  const auto err2 = config_error("[executor]\nbatch_rows = 0\n[a]\nhost=x\n");
  EXPECT_NE(err2.find("positive integer"), std::string::npos) << err2;
}

TEST(Config, AnUnknownExecutorKeyIsRefusedRatherThanIgnored) {
  // A typo'd safety knob that parses as "not set" is the failure this stops.
  const auto err = config_error("[executor]\nbatch_row = 10\n[a]\nhost=x\n");
  EXPECT_NE(err.find("unknown key batch_row"), std::string::npos) << err;
}

TEST(Config, ATrailingCommaInAListIsATypoNotAnEmptyEntry) {
  const auto err = config_error("[trust]\naccept = a, \n[a]\nhost=x\n");
  EXPECT_NE(err.find("empty entry"), std::string::npos) << err;
}

TEST(Config, AcceptingAKeyWithNoKeySectionIsRefused) {
  const auto err = config_error("[trust]\naccept = ed25519:abc\n[a]\nhost=x\n");
  EXPECT_NE(err.find("no [key ed25519:abc] section"), std::string::npos) << err;
}

TEST(Config, AKeyWithoutALabelIsRefused) {
  // A key nobody can name is a key nobody can talk about during an incident.
  const auto err = config_error(
      "[key ed25519:abc]\npublic_key = AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=\n"
      "[a]\nhost=x\n");
  EXPECT_NE(err.find("no label"), std::string::npos) << err;
}

TEST(Config, APublicKeyOfTheWrongLengthIsRefusedWithItsActualSize) {
  const auto err = config_error(
      "[key ed25519:abc]\nlabel = x\npublic_key = AAAA\n[a]\nhost=x\n");
  EXPECT_NE(err.find("32-byte"), std::string::npos) << err;
  EXPECT_NE(err.find("got 3 bytes"), std::string::npos) << err;
}

TEST(Config, AcceptsBothRawAndSpkiPublicKeyEncodings) {
  // `openssl pkey -pubout` emits SPKI; a raw 32-byte key is what many tools
  // hand out. Refusing either would be a papercut with no safety benefit.
  const auto& pub = test_key().pub;
  std::vector<unsigned char> spki = {0x30, 0x2a, 0x30, 0x05, 0x06, 0x03,
                                     0x2b, 0x65, 0x70, 0x03, 0x21, 0x00};
  spki.insert(spki.end(), pub.begin(), pub.end());
  const auto id = test_key().key_id;

  for (const auto& encoded : {base64(pub), base64(spki)}) {
    TempIni ini("[trust]\naccept = " + id + "\n[key " + id +
                "]\nlabel = k\npublic_key = " + encoded + "\n[a]\nhost=x\n");
    const auto r = pglaswell::Registry::from_ini(ini.path(), "t");
    EXPECT_EQ(r.trust().keys.at(id).public_key, pub);
  }
}

TEST(Config, OptionsInAConninfoIsRefusedWithTheReason) {
  const auto err = config_error("[a]\nhost = x\noptions = -c work_mem=64MB\n");
  EXPECT_NE(err.find("options"), std::string::npos) << err;
  EXPECT_NE(err.find("pooler"), std::string::npos) << err;
}

TEST(Config, InconsistentExecutorSettingsAreRefusedAtParseTime) {
  // A configuration that parses but cannot behave is worse than one that
  // fails to parse: the failure surfaces mid-migration instead of at startup.
  EXPECT_NE(config_error("[executor]\nbatch_rows = 100\nbatch_cap_rows = 50\n"
                         "[a]\nhost=x\n")
                .find("cap would fire on every batch"),
            std::string::npos);

  EXPECT_NE(config_error("[executor]\nthrottle_waiters = 9\npause_waiters = 8\n"
                         "[a]\nhost=x\n")
                .find("pause before throttling"),
            std::string::npos);

  EXPECT_NE(config_error("[executor]\npause_waiters = 4\nresume_waiters = 4\n"
                         "[a]\nhost=x\n")
                .find("flap"),
            std::string::npos);

  EXPECT_NE(config_error("[executor]\ncommit_interval_ms = 100\n"
                         "observer_tick_ms = 500\n[a]\nhost=x\n")
                .find("could not be seen"),
            std::string::npos);
}

TEST(Config, ADefaultConfigIsInternallyConsistent) {
  const pglaswell::ExecutorConfig e;
  EXPECT_NO_THROW(pglaswell::validate_executor(e, "defaults"));
}

TEST(Config, FromUrlNeedsNoFileAndConnectsToNothing) {
  const auto r = pglaswell::Registry::from_url("port=5555 dbname=x", "app");
  EXPECT_EQ(r.default_name(), "default");
  EXPECT_NE(r.get("default").conninfo.find("application_name=app"),
            std::string::npos);
}

TEST(Config, ConninfoValuesNeedingQuotingAreQuoted) {
  TempIni ini("[a]\nhost = x\napplication_name_hint = a b\n");
  const auto r = pglaswell::Registry::from_ini(ini.path(), "t");
  EXPECT_NE(r.get("a").conninfo.find("'a b'"), std::string::npos)
      << r.get("a").conninfo;
}
