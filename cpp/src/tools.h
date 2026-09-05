#pragma once

// The MCP tool set.
//
// Declared once, in one vector. tools/list and dispatch are both derived from
// it, so forgetting to register a tool is not possible: the entry IS the
// registration.
//
// Phase 3.5 ships the four that execute nothing:
//
//   checkPrivileges  what this role can actually do here
//   getSpecDigest    the canonical bytes to sign, and their digest
//   validateSpec     parse, both trust gates, and the ledger's state
//   planMigration    measure, plan, and show which reading decided each choice
//
// startMigration, jobStatus and cancelJob arrive with the executor in phase 4.

#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "catalog.h"
#include "config.h"
#include "ledger.h"
#include "planner.h"
#include "server.h"
#include "spec.h"
#include "trust.h"

namespace pglaswell {

// Everything a tool needs to answer. Held by the server and handed to each
// invocation, so a tool body reaches for its dependencies rather than
// constructing them.
struct ToolContext {
  Registry registry;
  ConnectionCache* cache = nullptr;

  const ConnConfig& connection(const json& args) const {
    const auto name = args.value("connection", registry.default_name());
    if (name.empty()) {
      throw std::runtime_error(
          "no connection is configured; pass a conninfo argument, set "
          "DATABASE_URL, or use --config");
    }
    return registry.get(name);
  }
};

namespace detail {

// A tool argument that names a connection. Added to every tool's schema in one
// place rather than repeated per tool.
inline json connection_property() {
  return json{{"connection",
               {{"type", "string"},
                {"description",
                 "name of a configured connection; defaults to the first in "
                 "the registry"}}}};
}

inline json spec_property() {
  return json{{"spec",
               {{"type", "object"},
                {"description",
                 "the migration specification, as a JSON object"}}}};
}

// Turns a SpecError into the {error, hint} shape every failure here takes. The
// hint names the exact thing to change; a hint that does not is not a hint.
inline json spec_error_payload(const SpecError& e) {
  return json{{"error", e.what()}, {"hint", e.hint()}};
}

}  // namespace detail

// --- checkPrivileges -------------------------------------------------------
//
// Worth calling first against an unfamiliar connection. A privilege-filtered
// answer is easy to mistake for an empty one, and this project has a sharper
// version of that problem than pg_licht does: a role that cannot see other
// backends' identities still sees their ungranted locks, so pacing works while
// the reported waiter is anonymous. That is `degraded`, not `denied`, and
// saying so is the whole point of the tool.
inline const char* kCheckPrivilegesSql = R"SQL(
SELECT JSONB_BUILD_OBJECT(
  'role', current_user,
  'isSuperuser', (SELECT usesuper FROM pg_user WHERE usename = current_user),
  'memberOf', JSONB_BUILD_OBJECT(
     'pg_monitor',        pg_has_role(current_user, 'pg_monitor', 'MEMBER'),
     'pg_read_all_stats', pg_has_role(current_user, 'pg_read_all_stats', 'MEMBER')),
  'ledger', JSONB_BUILD_OBJECT(
     'schemaPresent', to_regclass('laswell.trusted_key') IS NOT NULL,
     'canReadTrustedKey', CASE WHEN to_regclass('laswell.trusted_key') IS NULL
        THEN NULL ELSE has_table_privilege('laswell.trusted_key', 'SELECT') END,
     'canWriteTrustedKey', CASE WHEN to_regclass('laswell.trusted_key') IS NULL
        THEN NULL ELSE has_table_privilege('laswell.trusted_key', 'INSERT') END,
     'canWriteJob', CASE WHEN to_regclass('laswell.job') IS NULL
        THEN NULL ELSE has_table_privilege('laswell.job', 'INSERT') END),
  'isStandby', pg_is_in_recovery()
)
)SQL";

