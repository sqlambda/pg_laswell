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

enum class IntentKind { kAddColumn, kBackfill, kCreateIndex, kDropIndex,
                        kSetNotNull, kAddForeignKey, kAddCheckConstraint,
                        kDropConstraint, kAlterColumnType, kDropColumn,
                        kReplaceView, kAddUniqueConstraint,
                        kAddPrimaryKey, kAttachPartition,
                        kDetachPartition, kRenameTable, kRenameColumn,
                        kRenameConstraint, kCreateTable, kDropTable,
                        kDeleteRows, kSetRowSecurity, kCreatePolicy,
                        kDropPolicy, kSetTriggerState, kGrant, kRevoke };

inline const std::map<std::string, IntentKind>& intent_kinds() {
  static const std::map<std::string, IntentKind> kKinds = {
      {"add_column", IntentKind::kAddColumn},
      {"backfill", IntentKind::kBackfill},
      {"create_index", IntentKind::kCreateIndex},
      {"drop_index", IntentKind::kDropIndex},
      {"set_not_null", IntentKind::kSetNotNull},
      {"add_foreign_key", IntentKind::kAddForeignKey},
      {"add_check_constraint", IntentKind::kAddCheckConstraint},
      {"drop_constraint", IntentKind::kDropConstraint},
      {"alter_column_type", IntentKind::kAlterColumnType},
      {"drop_column", IntentKind::kDropColumn},
      {"replace_view", IntentKind::kReplaceView},
      {"add_unique_constraint", IntentKind::kAddUniqueConstraint},
      {"add_primary_key", IntentKind::kAddPrimaryKey},
      {"attach_partition", IntentKind::kAttachPartition},
      {"detach_partition", IntentKind::kDetachPartition},
      {"rename_table", IntentKind::kRenameTable},
      {"rename_column", IntentKind::kRenameColumn},
      {"rename_constraint", IntentKind::kRenameConstraint},
      {"create_table", IntentKind::kCreateTable},
      {"drop_table", IntentKind::kDropTable},
      {"delete_rows", IntentKind::kDeleteRows},
      {"set_row_security", IntentKind::kSetRowSecurity},
      {"create_policy", IntentKind::kCreatePolicy},
      {"drop_policy", IntentKind::kDropPolicy},
      {"set_trigger_state", IntentKind::kSetTriggerState},
      {"grant", IntentKind::kGrant},
      {"revoke", IntentKind::kRevoke},
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
       "verify_remaining", "assert_invariants", "preserve"},
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

  // The durable answer to "what did this row look like before".
  //
  // A predicate-driven backfill needs none of this: its own WHERE clause is
  // the record of what remains. A LOSSY one -- SET amount = amount * 1.1 --
  // has no such record, and the honest thing is not to VIEW the previous
  // values from some pinned snapshot but to KEEP them, in the same transaction
  // as the change, where they survive a crash and double as the revert path.
  if (in.body.contains("preserve")) {
    const auto& p = in.body["preserve"];
    if (!p.is_object()) {
      detail::fail(at + ".preserve must be an object", "");
    }
    detail::reject_unknown_keys(p, {"schema", "table"}, at + ".preserve");
    detail::require_identifier(detail::require_string(p, "schema", at + ".preserve"),
                               "preserve.schema", in.ordinal);
    detail::require_identifier(detail::require_string(p, "table", at + ".preserve"),
                               "preserve.table", in.ordinal);
    if (p.value("schema", "") == in.body.value("schema", "") &&
        p.value("table", "") == in.body.value("table", "")) {
      detail::fail(at + ".preserve names the table being backfilled",
                   "The pre-image has to go somewhere else, or the backfill "
                   "would rewrite the record of what it changed.");
    }
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

inline void parse_drop_index(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(in.body, {"kind", "schema", "table", "name"}, at);
  detail::require_identifier(detail::require_string(in.body, "schema", at), "schema", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "table", at), "table", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "name", at), "name", in.ordinal);
  // No comment key: dropping an object cannot document one.
}

