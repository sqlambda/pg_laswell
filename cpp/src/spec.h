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
                        kDropPolicy, kSetTriggerState, kGrant, kRevoke,
                        kCreateSchema, kDropSchema, kCreateExtension,
                        kDropExtension, kCreateType, kDropType, kAddEnumValue,
                        kCreateFunction, kDropFunction, kCreateTrigger,
                        kDropTrigger, kCreateSequence, kDropSequence,
                        kDropView, kAlterColumnDefault, kDropNotNull,
                        kAlterSequence, kAlterSchema, kAlterExtension,
                        kAlterDomain, kAlterFunction, kAlterView,
                        kAlterPolicy, kSetComment, kSetOwner,
                        kSetIdentity, kDropExpression, kSetColumnOptions,
                        kSetTableOptions, kSetLogged, kSetTablespace,
                        kSetAccessMethod, kSetReplicaIdentity, kClusterOn,
                        kCreateMaterializedView, kCreateStatistics,
                        kDropStatistics, kCreateRule, kDropRule,
                        kCreatePublication, kAlterPublication,
                        kDropPublication, kCreateSubscription,
                        kAlterSubscription, kDropSubscription,
                        kCreateObject, kDropObject, kAlterObject,
                        kCreateTableAs, kImportForeignSchema, kSecurityLabel,
                        kAlterDefaultPrivileges,
                        kInsertRows, kUpdateRows, kMergeRows, kCopyRows };

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
      {"create_schema", IntentKind::kCreateSchema},
      {"drop_schema", IntentKind::kDropSchema},
      {"create_extension", IntentKind::kCreateExtension},
      {"drop_extension", IntentKind::kDropExtension},
      {"create_type", IntentKind::kCreateType},
      {"drop_type", IntentKind::kDropType},
      {"add_enum_value", IntentKind::kAddEnumValue},
      {"create_function", IntentKind::kCreateFunction},
      {"drop_function", IntentKind::kDropFunction},
      {"create_trigger", IntentKind::kCreateTrigger},
      {"drop_trigger", IntentKind::kDropTrigger},
      {"create_sequence", IntentKind::kCreateSequence},
      {"drop_sequence", IntentKind::kDropSequence},
      {"drop_view", IntentKind::kDropView},
      {"alter_column_default", IntentKind::kAlterColumnDefault},
      {"drop_not_null", IntentKind::kDropNotNull},
      {"alter_sequence", IntentKind::kAlterSequence},
      {"alter_schema", IntentKind::kAlterSchema},
      {"alter_extension", IntentKind::kAlterExtension},
      {"alter_domain", IntentKind::kAlterDomain},
      {"alter_function", IntentKind::kAlterFunction},
      {"alter_view", IntentKind::kAlterView},
      {"alter_policy", IntentKind::kAlterPolicy},
      {"set_comment", IntentKind::kSetComment},
      {"set_owner", IntentKind::kSetOwner},
      {"set_identity", IntentKind::kSetIdentity},
      {"drop_expression", IntentKind::kDropExpression},
      {"set_column_options", IntentKind::kSetColumnOptions},
      {"set_table_options", IntentKind::kSetTableOptions},
      {"set_logged", IntentKind::kSetLogged},
      {"set_tablespace", IntentKind::kSetTablespace},
      {"set_access_method", IntentKind::kSetAccessMethod},
      {"set_replica_identity", IntentKind::kSetReplicaIdentity},
      {"cluster_on", IntentKind::kClusterOn},
      {"create_materialized_view", IntentKind::kCreateMaterializedView},
      {"create_statistics", IntentKind::kCreateStatistics},
      {"drop_statistics", IntentKind::kDropStatistics},
      {"create_rule", IntentKind::kCreateRule},
      {"drop_rule", IntentKind::kDropRule},
      {"create_publication", IntentKind::kCreatePublication},
      {"alter_publication", IntentKind::kAlterPublication},
      {"drop_publication", IntentKind::kDropPublication},
      {"create_subscription", IntentKind::kCreateSubscription},
      {"alter_subscription", IntentKind::kAlterSubscription},
      {"drop_subscription", IntentKind::kDropSubscription},
      {"create_object", IntentKind::kCreateObject},
      {"drop_object", IntentKind::kDropObject},
      {"alter_object", IntentKind::kAlterObject},
      {"create_table_as", IntentKind::kCreateTableAs},
      {"import_foreign_schema", IntentKind::kImportForeignSchema},
      {"security_label", IntentKind::kSecurityLabel},
      {"alter_default_privileges", IntentKind::kAlterDefaultPrivileges},
      {"insert_rows", IntentKind::kInsertRows},
      {"update_rows", IntentKind::kUpdateRows},
      {"merge_rows", IntentKind::kMergeRows},
      {"copy_rows", IntentKind::kCopyRows},
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

// The non-relation object an intent needs measured, as "kind:schema.name" --
// or "" for the intents that only touch tables. Declared once, here, so the
// observation and the planner cannot disagree about the key: they did, briefly,
// and every object read back as absent.
inline std::string object_key_for(const Intent& in) {
  const auto schema = in.body.value("schema", "");
  const auto name = in.body.value("name", "");
  switch (in.kind) {
    case IntentKind::kCreateSchema:
    case IntentKind::kDropSchema:
    case IntentKind::kAlterSchema:     return "schema:" + schema;
    case IntentKind::kCreateExtension:
    case IntentKind::kDropExtension:
    case IntentKind::kAlterExtension:  return "extension:" + name;
    case IntentKind::kCreateType:
    case IntentKind::kDropType:
    case IntentKind::kAddEnumValue:
    case IntentKind::kAlterDomain:     return "type:" + schema + "." + name;
    case IntentKind::kCreateFunction:
    case IntentKind::kDropFunction:
    case IntentKind::kAlterFunction:   return "function:" + schema + "." + name;
    case IntentKind::kCreateSequence:
    case IntentKind::kDropSequence:
    case IntentKind::kAlterSequence:   return "sequence:" + schema + "." + name;
    default:                           return {};
  }
}

// Everything an intent serialises against.
//
// This is the single answer used by BOTH the repository's concurrency grouping
// and the executor's advisory locks. They must not disagree, and before this
// existed they agreed only by sharing a bug: both keyed on qualified_table(),
// which is schema() + "." + table() -- and object intents carry `name`, not
// `table`. Measured 2026-09-05, create_type public.status_a and
// create_type public.status_b both came back as the relation "public.", and
// create_extension pg_trgm as ".". Two wrong directions at once: unrelated
// objects serialised against each other, and a drop of a type did NOT share a
// key with a column using it, so they could run at the same time.
//
// Relations keep their bare "schema.table" form so the repository's
// EXPLAIN-derived relations and foreign-key edges merge with these directly.
// Objects are "kind:name", which cannot collide with a qualified relation.
inline std::vector<std::string> conflict_keys(const Intent& in) {
  std::vector<std::string> keys;
  const auto schema = in.body.value("schema", "");
  const auto add = [&](const std::string& k) {
    if (!k.empty() && k != "." && k.back() != '.') keys.push_back(k);
  };

  const auto object = object_key_for(in);
  if (!object.empty()) add(object);
  if (!in.table().empty()) add(in.qualified_table());

  // The edges. A key is only useful if it catches the conflict that is NOT
  // between two intents naming the same thing -- a column of a type against a
  // drop of that type, a trigger against the function it calls.
  switch (in.kind) {
    case IntentKind::kAddColumn:
    case IntentKind::kAlterColumnType: {
      // A column's type may be a user-defined one in any schema. Unqualified
      // names are left alone: they are built-in types or resolve by
      // search_path, and inventing a schema for them would produce a key that
      // matches nothing.
      const auto type = in.body.value("type", "");
      const auto dot = type.find('.');
      if (dot != std::string::npos) {
        auto base = type.substr(0, type.find('('));
        while (!base.empty() && (base.back() == ' ' || base.back() == '[')) base.pop_back();
        add("type:" + base);
      }
      return keys;
    }
    case IntentKind::kCreateTable: {
      for (const auto& c : in.body.value("columns", json::array())) {
        const auto type = c.value("type", "");
        if (type.find('.') != std::string::npos) {
          add("type:" + type.substr(0, type.find('(')));
        }
      }
      return keys;
    }
    case IntentKind::kCreateTrigger: {
      // "shop.touch()" -> function:shop.touch
      auto fn = in.body.value("function", "");
      const auto paren = fn.find('(');
      if (paren != std::string::npos) fn = fn.substr(0, paren);
      if (fn.find('.') != std::string::npos) add("function:" + fn);
      return keys;
    }
    case IntentKind::kAttachPartition:
    case IntentKind::kDetachPartition:
      add(schema + "." + in.body.value("partition", ""));
      return keys;
    case IntentKind::kRenameTable:
      add(schema + "." + in.body.value("to", ""));
      return keys;
    case IntentKind::kAddForeignKey:
      add(in.body.value("references_schema", "") + "." +
          in.body.value("references_table", ""));
      return keys;
    // Kinds whose RELATION is named in "name" rather than "table". Missing one
    // here does not just weaken the locking -- tools.h derives what to observe
    // from this function, so the relation is never measured and the planner
    // sees it as absent. attach_partition refused every time for exactly that
    // reason until the conformance suite ran it.
    case IntentKind::kReplaceView:
    case IntentKind::kDropView:
    case IntentKind::kAlterView:
    case IntentKind::kCreateMaterializedView:
      add(schema + "." + in.body.value("name", ""));
      return keys;
    default:
      return keys;
  }
}

