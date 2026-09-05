#pragma once

// Migration specifications: parsing, and the closed set of change intents.
//
// A spec is a list of business-level CHANGE INTENTS, not a desired-state
// schema. `add_column a.x numeric` says what to do; it does not restate table
// `a`. There is no whole-schema diff and no need to represent PostgreSQL's DDL
// surface as schema keys.
//
// GOVERNING INVARIANT, enforced by a test:
//
//   The set of intent kinds the binary KNOWS is exactly the set it
//   IMPLEMENTS. There is no recognized-but-unsupported kind.
//
// So an unknown kind is a fatal refusal of the whole spec, never a skip. The
// failure this prevents is a spec written by a newer authoring tool, applied by
// an older binary, silently missing a step, and recorded in the ledger as
// fully applied.

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "canonical.h"

namespace pglaswell {

inline constexpr int kSpecVersion = 1;

enum class IntentKind { kAddColumn, kBackfill, kCreateIndex };

inline const std::map<std::string, IntentKind>& intent_kinds() {
  static const std::map<std::string, IntentKind> kKinds = {
      {"add_column", IntentKind::kAddColumn},
      {"backfill", IntentKind::kBackfill},
      {"create_index", IntentKind::kCreateIndex},
  };
  return kKinds;
}

inline std::string supported_kinds_list() {
  std::string out;
  for (const auto& [name, _] : intent_kinds()) {
    if (!out.empty()) out += ", ";
    out += name;
  }
  return out;
}

struct Intent {
  IntentKind kind;
  std::string kind_name;
  std::size_t ordinal = 0;
  json body;  // the validated intent object, verbatim

  std::string schema() const { return body.value("schema", ""); }
  std::string table() const { return body.value("table", ""); }
  std::string qualified_table() const { return schema() + "." + table(); }
};

struct Spec {
  json document;              // the whole file, as parsed
  json signed_projection;     // the allowlisted subset that is signed
  std::string canonical_bytes;
  std::string digest;         // sha256 hex of canonical_bytes
  std::string id;
  std::string description;
  std::string rationale;
  std::string target_database;
  int min_server_version = 0;
  // Business ordering, which no amount of measurement can derive. Relation
  // overlap says what MUST NOT run together; this says what must run FIRST.
  std::vector<std::string> depends_on;
  std::vector<Intent> intents;
  json signatures = json::array();
};

class SpecError : public std::runtime_error {
 public:
  SpecError(std::string message, std::string hint)
      : std::runtime_error(message), hint_(std::move(hint)) {}
  const std::string& hint() const { return hint_; }

 private:
  std::string hint_;
};

namespace detail {

[[noreturn]] inline void fail(const std::string& what, const std::string& hint) {
  throw SpecError(what, hint);
}

// Identifiers are validated rather than quoted-and-hoped. A spec is signed, so
// this is not an injection defence -- whoever controls the identifier controls
// the change. It is a determinism defence: an identifier needing quoting has a
// different rendering in the plan than in the catalog, and the idempotence
// check would compare unequal things.
inline void require_identifier(const std::string& v, const std::string& field,
                               std::size_t ordinal) {
  const std::string at = "intents[" + std::to_string(ordinal) + "]." + field;
  if (v.empty()) fail(at + " is empty", "Every intent must name its target.");
  if (v.size() > 63) {
    fail(at + " is longer than 63 bytes (\"" + v + "\")",
         "PostgreSQL truncates identifiers at NAMEDATALEN-1, which would make "
         "the planned name and the catalog name disagree.");
  }
  const bool ok_first = (v[0] >= 'a' && v[0] <= 'z') || v[0] == '_';
  if (!ok_first) {
    fail(at + " must start with a lower-case letter or underscore (\"" + v + "\")",
         "Unquoted identifiers are folded to lower case by PostgreSQL; a name "
         "that needs quoting must not be used here.");
  }
  for (const char c : v) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    if (!ok) {
      fail(at + " contains a character that would need quoting (\"" + v + "\")",
           "Use only [a-z0-9_].");
    }
  }
}

inline std::string require_string(const json& obj, const std::string& key,
                                  const std::string& at) {
  if (!obj.contains(key) || !obj[key].is_string() ||
      obj[key].get<std::string>().empty()) {
    fail(at + " is missing a non-empty \"" + key + "\"",
         "Add \"" + key + "\" to that intent.");
  }
  return obj[key].get<std::string>();
}

inline void reject_unknown_keys(const json& obj, const std::set<std::string>& allowed,
                                const std::string& at) {
  for (auto it = obj.begin(); it != obj.end(); ++it) {
    if (allowed.count(it.key()) == 0) {
      std::string list;
      for (const auto& a : allowed) {
        if (!list.empty()) list += ", ";
        list += a;
      }
      fail(at + " has an unknown key \"" + it.key() + "\"",
           "Accepted keys here are: " + list +
               ". An unknown key is refused rather than ignored, because a key "
               "this binary skips and a newer one honours is a silent "
               "difference between what was reviewed and what ran.");
    }
  }
}

}  // namespace detail