inline void parse_set_not_null(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(
      in.body, {"kind", "schema", "table", "column", "keep_check"}, at);
  detail::require_identifier(detail::require_string(in.body, "schema", at), "schema", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "table", at), "table", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "column", at), "column", in.ordinal);
  if (in.body.contains("keep_check") && !in.body["keep_check"].is_boolean()) {
    detail::fail(at + ".keep_check must be a boolean",
                 "The CHECK constraint is redundant once the column is NOT "
                 "NULL and costs time on every insert, so it is dropped by "
                 "default. Set keep_check to true to keep it.");
  }
}

inline void parse_add_foreign_key(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(
      in.body,
      {"kind", "schema", "table", "name", "columns", "references_schema",
       "references_table", "references_columns", "on_delete", "on_update"},
      at);
  detail::require_identifier(detail::require_string(in.body, "schema", at), "schema", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "table", at), "table", in.ordinal);
  // Named rather than derived, for the same reason an index is: a derived name
  // is a name nobody can search for during an incident.
  detail::require_identifier(detail::require_string(in.body, "name", at), "name", in.ordinal);
  detail::require_identifier(
      detail::require_string(in.body, "references_schema", at), "references_schema", in.ordinal);
  detail::require_identifier(
      detail::require_string(in.body, "references_table", at), "references_table", in.ordinal);

  for (const char* key : {"columns", "references_columns"}) {
    if (!in.body.contains(key) || !in.body[key].is_array() || in.body[key].empty()) {
      detail::fail(at + " needs a non-empty \"" + key + "\" array", "");
    }
    for (const auto& c : in.body[key]) {
      if (!c.is_string()) detail::fail(at + "." + key + " entries must be strings", "");
      detail::require_identifier(c.get<std::string>(), key, in.ordinal);
    }
  }
  if (in.body["columns"].size() != in.body["references_columns"].size()) {
    detail::fail(at + " has " + std::to_string(in.body["columns"].size()) +
                     " columns and " +
                     std::to_string(in.body["references_columns"].size()) +
                     " references_columns",
                 "A foreign key maps its columns one to one.");
  }
  static const std::set<std::string> kActions = {
      "NO ACTION", "RESTRICT", "CASCADE", "SET NULL", "SET DEFAULT"};
  for (const char* key : {"on_delete", "on_update"}) {
    if (!in.body.contains(key)) continue;
    const auto v = in.body.value(key, "");
    if (kActions.count(v) == 0) {
      std::string list;
      for (const auto& a : kActions) {
        if (!list.empty()) list += ", ";
        list += a;
      }
      detail::fail(at + "." + std::string(key) + " must be one of: " + list,
                   "Spelled exactly as PostgreSQL spells them, upper case.");
    }
  }
}

inline void parse_add_check_constraint(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(in.body,
                              {"kind", "schema", "table", "name", "expression"}, at);
  detail::require_identifier(detail::require_string(in.body, "schema", at), "schema", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "table", at), "table", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "name", at), "name", in.ordinal);
  (void)detail::require_string(in.body, "expression", at);
}