struct Spec {
  json document;              // the whole file, as parsed
  json signed_projection;     // the allowlisted subset that is signed
  std::string canonical_bytes;
  std::string digest;         // sha256 hex of canonical_bytes
  std::string id;
  std::string description;
  std::string rationale;
  std::string target_database;
  // Which environment this specification is FOR, and which release gates it.
  //
  // Both sit inside the signature -- target is in signed_top_level_keys() and
  // `release` is added to it -- and that is the whole point of putting them in
  // the specification rather than beside it. An environment an operator can
  // edit on the way to production is not a constraint, it is a comment; a
  // signed one cannot be retargeted without re-signing, which is a person
  // deciding again rather than a file being changed.
  //
  // Empty means unconstrained: a specification with no environment applies
  // anywhere, and one with no release tag is not held. Both defaults are
  // backwards compatible with every specification written before this existed.
  std::string target_environment;
  std::string release;
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

// Identifiers are validated here AND quoted when they are rendered into SQL.
// Both, and the division between them is the point.
//
// A spec is signed, so neither is an injection defence -- whoever controls the
// identifier controls the change. Validation is a DETERMINISM defence: an
// identifier PostgreSQL would case-fold has a different rendering in the plan
// than in the catalog, and the idempotence check would compare unequal things.
// So [a-z0-9_] is required, which rules out case-folding and every character
// that would need escaping.
//
// That was once thought to be sufficient, and it is not. It leaves reserved
// words: `order`, `user`, `end`, `desc`, `limit`, `table` are all legal
// PostgreSQL identifiers that match this shape, and all of them are legal
// column names in a real legacy schema -- which is the kind of database this
// tool exists for. Measured on 18.6, unquoted they are a syntax error in almost
// every position a planner emits, and the exceptions are worse than the rule:
// "SELECT ... FROM user.order" fails while "shop.orders.desc" parses, so the
// breakage is by position rather than by name and does not show up in testing
// against ordinary schemas.
//
// So the rule is: raw here, quoted there. The RAW name is the observation key,
// the repository's concurrency key and the executor's advisory-lock key -- all
// three must agree with the catalog, which returns names unquoted. The QUOTED
// name is what reaches SQL, via detail::quote_identifier and
// detail::quote_qualified in planner_base.h. Quoting is unconditional rather
// than "when it looks necessary": deciding case by case needs PostgreSQL's own
// keyword list, and quoting a name that did not need it changes nothing.
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
      "release", "depends_on", "intents"};
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
// --- the non-relation object kinds -----------------------------------------

// --- the ALTER forms for objects we can already create ---------------------

// --- identity, generated columns, storage and physical layout --------------

// --- materialized views, extended statistics, rules ------------------------

// --- logical replication, and the long tail --------------------------------

inline void parse_replication_name(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(in.body, {"kind", "name"}, at);
  detail::require_identifier(detail::require_string(in.body, "name", at), "name", in.ordinal);
}

inline void parse_publication(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(
      in.body, {"kind", "name", "tables", "all_tables", "operations", "add_tables",
                "drop_tables"}, at);
  detail::require_identifier(detail::require_string(in.body, "name", at), "name", in.ordinal);
  for (const char* k : {"tables", "add_tables", "drop_tables"}) {
    if (!in.body.contains(k)) continue;
    if (!in.body[k].is_array() || in.body[k].empty()) {
      detail::fail(std::string(at) + "." + k + " must be a non-empty array",
                   "Each entry is a schema-qualified table name.");
    }
  }
  static const std::set<std::string> kOps = {"insert", "update", "delete", "truncate"};
  for (const auto& o : in.body.value("operations", json::array())) {
    if (!o.is_string() || kOps.count(o.get<std::string>()) == 0) {
      detail::fail(at + ".operations has an unknown operation: " + o.dump(),
                   "Lower case, one of insert, update, delete, truncate.");
    }
  }
}

inline void parse_subscription(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(
      in.body, {"kind", "name", "connection", "publications", "enabled",
                "connect", "slot_name", "refresh"}, at);
  detail::require_identifier(detail::require_string(in.body, "name", at), "name", in.ordinal);
  if (in.body.contains("connection")) {
    const auto conn = detail::require_string(in.body, "connection", at);
    // A subscription's connection string is stored in pg_subscription in the
    // CLEAR -- verified 2026-09-05 -- and this tool stores every statement it
    // runs verbatim in laswell.step.sql, which is the whole point of the
    // ledger. A password in the spec would therefore be in the signed
    // specification, in git, and in the ledger, three places it must not be.
    //
    // Refused rather than redacted: redaction would make the ledger's record
    // of what ran untrue, and that record is the one thing this tool will not
    // compromise.
    auto lower = conn;
    for (auto& c : lower) c = static_cast<char>(std::tolower(c));
    if (lower.find("password") != std::string::npos) {
      detail::fail(
          at + ".connection must not carry a password",
          "A subscription's connection string is stored in pg_subscription in "
          "the clear, and pg_laswell stores every statement it runs verbatim in "
          "the ledger -- so a password here would be in the signed spec, in "
          "git and in laswell.step.sql. Put the credential in the server's "
          "~/.pgpass or a connection service file and name the service here. "
          "It is refused rather than redacted because a redacted ledger entry "
          "would no longer be what ran.");
    }
  }
  if (in.kind == IntentKind::kCreateSubscription) {
    detail::require_string(in.body, "connection", at);
    if (!in.body.contains("publications") || !in.body["publications"].is_array() ||
        in.body["publications"].empty()) {
      detail::fail(at + ".publications must be a non-empty array", "");
    }
  }
  for (const char* k : {"enabled", "connect", "refresh"}) {
    if (in.body.contains(k) && !in.body[k].is_boolean()) {
      detail::fail(std::string(at) + "." + k + " must be a boolean", "");
    }
  }
}

// The object types with no planning decision beyond existence and dependants.
// Two kinds cover all of them; anything later found to have a real decision is
// promoted to its own kind.
inline const std::set<std::string>& generic_object_types() {
  static const std::set<std::string> kTypes = {
      "AGGREGATE", "CAST", "COLLATION", "CONVERSION", "OPERATOR",
      "OPERATOR CLASS", "OPERATOR FAMILY", "TEXT SEARCH CONFIGURATION",
      "TEXT SEARCH DICTIONARY", "TEXT SEARCH PARSER", "TEXT SEARCH TEMPLATE",
      "TRANSFORM", "ACCESS METHOD", "LANGUAGE", "FOREIGN DATA WRAPPER",
      "SERVER", "USER MAPPING", "FOREIGN TABLE"};
  return kTypes;
}

inline void parse_alter_object(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(
      in.body, {"kind", "object_type", "schema", "name", "to", "owner",
                "set_schema"}, at);
  const auto ot = detail::require_string(in.body, "object_type", at);
  if (generic_object_types().count(ot) == 0) {
    detail::fail(at + ".object_type is not one alter_object handles: " + ot,
                 "Object types with their own planner have their own ALTER "
                 "intent, which says what that change costs.");
  }
  detail::require_string(in.body, "name", at);
  if (!in.body.contains("to") && !in.body.contains("owner") &&
      !in.body.contains("set_schema")) {
    detail::fail(at + " changes nothing", "Give to, owner or set_schema.");
  }
}

inline void parse_create_table_as(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(
      in.body, {"kind", "schema", "table", "definition", "comment", "with_data",
                "unlogged"}, at);
  detail::require_identifier(detail::require_string(in.body, "schema", at), "schema", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "table", at), "table", in.ordinal);
  detail::require_string(in.body, "definition", at);
  detail::require_string(in.body, "comment", at);
  if (in.body.contains("with_data") && !in.body["with_data"].is_boolean()) {
    detail::fail(at + ".with_data must be a boolean", "");
  }
}

inline void parse_import_foreign_schema(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(
      in.body, {"kind", "server", "remote_schema", "schema", "limit_to", "except"}, at);
  detail::require_identifier(detail::require_string(in.body, "server", at), "server", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "remote_schema", at),
                             "remote_schema", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "schema", at), "schema", in.ordinal);
  if (in.body.contains("limit_to") && in.body.contains("except")) {
    detail::fail(at + " cannot state both limit_to and except", "They are mutually exclusive.");
  }
}

inline void parse_security_label(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(
      in.body, {"kind", "object_type", "schema", "name", "provider", "label"}, at);
  detail::require_string(in.body, "object_type", at);
  detail::require_string(in.body, "name", at);
  detail::require_string(in.body, "provider", at);
  if (in.body.contains("label") && !in.body["label"].is_string()) {
    detail::fail(at + ".label must be a string, or absent to remove it", "");
  }
}

inline void parse_alter_default_privileges(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  const bool granting = in.body.value("grant", true);
  const std::string who = granting ? "to" : "from";
  detail::reject_unknown_keys(
      in.body, {"kind", "schema", "for_role", "object_type", "privileges",
                "grant", who}, at);
  static const std::set<std::string> kFor = {"TABLES", "SEQUENCES", "FUNCTIONS",
                                             "TYPES", "SCHEMAS"};
  if (kFor.count(detail::require_string(in.body, "object_type", at)) == 0) {
    detail::fail(at + ".object_type must be TABLES, SEQUENCES, FUNCTIONS, "
                      "TYPES or SCHEMAS",
                 "Plural: default privileges apply to a CLASS of future "
                 "objects, not to one.");
  }
  if (!in.body.contains("privileges") || !in.body["privileges"].is_array() ||
      in.body["privileges"].empty()) {
    detail::fail(at + ".privileges must be a non-empty array", "");
  }
  if (!in.body.contains(who) || !in.body[who].is_array() || in.body[who].empty()) {
    detail::fail(at + "." + who + " must be a non-empty array of roles", "");
  }
}