inline json check_privileges(ToolContext& ctx, const json& args) {
  const auto& cfg = ctx.connection(args);
  ReadSession s(cfg, std::nullopt, ctx.cache, 2000);
  const auto r = s.txn().exec(kCheckPrivilegesSql);
  json out = json::parse(r[0][0].as<std::string>());
  out["serverVersion"] = s.server_version();

  json notes = json::array();
  if (!out["memberOf"].value("pg_read_all_stats", false) &&
      !out.value("isSuperuser", false)) {
    notes.push_back(
        "degraded: without pg_read_all_stats this role cannot see other "
        "backends' query text or application_name. Lock waiters are still "
        "counted -- pg_locks and pg_blocking_pids() are visible to any role -- "
        "so pacing works, but a waiter will be reported anonymously. Do not "
        "read that as 'nobody is waiting'.");
  }
  if (out["ledger"].value("schemaPresent", false) &&
      out["ledger"].value("canWriteTrustedKey", false)) {
    notes.push_back(
        "warning: this role can INSERT into laswell.trusted_key, so it can "
        "grant itself trust. The trust gate is only as strong as that "
        "privilege boundary -- run bootstrap.sql as a different role, and "
        "REVOKE INSERT, UPDATE, DELETE ON laswell.trusted_key from this one.");
  }
  if (out.value("isStandby", false)) {
    notes.push_back(
        "denied: this connection is a standby. pg_laswell will not apply a "
        "migration to a replica.");
  }
  out["notes"] = notes;
  return out;
}

// --- getSpecDigest ---------------------------------------------------------
//
// Emits exactly the bytes that will be verified, so an author signs the thing
// that will actually be checked rather than a re-serialisation of it. This
// binary never signs; the private key stays out of the process.
inline json get_spec_digest(ToolContext&, const json& args) {
  if (!args.contains("spec") || !args["spec"].is_object()) {
    return json{{"error", "getSpecDigest needs a \"spec\" object"},
                {"hint", "Pass the specification document as the spec argument."}};
  }
  try {
    const auto spec = parse_spec(args["spec"]);
    return json{
        {"specId", spec.id},
        {"digest", spec.digest},
        {"canonicalBytes", spec.canonical_bytes},
        {"signWith",
         "openssl pkeyutl -sign -inkey <key>.pem -rawin -in <canonical bytes "
         "written to a file> | base64 -w0"},
        {"note",
         "Sign these bytes exactly. They are the RFC 8785 canonical form of "
         "the signed projection, so reformatting the spec file afterwards does "
         "not invalidate the signature -- but re-serialising through a "
         "different canonicaliser might."}};
  } catch (const SpecError& e) {
    return detail::spec_error_payload(e);
  }
}

// --- validateSpec ----------------------------------------------------------
inline json validate_spec(ToolContext& ctx, const json& args) {
  if (!args.contains("spec") || !args["spec"].is_object()) {
    return json{{"error", "validateSpec needs a \"spec\" object"},
                {"hint", "Pass the specification document as the spec argument."}};
  }

  Spec spec;
  try {
    spec = parse_spec(args["spec"]);
  } catch (const SpecError& e) {
    return detail::spec_error_payload(e);
  }

  json out{{"specId", spec.id},
           {"digest", spec.digest},
           {"description", spec.description},
           {"intentCount", spec.intents.size()}};

  // Gate 1: the local policy. Fails offline, before a connection is opened, so
  // a wrong-environment mistake costs a millisecond rather than a round trip.
  const auto gate1 = verify_against_policy(ctx.registry.trust(),
                                           spec.canonical_bytes, spec.signatures);
  out["clientTrust"] = json{{"accepted", gate1.verified},
                            {"configured", !gate1.not_configured},
                            {"keyId", gate1.key_id},
                            {"label", gate1.label}};
  if (!gate1.verified) out["clientTrust"]["reason"] = gate1.reason;

  const auto& cfg = ctx.connection(args);
  Ledger ledger(cfg, ctx.cache);
  const auto st = ledger.status();
  out["ledger"] = st.to_json();

  // Only a missing or wrong-version ledger short-circuits: in those cases
  // nothing can be said about the signer, and saying something anyway would be
  // a guess. An empty trusted-key set is NOT one of these -- it is a trust
  // answer, and gate 2 below reports it as one.
  if (!st.usable) {
    out["accepted"] = false;
    out["error"] = st.error;
    out["hint"] = st.hint;
    return out;
  }

  // Gate 2: the database. Authoritative, because it survives a wrong, stale or
  // permissive copy of anyone's config, and because it verifies against the
  // key bytes stored here rather than the ones the client offered.
  const auto gate2 = ledger.verify_signer(spec);
  out["databaseTrust"] = json{{"accepted", gate2.verified},
                              {"keyId", gate2.key_id},
                              {"label", gate2.label}};
  if (!gate2.verified) out["databaseTrust"]["reason"] = gate2.reason;

  // An unconfigured local policy is not a refusal: gate 2 decides.
  out["accepted"] = (gate1.verified || gate1.not_configured) && gate2.verified;
  if (!out["accepted"]) {
    out["error"] = gate2.verified ? gate1.reason : gate2.reason;
    out["hint"] =
        gate2.verified
            ? "This machine's laswell.ini does not accept that key. Add it to "
              "[trust] accept, with a matching [key <id>] section."
            : (st.hint.empty()
                   ? "The target database does not trust that signer. Trust is "
                     "granted in laswell.trusted_key by a privileged role, "
                     "deliberately not by pg_laswell."
                   : st.hint);
  }
  return out;
}