// A view body is business logic, like backfill's set/where. Enumerating a
// SELECT grammar as JSON keys is the unbounded-reimplementation trap the intent
// model exists to avoid, so the definition is written as SQL and signed as SQL.
// add_unique_constraint and add_primary_key take the same body: both are
// backed by a unique index, and both are planned through it.
// Bounds are STRUCTURED rather than a raw FOR VALUES clause, which is a
// departure from how backfill takes SQL -- and the reason is that pg_laswell
// has to render the bounds TWICE, in two different syntaxes: once as FOR VALUES
// for the attach, and once as a CHECK constraint, which is what turns the
// attach from a full scan into a catalog change. A raw clause could be copied
// into the first and not derived for the second.
// The three renames share a body: what is being renamed, and to what. `what`
// names the extra key -- "column" or "name" -- that a column or constraint
// rename needs and a table rename does not.
// Columns are STRUCTURED, not a DDL fragment.
//
// This is where the intent model is most at risk of becoming a SQL grammar
// written in JSON, so the vocabulary is deliberately the same one add_column
// already uses -- name, type, nullable, default, comment -- and nothing more.
// Anything a table needs beyond that (an index, a foreign key, a check, a
// primary key over several columns) has its own intent kind already, and
// stating it there keeps each change separately reviewable instead of buried
// in a CREATE TABLE nobody reads to the end.
// A retention purge is a migration by both tests: it changes state, and it is
// applied once. It is paced exactly as a backfill is, because a single DELETE
// over a retention window is the same outage a single UPDATE would be.
inline void parse_set_row_security(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(in.body, {"kind", "schema", "table", "enabled", "force"}, at);
  detail::require_identifier(detail::require_string(in.body, "schema", at), "schema", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "table", at), "table", in.ordinal);
  if (!in.body.contains("enabled") || !in.body["enabled"].is_boolean()) {
    detail::fail(at + ".enabled must be stated as a boolean",
                 "Row-level security is on or off; there is no default, "
                 "because turning it on with no policy hides every row.");
  }
  if (in.body.contains("force") && !in.body["force"].is_boolean()) {
    detail::fail(at + ".force must be a boolean", "");
  }
}

inline void parse_create_policy(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(
      in.body, {"kind", "schema", "table", "name", "command", "roles", "using",
                "check"}, at);
  detail::require_identifier(detail::require_string(in.body, "schema", at), "schema", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "table", at), "table", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "name", at), "name", in.ordinal);
  if (!in.body.contains("using") && !in.body.contains("check")) {
    detail::fail(at + " needs a \"using\" or \"check\" expression",
                 "A policy with neither restricts nothing and grants nothing; "
                 "it is almost certainly not what was meant.");
  }
  for (const char* k : {"using", "check", "command"}) {
    if (in.body.contains(k) && !in.body[k].is_string()) {
      detail::fail(at + "." + k + " must be a string", "");
    }
  }
  if (in.body.contains("roles")) {
    if (!in.body["roles"].is_array() || in.body["roles"].empty()) {
      detail::fail(at + ".roles must be a non-empty array", "Omit it for PUBLIC.");
    }
    for (const auto& r : in.body["roles"]) {
      if (!r.is_string()) detail::fail(at + ".roles must be strings", "");
      detail::require_identifier(r.get<std::string>(), "roles", in.ordinal);
    }
  }
}

inline void parse_drop_policy(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(in.body, {"kind", "schema", "table", "name"}, at);
  detail::require_identifier(detail::require_string(in.body, "schema", at), "schema", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "table", at), "table", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "name", at), "name", in.ordinal);
}

inline void parse_set_trigger_state(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(in.body, {"kind", "schema", "table", "trigger", "enabled"}, at);
  detail::require_identifier(detail::require_string(in.body, "schema", at), "schema", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "table", at), "table", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "trigger", at), "trigger", in.ordinal);
  if (!in.body.contains("enabled") || !in.body["enabled"].is_boolean()) {
    detail::fail(at + ".enabled must be stated as a boolean", "");
  }
}

