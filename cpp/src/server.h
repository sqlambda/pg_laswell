#pragma once

// pg_laswell — MCP server over stdio, JSON-RPC 2.0.
//
// Hand-rolled protocol, as in pg_licht: nlohmann/json is the only dependency
// the transport needs, and an SDK would be more code to audit than the ~200
// lines it would replace.
//
// Two deliberate departures from pg_licht's server.h, both stated here because
// they are the kind of thing a reader would otherwise assume was an oversight:
//
//  1. TOOLS ARE DECLARED ONCE. pg_licht keeps four parallel static structures
//     per tool -- get_tools_list(), tool_scopes(), tool_output_schemas() and an
//     if/else dispatch chain -- kept in sync only by tests, and its own notes
//     call that a cost not worth inheriting. Here a tool is one ToolDef entry
//     in one vector, and tools/list plus dispatch are both derived from it.
//     Forgetting to register a tool is not possible, because the entry IS the
//     registration.
//
//  2. handle_request() RETURNS the response rather than writing it. pg_licht
//     writes to std::cout from inside the handler, so its tests have to
//     redirect std::cout into a stringstream to observe a response. Returning
//     the value makes the transport testable with no global state, and it
//     matters more here than it did there: this server writes to stdout while
//     worker threads are running, so "who may touch stdout" needs to be one
//     answer -- run() -- rather than a convention.

#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace pglaswell {

using json = nlohmann::json;

// The MCP revisions this server will negotiate, newest last. Revision strings
// are ISO dates, so feature gating is plain string comparison rather than a
// parsed version type.
inline const std::vector<std::string>& supported_protocols() {
  static const std::vector<std::string> kSupported = {
      "2024-11-05", "2025-03-26", "2025-06-18", "2025-11-25"};
  return kSupported;
}

inline constexpr const char* kDefaultProtocol = "2025-06-18";
inline constexpr const char* kServerName = "pg-laswell";

// JSON-RPC error codes we actually emit. Spelled out rather than inlined so a
// grep for the number lands on the name.
inline constexpr int kParseError = -32700;
inline constexpr int kInvalidRequest = -32600;
inline constexpr int kMethodNotFound = -32601;
inline constexpr int kInvalidParams = -32602;

// One tool, declared once. `invoke` receives the already-extracted `arguments`
// object and returns the payload; the framing around it (content block,
// structuredContent, isError) is applied by the caller so no tool can get it
// subtly wrong.
//
// `invoke` is a std::function rather than a plain pointer so a tool can close
// over whatever it needs -- a connection registry, a cache, a job table --
// without this header knowing those types exist. That is what keeps the
// transport free of any dependency on the tools, and therefore free of pqxx.
// A configuration problem, distinct from a database problem.
//
// The two need different hints and the generic handler cannot tell them apart
// from the message alone -- string-sniffing an exception is exactly the mistake
// this project already made once with insufficient_privilege, where matching on
// SQLSTATE rather than on type gave the wrong answer for a whole error class. A
// ConfigError carries its own remedy in its message, so the handler must not
// replace it with advice about a server log for a statement that never ran.
struct ConfigError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

struct ToolDef {
  std::string name;         // camelCase on the wire
  std::string description;  // what the model reads when choosing
  std::function<json()> input_schema;
  std::function<json()> output_schema;
  struct {
    bool read_only = true;
    bool destructive = false;
    bool idempotent = true;
    bool long_running = false;  // returns a jobId; the work outlives the call
  } hints;
  std::function<json(const json& arguments)> invoke;
};

namespace detail {

// An error payload shaped the way every pg_laswell failure is shaped: what
// went wrong, and the exact thing to change. A hint that does not name a
// statement, a config key or a role is not a hint.
inline json error_payload(const std::string& error, const std::string& hint) {
  return json{{"error", error}, {"hint", hint}};
}

inline std::string negotiate_protocol(const json& params) {
  if (!params.is_object() || !params.contains("protocolVersion") ||
      !params["protocolVersion"].is_string()) {
    return kDefaultProtocol;
  }
  const auto requested = params["protocolVersion"].get<std::string>();
  for (const auto& p : supported_protocols()) {
    if (p == requested) return requested;
  }
  // An unknown revision is answered with our newest rather than refused: the
  // client is told what it got and can decide. Refusing here would make every
  // future revision a hard incompatibility with an old binary.
  return supported_protocols().back();
}

}  // namespace detail

class McpServer {
 public:
  McpServer() = default;
  explicit McpServer(std::vector<ToolDef> tools) : tools_(std::move(tools)) {}
  McpServer(const McpServer&) = delete;
  McpServer& operator=(const McpServer&) = delete;

  // Reads line-delimited JSON from stdin, writes line-delimited JSON to
  // stdout. The only place in the process that writes to stdout.
  void run(std::istream& in = std::cin, std::ostream& out = std::cout) {
    std::string line;
    while (std::getline(in, line)) {
      if (line.empty()) continue;
      std::optional<json> response;
      try {
        response = handle_request(json::parse(line));
      } catch (const json::parse_error& e) {
        std::cerr << "Parse error: " << e.what() << std::endl;
        response = json{{"jsonrpc", "2.0"},
                        {"id", nullptr},
                        {"error",
                         {{"code", kParseError}, {"message", "Parse error"}}}};
      } catch (const std::exception& e) {
        std::cerr << "Unhandled exception: " << e.what() << std::endl;
        response = json{{"jsonrpc", "2.0"},
                        {"id", nullptr},
                        {"error",
                         {{"code", kInvalidRequest}, {"message", e.what()}}}};
      }
      if (response) out << response->dump() << std::endl;
    }
  }