// The top-level keys that are covered by the signature. This is an ALLOWLIST,
// and that is the whole point. With a blocklist ("everything except
// signatures") an attacker appends a key the canonicalizer skips and the
// parser honours -- the classic signature-stripping bug. With an allowlist an
// unrecognised key fails parsing before verification even runs.
inline const std::set<std::string>& signed_top_level_keys() {
  static const std::set<std::string> kKeys = {
      "laswell_spec_version", "id", "description", "rationale", "target",
      "depends_on", "intents"};
  return kKeys;
}

// Adding an OPTIONAL key to this allowlist is backward-compatible, and that is
// the allowlist design paying off rather than luck. The signed projection is
// built from allowlisted keys PRESENT in the document, so a spec without
// depends_on canonicalises byte-identically before and after this change and
// its existing signature still verifies. A spec WITH depends_on is refused
// outright by an older binary -- unknown top-level key -- rather than applied
// with its dependencies silently ignored, which is the failure that matters.

inline void parse_add_column(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(
      in.body,
      {"kind", "schema", "table", "column", "type", "nullable", "default",
       "comment"},
      at);
  detail::require_identifier(detail::require_string(in.body, "schema", at), "schema", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "table", at), "table", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "column", at), "column", in.ordinal);
  (void)detail::require_string(in.body, "type", at);

  if (!in.body.contains("nullable") || !in.body["nullable"].is_boolean()) {
    detail::fail(at + " must state \"nullable\" explicitly",
                 "There is no default. NOT NULL on an existing table is a "
                 "different risk class from a nullable add, and which one you "
                 "meant must be written down rather than inferred.");
  }
  (void)detail::require_string(in.body, "comment", at);
}

inline void parse_backfill(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(
      in.body,
      {"kind", "schema", "table", "key", "set", "from", "where",
       "verify_remaining", "assert_invariants"},
      at);
  detail::require_identifier(detail::require_string(in.body, "schema", at), "schema", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "table", at), "table", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "key", at), "key", in.ordinal);

  if (!in.body.contains("set") || !in.body["set"].is_object() ||
      in.body["set"].empty()) {
    detail::fail(at + " needs a non-empty \"set\" object",
                 "Map each target column to the SQL expression that produces "
                 "its value, e.g. {\"region_id\": \"w.region_id\"}.");
  }
  for (auto it = in.body["set"].begin(); it != in.body["set"].end(); ++it) {
    detail::require_identifier(it.key(), "set." + it.key(), in.ordinal);
    if (!it.value().is_string() || it.value().get<std::string>().empty()) {
      detail::fail(at + ".set." + it.key() + " must be a non-empty SQL expression",
                   "Expressions are strings, e.g. \"w.region_id\".");
    }
  }

  // Naming convention, stated here because it is an interface contract:
  // `where`, `set` and `from` reference the target table BY ITS OWN NAME, not
  // by an alias -- "orders.warehouse_id = w.id", never "t.warehouse_id".
  // Tables introduced by `from` are referenced by whatever alias `from` gives
  // them. A spec written against the other convention fails in the dry run
  // with "missing FROM-clause entry", which is why the hint below names this.

  // An unfiltered backfill rewrites every row in the table, which is almost
  // always a mistake and is never what someone means by accident. Refusing it
  // costs one explicit "true" in the rare case it was intended.
  const auto where = in.body.value("where", "");
  if (where.empty()) {
    detail::fail(at + " has no \"where\" clause",
                 "An unfiltered backfill rewrites every row. If that is really "
                 "intended, write \"where\": \"true\" and say so in the "
                 "rationale.");
  }

  if (in.body.contains("assert_invariants")) {
    if (!in.body["assert_invariants"].is_array()) {
      detail::fail(at + ".assert_invariants must be an array", "");
    }
    for (const auto& inv : in.body["assert_invariants"]) {
      if (!inv.is_object()) detail::fail(at + ".assert_invariants entries must be objects", "");
      detail::reject_unknown_keys(inv, {"name", "query"},
                                  at + ".assert_invariants[]");
      (void)detail::require_string(inv, "name", at + ".assert_invariants[]");
      (void)detail::require_string(inv, "query", at + ".assert_invariants[]");
    }
  }
}