inline void parse_grant_like(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  const bool granting = in.kind == IntentKind::kGrant;
  const std::string who = granting ? "to" : "from";
  detail::reject_unknown_keys(
      in.body, {"kind", "schema", "table", "privileges", "columns", who}, at);
  detail::require_identifier(detail::require_string(in.body, "schema", at), "schema", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "table", at), "table", in.ordinal);
  if (!in.body.contains("privileges") || !in.body["privileges"].is_array() ||
      in.body["privileges"].empty()) {
    detail::fail(at + ".privileges must be a non-empty array",
                 "SELECT, INSERT, UPDATE, DELETE, TRUNCATE, REFERENCES, "
                 "TRIGGER, or ALL.");
  }
  static const std::set<std::string> kPrivs = {
      "SELECT", "INSERT", "UPDATE", "DELETE", "TRUNCATE",
      "REFERENCES", "TRIGGER", "ALL"};
  for (const auto& pv : in.body["privileges"]) {
    if (!pv.is_string() || kPrivs.count(pv.get<std::string>()) == 0) {
      detail::fail(at + ".privileges has an unknown privilege: " + pv.dump(),
                   "Written in upper case, one of SELECT, INSERT, UPDATE, "
                   "DELETE, TRUNCATE, REFERENCES, TRIGGER, ALL. They are not "
                   "passed through unchecked, because a typo would otherwise "
                   "reach the database as SQL.");
    }
  }
  if (!in.body.contains(who) || !in.body[who].is_array() || in.body[who].empty()) {
    detail::fail(at + "." + who + " must be a non-empty array of roles",
                 "Use \"PUBLIC\" for everyone.");
  }
  for (const auto& r : in.body[who]) {
    if (!r.is_string()) detail::fail(at + "." + who + " must be strings", "");
  }
  if (in.body.contains("columns")) {
    if (!in.body["columns"].is_array() || in.body["columns"].empty()) {
      detail::fail(at + ".columns must be a non-empty array", "");
    }
    for (const auto& cn : in.body["columns"]) {
      if (!cn.is_string()) detail::fail(at + ".columns must be strings", "");
      detail::require_identifier(cn.get<std::string>(), "columns", in.ordinal);
    }
  }
}

inline void parse_delete_rows(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(
      in.body, {"kind", "schema", "table", "key", "where", "verify_remaining",
                "preserve"}, at);
  detail::require_identifier(detail::require_string(in.body, "schema", at), "schema", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "table", at), "table", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "key", at), "key", in.ordinal);
  // Read with value() rather than require_string(), so the refusal below is the
  // one the author sees. require_string rejects an empty string first, with a
  // generic message -- which made this hint dead code until a test noticed.
  // Worded to match backfill's, because it is the same mistake.
  const auto where = in.body.value("where", "");
  if (!in.body.contains("where") || !in.body["where"].is_string() || where.empty()) {
    detail::fail(at + " has no \"where\" clause",
                 "An unfiltered delete empties the table. If that is really "
                 "intended, write \"where\": \"true\" and say so in the "
                 "rationale -- and consider whether drop_table is what you "
                 "meant instead.");
  }
  if (in.body.contains("verify_remaining") &&
      !in.body["verify_remaining"].is_string()) {
    detail::fail(at + ".verify_remaining must be a string", "");
  }
}

inline void parse_create_table(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(
      in.body, {"kind", "schema", "table", "columns", "comment", "primary_key",
                "partition_by", "unlogged"}, at);
  detail::require_identifier(detail::require_string(in.body, "schema", at), "schema", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "table", at), "table", in.ordinal);
  detail::require_string(in.body, "comment", at);
  if (!in.body.contains("columns") || !in.body["columns"].is_array() ||
      in.body["columns"].empty()) {
    detail::fail(at + ".columns must be a non-empty array",
                 "Each entry needs name, type, nullable and comment -- the "
                 "same vocabulary add_column uses.");
  }
  for (const auto& col : in.body["columns"]) {
    if (!col.is_object()) {
      detail::fail(at + ".columns entries must be objects", "");
    }
    detail::reject_unknown_keys(
        col, {"name", "type", "nullable", "default", "comment"},
        at + ".columns");
    detail::require_identifier(detail::require_string(col, "name", at), "name", in.ordinal);
    detail::require_string(col, "type", at);
    detail::require_string(col, "comment", at);
    if (!col.contains("nullable") || !col["nullable"].is_boolean()) {
      detail::fail(at + ".columns." + col.value("name", "?") +
                       ".nullable must be stated as a boolean",
                   "There is no default, for the same reason add_column has "
                   "none: NOT NULL is a different risk class and must be "
                   "written down.");
    }
  }
  if (in.body.contains("primary_key")) {
    if (!in.body["primary_key"].is_array() || in.body["primary_key"].empty()) {
      detail::fail(at + ".primary_key must be a non-empty array of columns", "");
    }
    for (const auto& c : in.body["primary_key"]) {
      if (!c.is_string()) detail::fail(at + ".primary_key must be strings", "");
    }
  }
}