inline void parse_create_object(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(
      in.body, {"kind", "object_type", "schema", "name", "definition", "comment"}, at);
  const auto ot = detail::require_string(in.body, "object_type", at);
  if (generic_object_types().count(ot) == 0) {
    detail::fail(at + ".object_type is not one create_object handles: " + ot,
                 "This kind is for object types with no planning decision "
                 "beyond existence and dependants. Anything with a lock to "
                 "name or a scan to avoid has its own intent kind -- if this "
                 "type needs one, that is the bug, not the spec.");
  }
  detail::require_string(in.body, "name", at);
  // The definition is the SQL after CREATE <type> <name>. This is the one
  // place the project accepts a fragment, and it is bounded: an operator class
  // definition has no JSON shape anybody would read.
  detail::require_string(in.body, "definition", at);
}

inline void parse_drop_object(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(in.body, {"kind", "object_type", "schema", "name"}, at);
  const auto ot = detail::require_string(in.body, "object_type", at);
  if (generic_object_types().count(ot) == 0) {
    detail::fail(at + ".object_type is not one drop_object handles: " + ot, "");
  }
  detail::require_string(in.body, "name", at);
}

inline void parse_create_matview(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(
      in.body, {"kind", "schema", "name", "definition", "comment", "with_data",
                "tablespace"}, at);
  detail::require_identifier(detail::require_string(in.body, "schema", at), "schema", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "name", at), "name", in.ordinal);
  detail::require_string(in.body, "comment", at);
  const auto def = detail::require_string(in.body, "definition", at);
  std::string head = def.substr(0, 6);
  for (auto& c : head) c = static_cast<char>(std::tolower(c));
  if (head == "create") {
    detail::fail(at + ".definition is the view's QUERY, not a CREATE statement",
                 "Give the SELECT alone, as replace_view does.");
  }
  if (in.body.contains("with_data") && !in.body["with_data"].is_boolean()) {
    detail::fail(at + ".with_data must be a boolean", "");
  }
}

inline void parse_create_statistics(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(
      in.body, {"kind", "schema", "name", "table", "columns", "kinds", "comment"}, at);
  detail::require_identifier(detail::require_string(in.body, "schema", at), "schema", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "name", at), "name", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "table", at), "table", in.ordinal);
  if (!in.body.contains("columns") || !in.body["columns"].is_array() ||
      in.body["columns"].size() < 2) {
    detail::fail(at + ".columns needs at least two columns",
                 "Extended statistics describe a CORRELATION between columns; "
                 "one column is what ANALYZE already does.");
  }
  for (const auto& c : in.body["columns"]) {
    detail::require_identifier(c.get<std::string>(), "columns", in.ordinal);
  }
  static const std::set<std::string> kKinds = {"ndistinct", "dependencies", "mcv"};
  for (const auto& k : in.body.value("kinds", json::array())) {
    if (!k.is_string() || kKinds.count(k.get<std::string>()) == 0) {
      detail::fail(at + ".kinds has an unknown statistic kind: " + k.dump(),
                   "One of ndistinct, dependencies, mcv.");
    }
  }
}

inline void parse_create_rule(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(
      in.body, {"kind", "schema", "table", "name", "event", "instead", "action",
                "where"}, at);
  detail::require_identifier(detail::require_string(in.body, "schema", at), "schema", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "table", at), "table", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "name", at), "name", in.ordinal);
  static const std::set<std::string> kEvents = {"SELECT", "INSERT", "UPDATE", "DELETE"};
  if (kEvents.count(detail::require_string(in.body, "event", at)) == 0) {
    detail::fail(at + ".event must be SELECT, INSERT, UPDATE or DELETE", "");
  }
  detail::require_string(in.body, "action", at);
}

inline void parse_drop_rule(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(in.body, {"kind", "schema", "table", "name"}, at);
  detail::require_identifier(detail::require_string(in.body, "schema", at), "schema", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "table", at), "table", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "name", at), "name", in.ordinal);
}

inline void parse_set_identity(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(in.body, {"kind", "schema", "table", "column", "identity"}, at);
  detail::require_identifier(detail::require_string(in.body, "schema", at), "schema", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "table", at), "table", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "column", at), "column", in.ordinal);
  // Absent means DROP IDENTITY. Present must be one of the two forms, upper
  // case, because the value becomes DDL verbatim.
  if (in.body.contains("identity")) {
    const auto v = detail::require_string(in.body, "identity", at);
    if (v != "ALWAYS" && v != "BY DEFAULT") {
      detail::fail(at + ".identity must be ALWAYS or BY DEFAULT",
                   "Omit the key entirely to DROP IDENTITY. ALWAYS refuses a "
                   "caller-supplied value; BY DEFAULT accepts one, which is "
                   "how a sequence gets out of step with its column.");
    }
  }
}

inline void parse_set_column_options(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(
      in.body, {"kind", "schema", "table", "column", "statistics", "storage",
                "compression"}, at);
  detail::require_identifier(detail::require_string(in.body, "schema", at), "schema", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "table", at), "table", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "column", at), "column", in.ordinal);
  if (in.body.contains("statistics") && !in.body["statistics"].is_number_integer()) {
    detail::fail(at + ".statistics must be an integer", "");
  }
  static const std::set<std::string> kStorage = {"PLAIN", "EXTERNAL", "EXTENDED", "MAIN"};
  if (in.body.contains("storage") &&
      kStorage.count(in.body.value("storage", "")) == 0) {
    detail::fail(at + ".storage must be PLAIN, EXTERNAL, EXTENDED or MAIN", "");
  }
  static const std::set<std::string> kComp = {"pglz", "lz4", "default"};
  if (in.body.contains("compression") &&
      kComp.count(in.body.value("compression", "")) == 0) {
    detail::fail(at + ".compression must be pglz, lz4 or default",
                 "lz4 needs a server built with it; the plan checks nothing "
                 "here, PostgreSQL refuses at execution if it is unavailable.");
  }
  if (!in.body.contains("statistics") && !in.body.contains("storage") &&
      !in.body.contains("compression")) {
    detail::fail(at + " changes nothing", "Give statistics, storage or compression.");
  }
}

inline void parse_set_table_options(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(in.body, {"kind", "schema", "table", "options", "reset"}, at);
  detail::require_identifier(detail::require_string(in.body, "schema", at), "schema", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "table", at), "table", in.ordinal);
  if (in.body.contains("options") && !in.body["options"].is_object()) {
    detail::fail(at + ".options must be an object of name to value", "");
  }
  const json options = in.body.value("options", json::object());
  for (const auto& [k, v] : options.items()) {
    detail::require_identifier(k, "options", in.ordinal);
    if (!v.is_string() && !v.is_number()) {
      detail::fail(at + ".options." + k + " must be a string or a number", "");
    }
  }
  if (in.body.contains("reset")) {
    if (!in.body["reset"].is_array()) detail::fail(at + ".reset must be an array", "");
    for (const auto& r : in.body["reset"]) {
      detail::require_identifier(r.get<std::string>(), "reset", in.ordinal);
    }
  }
  if (!in.body.contains("options") && !in.body.contains("reset")) {
    detail::fail(at + " changes nothing", "Give options or reset.");
  }
}

inline void parse_set_logged(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(in.body, {"kind", "schema", "table", "logged"}, at);
  detail::require_identifier(detail::require_string(in.body, "schema", at), "schema", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "table", at), "table", in.ordinal);
  if (!in.body.contains("logged") || !in.body["logged"].is_boolean()) {
    detail::fail(at + ".logged must be stated as a boolean",
                 "There is no default: UNLOGGED discards durability and "
                 "replication for the table, and both directions rewrite it.");
  }
}

inline void parse_set_tablespace(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(in.body, {"kind", "schema", "table", "tablespace"}, at);
  detail::require_identifier(detail::require_string(in.body, "schema", at), "schema", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "table", at), "table", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "tablespace", at),
                             "tablespace", in.ordinal);
}

inline void parse_set_access_method(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(in.body, {"kind", "schema", "table", "method"}, at);
  detail::require_identifier(detail::require_string(in.body, "schema", at), "schema", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "table", at), "table", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "method", at), "method", in.ordinal);
}

inline void parse_set_replica_identity(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(in.body, {"kind", "schema", "table", "identity", "index"}, at);
  detail::require_identifier(detail::require_string(in.body, "schema", at), "schema", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "table", at), "table", in.ordinal);
  static const std::set<std::string> kForms = {"DEFAULT", "FULL", "NOTHING", "USING INDEX"};
  const auto v = detail::require_string(in.body, "identity", at);
  if (kForms.count(v) == 0) {
    detail::fail(at + ".identity must be DEFAULT, FULL, NOTHING or USING INDEX", "");
  }
  if (v == "USING INDEX") {
    detail::require_identifier(detail::require_string(in.body, "index", at), "index", in.ordinal);
  }
}

inline void parse_cluster_on(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(in.body, {"kind", "schema", "table", "index"}, at);
  detail::require_identifier(detail::require_string(in.body, "schema", at), "schema", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "table", at), "table", in.ordinal);
  if (in.body.contains("index")) {
    detail::require_identifier(in.body.value("index", ""), "index", in.ordinal);
  }
}

inline void parse_alter_column_default(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(in.body, {"kind", "schema", "table", "column", "default"}, at);
  detail::require_identifier(detail::require_string(in.body, "schema", at), "schema", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "table", at), "table", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "column", at), "column", in.ordinal);
  // Absent means DROP DEFAULT, present means SET DEFAULT. Null is refused
  // rather than guessed at: "default": null could mean either, and the two are
  // different changes.
  if (in.body.contains("default") && !in.body["default"].is_string()) {
    detail::fail(at + ".default must be a string, or absent to drop it",
                 "Write the SQL expression as it would appear after DEFAULT. "
                 "Omit the key entirely to DROP DEFAULT; null is refused "
                 "because it reads as both.");
  }
}

