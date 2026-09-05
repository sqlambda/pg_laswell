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

#include "canonical.h"
#include "catalog.h"
#include "config.h"
#include "observations.h"
#include "planner.h"
#include "server.h"
#include "session.h"
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

TEST(Config, RefusesAnyGroupOrWorldAccessibleFile) {
  // Same rule as ~/.pgpass. The file holds credentials (so not readable) and
  // it holds the trust policy (so not writable). 0077 covers both.
  for (const mode_t mode : {mode_t(0644), mode_t(0622), mode_t(0604),
                            mode_t(0640), mode_t(0660)}) {
    const auto err = config_error("[a]\nhost = x\n", mode);
    EXPECT_NE(err.find("group- or world-accessible"), std::string::npos)
        << "mode " << std::oct << mode << ": " << err;
    EXPECT_NE(err.find("chmod 600"), std::string::npos) << err;
  }
  TempIni ok("[a]\nhost = x\n", 0600);
  EXPECT_NO_THROW(pglaswell::Registry::from_ini(ok.path(), "t"));
}

TEST(Config, CarriesCredentialsAndKeepsThePasswordOutOfTheEchoFields) {
  // dbname/user/password belong here, as in any other migration tool. The
  // password reaches libpq through the conninfo and is deliberately NOT copied
  // into the struct's echo fields, so nothing that prints a ConnConfig for
  // diagnostics can leak it.
  TempIni ini(R"(
[shop_prod]
host     = db.internal
port     = 5432
dbname   = shop
user     = laswell_runner
password = s3cr3t p@ss
)");
  const auto r = pglaswell::Registry::from_ini(ini.path(), "t");
  const auto& c = r.get("shop_prod");
  EXPECT_EQ(c.dbname, "shop");
  EXPECT_EQ(c.user, "laswell_runner");
  EXPECT_NE(c.conninfo.find("password='s3cr3t p@ss'"), std::string::npos)
      << c.conninfo;
  // The echo fields carry no password field at all, by construction.
  EXPECT_EQ(c.host, "db.internal");
  EXPECT_EQ(c.port, "5432");
}

TEST(Config, AServiceEntryIsCarriedThroughForPgserviceUsers) {
  TempIni ini("[prod]\nservice = shop-prod\n");
  const auto r = pglaswell::Registry::from_ini(ini.path(), "t");
  EXPECT_EQ(r.get("prod").service, "shop-prod");
  EXPECT_NE(r.get("prod").conninfo.find("service=shop-prod"), std::string::npos);
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
  TempIni ini("[a]\nhost = x\npassword = a b\n");
  const auto r = pglaswell::Registry::from_ini(ini.path(), "t");
  EXPECT_NE(r.get("a").conninfo.find("'a b'"), std::string::npos)
      << r.get("a").conninfo;
}

// --- sessions -------------------------------------------------------------

TEST(ConnectionCache, HoldsOnlyIdleConnectionsSoTheReaperCannotRaceAUser) {
  // The design's whole safety argument: take() erases, so an in-use
  // connection is not in the map at all.
  pglaswell::ConnectionCache cache(std::chrono::seconds(60));
  EXPECT_EQ(cache.idle_count(), 0u);
  EXPECT_EQ(cache.take("absent"), nullptr);
}

TEST_F(DatabaseTest, ReadSessionIsReadOnlyAndAlwaysRollsBack) {
  pglaswell::ConnConfig cfg;
  cfg.name = "t";
  cfg.conninfo = url_;
  pglaswell::ReadSession s(cfg);
  EXPECT_GT(s.server_version(), 140000);
  EXPECT_GT(s.backend_pid(), 0);
  EXPECT_FALSE(s.in_recovery());

  // A write must fail. libpqxx maps SQLSTATE 25006 (write in a read-only
  // transaction) onto insufficient_privilege -- the SAME class it uses for a
  // missing grant, and with an empty sqlstate() on that class. A writing tool
  // must never report this shape as "you need a GRANT".
  EXPECT_THROW(s.txn().exec("CREATE TEMP TABLE t_readonly_probe(i int)"),
               pqxx::sql_error);
}

TEST_F(DatabaseTest, ReadSessionAppliesItsStatementTimeoutInOneRoundTrip) {
  pglaswell::ConnConfig cfg;
  cfg.name = "t";
  cfg.conninfo = url_;
  cfg.statement_timeout_ms = 250;
  pglaswell::ReadSession s(cfg);
  EXPECT_EQ(s.txn().exec("SELECT current_setting('statement_timeout')")[0][0]
                .as<std::string>(),
            "250ms");
}

TEST_F(DatabaseTest, ConnectionCacheReusesAConnectionBetweenSessions) {
  pglaswell::ConnConfig cfg;
  cfg.name = "t";
  cfg.conninfo = url_;
  pglaswell::ConnectionCache cache(std::chrono::seconds(60));
  int first_pid = 0;
  {
    pglaswell::ReadSession s(cfg, std::nullopt, &cache);
    first_pid = s.backend_pid();
  }
  EXPECT_EQ(cache.idle_count(), 1u);
  {
    pglaswell::ReadSession s(cfg, std::nullopt, &cache);
    EXPECT_EQ(s.backend_pid(), first_pid) << "the cached connection was not reused";
  }
}