// --- planMigration ---------------------------------------------------------
//
// Measures, plans, and executes NOTHING. planMigration and startMigration share
// this entire path, so what startMigration runs is byte-identically what this
// showed -- asserted by comparing planDigest.
inline json plan_migration_tool(ToolContext& ctx, const json& args) {
  if (!args.contains("spec") || !args["spec"].is_object()) {
    return json{{"error", "planMigration needs a \"spec\" object"},
                {"hint", "Pass the specification document as the spec argument."}};
  }

  Spec spec;
  try {
    spec = parse_spec(args["spec"]);
  } catch (const SpecError& e) {
    return detail::spec_error_payload(e);
  }

  const auto& cfg = ctx.connection(args);
  const bool skip_trust = args.value("skipTrustChecks", false);
  if (!skip_trust) {
    const auto gate1 = verify_against_policy(ctx.registry.trust(),
                                             spec.canonical_bytes, spec.signatures);
    if (!gate1.verified && !gate1.not_configured) {
      return json{{"error", "the spec is not accepted by this machine: " + gate1.reason},
                  {"hint",
                   "Add the key to [trust] accept in laswell.ini, or pass "
                   "skipTrustChecks to plan an unsigned draft. A plan produced "
                   "with skipTrustChecks can never be executed."}};
    }
    Ledger ledger(cfg, ctx.cache);
    const auto gate2 = ledger.verify_signer(spec);
    if (!gate2.verified) {
      return json{{"error", "the target database does not accept this spec: " + gate2.reason},
                  {"hint",
                   "Trust is granted in laswell.trusted_key by a privileged "
                   "role. Call validateSpec for the full picture."}};
    }
  }

  // Observe only the tables the spec names. Nothing else is measured, so the
  // reading cannot be blamed on a table the change does not touch.
  std::vector<std::string> schemas, tables;
  for (const auto& in : spec.intents) {
    schemas.push_back(in.schema());
    tables.push_back(in.table());
  }
  Catalog cat(cfg, ctx.cache);
  const auto obs = cat.observe(schemas, tables);
  const auto plan = plan_migration(spec, obs, cfg.executor);

  json out = plan.to_json();

  // Apply the plan in a transaction that never commits, so the later steps are
  // checked against the schema the earlier ones produce. This is the property
  // the whole project rests on: PostgreSQL DDL is transactional, so a plan can
  // be tried before it is trusted.
  if (plan.ok && args.value("dryRun", true)) {
    std::vector<std::pair<int, std::vector<std::string>>> steps;
    std::vector<bool> forbidden;
    for (const auto& step : plan.steps) {
      if (step.action != Action::kApply) continue;
      steps.emplace_back(step.ordinal, step.sql);
      forbidden.push_back(step.txn_class == TxnClass::kForbidden);
    }
    const auto dry = cat.dry_run(steps, forbidden, obs.server_version);
    json d{{"ran", dry.ran},
           {"unverifiedSteps", dry.unverified_steps},
           {"note",
            "the plan was applied in a transaction and rolled back; nothing "
            "was committed. Steps listed as unverified cannot run inside a "
            "transaction block (CREATE INDEX CONCURRENTLY) and were skipped "
            "rather than silently passed."}};
    if (!dry.skipped_reason.empty()) d["skippedReason"] = dry.skipped_reason;
    if (!dry.problems.empty()) {
      d["problems"] = dry.problems;
      out["ok"] = false;
      out["error"] = "the plan failed when applied to a rolled-back transaction";
      std::string hint =
          "The statements above did not run against the real schema. If the "
          "spec looks right, this is a pg_laswell defect -- please report it "
          "with the rendered plan.";
      for (const auto& prob : dry.problems) {
        if (prob.find("FROM-clause") != std::string::npos ||
            prob.find("FROM") != std::string::npos) {
          hint =
              "A backfill's where/set/from expressions reference the target "
              "table BY ITS OWN NAME (\"orders.warehouse_id\"), not by an "
              "alias (\"t.warehouse_id\"). Tables introduced by \"from\" "
              "are referenced by the alias \"from\" gives them.";
          break;
        }
      }
      out["hint"] = hint;
    }
    out["dryRun"] = d;
  }

  out["planDigest"] = plan.digest();
  out["rendered"] = plan.render();
  out["executed"] = false;
  if (skip_trust) {
    out["draft"] = true;
    out["note"] =
        "planned with skipTrustChecks: signatures were not verified, and this "
        "plan cannot be executed.";
  }
  return out;
}