inline void parse_drop_not_null(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(in.body, {"kind", "schema", "table", "column"}, at);
  detail::require_identifier(detail::require_string(in.body, "schema", at), "schema", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "table", at), "table", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "column", at), "column", in.ordinal);
}

inline void parse_alter_sequence(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(
      in.body, {"kind", "schema", "name", "restart", "increment", "owned_by", "to"}, at);
  detail::require_identifier(detail::require_string(in.body, "schema", at), "schema", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "name", at), "name", in.ordinal);
  for (const char* k : {"restart", "increment"}) {
    if (in.body.contains(k) && !in.body[k].is_number_integer()) {
      detail::fail(std::string(at) + "." + k + " must be an integer", "");
    }
  }
  if (!in.body.contains("restart") && !in.body.contains("increment") &&
      !in.body.contains("owned_by") && !in.body.contains("to")) {
    detail::fail(at + " changes nothing",
                 "Give at least one of restart, increment, owned_by or to.");
  }
}

inline void parse_alter_schema(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(in.body, {"kind", "schema", "to", "owner"}, at);
  detail::require_identifier(detail::require_string(in.body, "schema", at), "schema", in.ordinal);
  if (!in.body.contains("to") && !in.body.contains("owner")) {
    detail::fail(at + " changes nothing", "Give \"to\" to rename, or \"owner\".");
  }
}

inline void parse_alter_extension(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(in.body, {"kind", "name", "version", "schema"}, at);
  detail::require_identifier(detail::require_string(in.body, "name", at), "name", in.ordinal);
  if (!in.body.contains("version") && !in.body.contains("schema")) {
    detail::fail(at + " changes nothing",
                 "Give \"version\" to update, or \"schema\" to move it.");
  }
}

inline void parse_alter_domain(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(
      in.body, {"kind", "schema", "name", "add_check", "constraint_name",
                "drop_constraint", "not_null", "default"}, at);
  detail::require_identifier(detail::require_string(in.body, "schema", at), "schema", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "name", at), "name", in.ordinal);
  if (in.body.contains("add_check")) {
    detail::require_string(in.body, "add_check", at);
    detail::require_identifier(detail::require_string(in.body, "constraint_name", at),
                               "constraint_name", in.ordinal);
  }
  if (!in.body.contains("add_check") && !in.body.contains("drop_constraint") &&
      !in.body.contains("not_null") && !in.body.contains("default")) {
    detail::fail(at + " changes nothing",
                 "Give add_check with constraint_name, drop_constraint, "
                 "not_null, or default.");
  }
}

inline void parse_alter_function(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(
      in.body, {"kind", "schema", "name", "arguments", "to", "owner",
                "search_path", "volatility"}, at);
  detail::require_identifier(detail::require_string(in.body, "schema", at), "schema", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "name", at), "name", in.ordinal);
  if (!in.body.contains("arguments") || !in.body["arguments"].is_array()) {
    detail::fail(at + ".arguments is required, even when empty",
                 "A function is identified by its argument types, not its "
                 "name: overloads share a name.");
  }
  if (!in.body.contains("to") && !in.body.contains("owner") &&
      !in.body.contains("search_path") && !in.body.contains("volatility")) {
    detail::fail(at + " changes nothing",
                 "Give to, owner, search_path or volatility.");
  }
}

inline void parse_alter_view(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(in.body, {"kind", "schema", "name", "to", "owner", "options"}, at);
  detail::require_identifier(detail::require_string(in.body, "schema", at), "schema", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "name", at), "name", in.ordinal);
  if (!in.body.contains("to") && !in.body.contains("owner") &&
      !in.body.contains("options")) {
    detail::fail(at + " changes nothing", "Give to, owner or options.");
  }
}

inline void parse_alter_policy(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(
      in.body, {"kind", "schema", "table", "name", "using", "check", "roles"}, at);
  detail::require_identifier(detail::require_string(in.body, "schema", at), "schema", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "table", at), "table", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "name", at), "name", in.ordinal);
  if (!in.body.contains("using") && !in.body.contains("check") &&
      !in.body.contains("roles")) {
    detail::fail(at + " changes nothing", "Give using, check or roles.");
  }
}

// The object types COMMENT ON and OWNER TO accept here. Deliberately a closed
// set rather than free text: the value goes straight into DDL, and a typo would
// otherwise reach the database as SQL.
inline const std::set<std::string>& commentable_object_types() {
  static const std::set<std::string> kTypes = {
      "TABLE", "COLUMN", "VIEW", "MATERIALIZED VIEW", "INDEX", "SEQUENCE",
      "SCHEMA", "TYPE", "DOMAIN", "FUNCTION", "TRIGGER", "CONSTRAINT",
      "POLICY", "EXTENSION"};
  return kTypes;
}

inline void parse_set_comment(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(
      in.body, {"kind", "object_type", "schema", "name", "table", "comment"}, at);
  const auto ot = detail::require_string(in.body, "object_type", at);
  if (commentable_object_types().count(ot) == 0) {
    detail::fail(at + ".object_type is not one this tool comments on: " + ot,
                 "Upper case, one of TABLE, COLUMN, VIEW, MATERIALIZED VIEW, "
                 "INDEX, SEQUENCE, SCHEMA, TYPE, DOMAIN, FUNCTION, TRIGGER, "
                 "CONSTRAINT, POLICY, EXTENSION. Not passed through unchecked: "
                 "it becomes DDL verbatim.");
  }
  detail::require_identifier(detail::require_string(in.body, "name", at), "name", in.ordinal);
  detail::require_string(in.body, "comment", at);
  // Three different shapes, and mixing them up produces valid SQL for the
  // wrong object rather than an error:
  //   most types      "schema" + "name"   -> COMMENT ON TABLE shop.orders
  //   SCHEMA/EXTENSION "name" alone       -> COMMENT ON SCHEMA reporting
  //   COLUMN/TRIGGER/CONSTRAINT/POLICY    -> named relative to "table"
  if ((ot == "SCHEMA" || ot == "EXTENSION") && in.body.contains("schema")) {
    detail::fail(at + " should not carry \"schema\" for a " + ot + " comment",
                 "A schema or extension IS the object: put its name in "
                 "\"name\". Passing both produces a comment on whatever "
                 "\"name\" happens to be, which is valid SQL for the wrong "
                 "object.");
  }
  // COLUMN, TRIGGER, CONSTRAINT and POLICY are qualified BY a table, which is
  // a different shape from the rest and easy to leave out.
  if ((ot == "COLUMN" || ot == "TRIGGER" || ot == "CONSTRAINT" || ot == "POLICY") &&
      !in.body.contains("table")) {
    detail::fail(at + " needs \"table\" for a " + ot + " comment",
                 "A column, trigger, constraint or policy is named relative to "
                 "its table.");
  }
}

inline void parse_set_owner(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(in.body, {"kind", "object_type", "schema", "name", "owner"}, at);
  const auto ot = detail::require_string(in.body, "object_type", at);
  static const std::set<std::string> kOwnable = {
      "TABLE", "VIEW", "MATERIALIZED VIEW", "SEQUENCE", "SCHEMA", "TYPE",
      "DOMAIN", "FUNCTION"};
  if (kOwnable.count(ot) == 0) {
    detail::fail(at + ".object_type cannot be given an owner here: " + ot,
                 "Upper case, one of TABLE, VIEW, MATERIALIZED VIEW, SEQUENCE, "
                 "SCHEMA, TYPE, DOMAIN, FUNCTION.");
  }
  detail::require_identifier(detail::require_string(in.body, "name", at), "name", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "owner", at), "owner", in.ordinal);
}

inline void parse_schema_like(Intent& in, bool creating) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  std::set<std::string> allowed = {"kind", "schema"};
  if (creating) { allowed.insert("comment"); allowed.insert("owner"); }
  detail::reject_unknown_keys(in.body, allowed, at);
  detail::require_identifier(detail::require_string(in.body, "schema", at), "schema", in.ordinal);
  if (creating) detail::require_string(in.body, "comment", at);
}

inline void parse_create_extension(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(in.body, {"kind", "name", "schema", "version"}, at);
  detail::require_identifier(detail::require_string(in.body, "name", at), "name", in.ordinal);
  // The schema is optional but strongly wanted, and the plan says why: an
  // extension's objects land wherever search_path pointed at install time, and
  // moving them afterwards is far harder than choosing now.
  if (in.body.contains("schema")) {
    detail::require_identifier(in.body.value("schema", ""), "schema", in.ordinal);
  }
  if (in.body.contains("version") && !in.body["version"].is_string()) {
    detail::fail(at + ".version must be a string", "");
  }
}

inline void parse_drop_extension(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(in.body, {"kind", "name"}, at);
  detail::require_identifier(detail::require_string(in.body, "name", at), "name", in.ordinal);
}

// schema + name, for the drops that need nothing else.
inline void parse_named_object(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(in.body, {"kind", "schema", "name"}, at);
  detail::require_identifier(detail::require_string(in.body, "schema", at), "schema", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "name", at), "name", in.ordinal);
}