TEST_F(DatabaseTest, WriteSessionCommitsOnlyWhenAsked) {
  pglaswell::ConnConfig cfg;
  cfg.name = "t";
  cfg.conninfo = url_;
  pglaswell::WriteSession w(cfg);

  w.begin("pg_laswell/test/setup");
  w.txn().exec("DROP TABLE IF EXISTS laswell_ws_probe");
  w.txn().exec("CREATE TABLE laswell_ws_probe(i int)");
  w.commit();

  // Rolled back: the row must not survive.
  w.begin("pg_laswell/test/rollback");
  w.txn().exec("INSERT INTO laswell_ws_probe VALUES (1)");
  w.rollback();

  w.begin("pg_laswell/test/check");
  EXPECT_EQ(w.txn().exec("SELECT count(*) FROM laswell_ws_probe")[0][0].as<int>(), 0);
  w.commit();

  // Committed: the row must survive.
  w.begin("pg_laswell/test/commit");
  w.txn().exec("INSERT INTO laswell_ws_probe VALUES (2)");
  w.commit();

  w.begin("pg_laswell/test/check2");
  EXPECT_EQ(w.txn().exec("SELECT count(*) FROM laswell_ws_probe")[0][0].as<int>(), 1);
  w.txn().exec("DROP TABLE laswell_ws_probe");
  w.commit();
}

TEST_F(DatabaseTest, WriteSessionSetsLockTimeoutAndApplicationNameOnEveryTransaction) {
  pglaswell::ConnConfig cfg;
  cfg.name = "t";
  cfg.conninfo = url_;
  cfg.executor.lock_timeout_ms = 1234;
  cfg.executor.commit_interval_ms = 1000;
  pglaswell::WriteSession w(cfg);

  w.begin("pg_laswell/job-abc/step-2");
  const auto r = w.txn().exec(
      "SELECT current_setting('lock_timeout'), current_setting('application_name'),"
      "       current_setting('idle_in_transaction_session_timeout')");
  EXPECT_EQ(r[0][0].as<std::string>(), "1234ms");
  EXPECT_EQ(r[0][1].as<std::string>(), "pg_laswell/job-abc/step-2");
  // A self-guard: if this process hangs, the server tears the transaction
  // down rather than leaving it holding locks.
  EXPECT_EQ(r[0][2].as<std::string>(), "3s");
  w.commit();
}

TEST_F(DatabaseTest, WriteSessionRefusesToOpenTwoTransactionsAtOnce) {
  pglaswell::ConnConfig cfg;
  cfg.name = "t";
  cfg.conninfo = url_;
  pglaswell::WriteSession w(cfg);
  w.begin("pg_laswell/test");
  EXPECT_THROW(w.begin("pg_laswell/test"), std::runtime_error);
  w.rollback();
}

TEST_F(DatabaseTest, ExecNontransactionalRefusesWhileATransactionIsOpen) {
  // Otherwise the statement fails with "cannot run inside a transaction
  // block", several layers away from the mistake.
  pglaswell::ConnConfig cfg;
  cfg.name = "t";
  cfg.conninfo = url_;
  pglaswell::WriteSession w(cfg);
  w.begin("pg_laswell/test");
  EXPECT_THROW(w.exec_nontransactional("SELECT 1"), std::runtime_error);
  w.rollback();
  EXPECT_NO_THROW(w.exec_nontransactional("SELECT 1"));
}

TEST_F(DatabaseTest, WithSessionSettingAppliesAndResetsEvenOnAnExceptionPath) {
  // This is the CIC path: statement_timeout must be lifted for a build that
  // legitimately runs for hours, and must not stay lifted afterwards.
  pglaswell::ConnConfig cfg;
  cfg.name = "t";
  cfg.conninfo = url_;
  pglaswell::WriteSession w(cfg);

  const auto inside = w.with_session_setting("statement_timeout", "4321", [&] {
    return w.exec_nontransactional("SELECT current_setting('statement_timeout')")[0][0]
        .as<std::string>();
  });
  EXPECT_EQ(inside, "4321ms");
  EXPECT_EQ(w.exec_nontransactional("SELECT current_setting('statement_timeout')")[0][0]
                .as<std::string>(),
            "0");

  EXPECT_THROW(w.with_session_setting("statement_timeout", "4321",
                                      [&]() -> int { throw std::runtime_error("boom"); }),
               std::runtime_error);
  EXPECT_EQ(w.exec_nontransactional("SELECT current_setting('statement_timeout')")[0][0]
                .as<std::string>(),
            "0")
      << "the session setting leaked past an exception";
}

// --- the planner (pure: no server involved) -------------------------------

namespace {

// A fixed Observations literal. This is what makes plan determinism testable:
// no catalog, no clock, no connection.
pglaswell::Observations observations(long long size_bytes, long long rows,
                                     int waiters = 0,
                                     const char* kind = "table") {
  pglaswell::Observations obs;
  obs.server_version = 180006;
  obs.server = json::parse(R"({
    "max_connections": 100, "reserved_connections": 3,
    "current_backends": 9, "active_backends": 1,
    "ungranted_locks": 0, "is_in_recovery": false
  })");
  obs.tables["shop.orders"] = json{
      {"exists", true},
      {"kind", kind},
      {"reltuples", rows},
      {"size_estimate", size_bytes},
      {"estimated_from", "2026-09-05T09:00:00Z"},
      {"lock_waiters", waiters},
      {"columns",
       json{{"id", {{"type", "bigint"}, {"not_null", true}}},
            {"warehouse_id", {{"type", "bigint"}, {"not_null", false}}},
            {"created_at", {{"type", "timestamp with time zone"}, {"not_null", true}}},
            {"fulfilment_region", {{"type", "text"}, {"not_null", false}}}}},
      {"indexes",
       json{{"orders_pkey",
             {{"is_valid", true}, {"is_unique", true}, {"is_primary", true},
              {"leading_column", "id"},
              {"definition", "CREATE UNIQUE INDEX orders_pkey ON shop.orders USING btree (id)"}}}}}};
  return obs;
}