inline void parse_drop_table(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(in.body, {"kind", "schema", "table"}, at);
  detail::require_identifier(detail::require_string(in.body, "schema", at), "schema", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "table", at), "table", in.ordinal);
  // No CASCADE, as everywhere else. Measured (S17): DROP TABLE ... CASCADE
  // removes dependent views and inbound foreign keys, none of which the spec
  // named.
}

inline void parse_rename(Intent& in, const std::string& what) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  std::set<std::string> allowed = {"kind", "schema", "table", "to"};
  if (!what.empty()) allowed.insert(what);
  detail::reject_unknown_keys(in.body, allowed, at);
  detail::require_identifier(detail::require_string(in.body, "schema", at), "schema", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "table", at), "table", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "to", at), "to", in.ordinal);
  if (!what.empty()) {
    detail::require_identifier(detail::require_string(in.body, what, at), what, in.ordinal);
  }
}

inline void parse_attach_partition(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(
      in.body, {"kind", "schema", "table", "partition", "from", "to", "values",
                "default"}, at);
  detail::require_identifier(detail::require_string(in.body, "schema", at), "schema", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "table", at), "table", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "partition", at),
                             "partition", in.ordinal);
  const bool range = in.body.contains("from") || in.body.contains("to");
  const bool list = in.body.contains("values");
  const bool deflt = in.body.value("default", false);
  if (range + list + deflt != 1) {
    detail::fail(at + " must state exactly one kind of bound",
                 "Use \"from\" and \"to\" for a range partition, \"values\" "
                 "for a list partition, or \"default\": true. They are "
                 "mutually exclusive, and one is required.");
  }
  if (range) {
    detail::require_string(in.body, "from", at);
    detail::require_string(in.body, "to", at);
  }
  if (list) {
    if (!in.body["values"].is_array() || in.body["values"].empty()) {
      detail::fail(at + ".values must be a non-empty array",
                   "List the key values this partition accepts.");
    }
    for (const auto& v : in.body["values"]) {
      if (!v.is_string()) {
        detail::fail(at + ".values must be strings",
                     "Write each value as it would appear in SQL, quotes "
                     "included, so the spec is what is executed.");
      }
    }
  }
}

inline void parse_detach_partition(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(in.body, {"kind", "schema", "table", "partition"}, at);
  detail::require_identifier(detail::require_string(in.body, "schema", at), "schema", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "table", at), "table", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "partition", at),
                             "partition", in.ordinal);
}

inline void parse_unique_like(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(in.body, {"kind", "schema", "table", "name", "columns"}, at);
  detail::require_identifier(detail::require_string(in.body, "schema", at), "schema", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "table", at), "table", in.ordinal);
  // The name is required rather than derived. PostgreSQL renames the backing
  // index to the constraint name, so a derived name is a name that appears in
  // the catalog, in pg_stat_user_indexes and in every monitoring dashboard --
  // without anyone having written it down.
  detail::require_identifier(detail::require_string(in.body, "name", at), "name", in.ordinal);
  if (!in.body.contains("columns") || !in.body["columns"].is_array() ||
      in.body["columns"].empty()) {
    detail::fail(at + ".columns must be a non-empty array",
                 "List the columns the constraint covers, in order.");
  }
  for (const auto& c : in.body["columns"]) {
    if (!c.is_string()) detail::fail(at + ".columns must be strings", "");
    detail::require_identifier(c.get<std::string>(), "columns", in.ordinal);
  }
}