inline void parse_create_type(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(
      in.body, {"kind", "schema", "name", "type_kind", "labels", "base",
                "check", "attributes", "comment", "not_null"}, at);
  detail::require_identifier(detail::require_string(in.body, "schema", at), "schema", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "name", at), "name", in.ordinal);
  detail::require_string(in.body, "comment", at);
  const auto tk = detail::require_string(in.body, "type_kind", at);
  if (tk == "enum") {
    if (!in.body.contains("labels") || !in.body["labels"].is_array() ||
        in.body["labels"].empty()) {
      detail::fail(at + ".labels must be a non-empty array for an enum",
                   "List the values, in the order they should sort.");
    }
    for (const auto& l : in.body["labels"]) {
      if (!l.is_string()) detail::fail(at + ".labels must be strings", "");
    }
  } else if (tk == "domain") {
    detail::require_string(in.body, "base", at);
  } else if (tk == "composite") {
    if (!in.body.contains("attributes") || !in.body["attributes"].is_array() ||
        in.body["attributes"].empty()) {
      detail::fail(at + ".attributes must be a non-empty array for a composite",
                   "Each entry needs name and type.");
    }
  } else {
    detail::fail(at + ".type_kind must be enum, domain or composite",
                 "Base types and ranges need C-level support or an operator "
                 "class, which is not a migration this tool plans.");
  }
}

inline void parse_add_enum_value(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(in.body, {"kind", "schema", "name", "value", "before", "after"}, at);
  detail::require_identifier(detail::require_string(in.body, "schema", at), "schema", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "name", at), "name", in.ordinal);
  detail::require_string(in.body, "value", at);
  if (in.body.contains("before") && in.body.contains("after")) {
    detail::fail(at + " cannot state both \"before\" and \"after\"",
                 "A new label goes in one place. Omit both to append.");
  }
}

inline void parse_create_function(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(
      in.body, {"kind", "schema", "name", "arguments", "returns", "language",
                "body", "volatility", "strict", "security_definer", "comment",
                "routine_kind"}, at);
  // A procedure differs from a function only in having no return type and
  // being CALLed, so it is a key here rather than two more intent kinds.
  const auto rk = in.body.value("routine_kind", "FUNCTION");
  if (rk != "FUNCTION" && rk != "PROCEDURE") {
    detail::fail(at + ".routine_kind must be FUNCTION or PROCEDURE", "");
  }
  if (rk == "PROCEDURE" && in.body.contains("returns")) {
    detail::fail(at + " is a PROCEDURE and cannot declare a return type",
                 "A procedure returns nothing and is invoked with CALL.");
  }
  detail::require_identifier(detail::require_string(in.body, "schema", at), "schema", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "name", at), "name", in.ordinal);
  if (rk == "FUNCTION") detail::require_string(in.body, "returns", at);
  detail::require_string(in.body, "language", at);
  detail::require_string(in.body, "body", at);
  detail::require_string(in.body, "comment", at);
  // Arguments are structured because the SIGNATURE is the function's identity:
  // the planner needs it to decide between CREATE OR REPLACE and a drop, and
  // to emit a legal DROP later.
  if (in.body.contains("arguments") && !in.body["arguments"].is_array()) {
    detail::fail(at + ".arguments must be an array of {name, type}", "");
  }
  for (const auto& a : in.body.value("arguments", json::array())) {
    if (!a.is_object() || !a.contains("type")) {
      detail::fail(at + ".arguments entries need at least a type", "");
    }
  }
}

inline void parse_drop_function(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(in.body, {"kind", "schema", "name", "arguments",
                                        "routine_kind"}, at);
  detail::require_identifier(detail::require_string(in.body, "schema", at), "schema", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "name", at), "name", in.ordinal);
  if (!in.body.contains("arguments") || !in.body["arguments"].is_array()) {
    detail::fail(at + ".arguments is required, even when empty",
                 "A function is identified by its argument types, not its "
                 "name: overloads share a name and DROP must say which one.");
  }
}

inline void parse_create_trigger(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(
      in.body, {"kind", "schema", "table", "name", "timing", "events",
                "function", "for_each", "when"}, at);
  detail::require_identifier(detail::require_string(in.body, "schema", at), "schema", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "table", at), "table", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "name", at), "name", in.ordinal);
  detail::require_string(in.body, "function", at);
  static const std::set<std::string> kTiming = {"BEFORE", "AFTER", "INSTEAD OF"};
  if (kTiming.count(detail::require_string(in.body, "timing", at)) == 0) {
    detail::fail(at + ".timing must be BEFORE, AFTER or INSTEAD OF", "");
  }
  if (!in.body.contains("events") || !in.body["events"].is_array() ||
      in.body["events"].empty()) {
    detail::fail(at + ".events must be a non-empty array",
                 "INSERT, UPDATE, DELETE or TRUNCATE.");
  }
  static const std::set<std::string> kEvents = {"INSERT", "UPDATE", "DELETE", "TRUNCATE"};
  for (const auto& e : in.body["events"]) {
    if (!e.is_string() || kEvents.count(e.get<std::string>()) == 0) {
      detail::fail(at + ".events has an unknown event: " + e.dump(),
                   "One of INSERT, UPDATE, DELETE, TRUNCATE, upper case. Not "
                   "passed through unchecked: a typo would reach the database "
                   "as SQL.");
    }
  }
}

inline void parse_drop_trigger(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(in.body, {"kind", "schema", "table", "name"}, at);
  detail::require_identifier(detail::require_string(in.body, "schema", at), "schema", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "table", at), "table", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "name", at), "name", in.ordinal);
}

inline void parse_create_sequence(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(
      in.body, {"kind", "schema", "name", "comment", "start", "increment",
                "owned_by"}, at);
  detail::require_identifier(detail::require_string(in.body, "schema", at), "schema", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "name", at), "name", in.ordinal);
  detail::require_string(in.body, "comment", at);
  for (const char* k : {"start", "increment"}) {
    if (in.body.contains(k) && !in.body[k].is_number_integer()) {
      detail::fail(std::string(at) + "." + k + " must be an integer", "");
    }
  }
}

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
      in.body, {"kind", "object_type", "schema", "table", "name", "arguments",
                "privileges", "columns", "all_in_schema", who}, at);
  // TABLE is the default so every spec written before this keeps working.
  const auto ot = in.body.value("object_type", "TABLE");
  static const std::set<std::string> kGrantable = {
      "TABLE", "SCHEMA", "SEQUENCE", "FUNCTION", "TYPE", "DOMAIN", "DATABASE"};
  if (kGrantable.count(ot) == 0) {
    detail::fail(at + ".object_type cannot be granted on here: " + ot,
                 "Upper case, one of TABLE, SCHEMA, SEQUENCE, FUNCTION, TYPE, "
                 "DOMAIN, DATABASE.");
  }
  if (ot == "TABLE") {
    detail::require_identifier(detail::require_string(in.body, "schema", at), "schema", in.ordinal);
    if (!in.body.value("all_in_schema", false)) {
      detail::require_identifier(detail::require_string(in.body, "table", at), "table", in.ordinal);
    }
  } else {
    detail::require_identifier(detail::require_string(in.body, "name", at), "name", in.ordinal);
  }
  if (in.body.contains("all_in_schema") && !in.body["all_in_schema"].is_boolean()) {
    detail::fail(at + ".all_in_schema must be a boolean", "");
  }
  if (!in.body.contains("privileges") || !in.body["privileges"].is_array() ||
      in.body["privileges"].empty()) {
    detail::fail(at + ".privileges must be a non-empty array",
                 "SELECT, INSERT, UPDATE, DELETE, TRUNCATE, REFERENCES, "
                 "TRIGGER, or ALL.");
  }
  // Each object type accepts a different set, and PostgreSQL rejects the
  // wrong one at execution -- by which point earlier steps have committed.
  static const std::map<std::string, std::set<std::string>> kPrivs = {
      {"TABLE", {"SELECT", "INSERT", "UPDATE", "DELETE", "TRUNCATE",
                 "REFERENCES", "TRIGGER", "MAINTAIN", "ALL"}},
      {"SCHEMA", {"USAGE", "CREATE", "ALL"}},
      {"SEQUENCE", {"USAGE", "SELECT", "UPDATE", "ALL"}},
      {"FUNCTION", {"EXECUTE", "ALL"}},
      {"TYPE", {"USAGE", "ALL"}},
      {"DOMAIN", {"USAGE", "ALL"}},
      {"DATABASE", {"CREATE", "CONNECT", "TEMPORARY", "TEMP", "ALL"}}};
  const auto& allowed = kPrivs.at(ot);
  for (const auto& pv : in.body["privileges"]) {
    if (!pv.is_string() || allowed.count(pv.get<std::string>()) == 0) {
      std::vector<std::string> names(allowed.begin(), allowed.end());
      std::string list;
      for (const auto& n : names) { if (!list.empty()) list += ", "; list += n; }
      detail::fail(at + ".privileges has one a " + ot + " does not accept: " +
                       pv.dump(),
                   "Upper case, one of " + list +
                       ". Checked here because PostgreSQL would reject it at "
                       "execution, by which point earlier steps have already "
                       "committed.");
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
    if (ot != "TABLE") {
      detail::fail(at + ".columns applies only to a TABLE grant",
                   "Column-level privileges exist for tables and views only.");
    }
    if (!in.body["columns"].is_array() || in.body["columns"].empty()) {
      detail::fail(at + ".columns must be a non-empty array", "");
    }
    for (const auto& cn : in.body["columns"]) {
      if (!cn.is_string()) detail::fail(at + ".columns must be strings", "");
      detail::require_identifier(cn.get<std::string>(), "columns", in.ordinal);
    }
  }
}