pglaswell::Spec spec_with(const json& intents) {
  json doc = minimal_spec();
  doc["intents"] = intents;
  return pglaswell::parse_spec(doc);
}

const pglaswell::Step* find_step(const pglaswell::Plan& p, const std::string& kind) {
  for (const auto& s : p.steps) {
    if (s.kind == kind) return &s;
  }
  return nullptr;
}

std::string all_sql(const pglaswell::Step& s) {
  std::string out;
  for (const auto& q : s.sql) out += q + "\n";
  return out;
}

}  // namespace

TEST(Planner, SameObservationsProduceAByteIdenticalPlan) {
  // The determinism receipt. If this ever drifts, "the plan you were shown is
  // the plan that ran" stops being checkable.
  const auto spec = pglaswell::parse_spec(minimal_spec());
  const pglaswell::ExecutorConfig cfg;
  const auto obs = observations(2LL * 1024 * 1024 * 1024, 8100000);

  const auto a = pglaswell::plan_migration(spec, obs, cfg);
  const auto b = pglaswell::plan_migration(spec, obs, cfg);
  EXPECT_EQ(a.to_json().dump(), b.to_json().dump());
  EXPECT_EQ(a.digest(), b.digest());
  EXPECT_FALSE(a.digest().empty());
}

TEST(Planner, ADifferentMeasurementProducesADifferentPlanDigest) {
  const auto spec = pglaswell::parse_spec(minimal_spec());
  const pglaswell::ExecutorConfig cfg;
  const auto small = pglaswell::plan_migration(spec, observations(1024, 10), cfg);
  const auto large =
      pglaswell::plan_migration(spec, observations(2LL << 30, 8100000), cfg);
  EXPECT_NE(small.digest(), large.digest());
}

TEST(Planner, ALargeTableChoosesAConcurrentIndexBuild) {
  const auto spec = pglaswell::parse_spec(minimal_spec());
  const auto plan = pglaswell::plan_migration(
      spec, observations(2LL * 1024 * 1024 * 1024, 8100000), {});
  const auto* s = find_step(plan, "create_index");
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(s->txn_class, pglaswell::TxnClass::kForbidden);
  EXPECT_NE(all_sql(*s).find("CREATE INDEX CONCURRENTLY"), std::string::npos);
  EXPECT_NE(s->why.find("64 MiB ceiling"), std::string::npos) << s->why;
}

TEST(Planner, ASmallQuietTableChoosesAPlainIndexBuild) {
  // CIC on a small table is machinery for nothing: two scans, a longer lock
  // than the plain build would have taken, and a failure mode the plain path
  // does not have.
  const auto spec = pglaswell::parse_spec(minimal_spec());
  const auto plan = pglaswell::plan_migration(spec, observations(1024 * 1024, 500), {});
  const auto* s = find_step(plan, "create_index");
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(s->txn_class, pglaswell::TxnClass::kOptional);
  EXPECT_NE(all_sql(*s).find("CREATE INDEX orders_open_by_region_idx"),
            std::string::npos);
  EXPECT_EQ(all_sql(*s).find("CONCURRENTLY"), std::string::npos);
  EXPECT_NE(s->lock.find("ShareLock"), std::string::npos);
}

TEST(Planner, ALockWaiterOnASmallTableForcesTheConcurrentPath) {
  // The check that makes the size ceiling safe: a small table with something
  // already queued on it is not a table to take ShareLock on.
  const auto spec = pglaswell::parse_spec(minimal_spec());
  const auto plan =
      pglaswell::plan_migration(spec, observations(1024 * 1024, 500, /*waiters=*/1), {});
  const auto* s = find_step(plan, "create_index");
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(s->txn_class, pglaswell::TxnClass::kForbidden);
  EXPECT_NE(s->why.find("lock waiters"), std::string::npos) << s->why;
}

TEST(Planner, EveryConcurrentBuildGainsAValidityCheck) {
  // A CIC can return without error and leave an invalid index. Reporting it
  // as succeeded because the statement returned is the failure that looks
  // like a success (spike S5).
  const auto spec = pglaswell::parse_spec(minimal_spec());
  const auto plan = pglaswell::plan_migration(spec, observations(2LL << 30, 8100000), {});
  ASSERT_NE(find_step(plan, "verify_index_valid"), nullptr)
      << "a concurrent build with no validity check";
  EXPECT_EQ(find_step(plan, "create_index")->detail.value("must_verify_valid", false),
            true);
}

TEST(Planner, APlainBuildNeedsNoValidityCheck) {
  const auto spec = pglaswell::parse_spec(minimal_spec());
  const auto plan = pglaswell::plan_migration(spec, observations(1024, 10), {});
  EXPECT_EQ(find_step(plan, "verify_index_valid"), nullptr);
}