inline void parse_create_index(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(
      in.body,
      {"kind", "schema", "table", "name", "columns", "unique", "method",
       "where", "comment", "on_equivalent_index"},
      at);
  detail::require_identifier(detail::require_string(in.body, "schema", at), "schema", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "table", at), "table", in.ordinal);
  // The index name is required rather than derived. A derived name is a name
  // nobody can search for at three in the morning.
  detail::require_identifier(detail::require_string(in.body, "name", at), "name", in.ordinal);

  if (!in.body.contains("columns") || !in.body["columns"].is_array() ||
      in.body["columns"].empty()) {
    detail::fail(at + " needs a non-empty \"columns\" array", "");
  }
  for (const auto& c : in.body["columns"]) {
    if (!c.is_string()) detail::fail(at + ".columns entries must be strings", "");
    detail::require_identifier(c.get<std::string>(), "columns", in.ordinal);
  }
  (void)detail::require_string(in.body, "comment", at);

  // What to do when an index equivalent to this one already exists under a
  // DIFFERENT name -- the common shape of a database where somebody built it by
  // hand. Default is to rename it, so the same spec converges every database on
  // the declared name: it creates the index where it is missing, and corrects
  // the name where it was made by hand.
  if (in.body.contains("on_equivalent_index")) {
    const auto v = in.body["on_equivalent_index"];
    if (!v.is_string() || (v != "rename" && v != "adopt" && v != "refuse")) {
      detail::fail(at + ".on_equivalent_index must be \"rename\", \"adopt\" "
                        "or \"refuse\"",
                   "rename (the default) renames the existing index to the "
                   "declared name; adopt accepts it where it is and changes "
                   "nothing; refuse treats it as a conflict.");
    }
  }
}