// --- the row source shared by every row-level DML kind ----------------------
//
// insert_rows, update_rows, merge_rows, copy_rows and delete_rows all have to
// answer the same question -- WHICH ROWS -- and there are exactly three honest
// answers. Parsing them in one place is what keeps the five kinds from drifting
// into five dialects:
//
//   "values"  literal rows, in the spec, covered by its signature. This is what
//             "specific rows" means: the change and the data are one reviewable
//             artefact, and the ledger records the exact rows that were written.
//   "select"  a query that produces the rows, for volume that cannot sensibly
//             be written out.
//   "where"   a predicate over the target, which only delete_rows can use --
//             an INSERT has no existing row to filter.
//
// The pacing rule falls out of this split rather than being bolted on, and it
// is the usual "measure, then say what decided it":
//
//   a `values` row count is known EXACTLY at plan time, so the planner runs it
//   in one transaction when it is small -- a half-applied seed is worse than a
//   failed one -- and paces it when it is not;
//
//   a `select` row count is NOT derivable, because planner.h is a pure function
//   with no database in it (planner_purity_check.cpp enforces that), so those
//   are always paced. The alternative is to state a number this tool cannot
//   derive, which is the one thing it does not do. Pacing a small result costs
//   one extra round trip.
namespace detail {

// Cells are JSON SCALARS ONLY -- string, number, boolean, null.
//
// A jsonb or array column takes its value as a STRING in PostgreSQL's own input
// syntax ("{\"k\": 1}", "{x,y}"), which the column-typed cast then parses.
// Measured on 18.6 (S21): with the cast, both round-trip exactly. Accepting a
// nested JSON object here instead would be ambiguous the moment a column is
// text[] rather than jsonb, and would make one spec mean two things.
inline void require_scalar_cell(const json& cell, const std::string& at) {
  if (cell.is_object() || cell.is_array()) {
    fail(at + " must be a string, number, boolean or null",
         "A jsonb or array value is written as a STRING in PostgreSQL's own "
         "input syntax -- \"{\\\"k\\\": 1}\" for jsonb, \"{x,y}\" for text[] -- "
         "and the cast to the column's real type parses it. A bare JSON object "
         "would be ambiguous for a text[] column.");
  }
}

// key_role is what the key means for this kind, used only in messages:
// "identifies the rows to update", and so on. Empty means the key is optional.
inline void parse_row_source(Intent& in, const std::string& at,
                             const std::string& key_role,
                             bool allow_where) {
  const bool has_values = in.body.contains("values");
  const bool has_select = in.body.contains("select");
  const bool has_where = allow_where && in.body.contains("where") &&
                         in.body["where"].is_string() &&
                         !in.body["where"].get<std::string>().empty();

  const int sources = (has_values ? 1 : 0) + (has_select ? 1 : 0) + (has_where ? 1 : 0);
  if (sources == 0) {
    fail(at + " names no rows",
         std::string("Give exactly one of \"values\" (literal rows, signed with "
                     "the spec), \"select\" (a query producing them)") +
             (allow_where ? " or \"where\" (a predicate over the target)." : ".") +
             " Which one you choose also decides pacing: a values count is "
             "known at plan time, a select's is not.");
  }
  if (sources > 1) {
    fail(at + " names rows more than one way",
         "\"values\", \"select\"" +
             std::string(allow_where ? " and \"where\"" : " and nothing else") +
             " are alternatives, not layers. Two of them in one intent have no "
             "single meaning, and guessing an order of precedence would make "
             "the plan depend on something the spec never said.");
  }

  if (has_select) {
    (void)require_string(in.body, "select", at);
    if (!key_role.empty() || true) {
      // A select is ALWAYS paced (its size is not derivable), and a keyset walk
      // needs a key to walk. Refused here rather than in the planner so the
      // author is told at authoring time.
      if (!in.body.contains("key")) {
        fail(at + " uses \"select\" but names no \"key\"",
             "A select's row count cannot be measured without running it, so "
             "these are always paced, and a paced walk needs a key column that "
             "the select returns. Add \"key\", and make sure the select "
             "produces it.");
      }
    }
  }

  if (has_values) {
    if (!in.body.contains("columns") || !in.body["columns"].is_array() ||
        in.body["columns"].empty()) {
      fail(at + " has \"values\" but no non-empty \"columns\"",
           "\"columns\" names what each row's entries are, in order. Positional "
           "rows with no column list would make a spec that reads correctly and "
           "writes the wrong column the moment the table gains one.");
    }
    std::set<std::string> seen;
    for (const auto& c : in.body["columns"]) {
      if (!c.is_string()) fail(at + ".columns entries must be strings", "");
      const auto name = c.get<std::string>();
      require_identifier(name, "columns", in.ordinal);
      if (!seen.insert(name).second) {
        fail(at + ".columns names \"" + name + "\" twice",
             "Each column may appear once. Two entries for one column have no "
             "meaning and PostgreSQL would refuse the statement anyway.");
      }
    }

    if (!in.body["values"].is_array() || in.body["values"].empty()) {
      fail(at + ".values must be a non-empty array of rows", "");
    }
    const auto arity = in.body["columns"].size();
    std::size_t r = 0;
    for (const auto& row : in.body["values"]) {
      const std::string row_at = at + ".values[" + std::to_string(r) + "]";
      if (!row.is_array()) {
        fail(row_at + " must be an array",
             "Each row is an array of values positionally matching \"columns\".");
      }
      if (row.size() != arity) {
        fail(row_at + " has " + std::to_string(row.size()) + " values but " +
                 "\"columns\" names " + std::to_string(arity),
             "Every row must line up with the column list. A short row would "
             "otherwise be padded with something the spec never said.");
      }
      for (std::size_t c = 0; c < row.size(); ++c) {
        require_scalar_cell(row[c], row_at + "[" + std::to_string(c) + "]");
      }
      ++r;
    }

    // The key must be a column the rows actually carry, or neither the keyset
    // walk nor the per-row match has anything to join on.
    if (in.body.contains("key")) {
      const auto key = in.body.value("key", "");
      bool found = false;
      for (const auto& c : in.body["columns"]) {
        if (c.get<std::string>() == key) found = true;
      }
      if (!found) {
        fail(at + ".key \"" + key + "\" is not in \"columns\"",
             "The key " +
                 (key_role.empty() ? std::string("has to be one of the values "
                                                 "each row supplies")
                                   : key_role) +
                 ", so every row must carry it.");
      }
    }
  }

  // Pacing may be forced either way. Left out, the planner decides and says
  // which reading decided it.
  if (in.body.contains("paced") && !in.body["paced"].is_boolean()) {
    fail(at + ".paced must be a boolean",
         "true paces the change into short transactions; false runs it in one, "
         "which is what a small seed usually wants. Omit it and the planner "
         "chooses on the measured row count and says so.");
  }
  if (in.body.contains("verify_remaining") &&
      !in.body["verify_remaining"].is_string()) {
    fail(at + ".verify_remaining must be a string", "");
  }
}

// preserve and assert_invariants, shared with backfill, which had them first.
inline void parse_preserve_and_invariants(Intent& in, const std::string& at) {
  if (in.body.contains("preserve")) {
    const auto& p = in.body["preserve"];
    if (!p.is_object()) fail(at + ".preserve must be an object", "");
    reject_unknown_keys(p, {"schema", "table"}, at + ".preserve");
    require_identifier(require_string(p, "schema", at + ".preserve"),
                       "preserve.schema", in.ordinal);
    require_identifier(require_string(p, "table", at + ".preserve"),
                       "preserve.table", in.ordinal);
    if (p.value("schema", "") == in.body.value("schema", "") &&
        p.value("table", "") == in.body.value("table", "")) {
      fail(at + ".preserve names the table being changed",
           "The pre-image has to go somewhere else, or the change would "
           "overwrite the record of what it changed.");
    }
  }
  if (in.body.contains("assert_invariants")) {
    if (!in.body["assert_invariants"].is_array()) {
      fail(at + ".assert_invariants must be an array", "");
    }
    for (const auto& inv : in.body["assert_invariants"]) {
      if (!inv.is_object()) fail(at + ".assert_invariants entries must be objects", "");
      reject_unknown_keys(inv, {"name", "query"}, at + ".assert_invariants[]");
      (void)require_string(inv, "name", at + ".assert_invariants[]");
      (void)require_string(inv, "query", at + ".assert_invariants[]");
    }
  }
}

}  // namespace detail

inline void parse_delete_rows(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(
      in.body, {"kind", "schema", "table", "key", "where", "columns", "values",
                "select", "paced", "verify_remaining", "preserve",
                "assert_invariants"}, at);
  detail::require_identifier(detail::require_string(in.body, "schema", at), "schema", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "table", at), "table", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "key", at), "key", in.ordinal);

  // The three forms are alternatives. `where` is the one this kind had first,
  // and its refusal is kept verbatim because it is the mistake people actually
  // make -- but it now only fires when `where` is the form being used, so
  // deleting an explicit list of keys no longer has to invent a predicate.
  const bool has_values = in.body.contains("values");
  const bool has_select = in.body.contains("select");
  if (!has_values && !has_select) {
    const auto where = in.body.value("where", "");
    if (!in.body.contains("where") || !in.body["where"].is_string() || where.empty()) {
      detail::fail(at + " names no rows to delete",
                   "Give \"where\" (a predicate), \"values\" (the exact keys, "
                   "which is what \"delete these rows\" usually means) or "
                   "\"select\" (a query producing them). An unfiltered delete "
                   "empties the table: if that is really intended, write "
                   "\"where\": \"true\" and say so in the rationale -- and "
                   "consider whether drop_table is what you meant instead.");
    }
  }
  detail::parse_row_source(in, at, "identifies the rows to delete", true);
  detail::parse_preserve_and_invariants(in, at);
}