inline void parse_replace_view(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(
      in.body, {"kind", "schema", "name", "definition", "comment"}, at);
  detail::require_identifier(detail::require_string(in.body, "schema", at), "schema", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "name", at), "name", in.ordinal);
  const auto def = detail::require_string(in.body, "definition", at);
  if (def.empty()) {
    detail::fail(at + ".definition must not be empty",
                 "A view needs a query. To remove a view, use a drop intent "
                 "rather than an empty definition.");
  }
  // The definition is a query, not a statement. Accepting "CREATE VIEW ..."
  // here would mean the tool no longer chooses between CREATE OR REPLACE and a
  // drop-and-rebuild -- which is the only decision this kind makes.
  std::string head = def.substr(0, 6);
  for (auto& c : head) c = static_cast<char>(std::tolower(c));
  if (head == "create") {
    detail::fail(
        at + ".definition is the view's QUERY, not a CREATE statement",
        "Give the SELECT alone. pg_laswell chooses between CREATE OR REPLACE "
        "and a drop-and-rebuild by reading the catalog, and a definition that "
        "already carries the statement takes that decision away from it.");
  }
}

inline void parse_alter_column_type(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(
      in.body, {"kind", "schema", "table", "column", "type", "using"}, at);
  detail::require_identifier(detail::require_string(in.body, "schema", at), "schema", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "table", at), "table", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "column", at), "column", in.ordinal);
  detail::require_string(in.body, "type", at);
  // "using" is optional and carries SQL, like backfill's set/where. It is the
  // only way to express a conversion PostgreSQL has no cast for, and refusing
  // it would push those changes out of the repository entirely -- which is the
  // failure mode drop_constraint was reinstated to avoid.
  if (in.body.contains("using") && !in.body["using"].is_string()) {
    throw std::runtime_error(at + ".using must be a string");
  }
}

inline void parse_drop_column(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(in.body, {"kind", "schema", "table", "column"}, at);
  detail::require_identifier(detail::require_string(in.body, "schema", at), "schema", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "table", at), "table", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "column", at), "column", in.ordinal);
  // No CASCADE, for the same reason drop_constraint has none, and with sharper
  // consequences here: measured on 18.6, DROP ... CASCADE on a table under a
  // three-level view stack left ZERO views standing. The dependent views are
  // rebuilt and restored instead.
}

inline void parse_drop_constraint(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(in.body, {"kind", "schema", "table", "name"}, at);
  detail::require_identifier(detail::require_string(in.body, "schema", at), "schema", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "table", at), "table", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "name", at), "name", in.ordinal);
  // No CASCADE key, and that is deliberate: CASCADE drops objects the spec
  // never named, which is exactly what a signed change must not do. Drop the
  // dependents in their own intents, where they are visible and reviewable.
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
      case IntentKind::kDropIndex:   parse_drop_index(in);   break;
      case IntentKind::kSetNotNull:  parse_set_not_null(in); break;
      case IntentKind::kAddForeignKey: parse_add_foreign_key(in); break;
      case IntentKind::kAddCheckConstraint: parse_add_check_constraint(in); break;
      case IntentKind::kDropConstraint: parse_drop_constraint(in); break;
    case IntentKind::kAlterColumnType: parse_alter_column_type(in); break;
    case IntentKind::kDropColumn: parse_drop_column(in); break;
    case IntentKind::kReplaceView: parse_replace_view(in); break;
    case IntentKind::kAddUniqueConstraint:
    case IntentKind::kAddPrimaryKey: parse_unique_like(in); break;
    case IntentKind::kAttachPartition: parse_attach_partition(in); break;
    case IntentKind::kDetachPartition: parse_detach_partition(in); break;
    case IntentKind::kRenameTable: parse_rename(in, {}); break;
    case IntentKind::kRenameColumn: parse_rename(in, "column"); break;
    case IntentKind::kRenameConstraint: parse_rename(in, "name"); break;
    case IntentKind::kCreateTable: parse_create_table(in); break;
    case IntentKind::kDropTable: parse_drop_table(in); break;
    case IntentKind::kDeleteRows: parse_delete_rows(in); break;
    case IntentKind::kSetRowSecurity: parse_set_row_security(in); break;
    case IntentKind::kCreatePolicy: parse_create_policy(in); break;
    case IntentKind::kDropPolicy: parse_drop_policy(in); break;
    case IntentKind::kSetTriggerState: parse_set_trigger_state(in); break;
    case IntentKind::kGrant:
    case IntentKind::kRevoke: parse_grant_like(in); break;
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
