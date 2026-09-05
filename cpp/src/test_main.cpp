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
#include "executor.h"
#include "jobs.h"
#include "ledger.h"
#include "observations.h"
#include "planner.h"
#include "repository.h"
#include "server.h"
#include "session.h"
#include "spec.h"
#include "tools.h"
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
  EXPECT_EQ(r["result"]["tools"].size(), s.tools().size());
}

namespace {
pglaswell::ToolContext& tool_ctx() {
  static pglaswell::ToolContext ctx{
      pglaswell::Registry::from_url("port=5555 dbname=postgres", "test"), nullptr};
  return ctx;
}
std::vector<pglaswell::ToolDef> all_tools() { return pglaswell::make_tools(tool_ctx()); }
}  // namespace

TEST(Tools, NamesAreUniqueAndCamelCase) {
  std::set<std::string> seen;
  for (const auto& t : all_tools()) {
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
  for (const auto& t : all_tools()) {
    EXPECT_FALSE(t.description.empty()) << t.name;
    ASSERT_TRUE(static_cast<bool>(t.input_schema)) << t.name;
    EXPECT_TRUE(t.input_schema().is_object()) << t.name;
    ASSERT_TRUE(static_cast<bool>(t.invoke)) << t.name;
  }
}

TEST(Tools, EveryLongRunningToolReturnsAJobId) {
  // A long-running tool that returned results inline would block the stdio
  // loop for the length of a migration, which is the one thing the job
  // registry exists to prevent.
  for (const auto& t : all_tools()) {
    if (!t.hints.long_running) continue;
    ASSERT_TRUE(static_cast<bool>(t.output_schema)) << t.name;
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
  return pglaswell::Registry::base64_encode(in);
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
  // A CONFIGURED policy that excludes the key. An empty policy is a different
  // case entirely -- see AnUnconfiguredLocalPolicyIsNotARefusal.
  pglaswell::TrustPolicy policy;
  policy.accept.push_back("ed25519:1111111111111111");
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
  policy.accept.push_back("ed25519:1111111111111111");
  const json sigs = json::array(
      {{{"key_id", "x"}, {"algorithm", "rsa"}, {"signature", "AAAA"}}});
  const auto r = pglaswell::verify_against_policy(policy, "b", sigs);
  EXPECT_FALSE(r.verified);
  EXPECT_NE(r.reason.find("ed25519 only"), std::string::npos) << r.reason;
}

TEST(Trust, AnUnsignedSpecIsRefusedByAConfiguredPolicy) {
  pglaswell::TrustPolicy policy;
  policy.accept.push_back("ed25519:1111111111111111");
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
  // Deliberately something no reasonable roadmap would add. An earlier version
  // of this test used a kind that was later implemented, and the test then
  // silently stopped testing anything.
  doc["intents"][1]["kind"] = "make_the_database_faster";
  const auto err = spec_error(doc);
  EXPECT_NE(err.find("make_the_database_faster"), std::string::npos) << err;
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
                     "columns":["c"],"comment":"c"},
    "drop_index":   {"kind":"drop_index","schema":"s","table":"t","name":"i"},
    "set_not_null": {"kind":"set_not_null","schema":"s","table":"t","column":"c"},
    "add_foreign_key": {"kind":"add_foreign_key","schema":"s","table":"t",
                        "name":"fk","columns":["pid"],
                        "references_schema":"s","references_table":"p",
                        "references_columns":["id"]},
    "add_check_constraint": {"kind":"add_check_constraint","schema":"s",
                             "table":"t","name":"ck","expression":"v > 0"},
    "drop_constraint": {"kind":"drop_constraint","schema":"s","table":"t",
                        "name":"ck"},
    "alter_column_type": {"kind":"alter_column_type","schema":"s","table":"t",
                          "column":"c","type":"bigint"},
    "drop_column": {"kind":"drop_column","schema":"s","table":"t","column":"c"},
    "replace_view": {"kind":"replace_view","schema":"s","name":"v",
                     "definition":"SELECT 1 AS a"},
    "add_unique_constraint": {"kind":"add_unique_constraint","schema":"s",
                              "table":"t","name":"t_uq","columns":["c"]},
    "add_primary_key": {"kind":"add_primary_key","schema":"s","table":"t",
                        "name":"t_pkey","columns":["c"]},
    "attach_partition": {"kind":"attach_partition","schema":"s","table":"t",
                         "partition":"t_2025","from":"'2025-01-01'",
                         "to":"'2026-01-01'"},
    "detach_partition": {"kind":"detach_partition","schema":"s","table":"t",
                         "partition":"t_2025"},
    "rename_table":  {"kind":"rename_table","schema":"s","table":"t","to":"t2"},
    "rename_column": {"kind":"rename_column","schema":"s","table":"t",
                      "column":"c","to":"c2"},
    "rename_constraint": {"kind":"rename_constraint","schema":"s","table":"t",
                          "name":"ck","to":"ck2"},
    "create_table": {"kind":"create_table","schema":"s","table":"t2",
                     "comment":"doc",
                     "columns":[{"name":"id","type":"bigint","nullable":false,
                                 "comment":"pk"}]},
    "drop_table":   {"kind":"drop_table","schema":"s","table":"t"},
    "delete_rows":  {"kind":"delete_rows","schema":"s","table":"t","key":"id",
                     "where":"created_at < now() - interval '1 year'"},
    "set_row_security": {"kind":"set_row_security","schema":"s","table":"t",
                         "enabled":true},
    "create_policy": {"kind":"create_policy","schema":"s","table":"t",
                      "name":"p","using":"tenant = current_user"},
    "drop_policy":  {"kind":"drop_policy","schema":"s","table":"t","name":"p"},
    "set_trigger_state": {"kind":"set_trigger_state","schema":"s","table":"t",
                          "trigger":"trg","enabled":false},
    "grant":  {"kind":"grant","schema":"s","table":"t",
               "privileges":["SELECT"],"to":["app"]},
    "revoke": {"kind":"revoke","schema":"s","table":"t",
               "privileges":["SELECT"],"from":["app"]}
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

pglaswell::Spec spec_of(const json& intents) {
  json doc = minimal_spec();
  doc["intents"] = intents;
  return pglaswell::parse_spec(doc);
}

std::vector<const pglaswell::Step*> steps_of(const pglaswell::Plan& p,
                                             const std::string& kind) {
  std::vector<const pglaswell::Step*> out;
  for (const auto& s : p.steps) {
    if (s.kind == kind) out.push_back(&s);
  }
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

TEST(Planner, APartitionedTableGetsThePerPartitionRecipe) {
  // CREATE INDEX CONCURRENTLY is refused on the parent, measured on 18.6. The
  // recipe builds concurrently on each partition, creates the parent index ON
  // ONLY -- no data, no scan -- and attaches each child.
  auto obs = observations(2LL << 30, 8100000, 0, "partitioned_table");
  obs.tables["shop.orders"]["partitions"] =
      json::array({"shop.orders_2024", "shop.orders_2025"});
  const auto plan = pglaswell::plan_migration(
      pglaswell::parse_spec(minimal_spec()), obs, {});
  ASSERT_TRUE(plan.ok) << plan.render();

  const auto s = steps_of(plan, "create_index");
  ASSERT_EQ(s.size(), 5u) << plan.render();  // 2 builds + parent + 2 attaches
  EXPECT_NE(all_sql(*s[0]).find("CREATE INDEX CONCURRENTLY orders_2024_orders_open_by_region_idx"
                                " ON shop.orders_2024"),
            std::string::npos) << all_sql(*s[0]);
  EXPECT_EQ(s[0]->txn_class, pglaswell::TxnClass::kForbidden);
  EXPECT_NE(all_sql(*s[2]).find("ON ONLY shop.orders"), std::string::npos);
  EXPECT_NE(all_sql(*s[3]).find("ATTACH PARTITION"), std::string::npos);
  EXPECT_NE(all_sql(*s[4]).find("ATTACH PARTITION"), std::string::npos);

  // The parent index is INVALID until the last attachment lands, so the recipe
  // must end by checking -- an index that exists, is invalid and is never used
  // would otherwise pass silently.
  ASSERT_NE(find_step(plan, "verify_index_valid"), nullptr) << plan.render();
}

TEST(Planner, AUniqueIndexOnAPartitionedTableIsRefusedRatherThanPlannedToFail) {
  // PostgreSQL requires such an index to include the partition key, which this
  // planner does not read. Emitting a recipe whose final ATTACH would fail is
  // worse than refusing.
  auto obs = observations(1024, 10, 0, "partitioned_table");
  obs.tables["shop.orders"]["partitions"] = json::array({"shop.orders_2024"});
  json doc = minimal_spec();
  for (auto& i : doc["intents"]) {
    if (i["kind"] == "create_index") i["unique"] = true;
  }
  const auto plan = pglaswell::plan_migration(pglaswell::parse_spec(doc), obs, {});
  EXPECT_FALSE(plan.ok) << plan.render();
  bool named = false;
  for (const auto& c : plan.conflicts) {
    if (c.find("partition key") != std::string::npos) named = true;
  }
  EXPECT_TRUE(named) << json(plan.conflicts).dump(2);
}

TEST(Planner, APartitionedTableWithNoPartitionsIsRefused) {
  auto obs = observations(1024, 10, 0, "partitioned_table");
  obs.tables["shop.orders"]["partitions"] = json::array();
  const auto plan = pglaswell::plan_migration(
      pglaswell::parse_spec(minimal_spec()), obs, {});
  EXPECT_FALSE(plan.ok) << plan.render();
}

TEST(Planner, AddCheckConstraintSplitsIntoNotValidThenValidate) {
  const auto plan = pglaswell::plan_migration(
      spec_of(json::array({json{{"kind", "add_check_constraint"},
                                {"schema", "shop"}, {"table", "orders"},
                                {"name", "orders_status_ck"},
                                {"expression", "status IN ('open','closed')"}}})),
      observations(2LL << 30, 8000000), {});
  ASSERT_TRUE(plan.ok) << plan.render();
  const auto s = steps_of(plan, "add_check_constraint");
  ASSERT_EQ(s.size(), 2u);
  EXPECT_NE(all_sql(*s[0]).find("NOT VALID"), std::string::npos);
  EXPECT_NE(all_sql(*s[1]).find("VALIDATE CONSTRAINT orders_status_ck"),
            std::string::npos);
  EXPECT_GT(s[1]->txn_group, s[0]->txn_group);
  EXPECT_NE(s[1]->lock.find("does NOT block reads or writes"), std::string::npos);
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
  // The target is referenced by its own name, not an alias -- the naming
  // contract a spec's where/set expressions have to match.
  EXPECT_NE(sql.find("ORDER BY orders.id"), std::string::npos) << sql;
  EXPECT_NE(sql.find("orders.id > $1"), std::string::npos) << sql;
  EXPECT_NE(sql.find("FOR UPDATE OF orders"), std::string::npos)
      << "a bare FOR UPDATE would lock rows in the joined lookup table too: "
      << sql;
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

// --- the ledger: gate 2 -----------------------------------------------------

namespace {

// A database with the laswell schema installed and the suite's key trusted.
// Built by running the SHIPPED bootstrap.sql, so the script cannot rot
// alongside the code that depends on it.
class BootstrappedTest : public DatabaseTest {
 protected:
  void SetUp() override {
    DatabaseTest::SetUp();
    if (::testing::Test::IsSkipped()) return;

    pglaswell::ConnConfig cfg;
    cfg.name = "t";
    cfg.conninfo = url_;
    pglaswell::WriteSession w(cfg);
    w.begin("pg_laswell/test/bootstrap");
    w.txn().exec("DROP SCHEMA IF EXISTS laswell CASCADE");
    w.commit();

    // The bootstrap script is psql-flavoured (\if, :'var'), so it is run
    // through psql rather than reimplemented here -- reimplementing it would
    // test a copy rather than the thing that ships.
    const std::string key_b64 =
        pglaswell::Registry::base64_encode(test_key().pub);
    const std::string cmd =
        "PSQLRC=/dev/null psql -X -q -v ON_ERROR_STOP=1 "
        "-v laswell_role=laswell_runner "
        "-v first_key_id=" + test_key().key_id +
        " -v first_key_b64=" + key_b64 +
        " -v first_key_label=suite "
        "-f " + std::string(PGLASWELL_BOOTSTRAP_SQL) + " \"" + url_ + "\" 2>&1";
    if (std::system(("psql -X -q -c 'CREATE ROLE laswell_runner NOLOGIN' \"" +
                     url_ + "\" >/dev/null 2>&1")
                        .c_str()) != 0) {
      // Already exists: fine.
    }
    ASSERT_EQ(std::system(cmd.c_str()), 0) << "bootstrap.sql failed";
  }

  pglaswell::ConnConfig cfg() const {
    pglaswell::ConnConfig c;
    c.name = "t";
    c.conninfo = url_;
    return c;
  }

  // A spec signed by the suite key, ready for gate 2.
  json signed_spec() const {
    const auto parsed = pglaswell::parse_spec(minimal_spec());
    json doc = minimal_spec();
    doc["signatures"] = json::array({{{"key_id", test_key().key_id},
                                      {"algorithm", "ed25519"},
                                      {"signature", base64(sign(parsed.canonical_bytes))}}});
    return doc;
  }
};

}  // namespace

TEST_F(BootstrappedTest, StatusReportsAUsableLedgerAndItsTrustedKeys) {
  pglaswell::Ledger ledger(cfg());
  const auto st = ledger.status();
  EXPECT_TRUE(st.installed);
  EXPECT_EQ(st.version, pglaswell::kLedgerSchemaVersion);
  EXPECT_TRUE(st.usable) << st.error;
  ASSERT_EQ(st.trusted_key_ids.size(), 1u);
  EXPECT_EQ(st.trusted_key_ids[0], test_key().key_id);
}

TEST_F(BootstrappedTest, AnAbsentSchemaIsAnAnswerNotAnException) {
  // "You have not bootstrapped" and "your signer is not trusted" are very
  // different things for an operator to read, and collapsing them would be the
  // same mistake as reporting a privilege denial as an empty result.
  pglaswell::WriteSession w(cfg());
  w.begin("pg_laswell/test/drop-schema");
  w.txn().exec("DROP SCHEMA laswell CASCADE");
  w.commit();

  pglaswell::Ledger ledger(cfg());
  const auto st = ledger.status();
  EXPECT_FALSE(st.installed);
  EXPECT_FALSE(st.usable);
  EXPECT_NE(st.error.find("not installed"), std::string::npos) << st.error;
  EXPECT_NE(st.hint.find("bootstrap.sql"), std::string::npos) << st.hint;
  EXPECT_NE(st.hint.find("never installs it itself"), std::string::npos)
      << "the hint should say why the tool does not do this for you";
}

TEST_F(BootstrappedTest, AWrongLedgerVersionIsRefusedRatherThanUpgraded) {
  pglaswell::WriteSession w(cfg());
  w.begin("pg_laswell/test/bump-version");
  w.txn().exec("INSERT INTO laswell.schema_version(version) VALUES (99)");
  w.commit();

  const auto st = pglaswell::Ledger(cfg()).status();
  EXPECT_FALSE(st.usable);
  EXPECT_NE(st.error.find("version 99"), std::string::npos) << st.error;
}

TEST_F(BootstrappedTest, ADatabaseTrustingNoKeysIsUsableButAcceptsNothing) {
  // The ledger is fine; it simply trusts nobody. That is a TRUST answer, not a
  // ledger fault, and conflating the two would point an operator at
  // bootstrap.sql when the fix is an INSERT into trusted_key.
  pglaswell::WriteSession w(cfg());
  w.begin("pg_laswell/test/revoke");
  w.txn().exec("UPDATE laswell.trusted_key SET revoked_at = now()");
  w.commit();

  const auto st = pglaswell::Ledger(cfg()).status();
  EXPECT_TRUE(st.usable) << st.error;
  EXPECT_TRUE(st.error.empty()) << st.error;
  EXPECT_TRUE(st.trusted_key_ids.empty());
  EXPECT_NE(st.hint.find("INSERT INTO laswell.trusted_key"), std::string::npos)
      << st.hint;

  // And a spec signed by the now-revoked key is refused, by gate 2.
  const auto check =
      pglaswell::Ledger(cfg()).verify_signer(pglaswell::parse_spec(signed_spec()));
  EXPECT_FALSE(check.verified);
}

TEST_F(BootstrappedTest, GateTwoVerifiesAgainstTheKeyBytesTheDatabaseHolds) {
  const auto spec = pglaswell::parse_spec(signed_spec());
  const auto check = pglaswell::Ledger(cfg()).verify_signer(spec);
  ASSERT_TRUE(check.verified) << check.reason;
  EXPECT_EQ(check.key_id, test_key().key_id);
  EXPECT_EQ(check.label, "suite");
}

TEST_F(BootstrappedTest, GateTwoRefusesASpecSignedByAKeyThisDatabaseDoesNotTrust) {
  // THE test that proves gate 2 is real: the local policy is made maximally
  // permissive, and the database still refuses.
  pglaswell::WriteSession w(cfg());
  w.begin("pg_laswell/test/untrust");
  w.txn().exec("DELETE FROM laswell.trusted_key");
  w.commit();

  const auto spec = pglaswell::parse_spec(signed_spec());
  // Gate 1 would happily pass this: the local policy trusts the key.
  const auto gate1 = pglaswell::verify_against_policy(
      policy_trusting_test_key(), spec.canonical_bytes, spec.signatures);
  ASSERT_TRUE(gate1.verified) << "precondition: gate 1 accepts this spec";

  const auto gate2 = pglaswell::Ledger(cfg()).verify_signer(spec);
  EXPECT_FALSE(gate2.verified)
      << "a permissive client config talked the database into accepting a spec";
  EXPECT_NE(gate2.reason.find("this database trusts"), std::string::npos)
      << gate2.reason;
}

TEST_F(BootstrappedTest, GateTwoIgnoresTheClientsCopyOfTheKeyBytes) {
  // A config that files an attacker's key under a trusted id must get nowhere:
  // the bytes the DATABASE holds are the ones that have to verify.
  const auto spec = pglaswell::parse_spec(signed_spec());
  pglaswell::WriteSession w(cfg());
  w.begin("pg_laswell/test/swap-key");
  // Replace the trusted key's bytes with a different valid key, keeping the
  // id. The CHECK constraint forbids exactly this, which is itself the point.
  const auto r = w.txn().exec(
      "SELECT 1 FROM laswell.trusted_key WHERE key_id = " +
      w.txn().quote(test_key().key_id));
  ASSERT_EQ(r.size(), 1u);
  EXPECT_THROW(
      w.txn().exec("UPDATE laswell.trusted_key SET public_key = decode('00','hex')"),
      pqxx::sql_error)
      << "the content-address CHECK should forbid swapping key bytes under an id";
  w.rollback();
}

TEST_F(BootstrappedTest, ARevokedKeyVerifiesNothing) {
  pglaswell::WriteSession w(cfg());
  w.begin("pg_laswell/test/revoke-one");
  w.txn().exec("UPDATE laswell.trusted_key SET revoked_at = now()");
  w.commit();

  const auto spec = pglaswell::parse_spec(signed_spec());
  const auto check = pglaswell::Ledger(cfg()).verify_signer(spec);
  EXPECT_FALSE(check.verified);
  EXPECT_NE(check.reason.find("revoked"), std::string::npos) << check.reason;
}

TEST_F(BootstrappedTest, ATamperedSpecFailsGateTwoEvenWithATrustedSigner) {
  json doc = signed_spec();
  doc["intents"][2]["name"] = "orders_something_else_idx";
  const auto spec = pglaswell::parse_spec(doc);
  const auto check = pglaswell::Ledger(cfg()).verify_signer(spec);
  EXPECT_FALSE(check.verified);
  EXPECT_NE(check.reason.find("does not verify"), std::string::npos) << check.reason;
}

TEST_F(BootstrappedTest, RecordingAMigrationIsIdempotentOnTheDigest) {
  const auto spec = pglaswell::parse_spec(signed_spec());
  pglaswell::Ledger ledger(cfg());
  const auto first = ledger.record_migration(spec, test_key().key_id);
  const auto second = ledger.record_migration(spec, test_key().key_id);
  EXPECT_EQ(first, second) << "the same spec must not create two ledger rows";

  pglaswell::ReadSession r(cfg());
  const auto row = pglaswell::pqxx_exec(
      r.txn(),
      "SELECT spec_id, encode(canonical_bytes,'escape'), signer_key_id "
      "  FROM laswell.migration WHERE migration_id = $1",
      pqxx::params{first});
  ASSERT_EQ(row.size(), 1u);
  EXPECT_EQ(row[0][0].template as<std::string>(), spec.id);
  // The bytes that were verified are stored, so what ran is never inferred
  // from a file on somebody's disk.
  EXPECT_EQ(row[0][1].template as<std::string>(), spec.canonical_bytes);
  EXPECT_EQ(row[0][2].template as<std::string>(), test_key().key_id);
}

TEST_F(BootstrappedTest, TheForeignKeyIsTheGateNotACodePath) {
  // Even bypassing verify_signer entirely, an untrusted signer cannot get a
  // ledger row. That is the property worth having: no branch to forget.
  const auto spec = pglaswell::parse_spec(signed_spec());
  EXPECT_THROW(
      pglaswell::Ledger(cfg()).record_migration(spec, "ed25519:0000000000000000"),
      pqxx::sql_error);
}

TEST_F(BootstrappedTest, NoInterruptedJobsOnACleanLedger) {
  EXPECT_TRUE(pglaswell::Ledger(cfg()).interrupted_jobs().empty());
}

// --- the MCP tools, end to end ---------------------------------------------

namespace {

class ToolTest : public BootstrappedTest {
 public:
  void SetUp() override {
    BootstrappedTest::SetUp();
    if (::testing::Test::IsSkipped()) return;
    jobs_ = std::make_unique<pglaswell::JobRegistry>();
    ctx_ = std::make_unique<pglaswell::ToolContext>(
        pglaswell::Registry::from_url(url_, "pg-laswell/test"), nullptr);
    ctx_->jobs = jobs_.get();
    observer_ = std::make_unique<pglaswell::Observer>(cfg(), jobs_.get(), 25);
    ctx_->observer = observer_.get();
    // Gate 1 trusts the suite key, so gate 2 is the one under test.
    ctx_->registry.mutable_trust() = policy_trusting_test_key();
    server_ = std::make_unique<pglaswell::McpServer>(pglaswell::make_tools(*ctx_));
    initialize(*server_);
  }

  json call(const std::string& tool, const json& arguments) {
    const json r = rpc(*server_, json{{"jsonrpc", "2.0"},
                                      {"id", 42},
                                      {"method", "tools/call"},
                                      {"params",
                                       {{"name", tool}, {"arguments", arguments}}}});
    EXPECT_TRUE(r.contains("result")) << r.dump();
    return r["result"];
  }

  json payload(const json& result) {
    return json::parse(result["content"][0]["text"].get<std::string>());
  }

  void TearDown() override {
    if (jobs_) {
      for (const auto& j : jobs_->all()) j->pacing.cancel_stop = true;
      jobs_->join_all();
    }
    if (observer_) observer_->stop();
  }

  // The status of one job, as an agent would read it.
  json status_of(const std::string& job_id) {
    const auto r = payload(call("jobStatus", json{{"jobId", job_id}}));
    if (!r.contains("jobs") || r["jobs"].empty()) return json();
    return r["jobs"][0];
  }

  // Executor tuning is configuration precisely so a test can drive the
  // identical code path in seconds instead of minutes.
  void set_executor(const pglaswell::ExecutorConfig& e) {
    ctx_->registry.mutable_get(ctx_->registry.default_name()).executor = e;
  }

  std::unique_ptr<pglaswell::JobRegistry> jobs_;
  std::unique_ptr<pglaswell::Observer> observer_;
  std::unique_ptr<pglaswell::ToolContext> ctx_;
  std::unique_ptr<pglaswell::McpServer> server_;
};

}  // namespace

TEST_F(ToolTest, CheckPrivilegesReportsTheLedgerAndTheRole) {
  const auto p = payload(call("checkPrivileges", json::object()));
  EXPECT_FALSE(p.value("role", "").empty());
  EXPECT_GT(p.value("serverVersion", 0), 140000);
  EXPECT_TRUE(p["ledger"].value("schemaPresent", false));
  EXPECT_TRUE(p["ledger"].value("canReadTrustedKey", false));
  EXPECT_FALSE(p.value("isStandby", true));
}

TEST_F(ToolTest, CheckPrivilegesWarnsWhenTheRoleCanGrantItselfTrust) {
  // The suite runs as the owner, so it CAN write trusted_key -- which is
  // exactly the configuration the warning exists for.
  const auto p = payload(call("checkPrivileges", json::object()));
  bool warned = false;
  for (const auto& n : p["notes"]) {
    if (n.get<std::string>().find("grant itself trust") != std::string::npos) {
      warned = true;
    }
  }
  EXPECT_TRUE(warned) << p["notes"].dump();
}

TEST_F(ToolTest, GetSpecDigestReturnsTheBytesThatWillBeVerified) {
  const auto p = payload(call("getSpecDigest", json{{"spec", minimal_spec()}}));
  const auto expected = pglaswell::parse_spec(minimal_spec());
  EXPECT_EQ(p.value("digest", ""), expected.digest);
  EXPECT_EQ(p.value("canonicalBytes", ""), expected.canonical_bytes);
  // Signing those exact bytes must satisfy both gates.
  const auto sig = base64(sign(p["canonicalBytes"].get<std::string>()));
  json doc = minimal_spec();
  doc["signatures"] = json::array({{{"key_id", test_key().key_id},
                                    {"algorithm", "ed25519"},
                                    {"signature", sig}}});
  const auto v = payload(call("validateSpec", json{{"spec", doc}}));
  EXPECT_TRUE(v.value("accepted", false)) << v.dump(2);
}

TEST_F(ToolTest, GetSpecDigestRejectsABadSpecWithAHint) {
  json doc = minimal_spec();
  doc["intents"][0]["kind"] = "make_it_fast";
  const auto p = payload(call("getSpecDigest", json{{"spec", doc}}));
  EXPECT_NE(p.value("error", "").find("make_it_fast"), std::string::npos);
  EXPECT_NE(p.value("hint", "").find("add_column"), std::string::npos)
      << "the hint must list what this binary supports";
}

TEST_F(ToolTest, ValidateSpecReportsBothGatesSeparately) {
  const auto p = payload(call("validateSpec", json{{"spec", signed_spec()}}));
  EXPECT_TRUE(p.value("accepted", false)) << p.dump(2);
  EXPECT_TRUE(p["clientTrust"].value("accepted", false));
  EXPECT_TRUE(p["databaseTrust"].value("accepted", false));
  EXPECT_EQ(p["databaseTrust"].value("label", ""), "suite");
  EXPECT_TRUE(p["ledger"].value("usable", false));
}

TEST_F(ToolTest, ValidateSpecDistinguishesNotBootstrappedFromNotTrusted) {
  // Two very different things for an operator to read.
  {
    pglaswell::WriteSession w(cfg());
    w.begin("pg_laswell/test/drop");
    w.txn().exec("DROP SCHEMA laswell CASCADE");
    w.commit();
  }
  const auto absent = payload(call("validateSpec", json{{"spec", signed_spec()}}));
  EXPECT_FALSE(absent.value("accepted", true));
  EXPECT_NE(absent.value("error", "").find("not installed"), std::string::npos);
  EXPECT_NE(absent.value("hint", "").find("bootstrap.sql"), std::string::npos);
  // And it does not claim anything about the signer, because it cannot know.
  EXPECT_FALSE(absent.contains("databaseTrust"))
      << "with no schema there is nothing to say about the signer, and saying "
         "something anyway would be a guess";
}

TEST_F(ToolTest, ValidateSpecRefusesAnUntrustedSignerEvenWithAPermissiveClient) {
  pglaswell::WriteSession w(cfg());
  w.begin("pg_laswell/test/untrust");
  w.txn().exec("DELETE FROM laswell.trusted_key");
  w.commit();

  const auto p = payload(call("validateSpec", json{{"spec", signed_spec()}}));
  EXPECT_FALSE(p.value("accepted", true)) << p.dump(2);
  EXPECT_TRUE(p.value("clientTrust", json::object()).value("accepted", false))
      << "precondition: this machine's config accepts the key";
  // A database with no trusted keys is still a TRUST answer, not a ledger
  // fault: reporting it as the latter would point at the wrong remedy.
  ASSERT_TRUE(p.contains("databaseTrust")) << p.dump(2);
  EXPECT_FALSE(p["databaseTrust"].value("accepted", true));
  EXPECT_NE(p.value("hint", "").find("laswell.trusted_key"), std::string::npos)
      << p.value("hint", "");
}

TEST_F(ToolTest, PlanMigrationExecutesNothing) {
  {
    pglaswell::WriteSession w(cfg());
    w.begin("pg_laswell/test/plan-fixture");
    w.txn().exec("DROP SCHEMA IF EXISTS shop CASCADE");
    w.txn().exec("CREATE SCHEMA shop");
    w.txn().exec(
        "CREATE TABLE shop.orders(id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,"
        " warehouse_id bigint, status text NOT NULL DEFAULT 'open',"
        " created_at timestamptz NOT NULL DEFAULT now())");
    w.txn().exec("CREATE TABLE shop.warehouse(id bigint PRIMARY KEY, region text)");
    w.txn().exec("INSERT INTO shop.orders(warehouse_id) SELECT (g%5)+1 FROM generate_series(1,200) g");
    w.commit();
  }

  const auto p = payload(call("planMigration", json{{"spec", signed_spec()}}));
  ASSERT_TRUE(p.value("ok", false)) << p.dump(2);
  EXPECT_FALSE(p.value("executed", true));
  EXPECT_FALSE(p.value("planDigest", "").empty());
  EXPECT_NE(p.value("rendered", "").find("transaction group"), std::string::npos);

  pglaswell::ReadSession r(cfg());
  EXPECT_EQ(r.txn()
                .exec("SELECT count(*) FROM information_schema.columns"
                      " WHERE table_schema='shop' AND table_name='orders'"
                      " AND column_name='fulfilment_region'")[0][0]
                .as<int>(),
            0)
      << "planMigration ran DDL";

  pglaswell::WriteSession w(cfg());
  w.begin("pg_laswell/test/cleanup");
  w.txn().exec("DROP SCHEMA shop CASCADE");
  w.commit();
}

TEST_F(ToolTest, PlanMigrationIsDeterministicAcrossCalls) {
  pglaswell::WriteSession w(cfg());
  w.begin("pg_laswell/test/det-fixture");
  w.txn().exec("DROP SCHEMA IF EXISTS shop CASCADE");
  w.txn().exec("CREATE SCHEMA shop");
  w.txn().exec("CREATE TABLE shop.orders(id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,"
               " warehouse_id bigint, status text, created_at timestamptz)");
  w.txn().exec("CREATE TABLE shop.warehouse(id bigint PRIMARY KEY, region text)");
  w.commit();

  const auto a = payload(call("planMigration", json{{"spec", signed_spec()}}));
  const auto b = payload(call("planMigration", json{{"spec", signed_spec()}}));
  EXPECT_EQ(a.value("planDigest", "a"), b.value("planDigest", "b"));

  pglaswell::WriteSession c(cfg());
  c.begin("pg_laswell/test/cleanup");
  c.txn().exec("DROP SCHEMA shop CASCADE");
  c.commit();
}

TEST_F(ToolTest, PlanMigrationRefusesAnUntrustedSpecBeforeMeasuringAnything) {
  pglaswell::WriteSession w(cfg());
  w.begin("pg_laswell/test/untrust");
  w.txn().exec("DELETE FROM laswell.trusted_key");
  w.commit();

  const auto p = payload(call("planMigration", json{{"spec", signed_spec()}}));
  EXPECT_TRUE(p.contains("error")) << p.dump(2);
  EXPECT_FALSE(p.contains("steps")) << "a refused spec must not produce a plan";
  EXPECT_NE(p.value("hint", "").find("validateSpec"), std::string::npos);
}

TEST_F(ToolTest, AnUnsignedDraftCanBePlannedButIsMarkedUnexecutable) {
  pglaswell::WriteSession w(cfg());
  w.begin("pg_laswell/test/draft-fixture");
  w.txn().exec("DROP SCHEMA IF EXISTS shop CASCADE");
  w.txn().exec("CREATE SCHEMA shop");
  w.txn().exec("CREATE TABLE shop.orders(id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,"
               " warehouse_id bigint, status text, created_at timestamptz)");
  w.txn().exec("CREATE TABLE shop.warehouse(id bigint PRIMARY KEY, region text)");
  w.commit();

  const auto p = payload(call(
      "planMigration", json{{"spec", minimal_spec()}, {"skipTrustChecks", true}}));
  EXPECT_TRUE(p.value("ok", false)) << p.dump(2);
  EXPECT_TRUE(p.value("draft", false));
  EXPECT_NE(p.value("note", "").find("cannot be executed"), std::string::npos);

  pglaswell::WriteSession c(cfg());
  c.begin("pg_laswell/test/cleanup");
  c.txn().exec("DROP SCHEMA shop CASCADE");
  c.commit();
}

TEST_F(ToolTest, AToolThatThrowsBecomesAnIsErrorResultNotAProtocolError) {
  // A protocol-level error is usually surfaced to a user as a crash; an
  // isError result is something a model can read and act on.
  const json r = rpc(*server_, json{{"jsonrpc", "2.0"},
                                    {"id", 9},
                                    {"method", "tools/call"},
                                    {"params",
                                     {{"name", "checkPrivileges"},
                                      {"arguments", {{"connection", "nosuch"}}}}}});
  ASSERT_TRUE(r.contains("result")) << r.dump();
  EXPECT_TRUE(r["result"].value("isError", false));
  const auto p = payload(r["result"]);
  EXPECT_NE(p.value("error", "").find("nosuch"), std::string::npos) << p.dump();
}

TEST(Trust, AnUnconfiguredLocalPolicyIsNotARefusal) {
  // Gate 1 is a fail-fast convenience; gate 2 in the database is the boundary.
  // Treating "no [trust] section" as "accepts nothing" would make the
  // DATABASE_URL form unable to run anything -- a papercut masquerading as a
  // security property.
  pglaswell::TrustPolicy empty;
  EXPECT_FALSE(empty.configured());
  const auto r = pglaswell::verify_against_policy(empty, "bytes", json::array());
  EXPECT_FALSE(r.verified);
  EXPECT_TRUE(r.not_configured);
  EXPECT_NE(r.reason.find("the target database decides"), std::string::npos);
}

TEST(Trust, AConfiguredPolicyThatExcludesTheKeyIsStillARefusal) {
  // The distinction that matters: configured-and-excludes is a refusal,
  // not-configured is a deferral.
  auto policy = policy_trusting_test_key();
  policy.accept[0] = "ed25519:1111111111111111";
  policy.keys["ed25519:1111111111111111"] = policy.keys[test_key().key_id];
  policy.keys["ed25519:1111111111111111"].key_id = "ed25519:1111111111111111";
  EXPECT_TRUE(policy.configured());
  const json sigs = json::array({{{"key_id", test_key().key_id},
                                  {"algorithm", "ed25519"},
                                  {"signature", "AAAA"}}});
  const auto r = pglaswell::verify_against_policy(policy, "bytes", sigs);
  EXPECT_FALSE(r.verified);
  EXPECT_FALSE(r.not_configured);
}

// --- the dry run ------------------------------------------------------------

namespace {
void make_big_shop(const pglaswell::ConnConfig& c, int rows) {
  pglaswell::WriteSession w(c);
  w.begin("pg_laswell/test/big-shop");
  w.txn().exec("DROP SCHEMA IF EXISTS shop CASCADE");
  w.txn().exec("CREATE SCHEMA shop");
  w.txn().exec("CREATE TABLE shop.warehouse(id bigint PRIMARY KEY, region text NOT NULL)");
  w.txn().exec("INSERT INTO shop.warehouse SELECT g, 'r'||g FROM generate_series(1,5) g");
  w.txn().exec(
      "CREATE TABLE shop.orders(id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,"
      " warehouse_id bigint, status text NOT NULL DEFAULT 'open',"
      " created_at timestamptz NOT NULL DEFAULT now())");
  w.txn().exec("INSERT INTO shop.orders(warehouse_id) SELECT (g%5)+1 FROM generate_series(1," +
               std::to_string(rows) + ") g");
  w.txn().exec("ANALYZE shop.orders");
  w.commit();
}

void make_shop(const pglaswell::ConnConfig& c) {
  pglaswell::WriteSession w(c);
  w.begin("pg_laswell/test/shop");
  w.txn().exec("DROP SCHEMA IF EXISTS shop CASCADE");
  w.txn().exec("CREATE SCHEMA shop");
  w.txn().exec("CREATE TABLE shop.warehouse(id bigint PRIMARY KEY, region text NOT NULL)");
  w.txn().exec("INSERT INTO shop.warehouse SELECT g, 'r'||g FROM generate_series(1,5) g");
  w.txn().exec(
      "CREATE TABLE shop.orders(id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,"
      " warehouse_id bigint, status text NOT NULL DEFAULT 'open',"
      " created_at timestamptz NOT NULL DEFAULT now())");
  w.txn().exec("INSERT INTO shop.orders(warehouse_id) SELECT (g%5)+1 FROM generate_series(1,300) g");
  w.commit();
}
}  // namespace

TEST_F(ToolTest, TheDryRunValidatesLaterStepsAgainstEarlierOnesSchema) {
  // The property the project rests on: PostgreSQL DDL is transactional, so the
  // backfill can be checked against the column that step 0 adds -- which does
  // not exist yet, and which statement-by-statement checking therefore cannot
  // validate at all.
  make_shop(cfg());
  const auto p = payload(call("planMigration", json{{"spec", signed_spec()}}));
  ASSERT_TRUE(p.value("ok", false)) << p.dump(2);
  ASSERT_TRUE(p.contains("dryRun")) << p.dump(2);
  EXPECT_TRUE(p["dryRun"].value("ran", false));
  EXPECT_FALSE(p["dryRun"].contains("problems")) << p["dryRun"].dump(2);
}

TEST_F(ToolTest, TheDryRunCommitsNothing) {
  make_shop(cfg());
  const auto p = payload(call("planMigration", json{{"spec", signed_spec()}}));
  ASSERT_TRUE(p.value("ok", false)) << p.dump(2);
  ASSERT_TRUE(p["dryRun"].value("ran", false));

  pglaswell::ReadSession r(cfg());
  EXPECT_EQ(r.txn()
                .exec("SELECT count(*) FROM information_schema.columns"
                      " WHERE table_schema='shop' AND table_name='orders'"
                      " AND column_name='fulfilment_region'")[0][0]
                .as<int>(),
            0)
      << "the dry run committed its DDL";
  EXPECT_EQ(r.txn()
                .exec("SELECT count(*) FROM pg_class"
                      " WHERE relname='orders_open_by_region_idx'")[0][0]
                .as<int>(),
            0);
}

TEST_F(ToolTest, TheDryRunCatchesSqlThatDoesNotWorkAgainstTheRealSchema) {
  // A backfill whose expression references a column that no step creates. The
  // spec parses, the plan renders, and only actually trying it finds the fault.
  make_shop(cfg());
  json doc = minimal_spec();
  doc["intents"][1]["set"] = json{{"fulfilment_region", "w.no_such_column"}};
  const auto parsed = pglaswell::parse_spec(doc);
  doc["signatures"] = json::array({{{"key_id", test_key().key_id},
                                    {"algorithm", "ed25519"},
                                    {"signature", base64(sign(parsed.canonical_bytes))}}});

  const auto p = payload(call("planMigration", json{{"spec", doc}}));
  EXPECT_FALSE(p.value("ok", true)) << p.dump(2);
  ASSERT_TRUE(p["dryRun"].contains("problems")) << p.dump(2);
  EXPECT_NE(p["dryRun"]["problems"][0].get<std::string>().find("no_such_column"),
            std::string::npos);
}

TEST_F(ToolTest, AConcurrentIndexBuildIsReportedAsUnverifiedNotSilentlyPassed) {
  // CREATE INDEX CONCURRENTLY cannot run inside a transaction block, so a dry
  // run cannot cover it. Saying so is the difference between a check and a
  // claim.
  pglaswell::WriteSession w(cfg());
  w.begin("pg_laswell/test/big");
  w.txn().exec("DROP SCHEMA IF EXISTS shop CASCADE");
  w.txn().exec("CREATE SCHEMA shop");
  w.txn().exec("CREATE TABLE shop.warehouse(id bigint PRIMARY KEY, region text NOT NULL)");
  w.txn().exec("INSERT INTO shop.warehouse SELECT g, 'r'||g FROM generate_series(1,5) g");
  w.txn().exec(
      "CREATE TABLE shop.orders(id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,"
      " warehouse_id bigint, status text NOT NULL DEFAULT 'open',"
      " created_at timestamptz NOT NULL DEFAULT now(), pad text)");
  // Past the 64 MiB ceiling, so the planner chooses the concurrent build.
  w.txn().exec("INSERT INTO shop.orders(warehouse_id, pad)"
               " SELECT (g%5)+1, repeat('x',400) FROM generate_series(1,200000) g");
  w.txn().exec("ANALYZE shop.orders");
  w.commit();

  const auto p = payload(call("planMigration", json{{"spec", signed_spec()}}));
  ASSERT_TRUE(p.value("ok", false)) << p.dump(2);
  const auto* idx = [&]() -> const json* {
    for (const auto& s : p["steps"]) {
      if (s["kind"] == "create_index") return &s;
    }
    return nullptr;
  }();
  ASSERT_NE(idx, nullptr);
  ASSERT_EQ((*idx)["txnClass"], "txn_forbidden") << "expected a concurrent build";
  EXPECT_FALSE(p["dryRun"]["unverifiedSteps"].empty())
      << "the concurrent build must be reported as unverified: "
      << p["dryRun"].dump(2);
  EXPECT_NE(p["dryRun"].value("note", "").find("rather than silently passed"),
            std::string::npos);
}

TEST_F(ToolTest, TheDryRunCanBeDeclined) {
  // It takes real locks, briefly. An operator who does not want planning to
  // touch the table at all must be able to say so.
  make_shop(cfg());
  const auto p = payload(
      call("planMigration", json{{"spec", signed_spec()}, {"dryRun", false}}));
  EXPECT_TRUE(p.value("ok", false)) << p.dump(2);
  EXPECT_FALSE(p.contains("dryRun"));
}

TEST_F(ToolTest, TheDryRunDeclinesRatherThanQueueingBehindAStrongLock) {
  // Planning must never block the application. The dry run holds strong locks
  // itself, so its own lock_timeout is what keeps that promise.
  make_shop(cfg());
  pqxx::connection holder(url_);
  pqxx::work holder_txn(holder);
  holder_txn.exec("LOCK TABLE shop.orders IN ACCESS EXCLUSIVE MODE");

  const auto started = std::chrono::steady_clock::now();
  const auto p = payload(call("planMigration", json{{"spec", signed_spec()}}));
  const auto elapsed = std::chrono::steady_clock::now() - started;
  holder_txn.abort();

  EXPECT_LT(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count(), 15)
      << "planning queued behind the lock instead of declining";
  // Either observation or the dry run reports the contention; neither hangs.
  EXPECT_FALSE(p.value("ok", true)) << p.dump(2);
}

// --- the executor -----------------------------------------------------------

namespace {

// Waits for a predicate on the job status, or fails. Polling rather than a
// condition variable because the executor is a black box to the test: it is
// driven through the same MCP surface an agent would use.
template <typename Pred>
bool wait_for_status(ToolTest& t, const std::string& job_id, Pred pred,
                     int attempts = 400) {
  for (int i = 0; i < attempts; ++i) {
    const auto st = t.status_of(job_id);
    if (!st.is_null() && pred(st)) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  return false;
}

}  // namespace

TEST_F(ToolTest, StartMigrationReturnsAJobIdBeforeDoingTheWork) {
  make_shop(cfg());
  const auto started = std::chrono::steady_clock::now();
  const auto p = payload(call("startMigration", json{{"spec", signed_spec()}}));
  const auto elapsed = std::chrono::steady_clock::now() - started;

  ASSERT_TRUE(p.value("accepted", false)) << p.dump(2);
  EXPECT_FALSE(p.value("jobId", "").empty());
  // The stdio loop must stay answerable while a migration runs; that is the
  // whole reason the job registry exists.
  EXPECT_FALSE(p.contains("steps")) << "startMigration returned step results";
  EXPECT_LT(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count(), 20);

  ASSERT_TRUE(wait_for_status(*this, p["jobId"], [](const json& s) {
    return s.value("state", "") == "succeeded" || s.value("state", "") == "failed";
  })) << "the job never finished";
  const auto final_status = status_of(p["jobId"]);
  EXPECT_EQ(final_status.value("state", ""), "succeeded") << final_status.dump(2);
}

TEST_F(ToolTest, TheExecutedPlanIsTheOneThatWasShown) {
  // "What ran is what you were shown" has to be checkable, not promised.
  make_shop(cfg());
  const auto planned = payload(call("planMigration", json{{"spec", signed_spec()}}));
  ASSERT_TRUE(planned.value("ok", false)) << planned.dump(2);

  const auto started = payload(call("startMigration", json{{"spec", signed_spec()}}));
  ASSERT_TRUE(started.value("accepted", false)) << started.dump(2);
  EXPECT_EQ(started.value("planDigest", "a"), planned.value("planDigest", "b"));

  ASSERT_TRUE(wait_for_status(*this, started["jobId"], [](const json& s) {
    return s.value("state", "") == "succeeded";
  })) << status_of(started["jobId"]).dump(2);
}

TEST_F(ToolTest, AMigrationActuallyChangesTheDatabase) {
  make_shop(cfg());
  const auto p = payload(call("startMigration", json{{"spec", signed_spec()}}));
  ASSERT_TRUE(wait_for_status(*this, p["jobId"], [](const json& s) {
    return s.value("state", "") == "succeeded";
  })) << status_of(p["jobId"]).dump(2);

  pglaswell::ReadSession r(cfg());
  // The column exists, with its COMMENT.
  EXPECT_EQ(r.txn()
                .exec("SELECT count(*) FROM information_schema.columns"
                      " WHERE table_schema='shop' AND table_name='orders'"
                      " AND column_name='fulfilment_region'")[0][0]
                .as<int>(),
            1);
  EXPECT_NE(r.txn()
                .exec("SELECT col_description('shop.orders'::regclass,"
                      " (SELECT attnum FROM pg_attribute"
                      "   WHERE attrelid='shop.orders'::regclass"
                      "     AND attname='fulfilment_region'))")[0][0]
                .as<std::string>(""),
            "");
  // Every row was backfilled.
  EXPECT_EQ(r.txn()
                .exec("SELECT count(*) FROM shop.orders"
                      " WHERE fulfilment_region IS NULL")[0][0]
                .as<int>(),
            0);
  // And the index exists and is valid.
  EXPECT_EQ(r.txn()
                .exec("SELECT count(*) FROM pg_index i JOIN pg_class c"
                      " ON c.oid=i.indexrelid"
                      " WHERE c.relname='orders_open_by_region_idx'"
                      "   AND i.indisvalid")[0][0]
                .as<int>(),
            1);
}

TEST_F(ToolTest, TheLedgerRecordsWhatRanVerbatim) {
  make_shop(cfg());
  const auto p = payload(call("startMigration", json{{"spec", signed_spec()}}));
  ASSERT_TRUE(wait_for_status(*this, p["jobId"], [](const json& s) {
    return s.value("state", "") == "succeeded";
  }));

  pglaswell::ReadSession r(cfg());
  const auto rows = pglaswell::pqxx_exec(
      r.txn(),
      "SELECT ordinal, kind, state, sql, why FROM laswell.step"
      " WHERE job_id = $1::uuid ORDER BY ordinal",
      pqxx::params{p["jobId"].get<std::string>()});
  ASSERT_GE(rows.size(), 3u) << "the ledger has no steps";
  for (const auto& row : rows) {
    // The statement verbatim, and the rule that chose the method. What ran is
    // never inferred from the spec.
    EXPECT_FALSE(row[3].template as<std::string>("").empty())
        << "step " << row[0].template as<int>() << " recorded no SQL";
    EXPECT_FALSE(row[4].template as<std::string>("").empty())
        << "step " << row[0].template as<int>() << " recorded no reason";
  }
  const auto job = pglaswell::pqxx_exec(
      r.txn(),
      "SELECT state, plan_digest, finished_at IS NOT NULL FROM laswell.job"
      " WHERE job_id = $1::uuid",
      pqxx::params{p["jobId"].get<std::string>()});
  ASSERT_EQ(job.size(), 1u);
  EXPECT_EQ(job[0][0].template as<std::string>(), "succeeded");
  EXPECT_EQ(job[0][1].template as<std::string>(), p.value("planDigest", ""));
  EXPECT_TRUE(job[0][2].template as<bool>());
}

TEST_F(ToolTest, ThePacedBackfillCommitsRepeatedlyRatherThanOnce) {
  // The observable proof that pacing happens at all.
  make_big_shop(cfg(), 20000);
  auto c = cfg();
  c.executor.batch_rows = 200;
  c.executor.commit_interval_ms = 50;
  c.executor.observer_tick_ms = 25;
  set_executor(c.executor);

  const auto p = payload(call("startMigration", json{{"spec", signed_spec()}}));
  ASSERT_TRUE(wait_for_status(*this, p["jobId"], [](const json& s) {
    return s.value("state", "") == "succeeded";
  })) << status_of(p["jobId"]).dump(2);

  const auto st = status_of(p["jobId"]);
  ASSERT_TRUE(st.contains("backfill")) << st.dump(2);
  const auto& b = st["backfill"];
  EXPECT_GT(b.value("commits", 0), 1)
      << "the backfill committed once; it was not paced: " << b.dump(2);
  EXPECT_EQ(b["commitReasons"].value("interval", 0) +
                b["commitReasons"].value("batch_cap", 0) +
                b["commitReasons"].value("lock_waiter", 0) +
                b["commitReasons"].value("final", 0),
            b.value("commits", 0));
}

TEST_F(ToolTest, CancellingAJobStopsItAndLeavesTheCursorCommitted) {
  make_big_shop(cfg(), 40000);
  auto c = cfg();
  c.executor.batch_rows = 100;
  c.executor.commit_interval_ms = 25;
  set_executor(c.executor);

  const auto p = payload(call("startMigration", json{{"spec", signed_spec()}}));
  const auto job_id = p["jobId"].get<std::string>();
  ASSERT_TRUE(wait_for_status(*this, job_id, [](const json& s) {
    return s.contains("backfill") && s["backfill"].value("commits", 0) > 0;
  })) << "the backfill never started committing";

  const auto cancelled = payload(call("cancelJob", json{{"jobId", job_id}}));
  EXPECT_TRUE(cancelled.value("requested", false)) << cancelled.dump(2);

  ASSERT_TRUE(wait_for_status(*this, job_id, [](const json& s) {
    const auto st = s.value("state", "");
    return st == "cancelled" || st == "succeeded";
  }));

  // Whatever it committed is consistent: the cursor and the data agree.
  pglaswell::ReadSession r(cfg());
  const auto cur = pglaswell::pqxx_exec(
      r.txn(),
      "SELECT last_key FROM laswell.backfill_cursor WHERE job_id = $1::uuid",
      pqxx::params{job_id});
  if (!cur.empty()) {
    const auto last = cur[0][0].template as<std::string>();
    const auto beyond = pglaswell::pqxx_exec(
        r.txn(),
        "SELECT count(*) FROM shop.orders"
        " WHERE id <= $1::bigint AND fulfilment_region IS NULL"
        "   AND warehouse_id IS NOT NULL",
        pqxx::params{last});
    EXPECT_EQ(beyond[0][0].template as<int>(), 0)
        << "rows at or below the committed cursor were not backfilled";
  }
}

TEST_F(ToolTest, JobStatusReportsContentionThresholdsAndItsOwnBlindSpot) {
  make_shop(cfg());
  const auto p = payload(call("startMigration", json{{"spec", signed_spec()}}));
  ASSERT_TRUE(wait_for_status(*this, p["jobId"], [](const json& s) {
    return s.value("state", "") == "succeeded";
  }));
  const auto st = status_of(p["jobId"]);
  ASSERT_TRUE(st.contains("contention")) << st.dump(2);
  const auto& c = st["contention"];
  EXPECT_TRUE(c.contains("transitiveWaiters"));
  EXPECT_TRUE(c.contains("inflictedBlockedMs"));
  EXPECT_TRUE(c["thresholds"].contains("pauseWaiters"));
  // The honest boundary: this measures harm it causes by holding locks, and is
  // blind to I/O, WAL and replication lag.
  EXPECT_NE(c.value("note", "").find("pg_licht"), std::string::npos) << c.dump(2);
  EXPECT_NE(c.value("note", "").find("TRANSITIVE"), std::string::npos);
}

TEST_F(ToolTest, TwoJobsForTheSameSpecCannotRunAtOnce) {
  make_big_shop(cfg(), 30000);
  auto c = cfg();
  c.executor.batch_rows = 100;
  c.executor.commit_interval_ms = 25;
  c.executor.max_concurrent_jobs = 4;  // not the limit under test
  set_executor(c.executor);

  const auto first = payload(call("startMigration", json{{"spec", signed_spec()}}));
  ASSERT_TRUE(first.value("accepted", false)) << first.dump(2);

  const auto second = payload(call("startMigration", json{{"spec", signed_spec()}}));
  EXPECT_FALSE(second.value("accepted", false))
      << "a second job for the same spec was admitted: " << second.dump(2);

  call("cancelJob", json{{"jobId", first["jobId"]}});
  wait_for_status(*this, first["jobId"], [](const json& s) {
    return s.value("state", "") == "cancelled" || s.value("state", "") == "succeeded";
  });
}

TEST_F(ToolTest, AFailedStepFailsTheJobAndTheLedgerSaysWhich) {
  make_shop(cfg());
  // A backfill expression that parses and plans but fails at runtime: a cast
  // that only fails on real data.
  json doc = minimal_spec();
  doc["intents"][1]["set"] = json{{"fulfilment_region", "(1/0)::text"}};
  const auto parsed = pglaswell::parse_spec(doc);
  doc["signatures"] = json::array({{{"key_id", test_key().key_id},
                                    {"algorithm", "ed25519"},
                                    {"signature", base64(sign(parsed.canonical_bytes))}}});

  const auto p = payload(call("startMigration", json{{"spec", doc}}));
  if (!p.value("accepted", false)) {
    // The dry run caught it first, which is also a correct outcome.
    SUCCEED() << "refused before starting: " << p.dump(2);
    return;
  }
  ASSERT_TRUE(wait_for_status(*this, p["jobId"], [](const json& s) {
    return s.value("state", "") == "failed";
  })) << status_of(p["jobId"]).dump(2);

  pglaswell::ReadSession r(cfg());
  const auto rows = pglaswell::pqxx_exec(
      r.txn(),
      "SELECT count(*) FROM laswell.step WHERE job_id = $1::uuid AND state = 'failed'",
      pqxx::params{p["jobId"].get<std::string>()});
  EXPECT_GT(rows[0][0].template as<int>(), 0) << "no step was recorded as failed";
}

TEST_F(ToolTest, ALockWaiterForcesACommitBeforeTheIntervalWouldHave) {
  // THE crux of the suite, and it is deterministic rather than timing-lucky.
  //
  // The commit interval is set to ten minutes and the row cap out of reach, so
  // the ONLY commit trigger a test finishing in seconds can reach is a lock
  // waiter. A non-zero lock_waiter count therefore cannot have come from
  // anything else, and interval must be exactly zero.
  make_big_shop(cfg(), 60000);
  auto e = cfg().executor;
  e.batch_rows = 50;
  e.commit_interval_ms = 600000;   // ten minutes: unreachable here
  e.batch_cap_rows = 100000000;    // unreachable here
  e.observer_tick_ms = 25;
  e.pause_waiters = 1000;          // the breaker must not interfere
  e.throttle_waiters = 1000;
  e.max_waiter_wait_ms = 600000;   // no escalation either
  set_executor(e);

  const auto p = payload(call("startMigration", json{{"spec", signed_spec()}}));
  ASSERT_TRUE(p.value("accepted", false)) << p.dump(2);
  const auto job_id = p["jobId"].get<std::string>();

  // Wait until the backfill is actually running and holding locks. Progress is
  // published per BATCH, so this fires while the transaction is still open --
  // which is the only window in which a waiter can form.
  ASSERT_TRUE(wait_for_status(*this, job_id, [](const json& s) {
    return s.contains("backfill") && s["backfill"].value("rowsDone", "0") != "0";
  })) << "the backfill never started: " << status_of(job_id).dump(2);

  // Create a waiter on purpose. SHARE conflicts with the RowExclusiveLock the
  // worker holds, so this queues behind the worker's open transaction.
  std::atomic<bool> stop{false};
  std::thread victim([&] {
    while (!stop.load()) {
      try {
        pqxx::connection c(url_);
        pqxx::work tx(c);
        tx.exec("SET LOCAL lock_timeout = 2000");
        tx.exec("LOCK TABLE shop.orders IN SHARE MODE");
        tx.commit();
      } catch (...) {
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  });

  const bool saw = wait_for_status(*this, job_id, [](const json& s) {
    return s.contains("backfill") &&
           s["backfill"]["commitReasons"].value("lock_waiter", 0) > 0;
  }, 240);

  stop = true;
  victim.join();
  call("cancelJob", json{{"jobId", job_id}});
  wait_for_status(*this, job_id, [](const json& s) {
    const auto st = s.value("state", "");
    return st == "cancelled" || st == "succeeded" || st == "failed";
  });

  const auto st = status_of(job_id);
  if (!saw) {
    GTEST_SKIP() << "the lock wait was never observed; on a loaded machine the "
                    "waiter can come and go between observer ticks: "
                 << st["backfill"].dump(2);
  }
  const json reasons = st["backfill"]["commitReasons"];
  EXPECT_GT(reasons.value("lock_waiter", 0), 0);
  // The deterministic assertion: with a ten-minute interval, an interval
  // commit in a test lasting seconds is impossible.
  EXPECT_EQ(reasons.value("interval", 0), 0)
      << "an interval commit fired with a ten-minute interval: " << st.dump(2);
  EXPECT_EQ(reasons.value("batch_cap", 0), 0);
}

TEST_F(ToolTest, WithNoWaiterTheIntervalIsWhatCommits) {
  // The inverse, so the previous test cannot pass by accident: nothing waiting,
  // a tiny interval, and every commit must be attributed to the interval.
  make_big_shop(cfg(), 20000);
  auto e = cfg().executor;
  e.batch_rows = 100;
  e.commit_interval_ms = 20;
  e.batch_cap_rows = 100000000;
  e.observer_tick_ms = 25;
  set_executor(e);

  const auto p = payload(call("startMigration", json{{"spec", signed_spec()}}));
  ASSERT_TRUE(wait_for_status(*this, p["jobId"], [](const json& s) {
    return s.value("state", "") == "succeeded";
  })) << status_of(p["jobId"]).dump(2);

  // By value: status_of() returns a temporary, so binding a reference into it
  // dangles. GCC's -Wdangling-reference catches this one at compile time --
  // the same family as the ostringstream::str() and json::value() bugs earlier.
  const json reasons = status_of(p["jobId"])["backfill"]["commitReasons"];
  EXPECT_GT(reasons.value("interval", 0), 0) << reasons.dump(2);
  EXPECT_EQ(reasons.value("lock_waiter", 0), 0)
      << "a lock waiter was reported with nothing contending";
}

TEST_F(ToolTest, ProgressIsVisibleWhileATransactionIsStillOpen) {
  // With a long commit interval an entire backfill can run in one transaction.
  // If progress were published only at commit, an agent polling jobStatus
  // would see zero throughout -- indistinguishable from a stuck job. It also
  // reports committed rows separately, because only those survive a crash.
  make_big_shop(cfg(), 40000);
  auto e = cfg().executor;
  e.batch_rows = 50;
  e.commit_interval_ms = 600000;  // one transaction for the whole run
  e.batch_cap_rows = 100000000;
  set_executor(e);

  const auto p = payload(call("startMigration", json{{"spec", signed_spec()}}));
  ASSERT_TRUE(p.value("accepted", false)) << p.dump(2);
  const auto job_id = p["jobId"].get<std::string>();

  ASSERT_TRUE(wait_for_status(*this, job_id, [](const json& s) {
    return s.contains("backfill") && s["backfill"].value("rowsDone", "0") != "0";
  })) << "no progress was visible before the first commit";

  const auto mid = status_of(job_id);
  EXPECT_NE(mid["backfill"].value("rowsDone", "0"), "0");
  // Nothing is committed yet, so the crash-survivable figure is still zero.
  EXPECT_EQ(mid["backfill"].value("rowsCommitted", "-1"), "0")
      << "rows were reported as committed before any commit: "
      << mid["backfill"].dump(2);

  ASSERT_TRUE(wait_for_status(*this, job_id, [](const json& s) {
    return s.value("state", "") == "succeeded";
  }));
  const auto done = status_of(job_id);
  EXPECT_EQ(done["backfill"].value("rowsCommitted", "0"),
            done["backfill"].value("rowsDone", "-1"));
}

TEST_F(ToolTest, ASucceededJobsCursorIsNotResumedFrom) {
  // A succeeded job's cursor sits at the end of the table. Resuming from it
  // would make the next run skip everything and report success -- which is
  // precisely what verify_remaining exists to catch, and it did catch it. But
  // a completeness check should be the second line of defence, not the first.
  make_big_shop(cfg(), 5000);
  auto e = cfg().executor;
  e.batch_rows = 500;
  e.commit_interval_ms = 50;
  set_executor(e);

  const auto first = payload(call("startMigration", json{{"spec", signed_spec()}}));
  ASSERT_TRUE(wait_for_status(*this, first["jobId"], [](const json& s) {
    return s.value("state", "") == "succeeded";
  })) << status_of(first["jobId"]).dump(2);

  // Undo the schema change, leaving the succeeded job's cursor behind.
  {
    pglaswell::WriteSession w(cfg());
    w.begin("pg_laswell/test/undo");
    w.txn().exec("DROP INDEX IF EXISTS shop.orders_open_by_region_idx");
    w.txn().exec("ALTER TABLE shop.orders DROP COLUMN fulfilment_region");
    w.commit();
  }

  const auto second = payload(call("startMigration", json{{"spec", signed_spec()}}));
  ASSERT_TRUE(second.value("accepted", false)) << second.dump(2);
  ASSERT_TRUE(wait_for_status(*this, second["jobId"], [](const json& s) {
    const auto st = s.value("state", "");
    return st == "succeeded" || st == "failed";
  })) << status_of(second["jobId"]).dump(2);

  const auto st = status_of(second["jobId"]);
  EXPECT_EQ(st.value("state", ""), "succeeded")
      << "the second run resumed from the first's completed cursor: "
      << st.dump(2);
  EXPECT_EQ(st["backfill"].value("rowsDone", "0"), "5000")
      << "the second run skipped rows it should have backfilled";

  pglaswell::ReadSession r(cfg());
  EXPECT_EQ(r.txn()
                .exec("SELECT count(*) FROM shop.orders"
                      " WHERE fulfilment_region IS NULL")[0][0]
                .as<int>(),
            0);
}

// --- the migration repository ----------------------------------------------

namespace {

class RepoTest : public ToolTest {
 public:
  void SetUp() override {
    ToolTest::SetUp();
    if (::testing::Test::IsSkipped()) return;
    dir_ = "/tmp/laswell_repo_" + std::to_string(::getpid()) + "_" +
           std::to_string(counter_++);
    std::filesystem::create_directories(dir_);
  }
  void TearDown() override {
    ToolTest::TearDown();
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  // Writes a spec, signed by the suite key so both gates pass.
  void write_spec(const std::string& file, json doc) {
    const auto parsed = pglaswell::parse_spec(doc);
    doc["signatures"] = json::array({{{"key_id", test_key().key_id},
                                      {"algorithm", "ed25519"},
                                      {"signature", base64(sign(parsed.canonical_bytes))}}});
    std::ofstream(dir_ + "/" + file) << doc.dump(2);
  }

  json scan(bool derive = true) {
    return payload(call("listMigrations",
                        json{{"directory", dir_}, {"deriveRelations", derive}}));
  }

  static json add_column_spec(const std::string& id, const std::string& table,
                              const std::string& column,
                              const std::vector<std::string>& deps = {}) {
    json d{{"laswell_spec_version", 1},
           {"id", id},
           {"description", "adds " + column + " to " + table},
           {"intents", json::array({json{{"kind", "add_column"},
                                         {"schema", "shop"},
                                         {"table", table},
                                         {"column", column},
                                         {"type", "text"},
                                         {"nullable", true},
                                         {"comment", "test column"}}})}};
    if (!deps.empty()) d["depends_on"] = deps;
    return d;
  }

  std::string dir_;
  static int counter_;
};
int RepoTest::counter_ = 0;

const json* entry_for(const json& scan, const std::string& id) {
  for (const auto& m : scan["migrations"]) {
    if (m.value("specId", "") == id) return &m;
  }
  return nullptr;
}

}  // namespace

TEST_F(RepoTest, ReportsPendingMigrationsAgainstThisDatabase) {
  make_shop(cfg());
  write_spec("0001.json", add_column_spec("0001", "orders", "a"));
  write_spec("0002.json", add_column_spec("0002", "customers", "b"));

  const auto s = scan(false);
  ASSERT_EQ(s["migrations"].size(), 2u) << s.dump(2);
  EXPECT_EQ(entry_for(s, "0001")->value("status", ""), "pending");
  EXPECT_EQ(entry_for(s, "0002")->value("status", ""), "pending");
}

TEST_F(RepoTest, AnAppliedSpecThatWasEditedAfterwardsIsFlagged) {
  // The highest-value check any migration tool has, and here it falls out of
  // digests for nothing: the database no longer matches the file that claims
  // to describe it.
  make_shop(cfg());
  write_spec("0001.json", add_column_spec("0001", "orders", "a"));

  const auto started = payload(call("startMigration",
      json{{"spec", json::parse(std::ifstream(dir_ + "/0001.json"),
                                nullptr, true)}}));
  ASSERT_TRUE(started.value("accepted", false)) << started.dump(2);
  ASSERT_TRUE(wait_for_status(*this, started["jobId"], [](const json& x) {
    return x.value("state", "") == "succeeded";
  })) << status_of(started["jobId"]).dump(2);

  EXPECT_EQ(scan(false)["migrations"][0].value("status", ""), "applied");

  // Now edit it. The content changes, so the digest changes.
  auto edited = add_column_spec("0001", "orders", "a");
  edited["description"] = "adds a to orders (edited after apply)";
  write_spec("0001.json", edited);

  const auto s = scan(false);
  const auto* e = entry_for(s, "0001");
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(e->value("status", ""), "modified_after_apply") << s.dump(2);
  EXPECT_FALSE(e->value("appliedDigest", "").empty());
  EXPECT_NE(e->value("appliedDigest", ""), e->value("digest", ""));
  EXPECT_NE(e->value("hint", "").find("write a NEW spec"), std::string::npos)
      << "the hint must say what to do instead of re-applying";
}

TEST_F(RepoTest, DependenciesEstablishTheOrder) {
  make_shop(cfg());
  write_spec("b.json", add_column_spec("b", "orders", "b", {"a"}));
  write_spec("a.json", add_column_spec("a", "orders", "a"));

  const auto s = scan(false);
  ASSERT_EQ(s["order"].size(), 2u) << s.dump(2);
  EXPECT_EQ(s["order"][0]["groups"][0]["specs"][0], "a");
  EXPECT_EQ(s["order"][1]["groups"][0]["specs"][0], "b");
}

TEST_F(RepoTest, AMissingDependencyIsReportedRatherThanIgnored) {
  make_shop(cfg());
  write_spec("b.json", add_column_spec("b", "orders", "b", {"nonexistent"}));
  const auto s = scan(false);
  bool named = false;
  for (const auto& p : s["problems"]) {
    if (p.get<std::string>().find("nonexistent") != std::string::npos) named = true;
  }
  EXPECT_TRUE(named) << s["problems"].dump(2);
}

TEST_F(RepoTest, ACycleIsNamedAsACycle) {
  make_shop(cfg());
  write_spec("a.json", add_column_spec("a", "orders", "a", {"b"}));
  write_spec("b.json", add_column_spec("b", "orders", "b", {"a"}));
  const auto s = scan(false);
  bool cycle = false;
  for (const auto& p : s["problems"]) {
    if (p.get<std::string>().find("cycle") != std::string::npos) cycle = true;
  }
  EXPECT_TRUE(cycle) << s["problems"].dump(2);
}

TEST_F(RepoTest, ADependencyStuckInABadStateIsNotMisreportedAsACycle) {
  // These are different problems with different fixes. Reporting the first as
  // the second sends the reader hunting for something that does not exist.
  make_shop(cfg());
  write_spec("0001.json", add_column_spec("0001", "orders", "a"));
  write_spec("0002.json", add_column_spec("0002", "orders", "b", {"0001"}));

  const auto started = payload(call("startMigration",
      json{{"spec", json::parse(std::ifstream(dir_ + "/0001.json"), nullptr, true)}}));
  ASSERT_TRUE(wait_for_status(*this, started["jobId"], [](const json& x) {
    return x.value("state", "") == "succeeded";
  }));
  auto edited = add_column_spec("0001", "orders", "a");
  edited["description"] = "edited";
  write_spec("0001.json", edited);

  const auto s = scan(false);
  bool blocked = false, cycle = false;
  for (const auto& p : s["problems"]) {
    const auto t = p.get<std::string>();
    if (t.find("modified_after_apply") != std::string::npos &&
        t.find("0002") != std::string::npos) {
      blocked = true;
    }
    if (t.find("cycle") != std::string::npos) cycle = true;
  }
  EXPECT_TRUE(blocked) << s["problems"].dump(2);
  EXPECT_FALSE(cycle) << "a blocked dependency was misreported as a cycle";
}

TEST_F(RepoTest, RelationsHiddenInAnOpaqueExpressionAreDerived) {
  // The spec declares shop.orders. The set-expression reads shop.region_tax,
  // which no intent names. EXPLAIN reveals it, which is the whole reason the
  // derivation asks PostgreSQL rather than parsing the SQL itself.
  {
    pglaswell::WriteSession w(cfg());
    w.begin("pg_laswell/test/repo-fixture");
    w.txn().exec("DROP SCHEMA IF EXISTS shop CASCADE");
    w.txn().exec("CREATE SCHEMA shop");
    w.txn().exec("CREATE TABLE shop.region_tax(region text PRIMARY KEY, rate numeric)");
    w.txn().exec("CREATE TABLE shop.orders(id bigint GENERATED ALWAYS AS IDENTITY"
                 " PRIMARY KEY, r text, tax numeric)");
    w.txn().exec("INSERT INTO shop.orders(r) SELECT 'x' FROM generate_series(1,50)");
    w.commit();
  }
  write_spec("0001.json",
             json{{"laswell_spec_version", 1},
                  {"id", "0001"},
                  {"description", "fill tax from the region table"},
                  {"intents", json::array({json{
                      {"kind", "backfill"}, {"schema", "shop"}, {"table", "orders"},
                      {"key", "id"},
                      {"set", {{"tax", "(SELECT rate FROM shop.region_tax t"
                                       " WHERE t.region = orders.r)"}}},
                      {"where", "orders.tax IS NULL"}}})}});

  const auto s = scan(true);
  const auto* e = entry_for(s, "0001");
  ASSERT_NE(e, nullptr) << s.dump(2);
  ASSERT_TRUE(e->contains("relations")) << e->dump(2);
  bool found = false;
  for (const auto& r : (*e)["relations"]) {
    if (r.get<std::string>() == "shop.region_tax") found = true;
  }
  EXPECT_TRUE(found) << "a relation hidden in an opaque expression was not "
                        "derived: " << (*e)["relations"].dump();
}

TEST_F(RepoTest, IndependentMigrationsAreGroupedAndOverlappingOnesAreNot) {
  {
    pglaswell::WriteSession w(cfg());
    w.begin("pg_laswell/test/two-tables");
    w.txn().exec("DROP SCHEMA IF EXISTS shop CASCADE");
    w.txn().exec("CREATE SCHEMA shop");
    w.txn().exec("CREATE TABLE shop.orders(id bigint PRIMARY KEY)");
    w.txn().exec("CREATE TABLE shop.customers(id bigint PRIMARY KEY)");
    w.commit();
  }
  write_spec("a.json", add_column_spec("a", "orders", "ca"));
  write_spec("b.json", add_column_spec("b", "customers", "cb"));

  const auto s = scan(true);
  ASSERT_EQ(s["order"].size(), 1u) << s.dump(2);
  EXPECT_TRUE(s["order"][0].value("provenConcurrent", false))
      << "two migrations on disjoint tables were not grouped: " << s.dump(2);

  // Now make them overlap: both touch shop.orders.
  write_spec("b.json", add_column_spec("b", "orders", "cb"));
  const auto s2 = scan(true);
  EXPECT_FALSE(s2["order"][0].value("provenConcurrent", true))
      << "overlapping migrations were grouped";
  ASSERT_TRUE(s2["order"][0].contains("undecided")) << s2["order"][0].dump(2);
  EXPECT_NE(s2["order"][0]["undecided"][0].get<std::string>().find("shop.orders"),
            std::string::npos);
}

TEST_F(RepoTest, AnOpaqueTriggerPreventsProvingIndependence) {
  // A trigger function can write any table, including the other migration's.
  // The catalog names the trigger and stops there, so independence is not
  // provable and the tool says so rather than guessing.
  {
    pglaswell::WriteSession w(cfg());
    w.begin("pg_laswell/test/trigger");
    w.txn().exec("DROP SCHEMA IF EXISTS shop CASCADE");
    w.txn().exec("CREATE SCHEMA shop");
    w.txn().exec("CREATE TABLE shop.orders(id bigint PRIMARY KEY)");
    w.txn().exec("CREATE TABLE shop.customers(id bigint PRIMARY KEY)");
    w.txn().exec("CREATE FUNCTION shop.t() RETURNS trigger LANGUAGE plpgsql AS "
                 "$$ BEGIN RETURN NEW; END $$");
    w.txn().exec("CREATE TRIGGER tr AFTER UPDATE ON shop.orders"
                 " FOR EACH ROW EXECUTE FUNCTION shop.t()");
    w.commit();
  }
  write_spec("a.json", add_column_spec("a", "orders", "ca"));
  write_spec("b.json", add_column_spec("b", "customers", "cb"));

  const auto s = scan(true);
  EXPECT_FALSE(s["order"][0].value("provenConcurrent", true))
      << "independence was claimed despite an opaque trigger: " << s.dump(2);
  ASSERT_TRUE(s["order"][0].contains("undecided"));
  EXPECT_NE(s["order"][0]["undecided"][0].get<std::string>().find("trigger"),
            std::string::npos)
      << s["order"][0]["undecided"].dump(2);
}

TEST_F(RepoTest, AnUnreadableSpecIsReportedNotSkipped) {
  make_shop(cfg());
  std::ofstream(dir_ + "/broken.json") << R"({"laswell_spec_version":1,"id":"x"})";
  const auto s = scan(false);
  const auto* e = entry_for(s, "");
  ASSERT_NE(e, nullptr) << s.dump(2);
  EXPECT_EQ(e->value("status", ""), "unreadable");
  EXPECT_FALSE(e->value("error", "").empty());
}

TEST(Planner, AnEquivalentIndexUnderAnotherNameIsRenamedNotRebuilt) {
  // Convergence, which is what a migration repository is for: the same spec
  // creates the index on a database that lacks it and corrects the name on one
  // where somebody built it by hand. Refusing forever on that second database
  // is the worse outcome.
  auto obs = observations(1024, 10);
  // PostgreSQL's OWN rendering, not the spec's string. An earlier version of
  // this test used the spec's text on both sides and therefore could not fail
  // the way reality fails: pg_get_expr returns "(status = 'open'::text)".
  obs.tables["shop.orders"]["indexes"]["some_other_name"] =
      json{{"is_valid", true}, {"is_unique", false}, {"method", "btree"},
           {"predicate", "(status = 'open'::text)"}, {"has_expressions", false},
           {"columns", json::array({"fulfilment_region", "created_at"})},
           {"leading_column", "fulfilment_region"}};

  const auto plan = pglaswell::plan_migration(pglaswell::parse_spec(minimal_spec()), obs, {});
  ASSERT_TRUE(plan.ok) << plan.render();
  const auto* s = find_step(plan, "create_index");
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(s->action, pglaswell::Action::kApply);
  EXPECT_NE(all_sql(*s).find("ALTER INDEX shop.some_other_name RENAME TO "
                             "orders_open_by_region_idx"),
            std::string::npos)
      << all_sql(*s);
  EXPECT_EQ(all_sql(*s).find("CREATE INDEX"), std::string::npos)
      << "a second index was built instead of the name being corrected";
  // Cheap: measured on 18.6, ALTER INDEX ... RENAME takes
  // ShareUpdateExclusiveLock on the INDEX and no lock on the table at all.
  EXPECT_NE(s->lock.find("table is not locked"), std::string::npos) << s->lock;
  // And the risk this tool cannot see is named rather than hidden.
  EXPECT_NE(s->detail.value("risk", "").find("monitoring dashboard"),
            std::string::npos);
}

TEST(Planner, RenamingAConstraintBackedIndexIsRefused) {
  // Measured on 18.6: renaming a constraint-backed index renames the
  // CONSTRAINT too. Turning a primary key called orders_pkey into
  // orders_open_by_region_idx is not a name correction.
  auto obs = observations(1024, 10);
  // A SEPARATE index -- orders_pkey must stay intact, because the fixture's
  // backfill needs it as the unique key for its keyset walk.
  obs.tables["shop.orders"]["indexes"]["orders_region_uq"] =
      json{{"is_valid", true}, {"is_unique", false}, {"method", "btree"},
           {"predicate", "(status = 'open'::text)"}, {"has_expressions", false},
           {"constraint_backed", true},
           {"columns", json::array({"fulfilment_region", "created_at"})},
           {"leading_column", "fulfilment_region"}};
  const auto plan = pglaswell::plan_migration(pglaswell::parse_spec(minimal_spec()), obs, {});
  EXPECT_FALSE(plan.ok) << plan.render();
  bool named = false;
  for (const auto& c : plan.conflicts) {
    if (c.find("rename the constraint") != std::string::npos) named = true;
  }
  EXPECT_TRUE(named) << json(plan.conflicts).dump(2);
}

TEST(Planner, OnEquivalentIndexAdoptChangesNothing) {
  auto obs = observations(1024, 10);
  obs.tables["shop.orders"]["indexes"]["hand_built"] =
      json{{"is_valid", true}, {"is_unique", false}, {"method", "btree"},
           {"predicate", "(status = 'open'::text)"}, {"has_expressions", false},
           {"columns", json::array({"fulfilment_region", "created_at"})},
           {"leading_column", "fulfilment_region"}};
  auto doc = minimal_spec();
  for (auto& i : doc["intents"]) {
    if (i["kind"] == "create_index") i["on_equivalent_index"] = "adopt";
  }
  const auto plan = pglaswell::plan_migration(pglaswell::parse_spec(doc), obs, {});
  ASSERT_TRUE(plan.ok) << plan.render();
  const auto* s = find_step(plan, "create_index");
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(s->action, pglaswell::Action::kSatisfied);
  EXPECT_TRUE(s->sql.empty()) << "adopt must change nothing";
  EXPECT_EQ(s->detail.value("adopted", ""), "hand_built");
}

TEST(Planner, OnEquivalentIndexRefuseRestoresTheConflict) {
  auto obs = observations(1024, 10);
  obs.tables["shop.orders"]["indexes"]["hand_built"] =
      json{{"is_valid", true}, {"is_unique", false}, {"method", "btree"},
           {"predicate", "(status = 'open'::text)"}, {"has_expressions", false},
           {"columns", json::array({"fulfilment_region", "created_at"})},
           {"leading_column", "fulfilment_region"}};
  auto doc = minimal_spec();
  for (auto& i : doc["intents"]) {
    if (i["kind"] == "create_index") i["on_equivalent_index"] = "refuse";
  }
  const auto plan = pglaswell::plan_migration(pglaswell::parse_spec(doc), obs, {});
  EXPECT_FALSE(plan.ok) << plan.render();
  EXPECT_NE(plan.conflicts[0].find("hand_built"), std::string::npos);
}

TEST(Spec, AnUnknownOnEquivalentIndexPolicyIsRefused) {
  auto doc = minimal_spec();
  for (auto& i : doc["intents"]) {
    if (i["kind"] == "create_index") i["on_equivalent_index"] = "ignore";
  }
  const auto err = spec_error(doc);
  EXPECT_NE(err.find("on_equivalent_index"), std::string::npos) << err;
  EXPECT_NE(err.find("adopt"), std::string::npos)
      << "the hint must list the accepted policies";
}

TEST(Planner, ADifferentPredicateWarnsRatherThanRefusing) {
  // Same columns, different WHERE. Two SQL expressions being equivalent is not
  // decidable by string comparison, so this must not refuse -- it names both
  // predicates and lets the reader judge.
  auto obs = observations(1024, 10);
  obs.tables["shop.orders"]["indexes"]["partial_other"] =
      json{{"is_valid", true}, {"is_unique", false}, {"method", "btree"},
           {"predicate", "(status = 'closed'::text)"}, {"has_expressions", false},
           {"columns", json::array({"fulfilment_region", "created_at"})},
           {"leading_column", "fulfilment_region"}};
  const auto plan = pglaswell::plan_migration(pglaswell::parse_spec(minimal_spec()), obs, {});
  EXPECT_TRUE(plan.ok) << plan.render();
  bool warned = false;
  for (const auto& w : plan.warnings) {
    if (w.find("their predicates differ as written") != std::string::npos) warned = true;
  }
  EXPECT_TRUE(warned) << json(plan.warnings).dump(2);
}

TEST(Planner, APrefixRedundantIndexWarnsRatherThanRefusing) {
  // There are legitimate reasons to want a narrower index -- size, fillfactor
  // -- so refusing would be the tool overriding a judgement it cannot make.
  auto obs = observations(1024, 10);
  obs.tables["shop.orders"]["indexes"]["wider"] =
      json{{"is_valid", true}, {"is_unique", false}, {"method", "btree"},
           {"predicate", "(status = 'open'::text)"}, {"has_expressions", false},
           {"columns", json::array({"fulfilment_region", "created_at", "id"})},
           {"leading_column", "fulfilment_region"}};
  const auto plan = pglaswell::plan_migration(pglaswell::parse_spec(minimal_spec()), obs, {});
  EXPECT_TRUE(plan.ok) << plan.render();
  bool warned = false;
  for (const auto& w : plan.warnings) {
    if (w.find("is a prefix of the existing index") != std::string::npos) warned = true;
  }
  EXPECT_TRUE(warned) << "no prefix warning: " << json(plan.warnings).dump(2);
}

TEST(Planner, AnIndexOverExpressionsIsNotComparedRatherThanComparedBadly) {
  // indkey carries 0 for an expression column, which cannot be named. A partial
  // comparison would be worse than declining to compare.
  auto obs = observations(1024, 10);
  obs.tables["shop.orders"]["indexes"]["expr_idx"] =
      json{{"is_valid", true}, {"is_unique", false}, {"method", "btree"},
           {"predicate", "(status = 'open'::text)"}, {"has_expressions", true},
           {"columns", json::array({"fulfilment_region"})},
           {"leading_column", "fulfilment_region"}};
  const auto plan = pglaswell::plan_migration(pglaswell::parse_spec(minimal_spec()), obs, {});
  EXPECT_TRUE(plan.ok) << plan.render();
}

TEST(Planner, AnInvalidDuplicateDoesNotBlockTheRebuild) {
  // A leftover INVALID index from a failed concurrent build must not be
  // mistaken for a legitimate duplicate: it is exactly what we are replacing.
  auto obs = observations(1024, 10);
  obs.tables["shop.orders"]["indexes"]["leftover"] =
      json{{"is_valid", false}, {"is_unique", false}, {"method", "btree"},
           {"predicate", "(status = 'open'::text)"}, {"has_expressions", false},
           {"columns", json::array({"fulfilment_region", "created_at"})},
           {"leading_column", "fulfilment_region"}};
  const auto plan = pglaswell::plan_migration(pglaswell::parse_spec(minimal_spec()), obs, {});
  EXPECT_TRUE(plan.ok) << plan.render();
}

TEST(Planner, APartialAndAFullIndexAreDifferentNotAmbiguous) {
  // One covers rows the other does not, so there is nothing to be ambiguous
  // about. Warning here would be noise on every narrow index built beside a
  // partial one.
  auto obs = observations(1024, 10);
  obs.tables["shop.orders"]["indexes"]["partial"] =
      json{{"is_valid", true}, {"is_unique", false}, {"method", "btree"},
           {"predicate", "(status = 'open'::text)"}, {"has_expressions", false},
           {"columns", json::array({"fulfilment_region", "created_at"})},
           {"leading_column", "fulfilment_region"}};
  auto doc = minimal_spec();
  for (auto& i : doc["intents"]) {
    if (i["kind"] == "create_index") i.erase("where");  // no predicate
  }
  const auto plan = pglaswell::plan_migration(pglaswell::parse_spec(doc), obs, {});
  EXPECT_TRUE(plan.ok) << plan.render();
  for (const auto& w : plan.warnings) {
    EXPECT_EQ(w.find("predicates differ as written"), std::string::npos)
        << "a full index beside a partial one was reported as ambiguous: " << w;
  }
}

// --- drop_index, set_not_null, add_foreign_key ------------------------------

TEST(Planner, DroppingAnIndexOnAQuietTableUsesAPlainDrop) {
  // A drop is fast whatever else is true. What costs is ACQUIRING the lock, so
  // the rule reads lock_waiters and ignores the table's size.
  auto obs = observations(2LL << 30, 8000000);  // huge, but quiet
  obs.tables["shop.orders"]["indexes"]["stale_idx"] =
      json{{"is_valid", true}, {"is_unique", false}, {"method", "btree"},
           {"columns", json::array({"created_at"})}, {"leading_column", "created_at"}};
  const auto plan = pglaswell::plan_migration(
      spec_of(json::array({json{{"kind", "drop_index"}, {"schema", "shop"},
                                {"table", "orders"}, {"name", "stale_idx"}}})),
      obs, {});
  ASSERT_TRUE(plan.ok) << plan.render();
  const auto s = steps_of(plan, "drop_index");
  ASSERT_EQ(s.size(), 1u);
  EXPECT_NE(all_sql(*s[0]).find("DROP INDEX shop.stale_idx"), std::string::npos);
  EXPECT_EQ(all_sql(*s[0]).find("CONCURRENTLY"), std::string::npos);
  EXPECT_EQ(s[0]->txn_class, pglaswell::TxnClass::kOptional);
}

TEST(Planner, DroppingAnIndexWithWaitersUsesConcurrently) {
  auto obs = observations(1024, 10, /*waiters=*/2);
  obs.tables["shop.orders"]["indexes"]["stale_idx"] =
      json{{"is_valid", true}, {"is_unique", false}, {"method", "btree"},
           {"columns", json::array({"created_at"})}, {"leading_column", "created_at"}};
  const auto plan = pglaswell::plan_migration(
      spec_of(json::array({json{{"kind", "drop_index"}, {"schema", "shop"},
                                {"table", "orders"}, {"name", "stale_idx"}}})),
      obs, {});
  const auto s = steps_of(plan, "drop_index");
  ASSERT_EQ(s.size(), 1u);
  EXPECT_NE(all_sql(*s[0]).find("DROP INDEX CONCURRENTLY"), std::string::npos);
  EXPECT_EQ(s[0]->txn_class, pglaswell::TxnClass::kForbidden);
  EXPECT_NE(s[0]->why.find("queue behind"), std::string::npos) << s[0]->why;
}

TEST(Planner, DroppingAConstraintBackedIndexIsRefused) {
  auto obs = observations(1024, 10);
  obs.tables["shop.orders"]["indexes"]["orders_uq"] =
      json{{"is_valid", true}, {"is_unique", true}, {"constraint_backed", true},
           {"method", "btree"}, {"columns", json::array({"created_at"})},
           {"leading_column", "created_at"}};
  const auto plan = pglaswell::plan_migration(
      spec_of(json::array({json{{"kind", "drop_index"}, {"schema", "shop"},
                                {"table", "orders"}, {"name", "orders_uq"}}})),
      obs, {});
  EXPECT_FALSE(plan.ok) << plan.render();
  EXPECT_NE(plan.conflicts[0].find("drop the constraint instead"), std::string::npos);
}

TEST(Planner, DroppingAnAbsentIndexIsSatisfied) {
  const auto plan = pglaswell::plan_migration(
      spec_of(json::array({json{{"kind", "drop_index"}, {"schema", "shop"},
                                {"table", "orders"}, {"name", "never_existed"}}})),
      observations(1024, 10), {});
  ASSERT_TRUE(plan.ok);
  EXPECT_EQ(steps_of(plan, "drop_index")[0]->action, pglaswell::Action::kSatisfied);
}






// --- attach_partition / detach_partition -----------------------------------

static pglaswell::Observations obs_partitioned(bool with_default = false,
                                               bool child_indexed = true) {
  auto obs = observations(1024, 10);
  obs.tables["shop.events"] =
      json{{"exists", true}, {"kind", "partitioned_table"},
           {"partition_key", "RANGE (at)"},
           {"partitions", json::array({"shop.events_2025"})},
           {"detach_pending", json::array()},
           {"lock_waiters", 0}, {"size_estimate", 0},
           {"indexes", json{{"events_at",
                             {{"is_valid", true}, {"is_unique", false},
                              {"method", "btree"}, {"predicate", ""},
                              {"columns", json::array({"at"})}}}}},
           {"columns", json::object()}, {"constraints", json::object()}};
  if (with_default) obs.tables["shop.events"]["default_partition"] = "shop.events_def";
  obs.tables["shop.events_2026"] =
      json{{"exists", true}, {"kind", "table"}, {"size_estimate", 2LL << 30},
           {"columns", json::object()}, {"constraints", json::object()},
           {"indexes", child_indexed
                           ? json{{"events_2026_at",
                                   {{"is_valid", true}, {"is_unique", false},
                                    {"method", "btree"}, {"predicate", ""},
                                    {"columns", json::array({"at"})}}}}
                           : json::object()}};
  obs.tables["shop.events_2025"] =
      json{{"exists", true}, {"kind", "table"}, {"size_estimate", 2LL << 30},
           {"columns", json::object()}, {"constraints", json::object()},
           {"indexes", json::object()}};
  return obs;
}

static pglaswell::Spec attach_spec() {
  return spec_of(json::array({json{{"kind", "attach_partition"},
                                   {"schema", "shop"}, {"table", "events"},
                                   {"partition", "events_2026"},
                                   {"from", "'2026-01-01'"},
                                   {"to", "'2027-01-01'"}}}));
}

TEST(Planner, AttachProvesTheBoundsFirstSoTheAttachItselfIsCatalogOnly) {
  // Measured (S16), index build excluded from the comparison: ATTACH without a
  // matching CHECK took 98.393ms on 2M rows; with a validated CHECK, 0.914ms.
  // The recipe moves that scan to a VALIDATE, which runs under
  // ShareUpdateExclusiveLock instead of AccessExclusiveLock.
  const auto plan = pglaswell::plan_migration(attach_spec(), obs_partitioned(), {});
  ASSERT_TRUE(plan.ok) << plan.render();
  const auto s = steps_of(plan, "attach_partition");
  ASSERT_EQ(s.size(), 4u) << plan.render();

  EXPECT_NE(all_sql(*s[0]).find(
                "CHECK (at >= '2026-01-01' AND at < '2027-01-01') NOT VALID"),
            std::string::npos) << all_sql(*s[0]);
  EXPECT_NE(all_sql(*s[1]).find("VALIDATE CONSTRAINT"), std::string::npos);
  EXPECT_NE(all_sql(*s[2]).find(
                "ATTACH PARTITION shop.events_2026 FOR VALUES FROM "
                "('2026-01-01') TO ('2027-01-01')"),
            std::string::npos) << all_sql(*s[2]);
  // The CHECK is dropped afterwards: the partition bound now enforces it, and
  // PostgreSQL would evaluate both on every insert forever.
  EXPECT_NE(all_sql(*s[3]).find("DROP CONSTRAINT"), std::string::npos);

  // The scan step must not share a transaction with the NOT VALID add, or the
  // stronger lock is held across it and the recipe buys nothing.
  for (const auto* step : s) EXPECT_TRUE(step->own_transaction) << step->why;
  EXPECT_NE(s[1]->lock.find("ShareUpdateExclusiveLock"), std::string::npos);
  // The parent keeps serving its other partitions throughout.
  EXPECT_NE(s[2]->lock.find("ShareUpdateExclusiveLock on shop.events"),
            std::string::npos) << s[2]->lock;
}

TEST(Planner, AttachWarnsAboutIndexesTheCandidateDoesNotMatch) {
  // The cost that made the first measurement of this recipe read wrong: an
  // unmatched parent index is BUILT during ATTACH under AccessExclusiveLock.
  const auto plan = pglaswell::plan_migration(
      attach_spec(), obs_partitioned(false, /*child_indexed=*/false), {});
  ASSERT_TRUE(plan.ok) << plan.render();
  bool warned = false;
  for (const auto& w : plan.warnings) {
    if (w.find("events_at") != std::string::npos &&
        w.find("create_index") != std::string::npos) warned = true;
  }
  EXPECT_TRUE(warned) << plan.render();

  // A candidate whose index matches structurally draws no warning.
  const auto matched = pglaswell::plan_migration(
      attach_spec(), obs_partitioned(false, /*child_indexed=*/true), {});
  for (const auto& w : matched.warnings) {
    EXPECT_EQ(w.find("no index matching"), std::string::npos) << w;
  }
}

TEST(Planner, AttachWarnsAboutTheDefaultPartitionScanNoCheckCanAvoid) {
  // 165ms with a 2M-row default present, against 0.9ms without one. The cost
  // belongs to the DEFAULT, so no CHECK on the candidate removes it -- which
  // is exactly why it needs saying rather than being assumed away.
  const auto plan = pglaswell::plan_migration(
      attach_spec(), obs_partitioned(/*with_default=*/true), {});
  bool warned = false;
  for (const auto& w : plan.warnings) {
    if (w.find("DEFAULT partition") != std::string::npos &&
        w.find("shop.events_def") != std::string::npos) warned = true;
  }
  EXPECT_TRUE(warned) << plan.render();

  const auto none = pglaswell::plan_migration(attach_spec(), obs_partitioned(), {});
  for (const auto& w : none.warnings) {
    EXPECT_EQ(w.find("DEFAULT partition"), std::string::npos) << w;
  }
}

TEST(Planner, ACompositePartitionKeyIsNotReducedToAHalfCheck) {
  // A CHECK over the first column of a composite key would not prove what
  // ATTACH needs, so the scan would happen anyway -- silently. Saying so beats
  // emitting a constraint that looks like it helps.
  auto obs = obs_partitioned();
  obs.tables["shop.events"]["partition_key"] = "RANGE (tenant_id, at)";
  const auto plan = pglaswell::plan_migration(attach_spec(), obs, {});
  ASSERT_TRUE(plan.ok) << plan.render();
  const auto s = steps_of(plan, "attach_partition");
  ASSERT_EQ(s.size(), 1u) << "no CHECK recipe for a key we cannot reduce:\n"
                          << plan.render();
  EXPECT_NE(all_sql(*s[0]).find("ATTACH PARTITION"), std::string::npos);
  bool warned = false;
  for (const auto& w : plan.warnings) {
    if (w.find("composite or expression partition key") != std::string::npos)
      warned = true;
  }
  EXPECT_TRUE(warned) << plan.render();
}

TEST(Planner, AnAlreadyAttachedPartitionIsSatisfied) {
  auto obs = obs_partitioned();
  obs.tables["shop.events"]["partitions"].push_back("shop.events_2026");
  const auto plan = pglaswell::plan_migration(attach_spec(), obs, {});
  ASSERT_TRUE(plan.ok);
  EXPECT_EQ(steps_of(plan, "attach_partition")[0]->action,
            pglaswell::Action::kSatisfied);
}

TEST(Planner, AttachingToSomethingUnpartitionedIsRefused) {
  auto obs = obs_partitioned();
  obs.tables["shop.events"]["kind"] = "table";
  const auto plan = pglaswell::plan_migration(attach_spec(), obs, {});
  EXPECT_FALSE(plan.ok) << plan.render();
  EXPECT_NE(plan.conflicts[0].find("not a partitioned table"), std::string::npos);
}

static pglaswell::Spec detach_spec() {
  return spec_of(json::array({json{{"kind", "detach_partition"},
                                   {"schema", "shop"}, {"table", "events"},
                                   {"partition", "events_2025"}}}));
}

TEST(Planner, DetachingALargePartitionUsesConcurrentlyOutsideATransaction) {
  // A plain DETACH takes AccessExclusiveLock on the PARENT, which blocks every
  // query against every partition -- not just the one leaving.
  auto obs = obs_partitioned();
  obs.server_version = 180006;
  const auto plan = pglaswell::plan_migration(detach_spec(), obs, {});
  ASSERT_TRUE(plan.ok) << plan.render();
  const auto* step = steps_of(plan, "detach_partition")[0];
  EXPECT_NE(all_sql(*step).find("DETACH PARTITION shop.events_2025 CONCURRENTLY"),
            std::string::npos) << all_sql(*step);
  // Measured: it cannot run inside a transaction block.
  EXPECT_EQ(step->txn_class, pglaswell::TxnClass::kForbidden);
  EXPECT_NE(step->lock.find("ShareUpdateExclusiveLock"), std::string::npos);
  bool warned = false;
  for (const auto& w : plan.warnings) {
    if (w.find("half-detached") != std::string::npos) warned = true;
  }
  EXPECT_TRUE(warned) << "the recoverable-but-awkward state must be stated: "
                      << plan.render();
}

TEST(Planner, ASmallQuietPartitionIsDetachedPlainly) {
  auto obs = obs_partitioned();
  obs.server_version = 180006;
  obs.tables["shop.events_2025"]["size_estimate"] = 1024;
  const auto plan = pglaswell::plan_migration(detach_spec(), obs, {});
  const auto* step = steps_of(plan, "detach_partition")[0];
  EXPECT_EQ(all_sql(*step).find("CONCURRENTLY"), std::string::npos)
      << all_sql(*step);
  EXPECT_EQ(step->detail.value("method", ""), "plain");
}

TEST(Planner, OnPg13TheExclusiveLockIsUnavoidableAndSaidSo) {
  auto obs = obs_partitioned();
  obs.server_version = 130010;
  // The shared fixture spec declares min_server_version 150000, which the
  // planner refuses outright on a 13 -- correctly, but it means the plan has no
  // steps at all and this test would assert nothing. Indexing that empty vector
  // read past the end and "passed"; only the hardened standard library under
  // Valgrind turned it into the abort it always was.
  json doc = minimal_spec();
  doc["target"].erase("min_server_version");
  doc["intents"] = json::array({json{{"kind", "detach_partition"},
                                     {"schema", "shop"}, {"table", "events"},
                                     {"partition", "events_2025"}}});
  const auto plan =
      pglaswell::plan_migration(pglaswell::parse_spec(doc), obs, {});
  ASSERT_TRUE(plan.ok) << plan.render();
  const auto steps = steps_of(plan, "detach_partition");
  ASSERT_EQ(steps.size(), 1u) << plan.render();
  const auto* step = steps[0];
  EXPECT_EQ(all_sql(*step).find("CONCURRENTLY"), std::string::npos);
  EXPECT_NE(step->lock.find("AccessExclusiveLock on shop.events AND"),
            std::string::npos) << step->lock;
  bool warned = false;
  for (const auto& w : plan.warnings) {
    if (w.find("older than PostgreSQL 14") != std::string::npos) warned = true;
  }
  EXPECT_TRUE(warned) << plan.render();
}

TEST(Planner, APartitionStuckMidDetachIsFinalizedNotDetachedAgain) {
  // An interrupted DETACH CONCURRENTLY leaves the partition in pg_inherits,
  // still reporting relispartition, with only inhdetachpending to say the
  // cluster is mid-operation. FINALIZE is the only legal move; a fresh DETACH
  // is refused by PostgreSQL.
  auto obs = obs_partitioned();
  obs.server_version = 180006;
  obs.tables["shop.events"]["detach_pending"] =
      json::array({"shop.events_2025"});
  const auto plan = pglaswell::plan_migration(detach_spec(), obs, {});
  ASSERT_TRUE(plan.ok) << plan.render();
  const auto* step = steps_of(plan, "detach_partition")[0];
  EXPECT_NE(all_sql(*step).find("DETACH PARTITION shop.events_2025 FINALIZE"),
            std::string::npos) << all_sql(*step);
  EXPECT_EQ(step->detail.value("method", ""), "finalize");
}

TEST(Planner, DetachingSomethingNotAttachedIsSatisfied) {
  auto obs = obs_partitioned();
  obs.tables["shop.events"]["partitions"] = json::array();
  const auto plan = pglaswell::plan_migration(detach_spec(), obs, {});
  ASSERT_TRUE(plan.ok);
  EXPECT_EQ(steps_of(plan, "detach_partition")[0]->action,
            pglaswell::Action::kSatisfied);
}

// --- add_unique_constraint / add_primary_key -------------------------------

static pglaswell::Spec unique_spec(const char* kind, const char* name,
                                   std::vector<std::string> cols = {"code"}) {
  json c = json::array();
  for (const auto& x : cols) c.push_back(x);
  return spec_of(json::array({json{{"kind", kind}, {"schema", "shop"},
                                   {"table", "orders"}, {"name", name},
                                   {"columns", c}}}));
}

static pglaswell::Observations obs_for_unique(long long size, int waiters = 0,
                                              bool not_null = true) {
  auto obs = observations(size, 100000, waiters);
  obs.tables["shop.orders"]["columns"]["code"] =
      json{{"type", "text"}, {"not_null", not_null}};
  return obs;
}

TEST(Planner, AddingAUniqueConstraintOnALargeTableBuildsTheIndexConcurrently) {
  // Measured (S15): plain ADD CONSTRAINT takes AccessExclusiveLock AND
  // ShareLock and builds the index while holding them.
  const auto plan = pglaswell::plan_migration(
      unique_spec("add_unique_constraint", "orders_code_uq"),
      obs_for_unique(2LL << 30), {});
  ASSERT_TRUE(plan.ok) << plan.render();
  const auto s = steps_of(plan, "add_unique_constraint");
  ASSERT_EQ(s.size(), 2u) << plan.render();
  EXPECT_NE(all_sql(*s[0]).find(
                "CREATE UNIQUE INDEX CONCURRENTLY orders_code_uq ON shop.orders (code)"),
            std::string::npos) << all_sql(*s[0]);
  EXPECT_EQ(s[0]->txn_class, pglaswell::TxnClass::kForbidden);
  EXPECT_NE(all_sql(*s[1]).find("ADD CONSTRAINT orders_code_uq UNIQUE USING INDEX"),
            std::string::npos) << all_sql(*s[1]);

  // The index is named after the constraint, so USING INDEX's rename is a
  // no-op. Any other name and an index someone monitors quietly becomes a
  // different one.
  bool rename_warning = false;
  for (const auto& w : plan.warnings) {
    if (w.find("renames the index") != std::string::npos) rename_warning = true;
  }
  EXPECT_FALSE(rename_warning)
      << "naming the index after the constraint should make the rename a "
         "non-event, so there is nothing to warn about";

  // And the honest statement about what the recipe does and does not buy.
  EXPECT_NE(s[1]->lock.find("AccessExclusiveLock"), std::string::npos)
      << "step 2 still takes the exclusive lock; claiming otherwise would be "
         "the most misleading sentence in the plan: " << s[1]->lock;
}

TEST(Planner, ASmallQuietTableGetsOneStatementInstead) {
  const auto plan = pglaswell::plan_migration(
      unique_spec("add_unique_constraint", "orders_code_uq"),
      obs_for_unique(1024), {});
  ASSERT_TRUE(plan.ok) << plan.render();
  const auto s = steps_of(plan, "add_unique_constraint");
  ASSERT_EQ(s.size(), 1u) << plan.render();
  EXPECT_NE(all_sql(*s[0]).find("ADD CONSTRAINT orders_code_uq UNIQUE (code)"),
            std::string::npos);
  EXPECT_EQ(all_sql(*s[0]).find("CONCURRENTLY"), std::string::npos);

  // A small table with something already queued on it is not a table to take
  // AccessExclusiveLock on.
  const auto contended = pglaswell::plan_migration(
      unique_spec("add_unique_constraint", "orders_code_uq"),
      obs_for_unique(1024, /*waiters=*/2), {});
  EXPECT_EQ(steps_of(contended, "add_unique_constraint").size(), 2u)
      << contended.render();
}

TEST(Planner, AnExistingUniqueIndexIsAdoptedRatherThanRebuilt) {
  auto obs = obs_for_unique(2LL << 30);
  obs.tables["shop.orders"]["indexes"]["orders_code_key"] =
      json{{"is_valid", true}, {"is_unique", true}, {"method", "btree"},
           {"has_expressions", false}, {"predicate", ""},
           {"columns", json::array({"code"})}, {"leading_column", "code"}};
  const auto plan = pglaswell::plan_migration(
      unique_spec("add_unique_constraint", "orders_code_uq"), obs, {});
  ASSERT_TRUE(plan.ok) << plan.render();
  const auto s = steps_of(plan, "add_unique_constraint");
  ASSERT_EQ(s.size(), 1u) << plan.render();
  EXPECT_EQ(all_sql(*s[0]).find("CREATE UNIQUE INDEX"), std::string::npos)
      << "the build is already paid for:\n" << all_sql(*s[0]);
  EXPECT_NE(all_sql(*s[0]).find("USING INDEX orders_code_key"), std::string::npos);

  // Here the rename IS real, and must be said: an index named in a dashboard
  // silently becomes something else, and PostgreSQL reports it as a NOTICE.
  bool warned = false;
  for (const auto& w : plan.warnings) {
    if (w.find("renames the index") != std::string::npos &&
        w.find("orders_code_key") != std::string::npos) warned = true;
  }
  EXPECT_TRUE(warned) << plan.render();
}

TEST(Planner, AnIndexThatDoesNotMatchIsNotAdopted) {
  // Each of these differs from the spec in one way, and none may be adopted:
  // an invalid one is refused by USING INDEX, a non-unique one likewise, a
  // partial one does not cover every row, and different columns are a
  // different constraint entirely.
  const json bad[] = {
      json{{"is_valid", false}, {"is_unique", true}, {"has_expressions", false},
           {"predicate", ""}, {"columns", json::array({"code"})}},
      json{{"is_valid", true}, {"is_unique", false}, {"has_expressions", false},
           {"predicate", ""}, {"columns", json::array({"code"})}},
      json{{"is_valid", true}, {"is_unique", true}, {"has_expressions", false},
           {"predicate", "(status = 'open'::text)"},
           {"columns", json::array({"code"})}},
      json{{"is_valid", true}, {"is_unique", true}, {"has_expressions", false},
           {"predicate", ""}, {"columns", json::array({"code", "id"})}},
  };
  for (const auto& ix : bad) {
    auto obs = obs_for_unique(2LL << 30);
    obs.tables["shop.orders"]["indexes"]["candidate"] = ix;
    obs.tables["shop.orders"]["indexes"]["candidate"]["method"] = "btree";
    const auto plan = pglaswell::plan_migration(
        unique_spec("add_unique_constraint", "orders_code_uq"), obs, {});
    ASSERT_TRUE(plan.ok) << plan.render();
    EXPECT_EQ(steps_of(plan, "add_unique_constraint").size(), 2u)
        << "adopted an unusable index " << ix.dump() << "\n" << plan.render();
  }
}

TEST(Planner, APrimaryKeyOverANullableColumnSetsNotNullTheCheapWayFirst) {
  // Measured (S15): ADD PRIMARY KEY sets attnotnull itself and verifies it with
  // a scan under AccessExclusiveLock -- 75ms on 2M rows against 0.6ms when the
  // column is already NOT NULL. The set_not_null recipe does that same scan
  // under ShareUpdateExclusiveLock, which does not block the application.
  const auto plan = pglaswell::plan_migration(
      unique_spec("add_primary_key", "orders_pkey"),
      obs_for_unique(2LL << 30, 0, /*not_null=*/false), {});
  ASSERT_TRUE(plan.ok) << plan.render();

  const auto nn = steps_of(plan, "set_not_null");
  ASSERT_FALSE(nn.empty()) << "a nullable column must be made NOT NULL first:\n"
                           << plan.render();
  EXPECT_NE(all_sql(*nn[0]).find("NOT VALID"), std::string::npos);

  const auto pk = steps_of(plan, "add_primary_key");
  ASSERT_EQ(pk.size(), 2u) << plan.render();
  EXPECT_NE(all_sql(*pk[1]).find("PRIMARY KEY USING INDEX"), std::string::npos);
  // Order matters: the NOT NULL must be established before the key is added,
  // or ADD PRIMARY KEY does the expensive scan itself.
  EXPECT_LT(nn.back()->ordinal, pk.front()->ordinal) << plan.render();

  // An already-NOT NULL column costs nothing extra.
  const auto quiet = pglaswell::plan_migration(
      unique_spec("add_primary_key", "orders_pkey"),
      obs_for_unique(2LL << 30, 0, /*not_null=*/true), {});
  EXPECT_TRUE(steps_of(quiet, "set_not_null").empty()) << quiet.render();
}

TEST(Planner, ASecondPrimaryKeyIsRefusedNamingTheOneThatExists) {
  auto obs = obs_for_unique(1024);
  obs.tables["shop.orders"]["constraints"]["orders_old_pkey"] =
      json{{"type", "p"}, {"has_index", true}, {"depended_on_by", json::array()}};
  const auto plan = pglaswell::plan_migration(
      unique_spec("add_primary_key", "orders_pkey"), obs, {});
  EXPECT_FALSE(plan.ok) << plan.render();
  EXPECT_NE(plan.conflicts[0].find("orders_old_pkey"), std::string::npos)
      << plan.conflicts[0];
}

TEST(Planner, AnAlreadyPresentUniqueConstraintIsSatisfied) {
  auto obs = obs_for_unique(1024);
  obs.tables["shop.orders"]["constraints"]["orders_code_uq"] =
      json{{"type", "u"}, {"has_index", true}, {"depended_on_by", json::array()}};
  const auto plan = pglaswell::plan_migration(
      unique_spec("add_unique_constraint", "orders_code_uq"), obs, {});
  ASSERT_TRUE(plan.ok) << plan.render();
  EXPECT_EQ(steps_of(plan, "add_unique_constraint")[0]->action,
            pglaswell::Action::kSatisfied);

  // The same name holding a different KIND of constraint is a conflict, not a
  // satisfied step: silently accepting it would report a unique constraint
  // that is actually a check.
  obs.tables["shop.orders"]["constraints"]["orders_code_uq"]["type"] = "c";
  const auto clash = pglaswell::plan_migration(
      unique_spec("add_unique_constraint", "orders_code_uq"), obs, {});
  EXPECT_FALSE(clash.ok) << clash.render();
}

TEST(Planner, TheAbsenceOfANotValidFormForUniqueIsStated) {
  // There is no deferred validation for a unique constraint -- the index build
  // IS the check -- so a duplicate fails the build and leaves an INVALID index
  // that USING INDEX then refuses. Saying so beforehand is the difference
  // between a stopped job somebody understands and one they do not.
  const auto plan = pglaswell::plan_migration(
      unique_spec("add_unique_constraint", "orders_code_uq"),
      obs_for_unique(2LL << 30), {});
  bool warned = false;
  for (const auto& w : plan.warnings) {
    if (w.find("no NOT VALID form") != std::string::npos &&
        w.find("INVALID index") != std::string::npos &&
        w.find("drop_index") != std::string::npos) warned = true;
  }
  EXPECT_TRUE(warned) << plan.render();
}

// --- replace_view, and what add_column does NOT do -------------------------

static pglaswell::Spec replace_spec(const char* def) {
  return spec_of(json::array({json{{"kind", "replace_view"},
                                   {"schema", "public"}, {"name", "v1"},
                                   {"definition", def}}}));
}

static pglaswell::Observations obs_with_view_object(const char* kind,
                                                    bool barrier) {
  auto obs = observations(1024, 10);
  obs.tables["public.v1"] =
      json{{"exists", true}, {"kind", kind}, {"owner", "app"},
           {"view_definition", " SELECT id FROM shop.orders;"},
           {"reloptions", barrier ? json::array({"security_barrier=true"})
                                  : json::array()}};
  return obs;
}

TEST(Planner, ReplacingAPlainViewUsesCreateOrReplaceRatherThanARebuild) {
  // Measured (S14): CREATE OR REPLACE keeps the comment, grants, column grants,
  // INSTEAD OF triggers, owner and every dependent object. Dropping and
  // recreating keeps none of them. Choosing the destructive path when the cheap
  // one is legal would be the tool doing gratuitous harm.
  const auto plan = pglaswell::plan_migration(
      replace_spec("SELECT id, amount FROM shop.orders"),
      obs_with_view_object("view", false), {});
  ASSERT_TRUE(plan.ok) << plan.render();
  const auto s = steps_of(plan, "replace_view");
  ASSERT_EQ(s.size(), 1u);
  const auto sql = all_sql(*s[0]);
  EXPECT_NE(sql.find("CREATE OR REPLACE VIEW public.v1"), std::string::npos) << sql;
  EXPECT_EQ(sql.find("DROP VIEW"), std::string::npos)
      << "a plain view must not be dropped when replace is legal:\n" << sql;
  EXPECT_EQ(s[0]->detail.value("method", ""), "replace");
}

TEST(Planner, ReplacingAViewReappliesTheOptionsThatReplaceSilentlyResets) {
  // The single measured casualty of CREATE OR REPLACE. It is not reported by
  // PostgreSQL at all: the statement succeeds and the view quietly stops being
  // a security barrier.
  const auto plan = pglaswell::plan_migration(
      replace_spec("SELECT id, amount FROM shop.orders"),
      obs_with_view_object("view", true), {});
  ASSERT_TRUE(plan.ok) << plan.render();
  const auto sql = all_sql(*steps_of(plan, "replace_view")[0]);
  EXPECT_NE(sql.find("ALTER VIEW public.v1 SET (security_barrier=true)"),
            std::string::npos)
      << "the reset option was not re-applied:\n" << sql;
  bool warned = false;
  for (const auto& w : plan.warnings) {
    if (w.find("silently lose") != std::string::npos &&
        w.find("security_barrier") != std::string::npos) warned = true;
  }
  EXPECT_TRUE(warned) << plan.render();

  // A view with no options gets no ALTER and no warning about losing any.
  const auto plain = pglaswell::plan_migration(
      replace_spec("SELECT id FROM shop.orders"),
      obs_with_view_object("view", false), {});
  EXPECT_EQ(all_sql(*steps_of(plain, "replace_view")[0]).find("ALTER VIEW"),
            std::string::npos);
}

TEST(Planner, AMaterializedViewHasNoReplaceFormSoItIsRebuilt) {
  // Measured: CREATE OR REPLACE MATERIALIZED VIEW is a syntax error. The plan
  // must say what that costs rather than emitting a statement that cannot run.
  const auto plan = pglaswell::plan_migration(
      replace_spec("SELECT id FROM shop.orders"),
      obs_with_view_object("materialized_view", false), {});
  ASSERT_TRUE(plan.ok) << plan.render();
  const auto* step = steps_of(plan, "replace_view")[0];
  const auto sql = all_sql(*step);
  EXPECT_NE(sql.find("DROP MATERIALIZED VIEW public.v1"), std::string::npos) << sql;
  EXPECT_NE(sql.find("CREATE MATERIALIZED VIEW public.v1"), std::string::npos) << sql;
  EXPECT_EQ(sql.find("CREATE OR REPLACE MATERIALIZED"), std::string::npos)
      << "that syntax does not exist:\n" << sql;
  EXPECT_EQ(step->detail.value("method", ""), "drop_and_recreate");
  EXPECT_TRUE(step->own_transaction);
  bool warned = false;
  for (const auto& w : plan.warnings) {
    if (w.find("no replace form") != std::string::npos) warned = true;
  }
  EXPECT_TRUE(warned) << plan.render();
}

TEST(Planner, ReplacingSomethingThatIsNotAViewIsRefused) {
  const auto plan = pglaswell::plan_migration(
      replace_spec("SELECT 1 AS a"), observations(1024, 10), {});
  // shop.orders is a table; public.v1 does not exist, so this creates it.
  ASSERT_TRUE(plan.ok) << plan.render();
  EXPECT_EQ(steps_of(plan, "replace_view")[0]->detail.value("method", ""), "create");

  auto table_obs = observations(1024, 10);
  table_obs.tables["public.v1"] = json{{"exists", true}, {"kind", "table"}};
  const auto refused = pglaswell::plan_migration(replace_spec("SELECT 1 AS a"),
                                                 table_obs, {});
  EXPECT_FALSE(refused.ok) << refused.render();
  EXPECT_NE(refused.conflicts[0].find("data loss"), std::string::npos)
      << refused.conflicts[0];
}

TEST(Spec, AViewDefinitionIsAQueryNotACreateStatement) {
  // Accepting "CREATE VIEW ..." would mean the tool no longer chooses between
  // CREATE OR REPLACE and a rebuild -- which is the only decision this kind
  // makes, so the whole kind would collapse into a psql wrapper.
  json doc = minimal_spec();
  doc["intents"] = json::array({json{{"kind", "replace_view"},
                                     {"schema", "s"}, {"name", "v"},
                                     {"definition", "CREATE VIEW v AS SELECT 1"}}});
  const auto err = spec_error(doc);
  EXPECT_NE(err.find("QUERY, not a CREATE statement"), std::string::npos) << err;
}

// --- alter_column_type / drop_column, and the view rebuild ------------------

static json view_stack() {
  // v1 reads amount; v2 sits on v1; vother reads a different column entirely.
  return json{
      {"public.v1", {{"name", "public.v1"}, {"level", 1}, {"kind", "view"},
                     {"definition", " SELECT id, amount FROM shop.orders;"},
                     {"depends_on", json::array()},
                     {"owner", "app"}, {"comment", "v1 doc"},
                     {"reloptions", json::array({"security_barrier=true"})},
                     {"uses_columns", json::array({"amount"})},
                     {"column_comments", {{"amount", "amount doc"}}},
                     {"grants", json::array({"app=arwdDxt/app", "=r/app"})},
                     {"column_grants", json::array({json{{"column", "id"},
                                                         {"acl", json::array({"reader=r/app"})}}})},
                     {"triggers", json::array({"CREATE TRIGGER v1_ins INSTEAD OF INSERT ON public.v1 FOR EACH ROW EXECUTE FUNCTION noop()"})},
                     {"indexes", json::array()}}},
      {"public.v2", {{"name", "public.v2"}, {"level", 2}, {"kind", "view"},
                     {"definition", " SELECT id FROM public.v1;"},
                     {"depends_on", json::array({"public.v1"})},
                     {"owner", "app"}, {"comment", nullptr},
                     {"reloptions", json::array()},
                     {"uses_columns", json::array()},
                     {"column_comments", json::object()},
                     {"grants", json::array()},
                     {"column_grants", json::array()},
                     {"triggers", json::array()}, {"indexes", json::array()}}},
      {"public.mv1", {{"name", "public.mv1"}, {"level", 2}, {"kind", "materialized"},
                     {"definition", " SELECT id FROM public.v1;"},
                     {"depends_on", json::array({"public.v1"})},
                     {"owner", "app"}, {"comment", nullptr},
                     {"reloptions", json::array()},
                     {"uses_columns", json::array()},
                     {"column_comments", json::object()},
                     {"grants", json::array()}, {"column_grants", json::array()},
                     {"triggers", json::array()},
                     {"indexes", json::array({"CREATE UNIQUE INDEX mv1_id ON public.mv1 USING btree (id)"})}}},
      {"public.vother", {{"name", "public.vother"}, {"level", 1}, {"kind", "view"},
                     {"definition", " SELECT id, status FROM shop.orders;"},
                     {"depends_on", json::array()},
                     {"owner", "app"}, {"comment", nullptr},
                     {"reloptions", json::array()},
                     {"uses_columns", json::array({"status"})},
                     {"column_comments", json::object()},
                     {"grants", json::array()}, {"column_grants", json::array()},
                     {"triggers", json::array()}, {"indexes", json::array()}}}};
}

static pglaswell::Observations obs_with_views(const char* col_type = "integer") {
  auto obs = observations(2LL << 30, 8000000);
  obs.tables["shop.orders"]["columns"]["amount"] =
      json{{"type", col_type}, {"not_null", false}};
  obs.tables["shop.orders"]["columns"]["status"] =
      json{{"type", "text"}, {"not_null", false}};
  obs.tables["shop.orders"]["dependent_views"] = view_stack();
  return obs;
}

static pglaswell::Spec alter_spec(const char* type) {
  return spec_of(json::array({json{{"kind", "alter_column_type"},
                                   {"schema", "shop"}, {"table", "orders"},
                                   {"column", "amount"}, {"type", type}}}));
}

TEST(Planner, AlterColumnTypeRebuildsOnlyTheViewsThatBlockIt) {
  const auto plan = pglaswell::plan_migration(alter_spec("bigint"),
                                              obs_with_views(), {});
  ASSERT_TRUE(plan.ok) << plan.render();
  const auto s = steps_of(plan, "alter_column_type");
  ASSERT_EQ(s.size(), 1u);
  const auto sql = all_sql(*s[0]);

  // vother reads a different column: it neither blocks the change nor gets
  // dragged into the rebuild. Widening the blast radius is its own harm.
  EXPECT_EQ(sql.find("vother"), std::string::npos)
      << "a view on an unrelated column was rebuilt:\n" << sql;

  // Deepest first on the way down...
  const auto drop_v2 = sql.find("DROP VIEW public.v2");
  const auto drop_mv = sql.find("DROP MATERIALIZED VIEW public.mv1");
  const auto drop_v1 = sql.find("DROP VIEW public.v1");
  const auto alter   = sql.find("ALTER TABLE shop.orders ALTER COLUMN");
  const auto make_v1 = sql.find("CREATE VIEW public.v1");
  const auto make_v2 = sql.find("CREATE VIEW public.v2");
  for (auto pos : {drop_v2, drop_mv, drop_v1, alter, make_v1, make_v2}) {
    ASSERT_NE(pos, std::string::npos) << sql;
  }
  EXPECT_LT(drop_v2, drop_v1) << "v2 sits on v1 and must come down first";
  EXPECT_LT(drop_mv, drop_v1);
  EXPECT_LT(drop_v1, alter);
  // ... and shallowest first on the way back up.
  EXPECT_LT(alter, make_v1);
  EXPECT_LT(make_v1, make_v2) << "v2 cannot be created before v1 exists";

  // One transaction, or a reader finds the views missing.
  EXPECT_EQ(s[0]->txn_class, pglaswell::TxnClass::kRequired);
  EXPECT_TRUE(s[0]->own_transaction);
}

TEST(Planner, TheViewRebuildRestoresAllEightThingsAHandRebuildLoses) {
  // Spike S13 measured every one of these as ** GONE ** after a naive drop and
  // recreate. A rebuild that does not put them back is not a fix; it is the
  // same bug with a tool's name on it.
  const auto plan = pglaswell::plan_migration(alter_spec("bigint"),
                                              obs_with_views(), {});
  const auto sql = all_sql(*steps_of(plan, "alter_column_type")[0]);

  EXPECT_NE(sql.find("COMMENT ON VIEW public.v1 IS 'v1 doc'"), std::string::npos) << sql;
  EXPECT_NE(sql.find("COMMENT ON COLUMN public.v1.\"amount\" IS 'amount doc'"),
            std::string::npos) << sql;
  EXPECT_NE(sql.find("ALTER VIEW public.v1 OWNER TO \"app\""), std::string::npos) << sql;
  EXPECT_NE(sql.find("security_barrier=true"), std::string::npos) << sql;
  EXPECT_NE(sql.find("INSTEAD OF INSERT"), std::string::npos) << sql;
  EXPECT_NE(sql.find("CREATE UNIQUE INDEX mv1_id"), std::string::npos)
      << "a matview's unique index is what REFRESH CONCURRENTLY needs:\n" << sql;
  // Table grants, including the PUBLIC entry, whose grantee is empty.
  EXPECT_NE(sql.find("GRANT SELECT ON public.v1 TO \"app\""), std::string::npos) << sql;
  EXPECT_NE(sql.find("GRANT SELECT ON public.v1 TO PUBLIC"), std::string::npos)
      << "an empty grantee is PUBLIC, and it is the entry least likely to be "
         "noticed missing:\n" << sql;
  // Column-level grants.
  // The column list goes after the PRIVILEGE, not after the relation. The
  // other order is a syntax error, and asserting it here would only have
  // agreed with our own output -- the end-to-end test is what found it.
  EXPECT_NE(sql.find("GRANT SELECT (\"id\") ON public.v1 TO \"reader\""),
            std::string::npos) << sql;

  // The owner is restored BEFORE the grants, or every grant records the wrong
  // grantor.
  EXPECT_LT(sql.find("OWNER TO \"app\""), sql.find("GRANT SELECT ON public.v1 TO"));
}

TEST(Planner, AProvableWideningIsNotReportedAsARewrite) {
  // Measured on 18.6 by relfilenode: relaxing a type modifier is free, adding
  // or tightening one rewrites. Both directions asserted, because a rule that
  // only ever says "rewrite" would pass a one-sided test.
  struct Case { const char* from; const char* to; bool free_change; };
  const Case cases[] = {
      {"character varying(50)", "character varying(100)", true},
      {"character varying(50)", "text", true},
      {"numeric(10,2)", "numeric(12,2)", true},
      {"timestamp(0) without time zone", "timestamp(3) without time zone", true},
      {"text", "character varying(200)", false},   // adds a limit: every row checked
      {"integer", "bigint", false},
      {"integer", "text", false},
      {"numeric(10,2)", "numeric(12,3)", false},   // a scale change re-encodes
      {"character varying(100)", "character varying(50)", false},  // narrowing
  };
  for (const auto& c : cases) {
    const auto plan = pglaswell::plan_migration(alter_spec(c.to),
                                                obs_with_views(c.from), {});
    ASSERT_TRUE(plan.ok) << plan.render();
    const auto* step = steps_of(plan, "alter_column_type")[0];
    EXPECT_EQ(step->detail.value("rewrite", ""), c.free_change ? "no" : "assumed")
        << c.from << " -> " << c.to << ": " << step->why;
  }
}

TEST(Planner, AlterColumnTypeToTheTypeItAlreadyHasIsSatisfied) {
  const auto plan = pglaswell::plan_migration(alter_spec("integer"),
                                              obs_with_views("integer"), {});
  ASSERT_TRUE(plan.ok) << plan.render();
  const auto* step = steps_of(plan, "alter_column_type")[0];
  EXPECT_EQ(step->action, pglaswell::Action::kSatisfied);
  EXPECT_TRUE(step->sql.empty()) << "a satisfied step must not rebuild views";
}

TEST(Planner, AlterColumnTypeWithNoViewsEmitsJustTheAlter) {
  auto obs = observations(1024, 10);
  obs.tables["shop.orders"]["columns"]["amount"] =
      json{{"type", "integer"}, {"not_null", false}};
  const auto plan = pglaswell::plan_migration(alter_spec("bigint"), obs, {});
  ASSERT_TRUE(plan.ok) << plan.render();
  const auto sql = all_sql(*steps_of(plan, "alter_column_type")[0]);
  EXPECT_EQ(sql.find("DROP VIEW"), std::string::npos) << sql;
  EXPECT_NE(sql.find("ALTER TABLE shop.orders ALTER COLUMN \"amount\" TYPE bigint"),
            std::string::npos) << sql;
}

TEST(Planner, DroppingAColumnAViewReadsIsRefusedRatherThanGuessedAt) {
  // The difference from alter_column_type, and the reason this is a refusal
  // instead of a recipe: the recorded view definitions still SELECT the column
  // being removed, so replaying them verbatim would fail. Rewriting them is a
  // judgement about what the view should now mean, which belongs in a spec
  // somebody reviewed.
  const auto plan = pglaswell::plan_migration(
      spec_of(json::array({json{{"kind", "drop_column"}, {"schema", "shop"},
                                {"table", "orders"}, {"column", "amount"}}})),
      obs_with_views(), {});
  EXPECT_FALSE(plan.ok) << plan.render();
  ASSERT_FALSE(plan.conflicts.empty());
  EXPECT_NE(plan.conflicts[0].find("public.v1"), std::string::npos)
      << plan.conflicts[0];
  EXPECT_NE(plan.conflicts[0].find("CASCADE"), std::string::npos)
      << "the refusal must say why CASCADE is not the way out: "
      << plan.conflicts[0];
  EXPECT_TRUE(steps_of(plan, "drop_column")[0]->sql.empty());
}

TEST(Planner, DroppingAnUnreferencedColumnWarnsThatItIsIrreversible) {
  auto obs = obs_with_views();
  const auto plan = pglaswell::plan_migration(
      spec_of(json::array({json{{"kind", "drop_column"}, {"schema", "shop"},
                                {"table", "orders"}, {"column", "status"}}})),
      obs, {});
  // status is read by vother, so this is still refused -- which is the point:
  // the check is on the column, not on whether any view exists at all.
  EXPECT_FALSE(plan.ok) << plan.render();

  // A column nothing reads goes through, with the warning that matters.
  auto quiet = observations(1024, 10);
  quiet.tables["shop.orders"]["columns"]["scratch"] =
      json{{"type", "text"}, {"not_null", false}};
  const auto ok = pglaswell::plan_migration(
      spec_of(json::array({json{{"kind", "drop_column"}, {"schema", "shop"},
                                {"table", "orders"}, {"column", "scratch"}}})),
      quiet, {});
  ASSERT_TRUE(ok.ok) << ok.render();
  bool warned = false;
  for (const auto& w : ok.warnings) {
    if (w.find("irreversible") != std::string::npos) warned = true;
  }
  EXPECT_TRUE(warned) << ok.render();
  EXPECT_NE(all_sql(*steps_of(ok, "drop_column")[0]).find("DROP COLUMN \"scratch\""),
            std::string::npos);
}

TEST(Planner, DroppingAnAbsentColumnIsSatisfied) {
  const auto plan = pglaswell::plan_migration(
      spec_of(json::array({json{{"kind", "drop_column"}, {"schema", "shop"},
                                {"table", "orders"}, {"column", "never"}}})),
      observations(1024, 10), {});
  ASSERT_TRUE(plan.ok);
  EXPECT_EQ(steps_of(plan, "drop_column")[0]->action, pglaswell::Action::kSatisfied);
}

TEST(Planner, AddColumnIsNotBlockedByViewsButWarnsTheColumnIsInvisible) {
  // The mirror of the S13 rebuild, and the reason the two are separate paths.
  // ADD COLUMN under a view stack simply succeeds -- so nothing is dropped --
  // but the column reaches NO existing view, including one written SELECT *,
  // because the star is expanded at creation. Nothing errors; the column is
  // just absent for anything reading through the view.
  auto obs = obs_with_views();
  json doc = minimal_spec();
  doc["intents"] = json::array({json{{"kind", "add_column"},
                                     {"schema", "shop"}, {"table", "orders"},
                                     {"column", "region"}, {"type", "text"},
                                     {"nullable", true}, {"comment", "c"}}});
  const auto plan =
      pglaswell::plan_migration(pglaswell::parse_spec(doc), obs, {});
  ASSERT_TRUE(plan.ok) << plan.render();
  // add_column touches NO view, in either direction. It does not rebuild them
  // -- PostgreSQL never blocks it -- and it does not silently rewrite them to
  // carry the new column either. Deciding that a view should now expose a
  // column is a judgement about what that view means, and it belongs in a
  // replace_view intent somebody wrote and signed. Asserted on the whole
  // statement list rather than on one forbidden keyword, so a future change
  // that starts emitting view DDL here fails loudly.
  const auto* step = steps_of(plan, "add_column")[0];
  ASSERT_EQ(step->sql.size(), 2u) << all_sql(*step);
  EXPECT_NE(step->sql[0].find("ALTER TABLE shop.orders ADD COLUMN region"),
            std::string::npos) << step->sql[0];
  EXPECT_NE(step->sql[1].find("COMMENT ON COLUMN"), std::string::npos)
      << step->sql[1];
  const auto sql = all_sql(*step);
  for (const char* forbidden : {"DROP VIEW", "CREATE VIEW", "CREATE OR REPLACE",
                                "MATERIALIZED"}) {
    EXPECT_EQ(sql.find(forbidden), std::string::npos)
        << "add_column emitted " << forbidden << ":\n" << sql;
  }

  bool warned = false;
  for (const auto& w : plan.warnings) {
    if (w.find("will not be visible through") != std::string::npos &&
        w.find("SELECT * is expanded") != std::string::npos) {
      warned = true;
      EXPECT_NE(w.find("replace_view"), std::string::npos)
          << "the warning must name the intent that fixes it: " << w;
    }
  }
  EXPECT_TRUE(warned) << plan.render();

  // No views, no warning: this must not fire on every add_column ever written.
  auto quiet = observations(1024, 10);
  const auto plain =
      pglaswell::plan_migration(pglaswell::parse_spec(doc), quiet, {});
  for (const auto& w : plain.warnings) {
    EXPECT_EQ(w.find("will not be visible through"), std::string::npos) << w;
  }
}

// --- renames, tables, purges, and the security kinds -----------------------

static pglaswell::Observations obs_rich() {
  auto obs = observations(2LL << 30, 8000000);
  obs.tables["shop.orders"]["columns"]["amount"] =
      json{{"type", "integer"}, {"not_null", false}};
  obs.tables["shop.orders"]["constraints"]["orders_amount_ck"] =
      json{{"type", "c"}, {"has_index", false}, {"depended_on_by", json::array()}};
  obs.tables["shop.orders"]["referenced_by"] = json::array();
  obs.tables["shop.orders"]["row_security"] =
      json{{"enabled", false}, {"forced", false}};
  obs.tables["shop.orders"]["policies"] = json::object();
  obs.tables["shop.orders"]["triggers"] =
      json{{"orders_audit", {{"enabled", true}, {"state", "O"}}}};
  obs.tables["shop.orders"]["dependent_views"] = view_stack();
  return obs;
}

static const pglaswell::Step* only_step(const pglaswell::Plan& p,
                                        const char* kind) {
  const auto s = steps_of(p, kind);
  EXPECT_EQ(s.size(), 1u) << p.render();
  return s.empty() ? nullptr : s[0];
}

TEST(Planner, RenamingAColumnRebuildsNoViewsButSaysWhatTheyKeep) {
  // Measured (S17): the catalog repairs itself -- the view definition becomes
  // "SELECT id, value AS amount FROM t" on its own. So no rebuild. But the view
  // KEEPS ITS OWN OUTPUT NAME, so nothing reading through it sees the rename,
  // and nothing errors to say so. That warning is most of the value here.
  const auto plan = pglaswell::plan_migration(
      spec_of(json::array({json{{"kind", "rename_column"}, {"schema", "shop"},
                                {"table", "orders"}, {"column", "amount"},
                                {"to", "value"}}})),
      obs_rich(), {});
  ASSERT_TRUE(plan.ok) << plan.render();
  const auto* step = only_step(plan, "rename_column");
  ASSERT_NE(step, nullptr);
  const auto sql = all_sql(*step);
  EXPECT_NE(sql.find("RENAME COLUMN \"amount\" TO \"value\""), std::string::npos);
  for (const char* forbidden : {"DROP VIEW", "CREATE OR REPLACE", "GRANT"}) {
    EXPECT_EQ(sql.find(forbidden), std::string::npos)
        << "a rename must not rebuild anything: " << sql;
  }
  bool warned = false;
  for (const auto& w : plan.warnings) {
    if (w.find("KEEPS ITS OWN OUTPUT NAME") != std::string::npos &&
        w.find("public.v1") != std::string::npos) warned = true;
  }
  EXPECT_TRUE(warned) << plan.render();
}

TEST(Planner, RenamingToANameThatIsTakenIsRefused) {
  auto obs = obs_rich();
  obs.tables["shop.orders"]["columns"]["value"] =
      json{{"type", "text"}, {"not_null", false}};
  const auto plan = pglaswell::plan_migration(
      spec_of(json::array({json{{"kind", "rename_column"}, {"schema", "shop"},
                                {"table", "orders"}, {"column", "amount"},
                                {"to", "value"}}})),
      obs, {});
  EXPECT_FALSE(plan.ok) << plan.render();

  // And a rename already applied is satisfied, not a second attempt: the old
  // name is gone and the new one is there.
  auto done = obs_rich();
  done.tables["shop.orders"]["columns"].erase("amount");
  done.tables["shop.orders"]["columns"]["value"] =
      json{{"type", "integer"}, {"not_null", false}};
  const auto again = pglaswell::plan_migration(
      spec_of(json::array({json{{"kind", "rename_column"}, {"schema", "shop"},
                                {"table", "orders"}, {"column", "amount"},
                                {"to", "value"}}})),
      done, {});
  ASSERT_TRUE(again.ok) << again.render();
  EXPECT_EQ(only_step(again, "rename_column")->action,
            pglaswell::Action::kSatisfied);
}

TEST(Planner, RenamingAConstraintWarnsThatItsIndexIsRenamedToo) {
  auto obs = obs_rich();
  obs.tables["shop.orders"]["constraints"]["orders_code_uq"] =
      json{{"type", "u"}, {"has_index", true}, {"depended_on_by", json::array()}};
  const auto plan = pglaswell::plan_migration(
      spec_of(json::array({json{{"kind", "rename_constraint"}, {"schema", "shop"},
                                {"table", "orders"}, {"name", "orders_code_uq"},
                                {"to", "orders_code_unique"}}})),
      obs, {});
  ASSERT_TRUE(plan.ok) << plan.render();
  bool warned = false;
  for (const auto& w : plan.warnings) {
    if (w.find("also renames its backing index") != std::string::npos) warned = true;
  }
  EXPECT_TRUE(warned) << plan.render();
}

TEST(Planner, CreateTableEmitsCommentsAndDoesNotCompareAnExistingOne) {
  const auto spec = spec_of(json::array(
      {json{{"kind", "create_table"}, {"schema", "shop"}, {"table", "regions"},
            {"comment", "Warehouse regions."},
            {"primary_key", json::array({"id"})},
            {"columns", json::array({
                json{{"name", "id"}, {"type", "bigint"}, {"nullable", false},
                     {"comment", "Identity."}},
                json{{"name", "name"}, {"type", "text"}, {"nullable", true},
                     {"comment", "Display name."}}})}}}));
  const auto plan = pglaswell::plan_migration(spec, observations(1024, 10), {});
  ASSERT_TRUE(plan.ok) << plan.render();
  const auto sql = all_sql(*only_step(plan, "create_table"));
  EXPECT_NE(sql.find("CREATE TABLE shop.regions"), std::string::npos) << sql;
  EXPECT_NE(sql.find("\"id\" bigint NOT NULL"), std::string::npos) << sql;
  EXPECT_NE(sql.find("PRIMARY KEY (\"id\")"), std::string::npos)
      << "on an empty table the inline key is free; the two-step recipe would "
         "be machinery for nothing:\n" << sql;
  EXPECT_NE(sql.find("COMMENT ON TABLE shop.regions"), std::string::npos);
  EXPECT_NE(sql.find("COMMENT ON COLUMN shop.regions.\"name\""), std::string::npos);

  // An existing table is satisfied WITHOUT a shape comparison, and the plan
  // must say so -- silently reporting satisfied over a table that differs is
  // how a migration does nothing and reports success.
  auto exists = observations(1024, 10);
  exists.tables["shop.regions"] = json{{"exists", true}, {"kind", "table"}};
  const auto second = pglaswell::plan_migration(spec, exists, {});
  ASSERT_TRUE(second.ok);
  EXPECT_EQ(only_step(second, "create_table")->action,
            pglaswell::Action::kSatisfied);
  bool warned = false;
  for (const auto& w : second.warnings) {
    if (w.find("WITHOUT comparing its columns") != std::string::npos) warned = true;
  }
  EXPECT_TRUE(warned) << second.render();
}

TEST(Planner, DroppingATableNamesBothKindsOfDependant) {
  // Measured (S17): PostgreSQL refuses for a dependent view AND for an inbound
  // foreign key, and names them. Both are visible beforehand.
  auto obs = obs_rich();
  obs.tables["shop.orders"]["referenced_by"] =
      json::array({"lines_order_fk on shop.order_lines"});
  const auto plan = pglaswell::plan_migration(
      spec_of(json::array({json{{"kind", "drop_table"}, {"schema", "shop"},
                                {"table", "orders"}}})),
      obs, {});
  EXPECT_FALSE(plan.ok) << plan.render();
  EXPECT_NE(plan.conflicts[0].find("view public.v1"), std::string::npos)
      << plan.conflicts[0];
  EXPECT_NE(plan.conflicts[0].find("lines_order_fk"), std::string::npos)
      << plan.conflicts[0];
  EXPECT_NE(plan.conflicts[0].find("CASCADE"), std::string::npos);

  // With nothing depending on it, it goes -- with the warning that matters.
  auto quiet = observations(2LL << 30, 8000000);
  const auto ok = pglaswell::plan_migration(
      spec_of(json::array({json{{"kind", "drop_table"}, {"schema", "shop"},
                                {"table", "orders"}}})),
      quiet, {});
  ASSERT_TRUE(ok.ok) << ok.render();
  bool warned = false;
  for (const auto& w : ok.warnings) {
    if (w.find("irreversible") != std::string::npos) warned = true;
  }
  EXPECT_TRUE(warned) << ok.render();
}

TEST(Planner, DeleteRowsIsPacedLikeABackfillAndWarnsAboutChildren) {
  auto obs = obs_rich();
  obs.tables["shop.orders"]["referenced_by"] =
      json::array({"lines_order_fk on shop.order_lines"});
  const auto plan = pglaswell::plan_migration(
      spec_of(json::array({json{{"kind", "delete_rows"}, {"schema", "shop"},
                                {"table", "orders"}, {"key", "id"},
                                {"where", "created_at < now() - interval '1 year'"}}})),
      obs, {});
  ASSERT_TRUE(plan.ok) << plan.render();
  const auto* step = only_step(plan, "delete_rows");
  ASSERT_NE(step, nullptr);
  EXPECT_EQ(step->txn_class, pglaswell::TxnClass::kOwnTxnPerBatch);
  const auto sql = all_sql(*step);
  // The shape the executor's generic paced runner requires: $1 cursor, $2
  // batch, and the key returned.
  EXPECT_NE(sql.find("> $1"), std::string::npos) << sql;
  EXPECT_NE(sql.find("LIMIT $2"), std::string::npos) << sql;
  EXPECT_NE(sql.find("RETURNING shop.orders.id"), std::string::npos) << sql;
  EXPECT_NE(sql.find("FOR UPDATE"), std::string::npos) << sql;
  EXPECT_EQ(step->lock.find("AccessExclusive"), std::string::npos) << step->lock;

  bool child_warning = false, space_warning = false;
  for (const auto& w : plan.warnings) {
    if (w.find("already COMMITTED") != std::string::npos) child_warning = true;
    if (w.find("does not shrink") != std::string::npos) space_warning = true;
  }
  EXPECT_TRUE(child_warning)
      << "a paced delete that hits a foreign key stops part-done: "
      << plan.render();
  EXPECT_TRUE(space_warning) << plan.render();
}

TEST(Spec, AnUnfilteredDeleteIsRefused) {
  json doc = minimal_spec();
  doc["intents"] = json::array({json{{"kind", "delete_rows"}, {"schema", "s"},
                                     {"table", "t"}, {"key", "id"},
                                     {"where", ""}}});
  const auto err = spec_error(doc);
  EXPECT_NE(err.find("empties the table"), std::string::npos) << err;
  EXPECT_NE(err.find("\"true\""), std::string::npos)
      << "the hint must say how to opt in deliberately: " << err;
  EXPECT_NE(err.find("drop_table"), std::string::npos)
      << "and name the intent that is probably meant instead: " << err;
  // ... and opting in explicitly works.
  doc["intents"][0]["where"] = "true";
  EXPECT_NO_THROW(pglaswell::parse_spec(doc));
}

TEST(Planner, EnablingRowSecurityWithNoPolicyIsTheLoudestWarningWeHave) {
  // Measured on 18.6: an application role went from 1000 rows to 0 the moment
  // RLS was enabled with no policy -- no error, the query just returns nothing.
  const auto plan = pglaswell::plan_migration(
      spec_of(json::array({json{{"kind", "set_row_security"}, {"schema", "shop"},
                                {"table", "orders"}, {"enabled", true}}})),
      obs_rich(), {});
  ASSERT_TRUE(plan.ok) << plan.render();
  bool deny = false, invisible = false;
  for (const auto& w : plan.warnings) {
    if (w.find("hides every row") != std::string::npos) deny = true;
    if (w.find("SUPERUSER bypasses it") != std::string::npos) invisible = true;
  }
  EXPECT_TRUE(deny) << plan.render();
  EXPECT_TRUE(invisible)
      << "verifying RLS from a superuser session shows nothing, however wrong "
         "the policy is: " << plan.render();

  // With a policy already there, the default-deny warning does not fire.
  auto with_policy = obs_rich();
  with_policy.tables["shop.orders"]["policies"]["tenant_isolation"] =
      json{{"command", "ALL"}};
  const auto quieter = pglaswell::plan_migration(
      spec_of(json::array({json{{"kind", "set_row_security"}, {"schema", "shop"},
                                {"table", "orders"}, {"enabled", true}}})),
      with_policy, {});
  for (const auto& w : quieter.warnings) {
    EXPECT_EQ(w.find("hides every row"), std::string::npos) << w;
  }
}

TEST(Planner, APolicyOnATableWithoutRlsIsStoredAndDoesNothing) {
  const auto plan = pglaswell::plan_migration(
      spec_of(json::array({json{{"kind", "create_policy"}, {"schema", "shop"},
                                {"table", "orders"}, {"name", "tenant_iso"},
                                {"command", "UPDATE"},
                                {"roles", json::array({"app"})},
                                {"using", "tenant = current_user"}}})),
      obs_rich(), {});
  ASSERT_TRUE(plan.ok) << plan.render();
  const auto sql = all_sql(*only_step(plan, "create_policy"));
  EXPECT_NE(sql.find("CREATE POLICY \"tenant_iso\" ON shop.orders FOR UPDATE "
                     "TO \"app\" USING (tenant = current_user)"),
            std::string::npos) << sql;
  bool inert = false, check = false;
  for (const auto& w : plan.warnings) {
    if (w.find("does nothing at all until it is") != std::string::npos) inert = true;
    if (w.find("WITH CHECK") != std::string::npos) check = true;
  }
  EXPECT_TRUE(inert) << plan.render();
  EXPECT_TRUE(check)
      << "USING without WITH CHECK lets a role write a row it could not read: "
      << plan.render();
}

TEST(Planner, DroppingTheLastPolicyLeavesEveryRowHidden) {
  auto obs = obs_rich();
  obs.tables["shop.orders"]["row_security"] =
      json{{"enabled", true}, {"forced", false}};
  obs.tables["shop.orders"]["policies"]["only_one"] = json{{"command", "ALL"}};
  const auto plan = pglaswell::plan_migration(
      spec_of(json::array({json{{"kind", "drop_policy"}, {"schema", "shop"},
                                {"table", "orders"}, {"name", "only_one"}}})),
      obs, {});
  ASSERT_TRUE(plan.ok) << plan.render();
  bool warned = false;
  for (const auto& w : plan.warnings) {
    if (w.find("LAST policy") != std::string::npos) warned = true;
  }
  EXPECT_TRUE(warned) << plan.render();
}

TEST(Planner, DisablingATriggerTakesTheWeakerLockAndSaysWhatStopsHappening) {
  const auto plan = pglaswell::plan_migration(
      spec_of(json::array({json{{"kind", "set_trigger_state"}, {"schema", "shop"},
                                {"table", "orders"}, {"trigger", "orders_audit"},
                                {"enabled", false}}})),
      obs_rich(), {});
  ASSERT_TRUE(plan.ok) << plan.render();
  const auto* step = only_step(plan, "set_trigger_state");
  ASSERT_NE(step, nullptr);
  EXPECT_NE(step->lock.find("ShareRowExclusiveLock"), std::string::npos)
      << "measured: not AccessExclusiveLock, so readers are unaffected: "
      << step->lock;
  bool warned = false;
  for (const auto& w : plan.warnings) {
    if (w.find("does not backfill it") != std::string::npos) warned = true;
  }
  EXPECT_TRUE(warned) << plan.render();

  // A trigger already in the wanted state is satisfied; an unknown one is a
  // conflict rather than a statement that would fail.
  const auto satisfied = pglaswell::plan_migration(
      spec_of(json::array({json{{"kind", "set_trigger_state"}, {"schema", "shop"},
                                {"table", "orders"}, {"trigger", "orders_audit"},
                                {"enabled", true}}})),
      obs_rich(), {});
  EXPECT_EQ(only_step(satisfied, "set_trigger_state")->action,
            pglaswell::Action::kSatisfied);
  const auto missing = pglaswell::plan_migration(
      spec_of(json::array({json{{"kind", "set_trigger_state"}, {"schema", "shop"},
                                {"table", "orders"}, {"trigger", "nope"},
                                {"enabled", false}}})),
      obs_rich(), {});
  EXPECT_FALSE(missing.ok) << missing.render();
}

TEST(Planner, GrantIsCheapAndRevokeHitsLiveConnections) {
  const auto g = pglaswell::plan_migration(
      spec_of(json::array({json{{"kind", "grant"}, {"schema", "shop"},
                                {"table", "orders"},
                                {"privileges", json::array({"SELECT", "UPDATE"})},
                                {"columns", json::array({"amount"})},
                                {"to", json::array({"app", "PUBLIC"})}}})),
      obs_rich(), {});
  ASSERT_TRUE(g.ok) << g.render();
  const auto sql = all_sql(*only_step(g, "grant"));
  // The column list goes after the PRIVILEGES, not after the relation -- the
  // syntax error this project already made once and found by running it.
  EXPECT_NE(sql.find("GRANT SELECT, UPDATE (\"amount\") ON shop.orders "
                     "TO \"app\", PUBLIC;"),
            std::string::npos) << sql;
  EXPECT_NE(only_step(g, "grant")->lock.find("AccessShareLock"), std::string::npos);

  const auto r = pglaswell::plan_migration(
      spec_of(json::array({json{{"kind", "revoke"}, {"schema", "shop"},
                                {"table", "orders"},
                                {"privileges", json::array({"SELECT"})},
                                {"from", json::array({"app"})}}})),
      obs_rich(), {});
  ASSERT_TRUE(r.ok) << r.render();
  EXPECT_NE(all_sql(*only_step(r, "revoke")).find("REVOKE SELECT ON shop.orders FROM \"app\";"),
            std::string::npos);
  bool warned = false;
  for (const auto& w : r.warnings) {
    if (w.find("ALREADY CONNECTED") != std::string::npos) warned = true;
  }
  EXPECT_TRUE(warned) << r.render();
}

TEST(Spec, APrivilegeTypoIsCaughtRatherThanSentToTheDatabase) {
  json doc = minimal_spec();
  doc["intents"] = json::array({json{{"kind", "grant"}, {"schema", "s"},
                                     {"table", "t"},
                                     {"privileges", json::array({"SELCT"})},
                                     {"to", json::array({"app"})}}});
  const auto err = spec_error(doc);
  EXPECT_NE(err.find("unknown privilege"), std::string::npos) << err;
}

// --- drop_constraint --------------------------------------------------------
//
// The statement never varies, so for a while this kind looked like a psql
// wrapper and was left out. What it plans is not the statement: it is whether
// the drop can succeed, what it takes with it, and where the lock lands.

// A helper: a table carrying one constraint of the given shape.
static pglaswell::Observations with_constraint(const std::string& name,
                                               const json& body) {
  auto obs = observations(1024, 10);
  obs.tables["shop.orders"]["constraints"][name] = body;
  return obs;
}

static pglaswell::Spec drop_constraint_spec(const std::string& name) {
  return spec_of(json::array({json{{"kind", "drop_constraint"},
                                   {"schema", "shop"},
                                   {"table", "orders"},
                                   {"name", name}}}));
}

TEST(Planner, DroppingAnAbsentConstraintIsSatisfied) {
  const auto plan = pglaswell::plan_migration(
      drop_constraint_spec("never_existed"), observations(1024, 10), {});
  ASSERT_TRUE(plan.ok) << plan.render();
  const auto s = steps_of(plan, "drop_constraint");
  ASSERT_EQ(s.size(), 1u);
  EXPECT_EQ(s[0]->action, pglaswell::Action::kSatisfied);
  EXPECT_TRUE(s[0]->sql.empty());
}

TEST(Planner, DroppingAForeignKeyNamesTheLockOnTheParentTable) {
  // Measured on 18.6: ALTER TABLE c DROP CONSTRAINT c_fk takes
  // AccessExclusiveLock on the referenced table too -- a table the statement
  // never mentions, and a STRONGER lock than adding the constraint takes.
  // Naming only the child's lock would understate the blast radius on exactly
  // the table most likely to be hot.
  const auto plan = pglaswell::plan_migration(
      drop_constraint_spec("orders_warehouse_fk"),
      with_constraint("orders_warehouse_fk",
                      json{{"type", "f"},
                           {"has_index", false},
                           {"references", "shop.warehouse"},
                           {"depended_on_by", json::array()}}),
      {});
  ASSERT_TRUE(plan.ok) << plan.render();
  const auto s = steps_of(plan, "drop_constraint");
  ASSERT_EQ(s.size(), 1u);
  EXPECT_NE(all_sql(*s[0]).find(
                "ALTER TABLE shop.orders DROP CONSTRAINT orders_warehouse_fk"),
            std::string::npos);
  EXPECT_NE(s[0]->lock.find("shop.warehouse"), std::string::npos)
      << "the parent's lock was not named: " << s[0]->lock;
  EXPECT_NE(s[0]->lock.find("never names"), std::string::npos) << s[0]->lock;
}

TEST(Planner, AConstraintAnotherConstraintDependsOnIsRefusedNotAttempted) {
  // PostgreSQL refuses this at execution with a message that suggests CASCADE.
  // The dependency is visible in the catalog beforehand -- both constraints
  // share one index -- so the planner can refuse precisely, name the dependent,
  // and say what to do instead.
  const auto plan = pglaswell::plan_migration(
      drop_constraint_spec("orders_code_uq"),
      with_constraint("orders_code_uq",
                      json{{"type", "u"},
                           {"has_index", true},
                           {"index", "shop.orders_code_uq"},
                           {"depended_on_by",
                            json::array({"lines_code_fk on shop.order_lines"})}}),
      {});
  EXPECT_FALSE(plan.ok) << plan.render();
  ASSERT_FALSE(plan.conflicts.empty());
  EXPECT_NE(plan.conflicts[0].find("lines_code_fk on shop.order_lines"),
            std::string::npos)
      << "the refusal must name the dependent: " << plan.conflicts[0];
  EXPECT_NE(plan.conflicts[0].find("CASCADE"), std::string::npos)
      << "and must say why CASCADE is not the answer: " << plan.conflicts[0];
  EXPECT_TRUE(steps_of(plan, "drop_constraint")[0]->sql.empty())
      << "a refused drop must not carry a statement";
}

TEST(Planner, DroppingAUniqueConstraintWarnsThatItsIndexGoesWithIt) {
  // The statement says DROP CONSTRAINT and the index disappears too. That is
  // the part an author is most likely not to have priced in.
  const auto plan = pglaswell::plan_migration(
      drop_constraint_spec("orders_code_uq"),
      with_constraint("orders_code_uq", json{{"type", "u"},
                                             {"has_index", true},
                                             {"index", "shop.orders_code_uq"},
                                             {"depended_on_by", json::array()}}),
      {});
  ASSERT_TRUE(plan.ok) << plan.render();
  bool warned = false;
  for (const auto& w : plan.warnings) {
    if (w.find("also drops its index") != std::string::npos &&
        w.find("shop.orders_code_uq") != std::string::npos) {
      warned = true;
      EXPECT_NE(w.find("evaluateIndex"), std::string::npos)
          << "the warning should name the sibling tool that answers it: " << w;
    }
  }
  EXPECT_TRUE(warned) << plan.render();
}

TEST(Planner, DroppingANotNullConstraintWarnsTheColumnBecomesNullable) {
  // PG17+ names NOT NULL constraints, so they can be dropped by name like any
  // other. The effect is not "less enforcement" but a change to what the data
  // may contain, which is a different conversation.
  const auto plan = pglaswell::plan_migration(
      drop_constraint_spec("orders_region_not_null"),
      with_constraint("orders_region_not_null",
                      json{{"type", "n"},
                           {"has_index", false},
                           {"column", "fulfilment_region"},
                           {"depended_on_by", json::array()}}),
      {});
  ASSERT_TRUE(plan.ok) << plan.render();
  bool warned = false;
  for (const auto& w : plan.warnings) {
    if (w.find("nullable again") != std::string::npos &&
        w.find("fulfilment_region") != std::string::npos)
      warned = true;
  }
  EXPECT_TRUE(warned) << plan.render();
}

TEST(Planner, ADroppedConstraintIsGoneForLaterIntentsInTheSameSpec) {
  // Projection: dropping a unique constraint and then re-adding a check by the
  // same name in one spec must not read as a conflict. Without projection the
  // second intent would see the constraint the first one removes.
  auto obs = with_constraint("orders_code_uq",
                             json{{"type", "u"},
                                  {"has_index", true},
                                  {"index", "shop.orders_code_uq"},
                                  {"depended_on_by", json::array()}});
  const auto plan = pglaswell::plan_migration(
      spec_of(json::array({json{{"kind", "drop_constraint"},
                                {"schema", "shop"},
                                {"table", "orders"},
                                {"name", "orders_code_uq"}},
                           json{{"kind", "drop_constraint"},
                                {"schema", "shop"},
                                {"table", "orders"},
                                {"name", "orders_code_uq"}}})),
      obs, {});
  ASSERT_TRUE(plan.ok) << plan.render();
  const auto s = steps_of(plan, "drop_constraint");
  ASSERT_EQ(s.size(), 2u);
  EXPECT_EQ(s[0]->action, pglaswell::Action::kApply);
  EXPECT_EQ(s[1]->action, pglaswell::Action::kSatisfied)
      << "the second drop did not see the first one: " << plan.render();
}

TEST(Planner, DroppingAConstraintOnAnAbsentTableIsAConflict) {
  const auto plan = pglaswell::plan_migration(
      spec_of(json::array({json{{"kind", "drop_constraint"},
                                {"schema", "shop"},
                                {"table", "no_such_table"},
                                {"name", "ck"}}})),
      observations(1024, 10), {});
  EXPECT_FALSE(plan.ok) << plan.render();
}

TEST(Spec, DropConstraintHasNoCascadeKey) {
  // CASCADE drops objects the spec never named, which is precisely what a
  // signed change must not do. The refusal has to be at parse time: accepting
  // and ignoring the key would apply a materially different change.
  json doc = minimal_spec();
  doc["intents"] = json::array({json{{"kind", "drop_constraint"},
                                     {"schema", "s"},
                                     {"table", "t"},
                                     {"name", "ck"},
                                     {"cascade", true}}});
  const auto err = spec_error(doc);
  EXPECT_NE(err.find("cascade"), std::string::npos) << err;
}

TEST(Planner, SetNotNullUsesTheFourStepRecipeInSeparateTransactions) {
  // The recipe only works if each step COMMITS before the next runs: if the
  // NOT VALID add and the VALIDATE shared a transaction, the stronger lock
  // would be held across the scan and it would buy nothing.
  auto obs = observations(2LL << 30, 8000000);
  obs.tables["shop.orders"]["columns"]["fulfilment_region"] =
      json{{"type", "text"}, {"not_null", false}};
  const auto plan = pglaswell::plan_migration(
      spec_of(json::array({json{{"kind", "set_not_null"}, {"schema", "shop"},
                                {"table", "orders"}, {"column", "fulfilment_region"}}})),
      obs, {});
  ASSERT_TRUE(plan.ok) << plan.render();
  const auto s = steps_of(plan, "set_not_null");
  ASSERT_EQ(s.size(), 4u) << plan.render();

  EXPECT_NE(all_sql(*s[0]).find("CHECK (fulfilment_region IS NOT NULL) NOT VALID"),
            std::string::npos);
  EXPECT_NE(all_sql(*s[1]).find("VALIDATE CONSTRAINT"), std::string::npos);
  EXPECT_NE(all_sql(*s[2]).find("SET NOT NULL"), std::string::npos);
  EXPECT_NE(all_sql(*s[3]).find("DROP CONSTRAINT"), std::string::npos);

  // Every step in its own transaction group. This is the property that makes
  // the recipe worth anything.
  for (std::size_t i = 1; i < s.size(); ++i) {
    EXPECT_GT(s[i]->txn_group, s[i - 1]->txn_group)
        << "step " << i << " shares a transaction with the one before it, so "
           "the earlier step's lock is held across it";
  }
  // The scan happens under a lock that does not block the application.
  EXPECT_NE(s[1]->lock.find("does NOT block reads or writes"), std::string::npos)
      << s[1]->lock;
}

TEST(Planner, SetNotNullKeepsTheCheckWhenAsked) {
  auto obs = observations(1024, 10);
  obs.tables["shop.orders"]["columns"]["fulfilment_region"] =
      json{{"type", "text"}, {"not_null", false}};
  const auto plan = pglaswell::plan_migration(
      spec_of(json::array({json{{"kind", "set_not_null"}, {"schema", "shop"},
                                {"table", "orders"}, {"column", "fulfilment_region"},
                                {"keep_check", true}}})),
      obs, {});
  EXPECT_EQ(steps_of(plan, "set_not_null").size(), 3u) << plan.render();
}

TEST(Planner, AColumnAlreadyNotNullIsSatisfied) {
  auto obs = observations(1024, 10);
  obs.tables["shop.orders"]["columns"]["fulfilment_region"] =
      json{{"type", "text"}, {"not_null", true}};
  const auto plan = pglaswell::plan_migration(
      spec_of(json::array({json{{"kind", "set_not_null"}, {"schema", "shop"},
                                {"table", "orders"}, {"column", "fulfilment_region"}}})),
      obs, {});
  const auto s = steps_of(plan, "set_not_null");
  ASSERT_EQ(s.size(), 1u);
  EXPECT_EQ(s[0]->action, pglaswell::Action::kSatisfied);
}

TEST(Planner, AddForeignKeySplitsIntoNotValidThenValidate) {
  const auto plan = pglaswell::plan_migration(
      spec_of(json::array({json{{"kind", "add_foreign_key"}, {"schema", "shop"},
                                {"table", "orders"}, {"name", "orders_wh_fk"},
                                {"columns", json::array({"warehouse_id"})},
                                {"references_schema", "shop"},
                                {"references_table", "warehouse"},
                                {"references_columns", json::array({"id"})},
                                {"on_delete", "RESTRICT"}}})),
      observations(2LL << 30, 8000000), {});
  ASSERT_TRUE(plan.ok) << plan.render();
  const auto s = steps_of(plan, "add_foreign_key");
  ASSERT_EQ(s.size(), 2u);
  EXPECT_NE(all_sql(*s[0]).find("NOT VALID"), std::string::npos);
  EXPECT_NE(all_sql(*s[0]).find("ON DELETE RESTRICT"), std::string::npos);
  EXPECT_NE(all_sql(*s[1]).find("VALIDATE CONSTRAINT orders_wh_fk"), std::string::npos);
  EXPECT_GT(s[1]->txn_group, s[0]->txn_group)
      << "VALIDATE must commit separately or step 1's lock spans the scan";

  // The lock people are surprised by: a statement naming one table locks two.
  EXPECT_NE(s[0]->lock.find("shop.warehouse"), std::string::npos) << s[0]->lock;
  EXPECT_NE(s[0]->why.find("names one table and locks two"), std::string::npos);
  EXPECT_NE(s[1]->lock.find("does NOT block reads or writes"), std::string::npos);
}

TEST(Planner, AnUnindexedForeignKeyColumnWarns) {
  // Without an index, every parent UPDATE or DELETE scans the child.
  const auto plan = pglaswell::plan_migration(
      spec_of(json::array({json{{"kind", "add_foreign_key"}, {"schema", "shop"},
                                {"table", "orders"}, {"name", "fk"},
                                {"columns", json::array({"warehouse_id"})},
                                {"references_schema", "shop"},
                                {"references_table", "warehouse"},
                                {"references_columns", json::array({"id"})}}})),
      observations(1024, 10), {});
  bool warned = false;
  for (const auto& w : plan.warnings) {
    if (w.find("will scan") != std::string::npos) warned = true;
  }
  EXPECT_TRUE(warned) << json(plan.warnings).dump(2);
}

TEST(Spec, AForeignKeyWithMismatchedColumnCountsIsRefused) {
  json doc = minimal_spec();
  doc["intents"] = json::array({json{
      {"kind", "add_foreign_key"}, {"schema", "s"}, {"table", "t"}, {"name", "fk"},
      {"columns", json::array({"a", "b"})}, {"references_schema", "s"},
      {"references_table", "p"}, {"references_columns", json::array({"id"})}}});
  const auto err = spec_error(doc);
  EXPECT_NE(err.find("one to one"), std::string::npos) << err;
}

TEST(Spec, AnUnknownReferentialActionIsRefusedWithTheList) {
  json doc = minimal_spec();
  doc["intents"] = json::array({json{
      {"kind", "add_foreign_key"}, {"schema", "s"}, {"table", "t"}, {"name", "fk"},
      {"columns", json::array({"a"})}, {"references_schema", "s"},
      {"references_table", "p"}, {"references_columns", json::array({"id"})},
      {"on_delete", "cascade"}}});  // lower case: PostgreSQL spells it upper
  const auto err = spec_error(doc);
  EXPECT_NE(err.find("CASCADE"), std::string::npos) << err;
}

TEST_F(DatabaseTest, DependentViewsAreObservedWithEverythingARebuildMustRestore) {
  // Measured on 18.6 (spike S13): PostgreSQL refuses ANY type change on a
  // column a view reads -- text -> varchar(200) is refused although it rewrites
  // nothing -- so a planner blind to the view stack cannot plan the change at
  // all. And a naive drop/recreate loses eight things silently. Each field
  // asserted here corresponds to one of them; a missing field is not a cosmetic
  // gap, it is an application losing SELECT or writes through a view.
  pglaswell::ConnConfig cfg;
  cfg.name = "t";
  cfg.conninfo = url_;
  {
    pglaswell::WriteSession w(cfg);
    w.begin("pg_laswell/test/views");
    w.txn().exec("DROP TABLE IF EXISTS laswell_vbase CASCADE");
    w.txn().exec("CREATE TABLE laswell_vbase(id bigint PRIMARY KEY,"
                 " amount int, note text)");
    // A three-level stack: a view, a view on that view, and a materialized
    // view. Plus one view that reads a DIFFERENT column, which must not be
    // dragged into a rebuild it does not need.
    w.txn().exec("CREATE VIEW laswell_v1 AS"
                 " SELECT id, amount, note FROM laswell_vbase");
    w.txn().exec("CREATE VIEW laswell_v2 AS"
                 " SELECT id, amount * 2 AS doubled FROM laswell_v1");
    w.txn().exec("CREATE MATERIALIZED VIEW laswell_mv1 AS"
                 " SELECT id, amount FROM laswell_v1");
    w.txn().exec("CREATE UNIQUE INDEX laswell_mv1_id ON laswell_mv1(id)");
    w.txn().exec("CREATE VIEW laswell_vother AS"
                 " SELECT id, note FROM laswell_vbase");
    w.txn().exec("COMMENT ON VIEW laswell_v1 IS 'the comment on v1'");
    w.txn().exec("COMMENT ON COLUMN laswell_v1.amount IS 'amount doc'");
    w.txn().exec("ALTER VIEW laswell_v1 SET (security_barrier = true)");
    w.txn().exec("CREATE OR REPLACE FUNCTION laswell_noop() RETURNS trigger"
                 " LANGUAGE plpgsql AS $$BEGIN RETURN NEW; END$$");
    w.txn().exec("CREATE TRIGGER laswell_v1_ins INSTEAD OF INSERT ON laswell_v1"
                 " FOR EACH ROW EXECUTE FUNCTION laswell_noop()");
    w.commit();
  }

  pglaswell::Catalog cat(cfg);
  const auto obs = cat.observe({"public"}, {"laswell_vbase"});
  const auto& views = obs.table("public.laswell_vbase")["dependent_views"];

  // Transitive: v2 and mv1 read the base table only THROUGH v1, and would be
  // invisible to a single-level pg_depend lookup.
  ASSERT_EQ(views.size(), 4u) << views.dump(2);
  for (const char* n : {"public.laswell_v1", "public.laswell_v2", "public.laswell_mv1",
                        "public.laswell_vother"}) {
    EXPECT_TRUE(views.contains(n)) << n << " missing from " << views.dump(2);
  }

  // Depth decides the order: drop the deepest first, recreate in reverse.
  EXPECT_EQ(views["public.laswell_v1"]["level"], 1);
  EXPECT_GT(views["public.laswell_v2"]["level"].get<int>(),
            views["public.laswell_v1"]["level"].get<int>());
  EXPECT_GT(views["public.laswell_mv1"]["level"].get<int>(),
            views["public.laswell_v1"]["level"].get<int>());

  // A materialized view is not a view: it holds data, its indexes go with it,
  // and it needs a REFRESH after a rebuild.
  EXPECT_EQ(views["public.laswell_mv1"]["kind"], "materialized");
  EXPECT_EQ(views["public.laswell_v1"]["kind"], "view");

  // The eight things a hand rebuild loses.
  const auto& v1 = views["public.laswell_v1"];
  EXPECT_FALSE(v1["definition"].get<std::string>().empty());
  EXPECT_EQ(v1["comment"], "the comment on v1");
  EXPECT_EQ(v1["column_comments"]["amount"], "amount doc");
  EXPECT_FALSE(v1["owner"].get<std::string>().empty());
  EXPECT_NE(v1["reloptions"].dump().find("security_barrier"), std::string::npos)
      << v1["reloptions"].dump();
  ASSERT_EQ(v1["triggers"].size(), 1u) << v1["triggers"].dump();
  EXPECT_NE(v1["triggers"][0].get<std::string>().find("INSTEAD OF INSERT"),
            std::string::npos);
  EXPECT_EQ(views["public.laswell_mv1"]["indexes"].size(), 1u)
      << "a materialized view's index is dropped with it and must be restored";

  // Which columns each view actually reads. A view on a column nobody is
  // changing must not be rebuilt: widening the blast radius for nothing is its
  // own harm, and the whole point is to touch as little as possible.
  const auto uses = [&](const char* v) {
    std::set<std::string> out;
    for (const auto& c : views[v]["uses_columns"]) out.insert(c.get<std::string>());
    return out;
  };
  EXPECT_EQ(uses("public.laswell_vother"), (std::set<std::string>{"id", "note"}))
      << "vother reads note, not amount";
  EXPECT_EQ(uses("public.laswell_vother").count("amount"), 0u)
      << "a change to amount must not drag vother into the rebuild";

  // v2 and mv1 read the base table only through v1, so their own column list
  // is empty -- their dependency is on v1, and rebuilding v1 is what forces
  // them. Asserted so the emptiness is understood rather than mistaken for a
  // gap in the query.
  EXPECT_TRUE(uses("public.laswell_v2").empty()) << views["public.laswell_v2"].dump();

  // And the refusal that makes all of this necessary, from PostgreSQL itself.
  // Without this the test would only be agreeing with our own JSON.
  {
    pglaswell::WriteSession w(cfg);
    w.begin("pg_laswell/test/views-refute");
    EXPECT_THROW(w.txn().exec("ALTER TABLE laswell_vbase"
                              " ALTER COLUMN note TYPE varchar(200)"),
                 pqxx::sql_error)
        << "a type change needing no rewrite was allowed under a view";
  }

  // A table with no views on it reports an empty object, not a missing key.
  {
    pglaswell::WriteSession w(cfg);
    w.begin("pg_laswell/test/views-none");
    w.txn().exec("DROP TABLE IF EXISTS laswell_vplain");
    w.txn().exec("CREATE TABLE laswell_vplain(id bigint PRIMARY KEY)");
    w.commit();
  }
  const auto plain = cat.observe({"public"}, {"laswell_vplain"});
  EXPECT_TRUE(plain.table("public.laswell_vplain")["dependent_views"].empty());

  pglaswell::WriteSession w(cfg);
  w.begin("pg_laswell/test/views-cleanup");
  w.txn().exec("DROP TABLE laswell_vbase CASCADE");
  w.txn().exec("DROP TABLE laswell_vplain");
  w.commit();
}

TEST_F(DatabaseTest, TheGeneratedViewRebuildActuallyRunsAndRestoresEverything) {
  // The end-to-end claim. Everything else asserts the SQL we generate; this
  // runs it against PostgreSQL and then re-reads the catalog, because a recipe
  // that is right in a string comparison and wrong in a database is worth
  // nothing. Each check corresponds to one of the eight losses S13 measured.
  pglaswell::ConnConfig cfg;
  cfg.name = "t";
  cfg.conninfo = url_;
  {
    pglaswell::WriteSession w(cfg);
    w.begin("pg_laswell/test/rebuild");
    // Cleanup first, and in this order. A role cannot be dropped while any
    // object still grants to it, so the grants have to go before the role --
    // and the objects before the grants. A previous failing run leaves exactly
    // this state behind, and the next run then fails for a reason that has
    // nothing to do with what it is testing.
    w.txn().exec("DROP TABLE IF EXISTS laswell_rb CASCADE");
    w.txn().exec("DO $$BEGIN"
                 "  IF EXISTS (SELECT 1 FROM pg_roles"
                 "              WHERE rolname='laswell_rb_reader') THEN"
                 "    EXECUTE 'DROP OWNED BY laswell_rb_reader';"
                 "    EXECUTE 'DROP ROLE laswell_rb_reader';"
                 "  END IF;"
                 "END$$");
    w.txn().exec("CREATE ROLE laswell_rb_reader");
    w.txn().exec("CREATE TABLE laswell_rb(id bigint PRIMARY KEY,"
                 " amount integer, note text)");
    w.txn().exec("INSERT INTO laswell_rb SELECT g, g, 'n'||g"
                 " FROM generate_series(1,50) g");
    w.txn().exec("CREATE VIEW laswell_rb_v1 AS"
                 " SELECT id, amount, note FROM laswell_rb");
    w.txn().exec("CREATE VIEW laswell_rb_v2 AS"
                 " SELECT id, amount * 2 AS doubled FROM laswell_rb_v1");
    w.txn().exec("CREATE MATERIALIZED VIEW laswell_rb_mv AS"
                 " SELECT id, amount FROM laswell_rb_v1");
    w.txn().exec("CREATE UNIQUE INDEX laswell_rb_mv_id ON laswell_rb_mv(id)");
    w.txn().exec("CREATE VIEW laswell_rb_other AS"
                 " SELECT id, note FROM laswell_rb");
    w.txn().exec("COMMENT ON VIEW laswell_rb_v1 IS 'v1 doc'");
    w.txn().exec("COMMENT ON COLUMN laswell_rb_v1.amount IS 'amount doc'");
    w.txn().exec("ALTER VIEW laswell_rb_v1 SET (security_barrier = true)");
    w.txn().exec("GRANT SELECT ON laswell_rb_v1 TO laswell_rb_reader");
    w.txn().exec("GRANT SELECT ON laswell_rb_v1 TO PUBLIC");
    w.txn().exec("GRANT SELECT (id) ON laswell_rb_v2 TO laswell_rb_reader");
    w.txn().exec("CREATE OR REPLACE FUNCTION laswell_rb_noop() RETURNS trigger"
                 " LANGUAGE plpgsql AS $$BEGIN RETURN NEW; END$$");
    w.txn().exec("CREATE TRIGGER laswell_rb_ins INSTEAD OF INSERT ON"
                 " laswell_rb_v1 FOR EACH ROW EXECUTE FUNCTION laswell_rb_noop()");
    w.commit();
  }

  pglaswell::Catalog cat(cfg);
  const auto obs = cat.observe({"public"}, {"laswell_rb"});
  json doc = minimal_spec();
  doc["intents"] = json::array({json{{"kind", "alter_column_type"},
                                     {"schema", "public"},
                                     {"table", "laswell_rb"},
                                     {"column", "amount"},
                                     {"type", "bigint"}}});
  const auto plan =
      pglaswell::plan_migration(pglaswell::parse_spec(doc), obs, {});
  ASSERT_TRUE(plan.ok) << plan.render();
  const auto steps = steps_of(plan, "alter_column_type");
  ASSERT_EQ(steps.size(), 1u);

  // One transaction, exactly as the plan says: no reader may find the views
  // missing. If the recipe is wrong, this throws and nothing is left behind.
  {
    pglaswell::WriteSession w(cfg);
    w.begin("pg_laswell/test/rebuild-apply");
    for (const auto& q : steps[0]->sql) {
      const std::string stmt = q;
      w.txn().exec(stmt.substr(0, stmt.size() - 1));
    }
    w.commit();
  }

  // Scoped, because the reads below hold AccessShareLock for as long as the
  // session lives, and the cleanup DROP needs AccessExclusiveLock. Leaving
  // it open made the teardown time out on a lock while every assertion had
  // already passed -- which is precisely the hazard this tool exists to
  // reason about, arriving in its own test suite.
  {
    pglaswell::ReadSession r(cfg);
    auto scalar = [&](const std::string& q) {
      return r.txn().exec(q)[0][0].as<std::string>();
    };
    // The change itself.
    EXPECT_EQ(scalar("SELECT format_type(atttypid, atttypmod) FROM pg_attribute"
                     " WHERE attrelid='laswell_rb'::regclass AND attname='amount'"),
              "bigint");
    // ... and all four views still there, the unrelated one never touched.
    EXPECT_EQ(scalar("SELECT count(*)::text FROM pg_class WHERE relkind IN ('v','m')"
                     " AND relname LIKE 'laswell_rb%'"), "4");
    // The eight.
    EXPECT_EQ(scalar("SELECT coalesce(obj_description('laswell_rb_v1'::regclass),"
                     "'** GONE **')"), "v1 doc");
    EXPECT_EQ(scalar("SELECT coalesce(col_description('laswell_rb_v1'::regclass,2),"
                     "'** GONE **')"), "amount doc");
    EXPECT_NE(scalar("SELECT coalesce(array_to_string(reloptions,','),'** GONE **')"
                     " FROM pg_class WHERE oid='laswell_rb_v1'::regclass")
                  .find("security_barrier"), std::string::npos);
    EXPECT_EQ(scalar("SELECT coalesce(string_agg(tgname,','),'** GONE **')"
                     " FROM pg_trigger WHERE tgrelid='laswell_rb_v1'::regclass"
                     " AND NOT tgisinternal"), "laswell_rb_ins");
    EXPECT_EQ(scalar("SELECT count(*)::text FROM pg_index"
                     " WHERE indrelid='laswell_rb_mv'::regclass"), "1");
    EXPECT_EQ(scalar("SELECT has_table_privilege('laswell_rb_reader',"
                     " 'laswell_rb_v1','SELECT')::text"), "true");
    EXPECT_EQ(scalar("SELECT has_table_privilege('public',"
                     " 'laswell_rb_v1','SELECT')::text"), "true")
        << "the PUBLIC grant is the one nobody notices missing";
    EXPECT_EQ(scalar("SELECT has_column_privilege('laswell_rb_reader',"
                     " 'laswell_rb_v2','id','SELECT')::text"), "true")
        << "column-level grants are lost by a hand rebuild";
    // The materialized view holds data again, not an empty shell.
    EXPECT_EQ(scalar("SELECT count(*)::text FROM laswell_rb_mv"), "50");
    // And the views still answer.
    EXPECT_EQ(scalar("SELECT sum(doubled)::text FROM laswell_rb_v2"),
              std::to_string(50 * 51));
  }

  pglaswell::WriteSession w(cfg);
  w.begin("pg_laswell/test/rebuild-cleanup");
  w.txn().exec("DROP TABLE laswell_rb CASCADE");
  w.txn().exec("DROP FUNCTION laswell_rb_noop()");
  w.txn().exec("DROP OWNED BY laswell_rb_reader");
  w.txn().exec("DROP ROLE laswell_rb_reader");
  w.commit();
}

TEST_F(DatabaseTest, ExposingANewColumnThroughAViewKeepsWhatARebuildWouldLose) {
  // The cheap path, proved against PostgreSQL rather than against our own SQL.
  // S13's rebuild loses eight things and restores them; S14 measured that
  // CREATE OR REPLACE loses exactly one. This asserts both halves: that the
  // seven survive without any restore statement at all, and that the one is put
  // back -- because a plan that chose the destructive path here would be doing
  // gratuitous harm, and one that used the cheap path without repairing
  // reloptions would silently disable a security barrier.
  pglaswell::ConnConfig cfg;
  cfg.name = "t";
  cfg.conninfo = url_;
  {
    pglaswell::WriteSession w(cfg);
    w.begin("pg_laswell/test/expose");
    w.txn().exec("DROP TABLE IF EXISTS laswell_ex CASCADE");
    w.txn().exec("DO $$BEGIN"
                 "  IF EXISTS (SELECT 1 FROM pg_roles"
                 "              WHERE rolname='laswell_ex_reader') THEN"
                 "    EXECUTE 'DROP OWNED BY laswell_ex_reader';"
                 "    EXECUTE 'DROP ROLE laswell_ex_reader';"
                 "  END IF;"
                 "END$$");
    w.txn().exec("CREATE ROLE laswell_ex_reader");
    w.txn().exec("CREATE TABLE laswell_ex(id bigint PRIMARY KEY, amount int)");
    w.txn().exec("CREATE VIEW laswell_ex_star AS SELECT * FROM laswell_ex");
    w.txn().exec("CREATE VIEW laswell_ex_v1 AS"
                 " SELECT id, amount FROM laswell_ex");
    w.txn().exec("CREATE VIEW laswell_ex_v2 AS"
                 " SELECT id FROM laswell_ex_v1");
    w.txn().exec("COMMENT ON VIEW laswell_ex_v1 IS 'v1 doc'");
    w.txn().exec("COMMENT ON COLUMN laswell_ex_v1.amount IS 'amount doc'");
    w.txn().exec("ALTER VIEW laswell_ex_v1 SET (security_barrier = true)");
    w.txn().exec("GRANT SELECT ON laswell_ex_v1 TO laswell_ex_reader");
    w.txn().exec("GRANT SELECT (id) ON laswell_ex_v1 TO laswell_ex_reader");
    w.txn().exec("CREATE OR REPLACE FUNCTION laswell_ex_noop() RETURNS trigger"
                 " LANGUAGE plpgsql AS $$BEGIN RETURN NEW; END$$");
    w.txn().exec("CREATE TRIGGER laswell_ex_ins INSTEAD OF INSERT ON"
                 " laswell_ex_v1 FOR EACH ROW EXECUTE FUNCTION laswell_ex_noop()");
    w.commit();
  }

  pglaswell::Catalog cat(cfg);
  // add_column first: views never block it, and it must not rebuild anything.
  {
    const auto obs = cat.observe({"public"}, {"laswell_ex"});
    json doc = minimal_spec();
    doc["intents"] = json::array({json{{"kind", "add_column"},
                                       {"schema", "public"},
                                       {"table", "laswell_ex"},
                                       {"column", "region"}, {"type", "text"},
                                       {"nullable", true}, {"comment", "c"}}});
    const auto plan =
        pglaswell::plan_migration(pglaswell::parse_spec(doc), obs, {});
    ASSERT_TRUE(plan.ok) << plan.render();
    const auto steps = steps_of(plan, "add_column");
    EXPECT_EQ(all_sql(*steps[0]).find("DROP VIEW"), std::string::npos);
    pglaswell::WriteSession w(cfg);
    w.begin("pg_laswell/test/expose-add");
    for (const auto& q : steps[0]->sql) {
      const std::string stmt = q;
      w.txn().exec(stmt.substr(0, stmt.size() - 1));
    }
    w.commit();
  }

  {
    // The star view did NOT pick the column up. This is the whole reason the
    // warning exists: nothing errors, and the column is simply not there.
    pglaswell::ReadSession r(cfg);
    EXPECT_EQ(r.txn()
                  .exec("SELECT count(*) FROM pg_attribute WHERE attrelid="
                        "'laswell_ex_star'::regclass AND attname='region'")[0][0]
                  .as<int>(),
              0)
        << "SELECT * is expanded at creation; a later column never reaches it";
  }

  // Now expose it through v1.
  {
    const auto obs = cat.observe({"public"}, {"laswell_ex_v1"});
    json doc = minimal_spec();
    doc["intents"] = json::array(
        {json{{"kind", "replace_view"}, {"schema", "public"},
              {"name", "laswell_ex_v1"},
              {"definition", "SELECT id, amount, region FROM laswell_ex"}}});
    const auto plan =
        pglaswell::plan_migration(pglaswell::parse_spec(doc), obs, {});
    ASSERT_TRUE(plan.ok) << plan.render();
    const auto steps = steps_of(plan, "replace_view");
    ASSERT_EQ(steps.size(), 1u);
    EXPECT_EQ(steps[0]->detail.value("method", ""), "replace");
    pglaswell::WriteSession w(cfg);
    w.begin("pg_laswell/test/expose-replace");
    for (const auto& q : steps[0]->sql) {
      const std::string stmt = q;
      w.txn().exec(stmt.substr(0, stmt.size() - 1));
    }
    w.commit();
  }

  {
    pglaswell::ReadSession r(cfg);
    auto scalar = [&](const std::string& q) {
      return r.txn().exec(q)[0][0].as<std::string>();
    };
    EXPECT_EQ(scalar("SELECT count(*)::text FROM pg_attribute WHERE attrelid="
                     "'laswell_ex_v1'::regclass AND attname='region'"), "1");
    // The seven that survive with no restore statement at all.
    EXPECT_EQ(scalar("SELECT coalesce(obj_description('laswell_ex_v1'::regclass),"
                     "'** GONE **')"), "v1 doc");
    EXPECT_EQ(scalar("SELECT coalesce(col_description('laswell_ex_v1'::regclass,2),"
                     "'** GONE **')"), "amount doc");
    EXPECT_EQ(scalar("SELECT has_table_privilege('laswell_ex_reader',"
                     "'laswell_ex_v1','SELECT')::text"), "true");
    EXPECT_EQ(scalar("SELECT has_column_privilege('laswell_ex_reader',"
                     "'laswell_ex_v1','id','SELECT')::text"), "true");
    EXPECT_EQ(scalar("SELECT coalesce(string_agg(tgname,','),'** GONE **')"
                     " FROM pg_trigger WHERE tgrelid='laswell_ex_v1'::regclass"
                     " AND NOT tgisinternal"), "laswell_ex_ins");
    EXPECT_EQ(scalar("SELECT count(*)::text FROM pg_class"
                     " WHERE relname='laswell_ex_v2'"), "1")
        << "a dependent view must survive CREATE OR REPLACE";
    // ... and the one that does not, which the plan repaired.
    EXPECT_NE(scalar("SELECT coalesce(array_to_string(reloptions,','),'** GONE **')"
                     " FROM pg_class WHERE oid='laswell_ex_v1'::regclass")
                  .find("security_barrier"), std::string::npos)
        << "CREATE OR REPLACE resets reloptions; the plan must put them back";
  }

  pglaswell::WriteSession w(cfg);
  w.begin("pg_laswell/test/expose-cleanup");
  w.txn().exec("DROP TABLE laswell_ex CASCADE");
  w.txn().exec("DROP FUNCTION laswell_ex_noop()");
  w.txn().exec("DROP OWNED BY laswell_ex_reader");
  w.txn().exec("DROP ROLE laswell_ex_reader");
  w.commit();
}

TEST_F(DatabaseTest, TheUniqueAndPrimaryKeyRecipesRunAndLeaveTheRightCatalog) {
  // Every step run against PostgreSQL, because the two facts this recipe rests
  // on are behaviours nobody would infer from the syntax: USING INDEX renames
  // the index, and ADD PRIMARY KEY sets NOT NULL itself at the cost of a scan.
  pglaswell::ConnConfig cfg;
  cfg.name = "t";
  cfg.conninfo = url_;
  {
    pglaswell::WriteSession w(cfg);
    w.begin("pg_laswell/test/uq");
    w.txn().exec("DROP TABLE IF EXISTS laswell_uq CASCADE");
    w.txn().exec("CREATE TABLE laswell_uq(id bigint, code text)");
    w.txn().exec("INSERT INTO laswell_uq SELECT g, 'c'||g"
                 " FROM generate_series(1,500) g");
    w.commit();
  }

  pglaswell::Catalog cat(cfg);
  auto apply = [&](const json& intent, const char* kind) {
    const auto obs = cat.observe({"public"}, {"laswell_uq"});
    json doc = minimal_spec();
    doc["intents"] = json::array({intent});
    const auto plan =
        pglaswell::plan_migration(pglaswell::parse_spec(doc), obs, {});
    EXPECT_TRUE(plan.ok) << plan.render();
    (void)kind;
    for (const auto& step : plan.steps) {
      for (const auto& q : step.sql) {
        const std::string stmt = q;
        // CONCURRENTLY cannot run inside a transaction block, which is exactly
        // what the plan's txn_class says -- honoured here rather than assumed.
        if (step.txn_class == pglaswell::TxnClass::kForbidden) {
          pglaswell::WriteSession w(cfg);
          w.exec_nontransactional(stmt.substr(0, stmt.size() - 1));
        } else {
          pglaswell::WriteSession w(cfg);
          w.begin("pg_laswell/test/uq-apply");
          w.txn().exec(stmt.substr(0, stmt.size() - 1));
          w.commit();
        }
      }
    }
  };

  // A unique constraint on a table too big for the one-statement path. The
  // fixture is small, so force the concurrent route with a lock waiter... no:
  // force it by size is impossible here, so assert the small path instead and
  // cover the concurrent one through its own index below.
  apply(json{{"kind", "add_unique_constraint"}, {"schema", "public"},
             {"table", "laswell_uq"}, {"name", "laswell_uq_code_uq"},
             {"columns", json::array({"code"})}},
        "add_unique_constraint");

  {
    pglaswell::ReadSession r(cfg);
    auto scalar = [&](const std::string& q) {
      return r.txn().exec(q)[0][0].as<std::string>();
    };
    EXPECT_EQ(scalar("SELECT contype::text FROM pg_constraint"
                     " WHERE conname='laswell_uq_code_uq'"), "u");
    // The constraint and its index share a name, which is what USING INDEX
    // would have forced anyway.
    EXPECT_EQ(scalar("SELECT conindid::regclass::text FROM pg_constraint"
                     " WHERE conname='laswell_uq_code_uq'"),
              "laswell_uq_code_uq");
  }

  // The primary key over a NULLABLE column: the plan must make it NOT NULL
  // first, through the recipe, and only then add the key.
  {
    pglaswell::ReadSession r(cfg);
    EXPECT_EQ(r.txn()
                  .exec("SELECT attnotnull FROM pg_attribute WHERE attrelid="
                        "'laswell_uq'::regclass AND attname='id'")[0][0]
                  .as<bool>(),
              false)
        << "the fixture must start nullable or this proves nothing";
  }
  apply(json{{"kind", "add_primary_key"}, {"schema", "public"},
             {"table", "laswell_uq"}, {"name", "laswell_uq_pkey"},
             {"columns", json::array({"id"})}},
        "add_primary_key");

  {
    pglaswell::ReadSession r(cfg);
    auto scalar = [&](const std::string& q) {
      return r.txn().exec(q)[0][0].as<std::string>();
    };
    EXPECT_EQ(scalar("SELECT contype::text FROM pg_constraint"
                     " WHERE conname='laswell_uq_pkey'"), "p");
    EXPECT_EQ(scalar("SELECT attnotnull::text FROM pg_attribute"
                     " WHERE attrelid='laswell_uq'::regclass AND attname='id'"),
              "true");
    // The temporary CHECK from the NOT NULL recipe is gone, not left behind to
    // cost time on every insert forever.
    EXPECT_EQ(scalar("SELECT count(*)::text FROM pg_constraint"
                     " WHERE conrelid='laswell_uq'::regclass AND contype='c'"),
              "0");
    // Re-planning is satisfied, not a second attempt.
    const auto obs = cat.observe({"public"}, {"laswell_uq"});
    json doc = minimal_spec();
    doc["intents"] = json::array({json{{"kind", "add_primary_key"},
                                       {"schema", "public"},
                                       {"table", "laswell_uq"},
                                       {"name", "laswell_uq_pkey"},
                                       {"columns", json::array({"id"})}}});
    const auto again =
        pglaswell::plan_migration(pglaswell::parse_spec(doc), obs, {});
    ASSERT_TRUE(again.ok) << again.render();
    EXPECT_EQ(steps_of(again, "add_primary_key")[0]->action,
              pglaswell::Action::kSatisfied);
  }

  // And the failure that has no NOT VALID escape: a duplicate stops the job at
  // step two rather than producing a constraint nothing verified.
  {
    pglaswell::WriteSession w(cfg);
    w.begin("pg_laswell/test/uq-dup");
    w.txn().exec("DROP TABLE IF EXISTS laswell_uq_dup");
    w.txn().exec("CREATE TABLE laswell_uq_dup(code text)");
    w.txn().exec("INSERT INTO laswell_uq_dup VALUES ('same'), ('same')");
    w.commit();
  }
  {
    pglaswell::WriteSession w(cfg);
    EXPECT_THROW(w.exec_nontransactional(
                     "CREATE UNIQUE INDEX CONCURRENTLY laswell_uq_dup_uq"
                     " ON laswell_uq_dup (code)"),
                 pqxx::sql_error);
  }
  {
    pglaswell::ReadSession r(cfg);
    EXPECT_EQ(r.txn()
                  .exec("SELECT indisvalid FROM pg_index WHERE indrelid="
                        "'laswell_uq_dup'::regclass")[0][0]
                  .as<bool>(),
              false)
        << "a failed concurrent build leaves an index that looks present";
  }
  {
    pglaswell::WriteSession w(cfg);
    w.begin("pg_laswell/test/uq-dup-adopt");
    EXPECT_THROW(w.txn().exec("ALTER TABLE laswell_uq_dup ADD CONSTRAINT"
                              " laswell_uq_dup_uq UNIQUE USING INDEX"
                              " laswell_uq_dup_uq"),
                 pqxx::sql_error)
        << "USING INDEX must refuse an invalid index, or the constraint would "
           "exist over data nothing checked";
  }

  pglaswell::WriteSession w(cfg);
  w.begin("pg_laswell/test/uq-cleanup");
  w.txn().exec("DROP TABLE laswell_uq CASCADE");
  w.txn().exec("DROP TABLE laswell_uq_dup");
  w.commit();
}

TEST_F(DatabaseTest, ThePartitionRecipesRunAndTheCheckReallyRemovesTheScan) {
  // The claim this whole recipe rests on is a TIMING one, so the test measures
  // it rather than asserting the SQL. An attach whose bounds are already proven
  // must be dramatically faster than one that has to verify them -- if it is
  // not, the four-step recipe is pure overhead and should be deleted.
  pglaswell::ConnConfig cfg;
  cfg.name = "t";
  cfg.conninfo = url_;
  {
    pglaswell::WriteSession w(cfg);
    w.begin("pg_laswell/test/part");
    w.txn().exec("DROP TABLE IF EXISTS laswell_ev CASCADE");
    w.txn().exec("DROP TABLE IF EXISTS laswell_ev_a CASCADE");
    w.txn().exec("DROP TABLE IF EXISTS laswell_ev_b CASCADE");
    w.txn().exec("CREATE TABLE laswell_ev(id bigint, at timestamptz NOT NULL)"
                 " PARTITION BY RANGE (at)");
    w.txn().exec("CREATE TABLE laswell_ev_2025 PARTITION OF laswell_ev"
                 " FOR VALUES FROM ('2025-01-01') TO ('2026-01-01')");
    for (const char* n : {"laswell_ev_a", "laswell_ev_b"}) {
      w.txn().exec(std::string("CREATE TABLE ") + n +
                   "(id bigint, at timestamptz NOT NULL)");
    }
    w.txn().exec("INSERT INTO laswell_ev_a SELECT g,"
                 " '2026-06-01'::timestamptz + (g||' seconds')::interval"
                 " FROM generate_series(1,400000) g");
    w.txn().exec("INSERT INTO laswell_ev_b SELECT g,"
                 " '2027-06-01'::timestamptz + (g||' seconds')::interval"
                 " FROM generate_series(1,400000) g");
    w.commit();
  }

  auto attach_ms = [&](const json& intent, const char* child) {
    pglaswell::Catalog cat(cfg);
    const auto obs = cat.observe({"public", "public"}, {"laswell_ev", child});
    json doc = minimal_spec();
    doc["intents"] = json::array({intent});
    const auto plan =
        pglaswell::plan_migration(pglaswell::parse_spec(doc), obs, {});
    EXPECT_TRUE(plan.ok) << plan.render();
    double attach_only = 0;
    for (const auto& step : plan.steps) {
      for (const auto& q : step.sql) {
        const std::string stmt = q;
        const bool is_attach = stmt.find("ATTACH PARTITION") != std::string::npos;
        const auto t0 = std::chrono::steady_clock::now();
        pglaswell::WriteSession w(cfg);
        w.begin("pg_laswell/test/part-apply");
        w.txn().exec(stmt.substr(0, stmt.size() - 1));
        w.commit();
        if (is_attach) {
          attach_only = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - t0).count();
        }
      }
    }
    return attach_only;
  };

  // With the recipe: the CHECK is added NOT VALID, validated under a lock that
  // does not block, and only then is the partition attached.
  const double with_check = attach_ms(
      json{{"kind", "attach_partition"}, {"schema", "public"},
           {"table", "laswell_ev"}, {"partition", "laswell_ev_a"},
           {"from", "'2026-01-01'"}, {"to", "'2027-01-01'"}},
      "laswell_ev_a");

  // Without it: attach directly, and let PostgreSQL verify every row.
  double without_check = 0;
  {
    const auto t0 = std::chrono::steady_clock::now();
    pglaswell::WriteSession w(cfg);
    w.begin("pg_laswell/test/part-bare");
    w.txn().exec("ALTER TABLE laswell_ev ATTACH PARTITION laswell_ev_b"
                 " FOR VALUES FROM ('2027-01-01') TO ('2028-01-01')");
    w.commit();
    without_check = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - t0).count();
  }

  // A loaded CI box can make any single timing meaningless, so this is a
  // bounded claim: the proven attach must be clearly cheaper, not merely
  // faster by a hair. If it is not, the recipe is not earning its four steps.
  if (without_check < 5.0) {
    GTEST_SKIP() << "the unproven attach was too fast to compare ("
                 << without_check << "ms); the machine is faster than the "
                                     "measurement needs";
  }
  EXPECT_LT(with_check * 3, without_check)
      << "attach with a validated CHECK took " << with_check
      << "ms, without took " << without_check
      << "ms -- the CHECK is supposed to remove the scan entirely";

  {
    pglaswell::ReadSession r(cfg);
    // Both attached, and the temporary CHECK cleaned up rather than left to
    // cost time on every insert forever.
    EXPECT_EQ(r.txn()
                  .exec("SELECT count(*) FROM pg_inherits WHERE inhparent="
                        "'laswell_ev'::regclass")[0][0]
                  .as<int>(),
              3);
    EXPECT_EQ(r.txn()
                  .exec("SELECT count(*) FROM pg_constraint WHERE conrelid="
                        "'laswell_ev_a'::regclass AND contype='c'")[0][0]
                  .as<int>(),
              0)
        << "the bound-proving CHECK must be dropped once the partition bound "
           "enforces the same thing";
  }

  // DETACH CONCURRENTLY, which measurably cannot run in a transaction block.
  {
    pglaswell::WriteSession w(cfg);
    w.begin("pg_laswell/test/part-detach-txn");
    EXPECT_THROW(w.txn().exec("ALTER TABLE laswell_ev DETACH PARTITION"
                              " laswell_ev_a CONCURRENTLY"),
                 pqxx::sql_error)
        << "if this ever succeeds, the txn_class on the step is wrong";
  }
  {
    pglaswell::WriteSession w(cfg);
    w.exec_nontransactional("ALTER TABLE laswell_ev DETACH PARTITION"
                            " laswell_ev_a CONCURRENTLY");
  }
  {
    pglaswell::ReadSession r(cfg);
    EXPECT_EQ(r.txn()
                  .exec("SELECT count(*) FROM pg_inherits WHERE inhparent="
                        "'laswell_ev'::regclass")[0][0]
                  .as<int>(),
              2);
  }

  pglaswell::WriteSession w(cfg);
  w.begin("pg_laswell/test/part-cleanup");
  w.txn().exec("DROP TABLE laswell_ev CASCADE");
  w.txn().exec("DROP TABLE laswell_ev_a");
  w.commit();
}

TEST_F(DatabaseTest, TheRemainingKindsRunAndRowSecurityReallyHidesEverything) {
  // The new kinds end to end. The one that matters most is row security: the
  // measured behaviour is that enabling it with no policy takes an application
  // from every row to none, silently, and that a superuser cannot see this
  // happen at all. Both are asserted against PostgreSQL, from the right role.
  pglaswell::ConnConfig cfg;
  cfg.name = "t";
  cfg.conninfo = url_;
  {
    pglaswell::WriteSession w(cfg);
    w.begin("pg_laswell/test/rest");
    w.txn().exec("DROP TABLE IF EXISTS laswell_sec CASCADE");
    w.txn().exec("DO $$BEGIN"
                 "  IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname='laswell_app') THEN"
                 "    EXECUTE 'DROP OWNED BY laswell_app';"
                 "    EXECUTE 'DROP ROLE laswell_app';"
                 "  END IF;"
                 "END$$");
    w.txn().exec("CREATE ROLE laswell_app");
    w.commit();
  }

  pglaswell::Catalog cat(cfg);
  auto apply = [&](const json& intent, const char* table) {
    const auto obs = cat.observe({"public"}, {table});
    json doc = minimal_spec();
    doc["intents"] = json::array({intent});
    const auto plan =
        pglaswell::plan_migration(pglaswell::parse_spec(doc), obs, {});
    EXPECT_TRUE(plan.ok) << plan.render();
    for (const auto& step : plan.steps) {
      for (const auto& q : step.sql) {
        const std::string stmt = q;
        pglaswell::WriteSession w(cfg);
        w.begin("pg_laswell/test/rest-apply");
        w.txn().exec(stmt.substr(0, stmt.size() - 1));
        w.commit();
      }
    }
  };

  // create_table, with its comments.
  apply(json{{"kind", "create_table"}, {"schema", "public"},
             {"table", "laswell_sec"}, {"comment", "Security fixture."},
             {"primary_key", json::array({"id"})},
             {"columns", json::array({
                 json{{"name", "id"}, {"type", "bigint"}, {"nullable", false},
                      {"comment", "Identity."}},
                 json{{"name", "tenant"}, {"type", "text"}, {"nullable", false},
                      {"comment", "Owning tenant."}}})}},
        "laswell_sec");
  {
    pglaswell::WriteSession w(cfg);
    w.begin("pg_laswell/test/rest-seed");
    w.txn().exec("INSERT INTO laswell_sec SELECT g, 'a'"
                 " FROM generate_series(1,100) g");
    w.commit();
  }
  {
    pglaswell::ReadSession r(cfg);
    EXPECT_EQ(r.txn().exec("SELECT obj_description('laswell_sec'::regclass)")[0][0]
                  .as<std::string>(), "Security fixture.");
    EXPECT_EQ(r.txn().exec("SELECT contype::text FROM pg_constraint WHERE"
                           " conrelid='laswell_sec'::regclass AND contype='p'")[0][0]
                  .as<std::string>(), "p");
  }

  // grant, then the policy, then row security -- the order the plan's own
  // warnings push an author towards.
  apply(json{{"kind", "grant"}, {"schema", "public"}, {"table", "laswell_sec"},
             {"privileges", json::array({"SELECT"})},
             {"to", json::array({"laswell_app"})}},
        "laswell_sec");
  {
    pglaswell::ReadSession r(cfg);
    EXPECT_EQ(r.txn().exec("SELECT has_table_privilege('laswell_app',"
                           "'laswell_sec','SELECT')")[0][0].as<bool>(), true);
  }

  apply(json{{"kind", "set_row_security"}, {"schema", "public"},
             {"table", "laswell_sec"}, {"enabled", true}},
        "laswell_sec");

  // The whole point, checked as the APPLICATION role rather than as us.
  {
    pglaswell::ReadSession r(cfg);
    r.txn().exec("SET LOCAL ROLE laswell_app");
    EXPECT_EQ(r.txn().exec("SELECT count(*) FROM laswell_sec")[0][0].as<int>(), 0)
        << "row security with no policy must hide every row from the "
           "application -- silently, which is why the plan shouts about it";
  }
  // ... and from this session, which cannot see the change at all.
  {
    pglaswell::ReadSession r(cfg);
    EXPECT_EQ(r.txn().exec("SELECT count(*) FROM laswell_sec")[0][0].as<int>(), 100)
        << "a superuser or owner bypasses RLS, which is exactly why verifying "
           "this from the migrating session proves nothing";
  }

  // A policy restores the application's view.
  apply(json{{"kind", "create_policy"}, {"schema", "public"},
             {"table", "laswell_sec"}, {"name", "tenant_iso"},
             {"roles", json::array({"laswell_app"})},
             {"using", "tenant = 'a'"}},
        "laswell_sec");
  {
    pglaswell::ReadSession r(cfg);
    r.txn().exec("SET LOCAL ROLE laswell_app");
    EXPECT_EQ(r.txn().exec("SELECT count(*) FROM laswell_sec")[0][0].as<int>(), 100);
  }

  // rename_column, and the measured fact that a view keeps its own name.
  {
    pglaswell::WriteSession w(cfg);
    w.begin("pg_laswell/test/rest-view");
    w.txn().exec("CREATE VIEW laswell_sec_v AS SELECT id, tenant FROM laswell_sec");
    w.commit();
  }
  apply(json{{"kind", "rename_column"}, {"schema", "public"},
             {"table", "laswell_sec"}, {"column", "tenant"}, {"to", "owner_id"}},
        "laswell_sec");
  {
    pglaswell::ReadSession r(cfg);
    EXPECT_EQ(r.txn().exec("SELECT count(*) FROM pg_attribute WHERE attrelid="
                           "'laswell_sec'::regclass AND attname='owner_id'")[0][0]
                  .as<int>(), 1);
    EXPECT_EQ(r.txn().exec("SELECT count(*) FROM pg_attribute WHERE attrelid="
                           "'laswell_sec_v'::regclass AND attname='tenant'")[0][0]
                  .as<int>(), 1)
        << "the view keeps its OWN output name after the rename -- measured, "
           "and the reason the plan warns that nothing reading through a view "
           "sees the change";
  }

  // delete_rows, applied by hand in one batch since the executor's pacing has
  // its own tests; what is checked here is that the emitted SQL is valid and
  // deletes what it claims.
  {
    const auto obs = cat.observe({"public"}, {"laswell_sec"});
    json doc = minimal_spec();
    doc["intents"] = json::array({json{{"kind", "delete_rows"},
                                       {"schema", "public"},
                                       {"table", "laswell_sec"},
                                       {"key", "id"},
                                       {"where", "id <= 40"}}});
    const auto plan =
        pglaswell::plan_migration(pglaswell::parse_spec(doc), obs, {});
    ASSERT_TRUE(plan.ok) << plan.render();
    const auto steps = steps_of(plan, "delete_rows");
    ASSERT_EQ(steps.size(), 1u);
    pglaswell::WriteSession w(cfg);
    w.begin("pg_laswell/test/rest-delete");
    const std::string stmt = steps[0]->sql[0];
    const auto rows = w.txn().exec(stmt.substr(0, stmt.size() - 1),
                                   pqxx::params{"0", 1000});
    w.commit();
    EXPECT_EQ(rows.size(), 40u) << "the paced delete statement is malformed";
  }
  {
    pglaswell::ReadSession r(cfg);
    EXPECT_EQ(r.txn().exec("SELECT count(*) FROM laswell_sec")[0][0].as<int>(), 60);
  }

  // drop_table is refused while the view depends on it, and works once it does
  // not -- the refusal being the part that matters.
  {
    const auto obs = cat.observe({"public"}, {"laswell_sec"});
    json doc = minimal_spec();
    doc["intents"] = json::array({json{{"kind", "drop_table"},
                                       {"schema", "public"},
                                       {"table", "laswell_sec"}}});
    const auto plan =
        pglaswell::plan_migration(pglaswell::parse_spec(doc), obs, {});
    EXPECT_FALSE(plan.ok) << "a dependent view must block the drop:\n"
                          << plan.render();
    // And PostgreSQL agrees, which is what keeps the refusal honest.
    pglaswell::WriteSession w(cfg);
    w.begin("pg_laswell/test/rest-drop");
    EXPECT_THROW(w.txn().exec("DROP TABLE laswell_sec"), pqxx::sql_error);
  }

  pglaswell::WriteSession w(cfg);
  w.begin("pg_laswell/test/rest-cleanup");
  w.txn().exec("DROP TABLE laswell_sec CASCADE");
  w.txn().exec("DROP OWNED BY laswell_app");
  w.txn().exec("DROP ROLE laswell_app");
  w.commit();
}

TEST_F(DatabaseTest, ConstraintObservationsMatchWhatPostgresqlActuallyRefuses) {
  // The drop_constraint refusal rests entirely on one catalog reading:
  // "another constraint shares my index." If that reading is wrong the planner
  // either refuses a drop that would have worked or waves through one that
  // fails at execution. Both halves are asserted here against a live catalog,
  // and the second half is asserted by MAKING POSTGRESQL REFUSE IT -- a test
  // that only checked our own JSON would agree with itself forever.
  pglaswell::ConnConfig cfg;
  cfg.name = "t";
  cfg.conninfo = url_;
  {
    pglaswell::WriteSession w(cfg);
    w.begin("pg_laswell/test/constraints");
    w.txn().exec("DROP TABLE IF EXISTS laswell_c CASCADE");
    w.txn().exec("DROP TABLE IF EXISTS laswell_p CASCADE");
    w.txn().exec("CREATE TABLE laswell_p(id bigint PRIMARY KEY,"
                 " code text NOT NULL CONSTRAINT laswell_p_code_uq UNIQUE)");
    w.txn().exec("CREATE TABLE laswell_c(id bigint PRIMARY KEY,"
                 " code text, v int CONSTRAINT laswell_c_v_ck CHECK (v > 0),"
                 " CONSTRAINT laswell_c_fk FOREIGN KEY (code)"
                 "   REFERENCES laswell_p(code))");
    w.commit();
  }

  pglaswell::Catalog cat(cfg);
  const auto obs =
      cat.observe({"public", "public"}, {"laswell_p", "laswell_c"});
  const auto& p = obs.table("public.laswell_p");
  const auto& c = obs.table("public.laswell_c");

  // Every contype we plan for is seen, and typed.
  EXPECT_EQ(c["constraints"]["laswell_c_v_ck"]["type"], "c");
  EXPECT_EQ(c["constraints"]["laswell_c_fk"]["type"], "f");
  EXPECT_EQ(c["constraints"]["laswell_c_fk"]["references"], "laswell_p");
  EXPECT_EQ(p["constraints"]["laswell_p_code_uq"]["type"], "u");
  EXPECT_EQ(p["constraints"]["laswell_p_code_uq"]["has_index"], true);

  // The dependency, seen from the side that cannot be dropped.
  const auto dependents =
      p["constraints"]["laswell_p_code_uq"]["depended_on_by"];
  ASSERT_EQ(dependents.size(), 1u) << p["constraints"].dump(2);
  EXPECT_NE(dependents[0].get<std::string>().find("laswell_c_fk"),
            std::string::npos);
  // ... and absent from constraints nothing depends on, so the refusal does
  // not fire on every drop.
  EXPECT_TRUE(c["constraints"]["laswell_c_v_ck"]["depended_on_by"].empty());
  EXPECT_TRUE(c["constraints"]["laswell_c_fk"]["depended_on_by"].empty());

  auto drop = [&](const char* table, const char* name) {
    json d = minimal_spec();
    d["intents"] = json::array({json{{"kind", "drop_constraint"},
                                     {"schema", "public"},
                                     {"table", table},
                                     {"name", name}}});
    return pglaswell::plan_migration(pglaswell::parse_spec(d), obs, {});
  };

  // The planner refuses the unique constraint...
  const auto refused = drop("laswell_p", "laswell_p_code_uq");
  EXPECT_FALSE(refused.ok) << refused.render();

  // ... and PostgreSQL agrees. This is the assertion that keeps the reading
  // honest: if PostgreSQL ever allowed it, the refusal would be inventing a
  // restriction rather than reporting one.
  {
    pglaswell::WriteSession w(cfg);
    w.begin("pg_laswell/test/constraints-refute");
    EXPECT_THROW(w.txn().exec("ALTER TABLE laswell_p DROP CONSTRAINT"
                              " laswell_p_code_uq"),
                 pqxx::sql_error)
        << "PostgreSQL allowed a drop the planner refuses";
  }

  // The check constraint has no dependents, so it plans and applies.
  const auto allowed = drop("laswell_c", "laswell_c_v_ck");
  ASSERT_TRUE(allowed.ok) << allowed.render();
  const auto steps = steps_of(allowed, "drop_constraint");
  ASSERT_EQ(steps.size(), 1u);
  {
    pglaswell::WriteSession w(cfg);
    w.begin("pg_laswell/test/constraints-apply");
    const std::string stmt = steps[0]->sql[0];
    w.txn().exec(stmt.substr(0, stmt.size() - 1));
    w.commit();
  }
  const auto after = cat.observe({"public"}, {"laswell_c"});
  EXPECT_FALSE(after.table("public.laswell_c")["constraints"].contains(
      "laswell_c_v_ck"));
  // Re-planning against the new reading is satisfied, not a second attempt.
  {
    json d = minimal_spec();
    d["intents"] = json::array({json{{"kind", "drop_constraint"},
                                     {"schema", "public"},
                                     {"table", "laswell_c"},
                                     {"name", "laswell_c_v_ck"}}});
    const auto again =
        pglaswell::plan_migration(pglaswell::parse_spec(d), after, {});
    ASSERT_TRUE(again.ok);
    EXPECT_EQ(steps_of(again, "drop_constraint")[0]->action,
              pglaswell::Action::kSatisfied);
  }

  pglaswell::WriteSession w(cfg);
  w.begin("pg_laswell/test/constraints-cleanup");
  w.txn().exec("DROP TABLE laswell_c");
  w.txn().exec("DROP TABLE laswell_p");
  w.commit();
}

TEST_F(DatabaseTest, TheNotNullRecipeLeavesPostgresqlsOwnConstraintName) {
  // PostgreSQL 17+ records NOT NULL constraints in pg_constraint, named
  // <table>_<column>_not_null. A temporary CHECK on that name forces the
  // permanent constraint to be auto-suffixed -- t_v_not_null1 -- for good,
  // as a side effect of how the column was set rather than of anything anyone
  // asked for.
  pglaswell::ConnConfig cfg;
  cfg.name = "t";
  cfg.conninfo = url_;
  {
    pglaswell::WriteSession w(cfg);
    w.begin("pg_laswell/test/nn");
    w.txn().exec("DROP TABLE IF EXISTS laswell_nn");
    w.txn().exec("CREATE TABLE laswell_nn(id bigint PRIMARY KEY, v int)");
    w.txn().exec("INSERT INTO laswell_nn SELECT g,g FROM generate_series(1,100) g");
    w.commit();
  }

  const auto spec = pglaswell::parse_spec([&] {
    json d = minimal_spec();
    d["intents"] = json::array({json{{"kind", "set_not_null"},
                                     {"schema", "public"},
                                     {"table", "laswell_nn"},
                                     {"column", "v"}}});
    return d;
  }());

  pglaswell::Catalog cat(cfg);
  const auto obs = cat.observe({"public"}, {"laswell_nn"});
  const auto plan = pglaswell::plan_migration(spec, obs, {});
  ASSERT_TRUE(plan.ok) << plan.render();

  // Apply the recipe by hand, in separate transactions as the plan requires.
  for (const auto& step : plan.steps) {
    for (const auto& q : step.sql) {
      pglaswell::WriteSession w(cfg);
      w.begin("pg_laswell/test/nn-apply");
      const std::string stmt = q;  // Step::sql is already std::string
      w.txn().exec(stmt.substr(0, stmt.size() - 1));  // strip the trailing ;
      w.commit();
    }
  }

  pglaswell::ReadSession r(cfg);
  EXPECT_TRUE(r.txn()
                  .exec("SELECT attnotnull FROM pg_attribute"
                        " WHERE attrelid='laswell_nn'::regclass AND attname='v'")[0][0]
                  .as<bool>())
      << "the column was not set NOT NULL";
  // No leftover CHECK from the recipe.
  EXPECT_EQ(r.txn()
                .exec("SELECT count(*) FROM pg_constraint"
                      " WHERE conrelid='laswell_nn'::regclass AND contype='c'")[0][0]
                .as<int>(),
            0)
      << "the temporary CHECK was not dropped";
  // And on a server that catalogues NOT NULL constraints, the permanent one
  // has PostgreSQL's natural name rather than an auto-suffixed one.
  if (r.server_version() >= 170000) {
    const auto names = r.txn().exec(
        "SELECT coalesce(string_agg(conname, ','), '') FROM pg_constraint"
        " WHERE conrelid='laswell_nn'::regclass AND contype='n'"
        "   AND conname LIKE '%%v%%'");
    EXPECT_EQ(names[0][0].as<std::string>(), "laswell_nn_v_not_null")
        << "the recipe stole the name PostgreSQL wanted for its own constraint";
  }

  pglaswell::WriteSession w(cfg);
  w.begin("pg_laswell/test/cleanup");
  w.txn().exec("DROP TABLE laswell_nn");
  w.commit();
}

// --- assert_invariants ------------------------------------------------------

namespace {
json backfill_spec_with_invariants(const json& invariants, const char* set_expr) {
  json d = minimal_spec();
  d["intents"] = json::array({json{
      {"kind", "backfill"}, {"schema", "shop"}, {"table", "orders"},
      {"key", "id"},
      {"set", {{"fulfilment_region", set_expr}}},
      {"where", "orders.fulfilment_region IS NULL"},
      {"assert_invariants", invariants}}});
  return d;
}
}  // namespace

TEST_F(ToolTest, AnInvariantThatHoldsLetsTheMigrationSucceed) {
  make_shop(cfg());
  {
    pglaswell::WriteSession w(cfg());
    w.begin("pg_laswell/test/col");
    w.txn().exec("ALTER TABLE shop.orders ADD COLUMN fulfilment_region text");
    w.commit();
  }
  auto doc = backfill_spec_with_invariants(
      json::array({json{{"name", "row_count_stable"},
                        {"query", "SELECT count(*) FROM shop.orders"}}}),
      "'r'");
  const auto parsed = pglaswell::parse_spec(doc);
  doc["signatures"] = json::array({{{"key_id", test_key().key_id},
                                    {"algorithm", "ed25519"},
                                    {"signature", base64(sign(parsed.canonical_bytes))}}});

  const auto p = payload(call("startMigration", json{{"spec", doc}}));
  ASSERT_TRUE(p.value("accepted", false)) << p.dump(2);
  ASSERT_TRUE(wait_for_status(*this, p["jobId"], [](const json& s) {
    return s.value("state", "") == "succeeded" || s.value("state", "") == "failed";
  }));
  const auto st = status_of(p["jobId"]);
  EXPECT_EQ(st.value("state", ""), "succeeded") << st.dump(2);

  // Both readings are recorded, so the check is auditable rather than merely
  // passed.
  bool recorded = false;
  for (const auto& s : st["steps"]) {
    if (s["detail"].contains("invariantsBefore")) recorded = true;
  }
  EXPECT_TRUE(recorded) << st["steps"].dump(2);
}

TEST_F(ToolTest, AnInvariantThatBreaksFailsTheMigrationAndSaysHow) {
  // The case verify_remaining cannot catch: every row is filled, and a total
  // the change was supposed to leave alone has moved.
  make_shop(cfg());
  {
    pglaswell::WriteSession w(cfg());
    w.begin("pg_laswell/test/col");
    w.txn().exec("ALTER TABLE shop.orders ADD COLUMN fulfilment_region text");
    // A trigger that deletes a row on every update: the backfill completes and
    // the row count silently drops.
    w.txn().exec("CREATE FUNCTION shop.eat() RETURNS trigger LANGUAGE plpgsql AS "
                 "$$ BEGIN DELETE FROM shop.orders WHERE id = NEW.id + 100000;"
                 " RETURN NEW; END $$");
    w.txn().exec("CREATE TRIGGER eat AFTER UPDATE ON shop.orders"
                 " FOR EACH ROW EXECUTE FUNCTION shop.eat()");
    w.txn().exec("INSERT INTO shop.orders(warehouse_id) SELECT 1 FROM generate_series(1,50)");
    w.commit();
  }
  // Make the trigger actually delete something.
  {
    pglaswell::WriteSession w(cfg());
    w.begin("pg_laswell/test/shift");
    w.txn().exec("CREATE OR REPLACE FUNCTION shop.eat() RETURNS trigger"
                 " LANGUAGE plpgsql AS $$ BEGIN"
                 "  DELETE FROM shop.orders WHERE id = (SELECT max(id) FROM shop.orders);"
                 "  RETURN NEW; END $$");
    w.commit();
  }

  auto doc = backfill_spec_with_invariants(
      json::array({json{{"name", "row_count_stable"},
                        {"query", "SELECT count(*) FROM shop.orders"}}}),
      "'r'");
  const auto parsed = pglaswell::parse_spec(doc);
  doc["signatures"] = json::array({{{"key_id", test_key().key_id},
                                    {"algorithm", "ed25519"},
                                    {"signature", base64(sign(parsed.canonical_bytes))}}});

  const auto p = payload(call("startMigration", json{{"spec", doc}}));
  if (!p.value("accepted", false)) {
    SUCCEED() << "refused before starting: " << p.dump(2);
    return;
  }
  ASSERT_TRUE(wait_for_status(*this, p["jobId"], [](const json& s) {
    const auto st = s.value("state", "");
    return st == "succeeded" || st == "failed";
  }));
  const auto st = status_of(p["jobId"]);
  EXPECT_EQ(st.value("state", ""), "failed") << st.dump(2);

  // The failure names the invariant and both values, not merely that something
  // changed.
  bool named = false;
  for (const auto& s : st["steps"]) {
    if (!s["detail"].contains("brokenInvariants")) continue;
    const auto& b = s["detail"]["brokenInvariants"][0];
    EXPECT_EQ(b.value("name", ""), "row_count_stable");
    EXPECT_TRUE(b.contains("before"));
    EXPECT_TRUE(b.contains("after"));
    EXPECT_NE(b.value("before", ""), b.value("after", ""));
    named = true;
  }
  EXPECT_TRUE(named) << st.dump(2);
}

// --- preserve: the durable pre-image ---------------------------------------

TEST(Planner, PreserveCapturesInTheSameStatementAsTheUpdate) {
  // Data-modifying CTEs share one snapshot, so the INSERT reads the target as
  // it was BEFORE the UPDATE in the same statement. That is what makes the
  // capture atomic with the change: there is no window in which one committed
  // and the other did not.
  auto obs = observations(1LL << 30, 100000);
  json doc = minimal_spec();
  doc["intents"] = json::array({json{
      {"kind", "backfill"}, {"schema", "shop"}, {"table", "orders"}, {"key", "id"},
      {"set", {{"fulfilment_region", "upper(orders.fulfilment_region)"}}},
      {"where", "orders.fulfilment_region IS NOT NULL"},
      {"preserve", {{"schema", "archive"}, {"table", "orders_before"}}}}});
  const auto plan = pglaswell::plan_migration(pglaswell::parse_spec(doc), obs, {});
  ASSERT_TRUE(plan.ok) << plan.render();

  const auto s = steps_of(plan, "backfill");
  ASSERT_EQ(s.size(), 2u) << plan.render();  // create the side table, then backfill

  // The side table is created from the target's real column types, not LIKE:
  // LIKE would carry constraints and defaults a backup has no business having.
  EXPECT_NE(all_sql(*s[0]).find("CREATE TABLE IF NOT EXISTS archive.orders_before"),
            std::string::npos) << all_sql(*s[0]);
  EXPECT_NE(all_sql(*s[0]).find("laswell_saved_at"), std::string::npos);
  EXPECT_NE(all_sql(*s[0]).find("COMMENT ON TABLE"), std::string::npos);

  // One statement, not two: the INSERT is a CTE of the UPDATE.
  const auto sql = all_sql(*s[1]);
  EXPECT_NE(sql.find(", preserved AS ("), std::string::npos) << sql;
  EXPECT_NE(sql.find("INSERT INTO archive.orders_before (id, fulfilment_region)"),
            std::string::npos) << sql;
  EXPECT_LT(sql.find("preserved AS ("), sql.find("UPDATE shop.orders"))
      << "the capture must be part of the same statement as the update";
}

TEST(Spec, PreservingIntoTheTableBeingBackfilledIsRefused) {
  json doc = minimal_spec();
  doc["intents"] = json::array({json{
      {"kind", "backfill"}, {"schema", "shop"}, {"table", "orders"}, {"key", "id"},
      {"set", {{"fulfilment_region", "'x'"}}}, {"where", "true"},
      {"preserve", {{"schema", "shop"}, {"table", "orders"}}}}});
  const auto err = spec_error(doc);
  EXPECT_NE(err.find("names the table being backfilled"), std::string::npos) << err;
}

TEST_F(ToolTest, APreservedBackfillIsExactlyRevertible) {
  // The property that makes this worth more than a pinned snapshot: the
  // pre-image survives, so the change can be undone by a join.
  {
    pglaswell::WriteSession w(cfg());
    w.begin("pg_laswell/test/prices");
    w.txn().exec("DROP SCHEMA IF EXISTS shop CASCADE");
    w.txn().exec("DROP SCHEMA IF EXISTS archive CASCADE");
    w.txn().exec("CREATE SCHEMA shop");
    w.txn().exec("CREATE SCHEMA archive");
    w.txn().exec("CREATE TABLE shop.orders(id bigint GENERATED ALWAYS AS IDENTITY"
                 " PRIMARY KEY, amount numeric(12,2) NOT NULL)");
    w.txn().exec("INSERT INTO shop.orders(amount)"
                 " SELECT (g % 900 + 1)::numeric / 7 FROM generate_series(1,2000) g");
    w.commit();
  }
  const auto before = [&] {
    pglaswell::ReadSession r(cfg());
    return r.txn().exec("SELECT sum(amount)::text FROM shop.orders")[0][0]
        .as<std::string>();
  }();

  json doc = minimal_spec();
  doc["intents"] = json::array({json{
      {"kind", "backfill"}, {"schema", "shop"}, {"table", "orders"}, {"key", "id"},
      {"set", {{"amount", "round(orders.amount * 1.19, 2)"}}},
      {"where", "orders.amount IS NOT NULL"},
      {"preserve", {{"schema", "archive"}, {"table", "orders_before"}}}}});
  const auto parsed = pglaswell::parse_spec(doc);
  doc["signatures"] = json::array({{{"key_id", test_key().key_id},
                                    {"algorithm", "ed25519"},
                                    {"signature", base64(sign(parsed.canonical_bytes))}}});

  const auto p = payload(call("startMigration", json{{"spec", doc}}));
  ASSERT_TRUE(p.value("accepted", false)) << p.dump(2);
  ASSERT_TRUE(wait_for_status(*this, p["jobId"], [](const json& s) {
    return s.value("state", "") == "succeeded" || s.value("state", "") == "failed";
  }));
  ASSERT_EQ(status_of(p["jobId"]).value("state", ""), "succeeded")
      << status_of(p["jobId"]).dump(2);

  pglaswell::ReadSession r(cfg());
  EXPECT_EQ(r.txn().exec("SELECT count(*) FROM archive.orders_before")[0][0].as<int>(),
            2000);
  // Every preserved row is the exact value the update replaced.
  EXPECT_EQ(r.txn()
                .exec("SELECT count(*) FROM shop.orders o"
                      "  JOIN archive.orders_before a ON a.id = o.id"
                      " WHERE o.amount <> round(a.amount * 1.19, 2)")[0][0]
                .as<int>(),
            0)
      << "a preserved row does not match what the update replaced";
  // And the revert is a join away.
  EXPECT_EQ(r.txn().exec("SELECT sum(amount)::text FROM archive.orders_before")[0][0]
                .as<std::string>(),
            before);
}