// --- insert_rows ------------------------------------------------------------
inline void parse_insert_rows(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(
      in.body, {"kind", "schema", "table", "key", "columns", "values", "select",
                "on_conflict", "conflict_target", "conflict_where",
                "update_columns", "overriding", "paced", "verify_remaining",
                "assert_invariants"}, at);
  detail::require_identifier(detail::require_string(in.body, "schema", at), "schema", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "table", at), "table", in.ordinal);
  if (in.body.contains("key")) {
    detail::require_identifier(in.body.value("key", ""), "key", in.ordinal);
  }
  detail::parse_row_source(in, at, "", false);

  // The select form has to say which columns it is filling, because
  // INSERT ... SELECT with no column list binds by POSITION -- and a table that
  // gains a column silently shifts every value one place to the left.
  if (in.body.contains("select") &&
      (!in.body.contains("columns") || !in.body["columns"].is_array() ||
       in.body["columns"].empty())) {
    detail::fail(at + " uses \"select\" but names no \"columns\"",
                 "INSERT ... SELECT with no column list binds by position, so "
                 "adding a column to the table would silently shift every "
                 "value. Name the target columns, in the order the select "
                 "returns them.");
  }

  const auto on_conflict = in.body.value("on_conflict", "refuse");
  if (in.body.contains("on_conflict")) {
    if (!in.body["on_conflict"].is_string() ||
        (on_conflict != "refuse" && on_conflict != "skip" && on_conflict != "update")) {
      detail::fail(at + ".on_conflict must be \"refuse\", \"skip\" or \"update\"",
                   "refuse (the default) emits no ON CONFLICT clause, so a "
                   "duplicate is an error and the migration stops; skip emits "
                   "DO NOTHING, which makes re-running the spec harmless; "
                   "update emits DO UPDATE, which is an upsert -- and if that "
                   "is what you want against an existing table, merge_rows says "
                   "it more directly.");
    }
  }
  if (on_conflict != "refuse") {
    if (!in.body.contains("conflict_target") ||
        !in.body["conflict_target"].is_array() ||
        in.body["conflict_target"].empty()) {
      detail::fail(at + ".on_conflict \"" + on_conflict +
                       "\" needs a \"conflict_target\"",
                   "Name the columns of the unique index that decides what a "
                   "conflict IS. PostgreSQL has no default arbiter: measured on "
                   "18.6, ON CONFLICT with no matching unique index is refused "
                   "outright rather than falling back to the primary key.");
    }
    for (const auto& c : in.body["conflict_target"]) {
      if (!c.is_string()) detail::fail(at + ".conflict_target entries must be strings", "");
      detail::require_identifier(c.get<std::string>(), "conflict_target", in.ordinal);
    }
  } else if (in.body.contains("conflict_target")) {
    detail::fail(at + " has a \"conflict_target\" but on_conflict is \"refuse\"",
                 "An arbiter with nothing to arbitrate is a spec whose author "
                 "meant one thing and wrote another. Set on_conflict to \"skip\" "
                 "or \"update\", or drop the target.");
  }
  if (in.body.contains("conflict_where") &&
      (!in.body["conflict_where"].is_string() ||
       in.body["conflict_where"].get<std::string>().empty())) {
    detail::fail(at + ".conflict_where must be a non-empty string",
                 "This is the predicate of a PARTIAL unique index, repeated so "
                 "the arbiter matches it. Measured on 18.6: against a partial "
                 "unique index, ON CONFLICT (col) is refused and "
                 "ON CONFLICT (col) WHERE <predicate> is accepted.");
  }
  if (in.body.contains("update_columns")) {
    if (on_conflict != "update") {
      detail::fail(at + " has \"update_columns\" but on_conflict is not \"update\"",
                   "Nothing would be updated, so the list has no effect.");
    }
    if (!in.body["update_columns"].is_array() || in.body["update_columns"].empty()) {
      detail::fail(at + ".update_columns must be a non-empty array", "");
    }
    for (const auto& c : in.body["update_columns"]) {
      if (!c.is_string()) detail::fail(at + ".update_columns entries must be strings", "");
      detail::require_identifier(c.get<std::string>(), "update_columns", in.ordinal);
    }
  }
  if (in.body.contains("overriding")) {
    const auto o = in.body.value("overriding", "");
    if (!in.body["overriding"].is_string() || (o != "system" && o != "user")) {
      detail::fail(at + ".overriding must be \"system\" or \"user\"",
                   "\"system\" emits OVERRIDING SYSTEM VALUE, which is the only "
                   "way to write an explicit value into a GENERATED ALWAYS "
                   "identity column; \"user\" emits OVERRIDING USER VALUE, "
                   "which discards the value supplied and lets the sequence "
                   "decide.");
    }
  }
  detail::parse_preserve_and_invariants(in, at);
}

// --- update_rows ------------------------------------------------------------
//
// Not a duplicate of backfill, and the difference is the whole reason it
// exists. backfill applies ONE EXPRESSION to MANY rows -- "region_id =
// w.region_id where region_id is null". update_rows applies A DIFFERENT VALUE
// TO EACH ROW, which no expression can say: row 41 becomes 'NA' and row 42
// becomes 'EU' because someone decided so, not because a rule derives it.
inline void parse_update_rows(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(
      in.body, {"kind", "schema", "table", "key", "columns", "values", "select",
                "paced", "verify_remaining", "preserve", "assert_invariants"},
      at);
  detail::require_identifier(detail::require_string(in.body, "schema", at), "schema", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "table", at), "table", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "key", at), "key", in.ordinal);
  detail::parse_row_source(in, at, "identifies the row each set of values "
                                   "belongs to", false);

  if (!in.body.contains("columns") || !in.body["columns"].is_array() ||
      in.body["columns"].empty()) {
    detail::fail(at + " needs a non-empty \"columns\"",
                 "Name the key and the columns being set, in the order each "
                 "row supplies them -- the select form needs it for the same "
                 "reason as the values form: to know which returned column is "
                 "which.");
  }
  const auto key = in.body.value("key", "");
  bool key_in_columns = false;
  for (const auto& c : in.body["columns"]) {
    if (c.is_string() && c.get<std::string>() == key) key_in_columns = true;
  }
  if (!key_in_columns) {
    detail::fail(at + ".columns does not include the key \"" + key + "\"",
                 "The key identifies which row each set of values belongs to, "
                 "so it has to be one of the columns supplied.");
  }
  if (in.body["columns"].size() < 2) {
    detail::fail(at + " sets no columns",
                 "\"columns\" holds the key plus at least one column to change. "
                 "With only the key there is nothing to update, and if the "
                 "intent was to prove those rows exist, an assert_invariants "
                 "entry says that without writing to them.");
  }
  detail::parse_preserve_and_invariants(in, at);
}

// --- merge_rows -------------------------------------------------------------
inline void parse_merge_rows(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(
      in.body, {"kind", "schema", "table", "key", "columns", "values", "select",
                "when_matched", "update_columns", "when_not_matched",
                "when_not_matched_by_source", "paced", "verify_remaining",
                "preserve", "assert_invariants"}, at);
  detail::require_identifier(detail::require_string(in.body, "schema", at), "schema", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "table", at), "table", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "key", at), "key", in.ordinal);
  detail::parse_row_source(in, at, "matches a source row to a target row", false);

  if (!in.body.contains("columns") || !in.body["columns"].is_array() ||
      in.body["columns"].empty()) {
    detail::fail(at + " needs a non-empty \"columns\"",
                 "Name the key and the columns the source carries, in order. "
                 "MERGE needs them to build both the INSERT and the UPDATE.");
  }
  const auto key = in.body.value("key", "");
  bool key_in_columns = false;
  for (const auto& c : in.body["columns"]) {
    if (!c.is_string()) detail::fail(at + ".columns entries must be strings", "");
    if (c.get<std::string>() == key) key_in_columns = true;
  }
  if (!key_in_columns) {
    detail::fail(at + ".columns does not include the key \"" + key + "\"",
                 "The key is what ON matches source rows to target rows by, so "
                 "the source has to carry it.");
  }

  const auto matched = in.body.value("when_matched", "update");
  if (in.body.contains("when_matched") &&
      (!in.body["when_matched"].is_string() ||
       (matched != "update" && matched != "delete" && matched != "nothing"))) {
    detail::fail(at + ".when_matched must be \"update\", \"delete\" or \"nothing\"",
                 "What to do with a source row that already has a match in the "
                 "target. \"update\" is the default.");
  }
  const auto not_matched = in.body.value("when_not_matched", "insert");
  if (in.body.contains("when_not_matched") &&
      (!in.body["when_not_matched"].is_string() ||
       (not_matched != "insert" && not_matched != "nothing"))) {
    detail::fail(at + ".when_not_matched must be \"insert\" or \"nothing\"", "");
  }
  const auto by_source = in.body.value("when_not_matched_by_source", "nothing");
  if (in.body.contains("when_not_matched_by_source") &&
      (!in.body["when_not_matched_by_source"].is_string() ||
       (by_source != "nothing" && by_source != "delete"))) {
    detail::fail(at + ".when_not_matched_by_source must be \"nothing\" or \"delete\"",
                 "\"delete\" removes every target row the source does not "
                 "mention, which turns this from an upsert into a full "
                 "replacement of the table's contents.");
  }
  if (matched == "nothing" && not_matched == "nothing" && by_source == "nothing") {
    detail::fail(at + " would do nothing",
                 "All three WHEN branches are \"nothing\", so the statement has "
                 "no effect. At least one branch has to act.");
  }
  if (in.body.contains("update_columns")) {
    if (matched != "update") {
      detail::fail(at + " has \"update_columns\" but when_matched is \"" +
                       matched + "\"",
                   "Nothing would be updated, so the list has no effect.");
    }
    if (!in.body["update_columns"].is_array() || in.body["update_columns"].empty()) {
      detail::fail(at + ".update_columns must be a non-empty array", "");
    }
    for (const auto& c : in.body["update_columns"]) {
      if (!c.is_string()) detail::fail(at + ".update_columns entries must be strings", "");
      detail::require_identifier(c.get<std::string>(), "update_columns", in.ordinal);
    }
  }
  detail::parse_preserve_and_invariants(in, at);
}