// Parses and validates a spec document. Throws SpecError, whose hint names the
// exact thing to change.
inline Spec parse_spec(const json& doc) {
  if (!doc.is_object()) {
    detail::fail("the spec is not a JSON object", "A spec is a single object.");
  }

  for (auto it = doc.begin(); it != doc.end(); ++it) {
    if (it.key() == "signatures") continue;
    if (signed_top_level_keys().count(it.key()) == 0) {
      std::string allowed;
      for (const auto& k : signed_top_level_keys()) {
        if (!allowed.empty()) allowed += ", ";
        allowed += k;
      }
      detail::fail("unknown top-level key \"" + it.key() + "\"",
                   "Accepted top-level keys are: " + allowed +
                       ", signatures. Unknown keys are refused rather than "
                       "ignored: a key outside the signed projection that the "
                       "parser honoured would be a signature bypass.");
    }
  }

  Spec s;
  s.document = doc;

  if (!doc.contains("laswell_spec_version") ||
      !doc["laswell_spec_version"].is_number_integer()) {
    detail::fail("the spec has no integer \"laswell_spec_version\"",
                 "Add \"laswell_spec_version\": " + std::to_string(kSpecVersion) + ".");
  }
  const auto version = doc["laswell_spec_version"].get<int>();
  if (version != kSpecVersion) {
    detail::fail("spec version " + std::to_string(version) +
                     " is not supported by this binary",
                 "This build understands version " +
                     std::to_string(kSpecVersion) +
                     ". Upgrade pg_laswell, or use a spec of that version.");
  }

  s.id = detail::require_string(doc, "id", "the spec");
  // JSON has no comments, and these specs are increasingly written by agents
  // rather than by hand. `description` is how a human says what the change is
  // for; it is required, and it is inside the signature, so the reasoning is
  // signed alongside the change and lands in the ledger.
  s.description = detail::require_string(doc, "description", "the spec");
  s.rationale = doc.value("rationale", "");

  if (doc.contains("depends_on")) {
    if (!doc["depends_on"].is_array()) {
      detail::fail("\"depends_on\" must be an array of spec ids", "");
    }
    for (const auto& d : doc["depends_on"]) {
      if (!d.is_string() || d.get<std::string>().empty()) {
        detail::fail("depends_on entries must be non-empty spec ids", "");
      }
      const auto id = d.get<std::string>();
      if (id == s.id) {
        detail::fail("spec \"" + s.id + "\" depends on itself", "");
      }
      s.depends_on.push_back(id);
    }
  }

  if (doc.contains("target")) {
    if (!doc["target"].is_object()) {
      detail::fail("\"target\" must be an object", "");
    }
    detail::reject_unknown_keys(doc["target"], {"database", "min_server_version"},
                                "target");
    s.target_database = doc["target"].value("database", "");
    if (doc["target"].contains("min_server_version")) {
      if (!doc["target"]["min_server_version"].is_number_integer()) {
        detail::fail("target.min_server_version must be an integer",
                     "Use the PG_VERSION_NUM form, e.g. 150000 for 15.0.");
      }
      s.min_server_version = doc["target"]["min_server_version"].get<int>();
    }
  }

  if (!doc.contains("intents") || !doc["intents"].is_array() ||
      doc["intents"].empty()) {
    detail::fail("the spec has no intents",
                 "Add at least one intent. Supported kinds: " +
                     supported_kinds_list() + ".");
  }

  std::size_t ordinal = 0;
  for (const auto& body : doc["intents"]) {
    const std::string at = "intents[" + std::to_string(ordinal) + "]";
    if (!body.is_object()) detail::fail(at + " is not an object", "");
    const auto kind_name = detail::require_string(body, "kind", at);
    const auto it = intent_kinds().find(kind_name);
    if (it == intent_kinds().end()) {
      detail::fail("unknown intent kind \"" + kind_name + "\" at " + at,
                   "pg_laswell " PGLASWELL_VERSION " supports: " +
                       supported_kinds_list() +
                       ". Upgrade the binary, or split the spec. An unknown "
                       "kind refuses the whole spec rather than skipping the "
                       "step, because a silently missing step recorded as "
                       "applied is the worst outcome available.");
    }

    Intent in;
    in.kind = it->second;
    in.kind_name = kind_name;
    in.ordinal = ordinal;
    in.body = body;
    switch (in.kind) {
      case IntentKind::kAddColumn:   parse_add_column(in);   break;
      case IntentKind::kBackfill:    parse_backfill(in);     break;
      case IntentKind::kCreateIndex: parse_create_index(in); break;
    }
    s.intents.push_back(std::move(in));
    ++ordinal;
  }

  if (doc.contains("signatures")) {
    if (!doc["signatures"].is_array()) {
      detail::fail("\"signatures\" must be an array", "");
    }
    s.signatures = doc["signatures"];
  }

  // The signed projection: the allowlisted keys, and nothing else. Built by
  // copying what is allowed rather than by erasing what is not, so a key added
  // to the document in future is excluded by default instead of included by
  // accident.
  s.signed_projection = json::object();
  for (const auto& k : signed_top_level_keys()) {
    if (doc.contains(k)) s.signed_projection[k] = doc.at(k);
  }
  s.canonical_bytes = canonicalize(s.signed_projection);
  s.digest = to_hex(sha256(s.canonical_bytes));
  return s;
}

}  // namespace pglaswell