// --- registration ----------------------------------------------------------

inline json no_args_schema() {
  return json{{"type", "object"},
              {"properties", detail::connection_property()}};
}

inline json spec_schema() {
  json props = detail::spec_property();
  props.update(detail::connection_property());
  return json{{"type", "object"},
              {"properties", props},
              {"required", json::array({"spec"})}};
}

inline json plan_schema_in() {
  json props = detail::spec_property();
  props.update(detail::connection_property());
  props["skipTrustChecks"] = {
      {"type", "boolean"},
      {"description",
       "plan an unsigned draft without verifying signatures. Such a plan can "
       "never be executed."}};
  return json{{"type", "object"},
              {"properties", props},
              {"required", json::array({"spec"})}};
}

// The sole registration site. One entry per tool; tools/list and dispatch are
// both derived from this, so a tool cannot exist without being listed and
// cannot be listed without existing.
inline std::vector<ToolDef> make_tools(ToolContext& ctx) {
  std::vector<ToolDef> tools;

  tools.push_back(ToolDef{
      "checkPrivileges",
      "report what the connecting role can actually do here: whether the "
      "laswell ledger is installed, whether this role can read the trust table "
      "and write the ledger, and whether it can see other backends. Worth "
      "calling first against an unfamiliar connection, because a "
      "privilege-filtered answer is easy to mistake for an empty one.",
      no_args_schema,
      [] { return json{{"type", "object"}}; },
      {true, false, true, false},
      [&ctx](const json& a) { return check_privileges(ctx, a); }});

  tools.push_back(ToolDef{
      "getSpecDigest",
      "return the exact canonical bytes to sign for a specification, and their "
      "SHA-256 digest. Sign these bytes; pg_laswell verifies signatures but "
      "never creates them, so the private key stays outside this process.",
      spec_schema,
      [] { return json{{"type", "object"}}; },
      {true, false, true, false},
      [&ctx](const json& a) { return get_spec_digest(ctx, a); }});

  tools.push_back(ToolDef{
      "validateSpec",
      "parse a specification and check it against both trust gates: this "
      "machine's configured keys, and the keys the target database itself "
      "trusts. Reports the ledger's state too, so 'you have not bootstrapped' "
      "is distinguishable from 'your signer is not trusted here'.",
      spec_schema,
      [] { return json{{"type", "object"}}; },
      {true, false, true, false},
      [&ctx](const json& a) { return validate_spec(ctx, a); }});

  tools.push_back(ToolDef{
      "planMigration",
      "measure the target tables and produce the ordered plan that would be "
      "applied, saying which reading decided each choice. Executes nothing. "
      "The plan names its transaction-group boundaries, which is where "
      "atomicity ends, and carries a planDigest that startMigration must "
      "reproduce.",
      plan_schema_in,
      [] { return json{{"type", "object"}}; },
      {true, false, true, false},
      [&ctx](const json& a) { return plan_migration_tool(ctx, a); }});

  return tools;
}

}  // namespace pglaswell