// --- copy_rows --------------------------------------------------------------
//
// COPY is here because it is one of PostgreSQL's ways to write rows and leaving
// it out would be a gap rather than a decision. What the planner has to say
// about it is mostly what it CANNOT do: measured on 18.6 (S21), a COPY whose
// second of three rows violates a constraint rolls back all three, so it is one
// transaction's worth of work whatever its size and the paced executor has
// nothing to pace. It is not a fast path around constraints either -- foreign
// keys and row triggers fire exactly as they do for INSERT.
//
// NO `WITH` OPTIONS, AND THE REASON IS THE CLIENT LIBRARY RATHER THAN A
// JUDGEMENT. PostgreSQL 17 and 18 added ON_ERROR ignore, LOG_VERBOSITY and
// REJECT_LIMIT, and they were in an earlier draft of this kind. libpqxx 7.10
// offers exactly one sanctioned way to send COPY data -- pqxx::stream_to --
// which builds its own statement and accepts no options; the two entry points
// that would allow one, connection::raw_connection() and write_copy_line(), are
// private and reachable only through internal gate classes. Probed against
// 18.6, stream_to sends:
//
//     COPY cf_probe(id, code) FROM STDIN
//
// This tool records the statement that ran, verbatim, and it will not accept a
// spec key it cannot honour -- a spec asking for ON_ERROR ignore and getting a
// COPY that stops on the first bad row would be worse than no COPY at all. So
// those keys are refused here, by name, rather than silently dropped.
//
// The gap is narrow in practice: ON_ERROR ignore is a data-loading convenience,
// and a migration that is willing to skip rows it cannot parse is not really
// making a reviewed change. Reaching it would mean linking libpq directly,
// which this binary deliberately does not (libpq arrives through libpqxx, and
// the packaging derives its dependencies from what is actually linked).
inline void parse_copy_rows(Intent& in) {
  const std::string at = "intents[" + std::to_string(in.ordinal) + "]";
  for (const char* unsupported : {"on_error", "reject_limit", "freeze"}) {
    if (in.body.contains(unsupported)) {
      detail::fail(at + " uses \"" + unsupported +
                       "\", which this build cannot send",
                   "COPY's WITH options -- ON_ERROR, REJECT_LIMIT, "
                   "LOG_VERBOSITY, FREEZE -- cannot be expressed through "
                   "libpqxx's COPY interface, which builds its own "
                   "\"COPY t(cols) FROM STDIN\" and takes no options. Rather "
                   "than accept the key and quietly send a statement that does "
                   "something else, it is refused. For rows that may not parse, "
                   "insert_rows with \"on_conflict\": \"skip\" handles the "
                   "duplicate case and reports honestly on the rest.");
    }
  }
  detail::reject_unknown_keys(
      in.body, {"kind", "schema", "table", "columns", "values",
                "verify_remaining", "assert_invariants"}, at);
  detail::require_identifier(detail::require_string(in.body, "schema", at), "schema", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "table", at), "table", in.ordinal);

  // No `select` form: COPY ... FROM takes a stream, not a query. Someone
  // reaching for that wants insert_rows, and saying so beats a generic
  // unknown-key refusal.
  if (in.body.contains("select")) {
    detail::fail(at + " has \"select\", which COPY cannot take",
                 "COPY ... FROM reads a stream of rows, not a query. To insert "
                 "the result of a query, use insert_rows with \"select\" -- "
                 "which also paces, where a COPY cannot.");
  }
  detail::parse_row_source(in, at, "", false);
  if (!in.body.contains("values")) {
    detail::fail(at + " needs \"values\"",
                 "copy_rows sends literal rows over the protocol as "
                 "COPY ... FROM STDIN. Reading a server-side file would need "
                 "superuser and would put the real change somewhere the "
                 "signature does not cover.");
  }
  if (!in.body.contains("columns") || !in.body["columns"].is_array() ||
      in.body["columns"].empty()) {
    detail::fail(at + " needs a non-empty \"columns\"",
                 "COPY with no column list binds by position, so a table that "
                 "gains a column would silently shift every value.");
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

  if (doc.contains("release")) {
    // A tag, not a version: pg_laswell never orders releases or reasons about
    // what one supersedes. It asks the target database one question -- has this
    // tag been approved here -- and that is a lookup, not a comparison.
    s.release = detail::require_string(doc, "release", "spec");
  }

  if (doc.contains("target")) {
    if (!doc["target"].is_object()) {
      detail::fail("\"target\" must be an object", "");
    }
    detail::reject_unknown_keys(
        doc["target"], {"database", "environment", "min_server_version"},
        "target");
    s.target_database = doc["target"].value("database", "");
    if (doc["target"].contains("environment")) {
      s.target_environment = detail::require_string(doc["target"], "environment",
                                                    "target");
    }
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
    case IntentKind::kInsertRows: parse_insert_rows(in); break;
    case IntentKind::kUpdateRows: parse_update_rows(in); break;
    case IntentKind::kMergeRows:  parse_merge_rows(in);  break;
    case IntentKind::kCopyRows:   parse_copy_rows(in);   break;
    case IntentKind::kSetRowSecurity: parse_set_row_security(in); break;
    case IntentKind::kCreatePolicy: parse_create_policy(in); break;
    case IntentKind::kDropPolicy: parse_drop_policy(in); break;
    case IntentKind::kSetTriggerState: parse_set_trigger_state(in); break;
    case IntentKind::kGrant:
    case IntentKind::kRevoke: parse_grant_like(in); break;
    case IntentKind::kCreateSchema: parse_schema_like(in, true); break;
    case IntentKind::kDropSchema: parse_schema_like(in, false); break;
    case IntentKind::kCreateExtension: parse_create_extension(in); break;
    case IntentKind::kDropExtension: parse_drop_extension(in); break;
    case IntentKind::kCreateType: parse_create_type(in); break;
    case IntentKind::kDropType: parse_named_object(in); break;
    case IntentKind::kAddEnumValue: parse_add_enum_value(in); break;
    case IntentKind::kCreateFunction: parse_create_function(in); break;
    case IntentKind::kDropFunction: parse_drop_function(in); break;
    case IntentKind::kCreateTrigger: parse_create_trigger(in); break;
    case IntentKind::kDropTrigger: parse_drop_trigger(in); break;
    case IntentKind::kCreateSequence: parse_create_sequence(in); break;
    case IntentKind::kDropSequence: parse_named_object(in); break;
    case IntentKind::kDropView: parse_named_object(in); break;
    case IntentKind::kAlterColumnDefault: parse_alter_column_default(in); break;
    case IntentKind::kDropNotNull: parse_drop_not_null(in); break;
    case IntentKind::kAlterSequence: parse_alter_sequence(in); break;
    case IntentKind::kAlterSchema: parse_alter_schema(in); break;
    case IntentKind::kAlterExtension: parse_alter_extension(in); break;
    case IntentKind::kAlterDomain: parse_alter_domain(in); break;
    case IntentKind::kAlterFunction: parse_alter_function(in); break;
    case IntentKind::kAlterView: parse_alter_view(in); break;
    case IntentKind::kAlterPolicy: parse_alter_policy(in); break;
    case IntentKind::kSetComment: parse_set_comment(in); break;
    case IntentKind::kSetOwner: parse_set_owner(in); break;
    case IntentKind::kSetIdentity: parse_set_identity(in); break;
    case IntentKind::kDropExpression: parse_drop_not_null(in); break;
    case IntentKind::kSetColumnOptions: parse_set_column_options(in); break;
    case IntentKind::kSetTableOptions: parse_set_table_options(in); break;
    case IntentKind::kSetLogged: parse_set_logged(in); break;
    case IntentKind::kSetTablespace: parse_set_tablespace(in); break;
    case IntentKind::kSetAccessMethod: parse_set_access_method(in); break;
    case IntentKind::kSetReplicaIdentity: parse_set_replica_identity(in); break;
    case IntentKind::kClusterOn: parse_cluster_on(in); break;
    case IntentKind::kCreateMaterializedView: parse_create_matview(in); break;
    case IntentKind::kCreateStatistics: parse_create_statistics(in); break;
    case IntentKind::kDropStatistics: parse_named_object(in); break;
    case IntentKind::kCreateRule: parse_create_rule(in); break;
    case IntentKind::kDropRule: parse_drop_rule(in); break;
    case IntentKind::kCreatePublication:
    case IntentKind::kAlterPublication: parse_publication(in); break;
    case IntentKind::kDropPublication: parse_replication_name(in); break;
    case IntentKind::kCreateSubscription:
    case IntentKind::kAlterSubscription: parse_subscription(in); break;
    case IntentKind::kDropSubscription: parse_replication_name(in); break;
    case IntentKind::kCreateObject: parse_create_object(in); break;
    case IntentKind::kDropObject: parse_drop_object(in); break;
    case IntentKind::kAlterObject: parse_alter_object(in); break;
    case IntentKind::kCreateTableAs: parse_create_table_as(in); break;
    case IntentKind::kImportForeignSchema: parse_import_foreign_schema(in); break;
    case IntentKind::kSecurityLabel: parse_security_label(in); break;
    case IntentKind::kAlterDefaultPrivileges: parse_alter_default_privileges(in); break;
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