TEST(Planner, AnInvalidIndexIsDroppedConcurrentlyBeforeRebuilding) {
  // Never a plain DROP INDEX: that takes AccessExclusiveLock, which is exactly
  // what CIC was chosen to avoid, and would turn a retry into the outage the
  // original build prevented.
  auto obs = observations(2LL << 30, 8100000);
  obs.tables["shop.orders"]["indexes"]["orders_open_by_region_idx"] =
      json{{"is_valid", false}, {"is_unique", false},
           {"definition", "CREATE INDEX ..."}, {"leading_column", "fulfilment_region"}};
  const auto plan = pglaswell::plan_migration(pglaswell::parse_spec(minimal_spec()), obs, {});
  const auto* s = find_step(plan, "create_index");
  ASSERT_NE(s, nullptr);
  const auto sql = all_sql(*s);
  EXPECT_NE(sql.find("DROP INDEX CONCURRENTLY"), std::string::npos) << sql;
  EXPECT_EQ(sql.find("\nDROP INDEX orders"), std::string::npos) << "plain DROP: " << sql;
  EXPECT_EQ(s->detail.value("recovering_invalid_index", false), true);
}

TEST(Planner, AValidIndexOfTheSameNameIsSatisfiedNotRebuilt) {
  auto obs = observations(2LL << 30, 8100000);
  obs.tables["shop.orders"]["indexes"]["orders_open_by_region_idx"] =
      json{{"is_valid", true}, {"is_unique", false},
           {"definition", "CREATE INDEX ..."}, {"leading_column", "fulfilment_region"}};
  const auto plan = pglaswell::plan_migration(pglaswell::parse_spec(minimal_spec()), obs, {});
  const auto* s = find_step(plan, "create_index");
  ASSERT_NE(s, nullptr);
  // "satisfied" is a success recorded with its justification, not a skip.
  EXPECT_EQ(s->action, pglaswell::Action::kSatisfied);
  EXPECT_TRUE(plan.ok);
  EXPECT_NE(s->why.find("already present and valid"), std::string::npos);
}

TEST(Planner, APartitionedTableRefusesTheConcurrentBuildWithARecipe) {
  // Measured on 18.6: CREATE INDEX CONCURRENTLY is refused outright there.
  const auto plan = pglaswell::plan_migration(
      pglaswell::parse_spec(minimal_spec()),
      observations(2LL << 30, 8100000, 0, "partitioned_table"), {});
  EXPECT_FALSE(plan.ok);
  ASSERT_FALSE(plan.conflicts.empty());
  bool mentions_recipe = false;
  for (const auto& c : plan.conflicts) {
    if (c.find("ATTACH PARTITION") != std::string::npos) mentions_recipe = true;
  }
  EXPECT_TRUE(mentions_recipe) << "the refusal must name the alternative";
}

TEST(Planner, AnExistingCompatibleColumnIsSatisfied) {
  const auto plan = pglaswell::plan_migration(
      pglaswell::parse_spec(minimal_spec()), observations(1024, 10), {});
  const auto* s = find_step(plan, "add_column");
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(s->action, pglaswell::Action::kSatisfied);
}

TEST(Planner, AnIncompatibleColumnTypeIsAConflictAndNothingRuns) {
  auto obs = observations(1024, 10);
  obs.tables["shop.orders"]["columns"]["fulfilment_region"] =
      json{{"type", "integer"}, {"not_null", false}};
  const auto plan = pglaswell::plan_migration(pglaswell::parse_spec(minimal_spec()), obs, {});
  EXPECT_FALSE(plan.ok);
  ASSERT_FALSE(plan.conflicts.empty());
  EXPECT_NE(plan.conflicts[0].find("exists as integer"), std::string::npos)
      << plan.conflicts[0];
}

TEST(Planner, AddColumnIsPlannedWhenTheColumnIsAbsent) {
  auto obs = observations(1024, 10);
  obs.tables["shop.orders"]["columns"].erase("fulfilment_region");
  const auto plan = pglaswell::plan_migration(pglaswell::parse_spec(minimal_spec()), obs, {});
  const auto* s = find_step(plan, "add_column");
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(s->action, pglaswell::Action::kApply);
  EXPECT_NE(all_sql(*s).find("ALTER TABLE shop.orders ADD COLUMN fulfilment_region text"),
            std::string::npos);
  // The COMMENT rides in the same transaction group as the ALTER.
  EXPECT_NE(all_sql(*s).find("COMMENT ON COLUMN"), std::string::npos);
  EXPECT_EQ(s->txn_class, pglaswell::TxnClass::kRequired);
}