  // Returns the response, or nullopt for a notification (which by JSON-RPC
  // definition gets no reply). Public because the tests drive the transport
  // through it directly -- see the header comment.
  std::optional<json> handle_request(const json& req) {
    if (!req.is_object() || !req.contains("method") ||
        !req["method"].is_string()) {
      return make_error(req.value("id", json(nullptr)), kInvalidRequest,
                        "Invalid Request");
    }
    const auto method = req["method"].get<std::string>();
    const json id = req.contains("id") ? req["id"] : json(nullptr);
    const json params =
        req.contains("params") ? req["params"] : json::object();

    // Notifications carry no id and must produce no response at all. Replying
    // to one is a protocol violation that some clients tolerate and others
    // treat as a desync, so it is checked before anything else.
    const bool is_notification = !req.contains("id");

    if (method == "initialize") return make_result(id, initialize(params));
    if (method == "notifications/initialized") return std::nullopt;
    if (method == "ping") return make_result(id, json::object());
    if (method == "tools/list") return make_result(id, tools_list());
    if (method == "tools/call") return tools_call(id, params);

    if (is_notification) return std::nullopt;
    return make_error(id, kMethodNotFound, "Method not found: " + method);
  }

  const std::string& protocol() const { return protocol_; }
  const std::vector<ToolDef>& tools() const { return tools_; }

 private:
  json initialize(const json& params) {
    protocol_ = detail::negotiate_protocol(params);
    return json{
        {"protocolVersion", protocol_},
        {"capabilities", {{"tools", json::object()}}},
        {"serverInfo",
         {{"name", kServerName}, {"version", PGLASWELL_VERSION}}},
        {"instructions",
         "Applies signed migration specs to PostgreSQL. Plan before you apply: "
         "planMigration measures the target and shows the statements it would "
         "run, and startMigration executes exactly that plan. Long-running "
         "work returns a jobId immediately; poll jobStatus for progress, "
         "contention and an ETA. Use pg_licht alongside this server to read "
         "load, locks and capacity."}};
  }

  json tools_list() const {
    json tools = json::array();
    const bool wants_annotations = protocol_ >= "2025-03-26";
    const bool wants_output_schema = protocol_ >= "2025-06-18";

    for (const auto& t : tools_) {
      json entry{{"name", t.name},
                 {"description", t.description},
                 {"inputSchema", t.input_schema()}};
      if (wants_output_schema && t.output_schema) {
        entry["outputSchema"] = t.output_schema();
      }
      if (wants_annotations) {
        entry["annotations"] = {{"readOnlyHint", t.hints.read_only},
                                {"destructiveHint", t.hints.destructive},
                                {"idempotentHint", t.hints.idempotent}};
      }
      tools.push_back(std::move(entry));
    }
    return json{{"tools", std::move(tools)}};
  }

  std::optional<json> tools_call(const json& id, const json& params) {
    if (!params.is_object() || !params.contains("name") ||
        !params["name"].is_string()) {
      return make_error(id, kInvalidParams, "tools/call requires a tool name");
    }
    const auto name = params["name"].get<std::string>();
    const json arguments = params.contains("arguments") &&
                                   params["arguments"].is_object()
                               ? params["arguments"]
                               : json::object();

    for (const auto& t : tools_) {
      if (name != t.name) continue;
      try {
        return make_result(id, tool_result(t.invoke(arguments)));
      } catch (const ConfigError& e) {
        // Its message already names the thing to change, so the hint points at
        // where to change it rather than at a database that was never reached.
        return make_result(
            id, tool_error(detail::error_payload(
                    e.what(),
                    "This is a configuration problem, not a database one: no "
                    "connection was attempted. See CONFIGURATION in "
                    "man pg_laswell_mcp.")));
      } catch (const std::exception& e) {
        // A tool that throws produces an isError result rather than a
        // JSON-RPC error: the model can read and act on the former, whereas a
        // protocol-level error is usually surfaced to the user as a crash.
        return make_result(
            id, tool_error(detail::error_payload(
                    e.what(), "See the server log for the failing statement.")));
      }
    }
    return make_error(id, kMethodNotFound, "Unknown tool: " + name);
  }

  json tool_result(const json& payload) const {
    json out;
    if (protocol_ >= "2025-06-18") out["structuredContent"] = payload;
    // The text block is serialised compactly rather than with dump(2): the
    // indentation buys a caller nothing, since every consumer parses it as
    // JSON. It is sent to every client because a client can advertise a
    // revision whose structuredContent support it does not actually implement,
    // and this server cannot tell which.
    out["content"] = json::array(
        {json{{"type", "text"}, {"text", payload.dump()}}});
    out["isError"] = false;
    return out;
  }

  json tool_error(const json& payload) const {
    json out = tool_result(payload);
    out["isError"] = true;
    return out;
  }

  static json make_result(const json& id, json result) {
    return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
  }

  static json make_error(const json& id, int code, const std::string& message) {
    return json{{"jsonrpc", "2.0"},
                {"id", id},
                {"error", {{"code", code}, {"message", message}}}};
  }

  std::string protocol_ = kDefaultProtocol;
  std::vector<ToolDef> tools_;
};

}  // namespace pglaswell