TEST(Planner, NotNullWithNoDefaultIsRefusedWithTheThreeStepAlternative) {
  auto obs = observations(1024, 10);
  obs.tables["shop.orders"]["columns"].erase("fulfilment_region");
  const auto spec = spec_with(json::array({json::parse(R"({
      "kind":"add_column","schema":"shop","table":"orders","column":"fulfilment_region",
      "type":"text","nullable":false,"comment":"c"})")}));
  const auto plan = pglaswell::plan_migration(spec, obs, {});
  EXPECT_FALSE(plan.ok);
  EXPECT_NE(plan.conflicts[0].find("backfill it, then set NOT NULL"), std::string::npos)
      << plan.conflicts[0];
}

TEST(Planner, BackfillWithoutAUniqueKeyIsRefusedNamingTheIndexNeeded) {
  auto obs = observations(1024, 10);
  obs.tables["shop.orders"]["indexes"] = json::object();  // no unique index on id
  const auto plan = pglaswell::plan_migration(pglaswell::parse_spec(minimal_spec()), obs, {});
  EXPECT_FALSE(plan.ok);
  bool names_the_fix = false;
  for (const auto& c : plan.conflicts) {
    if (c.find("CREATE UNIQUE INDEX CONCURRENTLY") != std::string::npos) {
      names_the_fix = true;
    }
  }
  EXPECT_TRUE(names_the_fix) << "the refusal must name the index to create";
}

TEST(Planner, TheBackfillBatchUsesForUpdateWithoutSkipLocked) {
  // SKIP LOCKED would silently skip contended rows while the cursor advanced
  // past them, leaving a backfill that reports complete and is not.
  const auto plan = pglaswell::plan_migration(
      pglaswell::parse_spec(minimal_spec()), observations(1LL << 30, 8100000), {});
  const auto* s = find_step(plan, "backfill");
  ASSERT_NE(s, nullptr);
  const auto sql = all_sql(*s);
  EXPECT_NE(sql.find("FOR UPDATE"), std::string::npos) << sql;
  EXPECT_EQ(sql.find("SKIP LOCKED"), std::string::npos)
      << "SKIP LOCKED would make the cursor lie: " << sql;
  EXPECT_NE(sql.find("ORDER BY t.id"), std::string::npos) << sql;
  EXPECT_NE(sql.find("t.id > $1"), std::string::npos) << sql;
  EXPECT_EQ(s->txn_class, pglaswell::TxnClass::kOwnTxnPerBatch);
}

TEST(Planner, BackfillOnAHugeTableWithoutAPartialIndexWarnsButProceeds) {
  const auto plan = pglaswell::plan_migration(
      pglaswell::parse_spec(minimal_spec()), observations(1LL << 30, 8100000), {});
  EXPECT_TRUE(plan.ok);
  bool warned = false;
  for (const auto& w : plan.warnings) {
    if (w.find("no partial index") != std::string::npos) warned = true;
  }
  EXPECT_TRUE(warned) << "each batch may rescan; that should be said";
}

TEST(Planner, TransactionGroupsBreakAtEveryNonAtomicStep) {
  // The rendered boundary is exactly where atomicity ends, which is what a
  // reader needs to see before approving a plan.
  auto obs = observations(2LL << 30, 8100000);
  obs.tables["shop.orders"]["columns"].erase("fulfilment_region");
  const auto plan = pglaswell::plan_migration(pglaswell::parse_spec(minimal_spec()), obs, {});
  ASSERT_GE(plan.steps.size(), 3u);
  const auto* add = find_step(plan, "add_column");
  const auto* back = find_step(plan, "backfill");
  const auto* idx = find_step(plan, "create_index");
  ASSERT_NE(add, nullptr);
  ASSERT_NE(back, nullptr);
  ASSERT_NE(idx, nullptr);
  EXPECT_LT(add->txn_group, back->txn_group) << "a paced backfill must start a new group";
  EXPECT_LT(back->txn_group, idx->txn_group) << "CIC must start a new group";
}

TEST(Planner, TheRenderedPlanNamesTheAtomicityBoundaries) {
  auto obs = observations(2LL << 30, 8100000);
  obs.tables["shop.orders"]["columns"].erase("fulfilment_region");
  const auto text =
      pglaswell::plan_migration(pglaswell::parse_spec(minimal_spec()), obs, {}).render();
  EXPECT_NE(text.find("NOT atomic: paced, many commits"), std::string::npos) << text;
  EXPECT_NE(text.find("NOT atomic: cannot run inside a transaction block"),
            std::string::npos)
      << text;
  EXPECT_NE(text.find("why:"), std::string::npos) << text;
}

TEST(Planner, ARefusedPlanRendersAsRefusedAndRunsNothing) {
  auto obs = observations(1024, 10);
  obs.tables["shop.orders"]["columns"]["fulfilment_region"] = json{{"type", "integer"}};
  const auto plan = pglaswell::plan_migration(pglaswell::parse_spec(minimal_spec()), obs, {});
  EXPECT_FALSE(plan.ok);
  EXPECT_NE(plan.render().find("REFUSED. Nothing will run."), std::string::npos);
}

TEST(Planner, RefusesToPlanAgainstAStandby) {
  auto obs = observations(1024, 10);
  obs.server["is_in_recovery"] = true;
  const auto plan = pglaswell::plan_migration(pglaswell::parse_spec(minimal_spec()), obs, {});
  EXPECT_FALSE(plan.ok);
  EXPECT_NE(plan.conflicts[0].find("standby"), std::string::npos);
}

TEST(Planner, RefusesWhenTheServerIsOlderThanTheSpecRequires) {
  auto obs = observations(1024, 10);
  obs.server_version = 140000;
  const auto plan = pglaswell::plan_migration(pglaswell::parse_spec(minimal_spec()), obs, {});
  EXPECT_FALSE(plan.ok);
  EXPECT_NE(plan.conflicts[0].find("150000"), std::string::npos);
}

TEST(Planner, TheBudgetSaysWhoseCeilingItIsMeasuring) {
  // max_connections is the SERVER's ceiling. The application usually collapses
  // at its own pool limit first, and that is invisible from here -- so an
  // unconfigured budget must say so rather than imply a precision it lacks.
  const auto spec = pglaswell::parse_spec(minimal_spec());
  const auto unconfigured = pglaswell::plan_migration(spec, observations(1024, 10), {});
  EXPECT_EQ(unconfigured.budget["headroomSource"], "max_connections");
  EXPECT_TRUE(unconfigured.budget.contains("caveat"));
  EXPECT_EQ(unconfigured.budget["effectiveHeadroom"], 88);  // 100 - 3 - 9

  pglaswell::ExecutorConfig cfg;
  cfg.app_pool_size = 20;
  const auto configured = pglaswell::plan_migration(spec, observations(1024, 10), cfg);
  EXPECT_EQ(configured.budget["headroomSource"], "app_pool_size");
  EXPECT_EQ(configured.budget["effectiveHeadroom"], 20);
  EXPECT_FALSE(configured.budget.contains("caveat"));
}

TEST(Planner, ExecutorTuningReachesTheBackfillStep) {
  pglaswell::ExecutorConfig cfg;
  cfg.batch_rows = 50;
  cfg.commit_interval_ms = 100;
  cfg.observer_tick_ms = 25;
  const auto plan = pglaswell::plan_migration(
      pglaswell::parse_spec(minimal_spec()), observations(1LL << 30, 8100000), cfg);
  const auto* s = find_step(plan, "backfill");
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(s->detail["batch_rows"], 50);
  EXPECT_EQ(s->detail["commit_interval_ms"], 100);
  EXPECT_NE(s->why.find("50 rows per batch"), std::string::npos) << s->why;
}

TEST(Planner, AnIntentIsPlannedAgainstTheStateItsPredecessorsWillLeave) {
  // The commonest spec there is: add a column, then backfill it. The backfill
  // must be planned against the catalog as step 0 will leave it, not as it is
  // now -- otherwise the plan refuses its own first step's work.
  auto obs = observations(1LL << 30, 8100000);
  obs.tables["shop.orders"]["columns"].erase("fulfilment_region");

  const auto plan = pglaswell::plan_migration(pglaswell::parse_spec(minimal_spec()), obs, {});
  EXPECT_TRUE(plan.ok) << (plan.conflicts.empty() ? "" : plan.conflicts[0]);

  const auto* add = find_step(plan, "add_column");
  const auto* back = find_step(plan, "backfill");
  ASSERT_NE(add, nullptr);
  ASSERT_NE(back, nullptr);
  EXPECT_EQ(add->action, pglaswell::Action::kApply);
  EXPECT_EQ(back->action, pglaswell::Action::kApply);
}

TEST(Planner, ProjectionDoesNotHideAGenuinelyMissingColumn) {
  // The projection must not become a way for a backfill to reference anything
  // it likes: only what an earlier intent actually adds.
  auto obs = observations(1LL << 30, 8100000);
  obs.tables["shop.orders"]["columns"].erase("fulfilment_region");
  auto doc = minimal_spec();
  doc["intents"][1]["set"] = json{{"nonexistent_column", "1"}};
  const auto plan = pglaswell::plan_migration(pglaswell::parse_spec(doc), obs, {});
  EXPECT_FALSE(plan.ok);
  EXPECT_NE(plan.conflicts[0].find("nonexistent_column"), std::string::npos)
      << plan.conflicts[0];
}

TEST(Planner, ProjectionOnlyAppliesToStepsThatWillActuallyRun) {
  // A conflicted or satisfied step must not project. If add_column conflicts,
  // the backfill that depends on it must conflict too rather than being
  // planned against a column that will never exist.
  auto obs = observations(1LL << 30, 8100000);
  obs.tables["shop.orders"]["columns"]["fulfilment_region"] =
      json{{"type", "integer"}, {"not_null", false}};  // incompatible -> conflict
  const auto plan = pglaswell::plan_migration(pglaswell::parse_spec(minimal_spec()), obs, {});
  EXPECT_FALSE(plan.ok);
}

TEST(Planner, AnIndexCreatedByAnEarlierIntentCanSupportALaterBackfillKey) {
  // The projection covers indexes too: a unique index built in step 0 makes
  // the keyset walk in step 1 admissible.
  auto obs = observations(1LL << 30, 8100000);
  obs.tables["shop.orders"]["indexes"] = json::object();  // no unique index at all

  const auto spec = spec_with(json::array({
      json::parse(R"({"kind":"create_index","schema":"shop","table":"orders",
                      "name":"orders_id_uq","columns":["id"],"unique":true,
                      "comment":"keyset support"})"),
      json::parse(R"({"kind":"backfill","schema":"shop","table":"orders","key":"id",
                      "set":{"fulfilment_region":"'x'"},"where":"fulfilment_region IS NULL"})")}));

  const auto plan = pglaswell::plan_migration(spec, obs, {});
  EXPECT_TRUE(plan.ok) << (plan.conflicts.empty() ? "" : plan.conflicts[0]);
  const auto* back = find_step(plan, "backfill");
  ASSERT_NE(back, nullptr);
  EXPECT_EQ(back->detail.value("supporting_index", ""), "orders_id_uq");
}

TEST(Planner, TheRenderedPlanIsStableAcrossRuns) {
  // render() feeds a human's approval decision; it must not reorder or
  // reword itself between two calls on the same inputs.
  auto obs = observations(2LL << 30, 8100000);
  obs.tables["shop.orders"]["columns"].erase("fulfilment_region");
  const auto spec = pglaswell::parse_spec(minimal_spec());
  EXPECT_EQ(pglaswell::plan_migration(spec, obs, {}).render(),
            pglaswell::plan_migration(spec, obs, {}).render());
}

// --- catalog: observation against a live server ---------------------------

TEST_F(DatabaseTest, ObservesStructureSizeAndLockStateOfARealTable) {
  pglaswell::ConnConfig cfg;
  cfg.name = "t";
  cfg.conninfo = url_;
  {
    pglaswell::WriteSession w(cfg);
    w.begin("pg_laswell/test/catalog-fixture");
    w.txn().exec("DROP TABLE IF EXISTS laswell_obs CASCADE");
    w.txn().exec(
        "CREATE TABLE laswell_obs("
        "  id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,"
        "  region text,"
        "  created_at timestamptz NOT NULL DEFAULT now())");
    w.txn().exec("INSERT INTO laswell_obs(region) "
                 "SELECT 'r'||(g%5) FROM generate_series(1,500) g");
    w.commit();
  }

  pglaswell::Catalog cat(cfg);
  const auto obs = cat.observe({"public"}, {"laswell_obs"});
  ASSERT_TRUE(obs.has_table("public.laswell_obs")) << obs.tables.dump();
  const auto& t = obs.table("public.laswell_obs");
  EXPECT_EQ(t["kind"], "table");
  EXPECT_TRUE(t["columns"].contains("region"));
  EXPECT_EQ(t["columns"]["created_at"]["not_null"], true);
  EXPECT_EQ(t["columns"]["region"]["type"], "text");
  EXPECT_EQ(t["columns"]["id"]["type"], "bigint");
  // The primary key is the unique index a keyset backfill needs.
  EXPECT_EQ(t["indexes"]["laswell_obs_pkey"]["is_unique"], true);
  EXPECT_EQ(t["indexes"]["laswell_obs_pkey"]["leading_column"], "id");
  EXPECT_EQ(t["lock_waiters"], 0);
  EXPECT_GT(obs.server["max_connections"].get<int>(), 0);
  EXPECT_EQ(obs.server["is_in_recovery"], false);
  EXPECT_GT(obs.server_version, 140000);

  // A table that does not exist is an absence, not an error.
  const auto missing = cat.observe({"public"}, {"laswell_no_such_table"});
  EXPECT_FALSE(missing.has_table("public.laswell_no_such_table"));

  pglaswell::WriteSession w(cfg);
  w.begin("pg_laswell/test/cleanup");
  w.txn().exec("DROP TABLE laswell_obs");
  w.commit();
}

TEST_F(DatabaseTest, SizeEscalationIsSeparateFromObservationBecauseItTakesALock) {
  pglaswell::ConnConfig cfg;
  cfg.name = "t";
  cfg.conninfo = url_;
  {
    pglaswell::WriteSession w(cfg);
    w.begin("pg_laswell/test/size-fixture");
    w.txn().exec("DROP TABLE IF EXISTS laswell_size");
    w.txn().exec("CREATE TABLE laswell_size(id int primary key, pad text)");
    w.txn().exec("INSERT INTO laswell_size SELECT g, repeat('x',200) "
                 "FROM generate_series(1,2000) g");
    w.txn().exec("ANALYZE laswell_size");
    w.commit();
  }

  pglaswell::Catalog cat(cfg);
  auto obs = cat.observe({"public"}, {"laswell_size"});
  // observe() takes no AccessShareLock on the heap for sizing: it reads
  // relpages only.
  EXPECT_TRUE(obs.table("public.laswell_size").contains("size_estimate"));
  EXPECT_FALSE(obs.table("public.laswell_size").contains("size_measured"));

  cat.escalate_size(obs, "public", "laswell_size");
  EXPECT_TRUE(obs.table("public.laswell_size").contains("size_measured"));
  EXPECT_GT(obs.table("public.laswell_size")["size_measured"].get<long long>(), 0);

  pglaswell::WriteSession w(cfg);
  w.begin("pg_laswell/test/cleanup");
  w.txn().exec("DROP TABLE laswell_size");
  w.commit();
}

TEST_F(DatabaseTest, ObservationSeesALockWaiterAppear) {
  // The reading the index rule depends on: a table with something already
  // queued on it must not be built plainly, whatever its size.
  //
  // The holder takes RowExclusiveLock (an ordinary write) rather than
  // AccessExclusiveLock, deliberately: AccessExclusive would also block the
  // observation itself, which is a different behaviour covered by the next
  // test. RowExclusive is compatible with the AccessShareLock that
  // pg_get_indexdef() needs, so observation stays free to report.
  pglaswell::ConnConfig cfg;
  cfg.name = "t";
  cfg.conninfo = url_;
  {
    pglaswell::WriteSession w(cfg);
    w.begin("pg_laswell/test/waiter-fixture");
    w.txn().exec("DROP TABLE IF EXISTS laswell_waiters");
    w.txn().exec("CREATE TABLE laswell_waiters(id int primary key)");
    w.txn().exec("INSERT INTO laswell_waiters SELECT generate_series(1,10)");
    w.commit();
  }

  pglaswell::Catalog cat(cfg);
  EXPECT_EQ(cat.observe({"public"}, {"laswell_waiters"})
                .table("public.laswell_waiters")["lock_waiters"],
            0);

  // Hold RowExclusiveLock on a plain connection, so no idle-in-transaction
  // timeout can release it underneath the test.
  pqxx::connection holder(url_);
  pqxx::work holder_txn(holder);
  holder_txn.exec("UPDATE laswell_waiters SET id = id WHERE id = 1");

  std::atomic<bool> victim_blocked{false};
  std::thread victim([&] {
    try {
      pqxx::connection c(url_);
      pqxx::work tx(c);
      victim_blocked = true;
      tx.exec("LOCK TABLE laswell_waiters IN SHARE MODE");  // conflicts, waits
      tx.commit();
    } catch (...) {
    }
  });

  int seen = 0;
  for (int i = 0; i < 120 && seen == 0; ++i) {
    seen = cat.observe({"public"}, {"laswell_waiters"})
               .table("public.laswell_waiters")
               .value("lock_waiters", 0);
    if (seen == 0) std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  holder_txn.abort();
  victim.join();

  EXPECT_TRUE(victim_blocked.load());
  if (seen == 0) {
    GTEST_SKIP() << "the queued lock request was never observed; on a loaded "
                    "machine the waiter can come and go between polls";
  }
  EXPECT_GT(seen, 0);

  pglaswell::WriteSession w(cfg);
  w.begin("pg_laswell/test/cleanup");
  w.txn().exec("DROP TABLE laswell_waiters");
  w.commit();
}

TEST_F(DatabaseTest, ObservationUnderAStrongLockReportsRatherThanHangs) {
  // pg_get_indexdef() opens the index relation and so takes AccessShareLock.
  // Measured 2026-09-05 on 18.6: it blocks outright behind AccessExclusiveLock,
  // while plain pg_class/pg_index/pg_attribute reads and format_type() do not.
  //
  // Without a bound, observing a table mid-ALTER would hang for the length of
  // the ALTER -- the planner's own measurement blocking on the thing it is
  // planning around. That is the hazard already documented for pg_table_size(),
  // and this is the same hazard arriving through a different door.
  pglaswell::ConnConfig cfg;
  cfg.name = "t";
  cfg.conninfo = url_;
  {
    pglaswell::WriteSession w(cfg);
    w.begin("pg_laswell/test/strong-lock-fixture");
    w.txn().exec("DROP TABLE IF EXISTS laswell_strong");
    w.txn().exec("CREATE TABLE laswell_strong(id int primary key, x text)");
    w.txn().exec("CREATE INDEX laswell_strong_x ON laswell_strong(x)");
    w.commit();
  }

  pqxx::connection holder(url_);
  pqxx::work holder_txn(holder);
  holder_txn.exec("LOCK TABLE laswell_strong IN ACCESS EXCLUSIVE MODE");

  const auto started = std::chrono::steady_clock::now();
  pglaswell::Catalog cat(cfg);
  const auto obs = cat.observe({"public"}, {"laswell_strong"});
  const auto elapsed = std::chrono::steady_clock::now() - started;

  const auto& t = obs.table("public.laswell_strong");
  EXPECT_TRUE(t.value("observation_blocked", false))
      << "observation should report the lock, not read through it: " << t.dump();
  EXPECT_NE(t.value("blocked_reason", "").find("currentLocks"), std::string::npos)
      << "the reason should point at the tool that names the blocker";
  EXPECT_LT(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count(), 10)
      << "observation hung instead of timing out";

  // And the planner refuses outright rather than planning on a blind reading.
  const auto plan = pglaswell::plan_migration(pglaswell::parse_spec(minimal_spec()),
                                              obs, {});
  auto doc = minimal_spec();
  for (auto& i : doc["intents"]) i["table"] = "laswell_strong";
  const auto plan2 = pglaswell::plan_migration(
      pglaswell::parse_spec(doc),
      [&] {
        auto o = obs;
        o.tables["shop.laswell_strong"] = t;
        return o;
      }(),
      {});
  EXPECT_FALSE(plan2.ok);

  holder_txn.abort();
  pglaswell::WriteSession w(cfg);
  w.begin("pg_laswell/test/cleanup");
  w.txn().exec("DROP TABLE laswell_strong");
  w.commit();
  (void)plan;
}

TEST_F(DatabaseTest, PlanningEndToEndAgainstARealCatalog) {
  // Observation and planning joined up: the plan for a real, small, quiet
  // table must choose the plain build, and must not run anything.
  pglaswell::ConnConfig cfg;
  cfg.name = "t";
  cfg.conninfo = url_;
  {
    pglaswell::WriteSession w(cfg);
    w.begin("pg_laswell/test/e2e-fixture");
    w.txn().exec("DROP TABLE IF EXISTS orders CASCADE");
    w.txn().exec("DROP SCHEMA IF EXISTS shop CASCADE");
    w.txn().exec("CREATE SCHEMA shop");
    w.txn().exec(
        "CREATE TABLE shop.orders("
        "  id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,"
        "  warehouse_id bigint,"
        "  status text NOT NULL DEFAULT 'open',"
        "  created_at timestamptz NOT NULL DEFAULT now())");
    w.txn().exec("CREATE TABLE shop.warehouse(id bigint PRIMARY KEY, region text)");
    w.txn().exec("INSERT INTO shop.orders(warehouse_id) "
                 "SELECT (g%5)+1 FROM generate_series(1,200) g");
    w.commit();
  }

  pglaswell::Catalog cat(cfg);
  const auto spec = pglaswell::parse_spec(minimal_spec());
  const auto obs = cat.observe({"shop", "shop"}, {"orders", "warehouse"});
  const auto plan = pglaswell::plan_migration(spec, obs, {});

  ASSERT_TRUE(plan.ok) << plan.render();
  // fulfilment_region does not exist yet -> planned; the backfill that
  // follows is planned against the projected column.
  EXPECT_EQ(find_step(plan, "add_column")->action, pglaswell::Action::kApply);
  EXPECT_EQ(find_step(plan, "backfill")->action, pglaswell::Action::kApply);
  // 200 rows, nothing waiting -> the plain build is correct here.
  EXPECT_EQ(find_step(plan, "create_index")->txn_class, pglaswell::TxnClass::kOptional);

  // Planning ran nothing.
  pglaswell::ReadSession r(cfg);
  EXPECT_EQ(r.txn()
                .exec("SELECT count(*) FROM information_schema.columns "
                      "WHERE table_schema='shop' AND table_name='orders' "
                      "AND column_name='fulfilment_region'")[0][0]
                .as<int>(),
            0)
      << "planMigration must never execute";

  pglaswell::WriteSession w(cfg);
  w.begin("pg_laswell/test/cleanup");
  w.txn().exec("DROP SCHEMA shop CASCADE");
  w.commit();
}
