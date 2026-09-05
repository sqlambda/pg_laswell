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
#include "executor.h"
#include "jobs.h"
#include "ledger.h"
#include "planner.h"
#include "repository.h"
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
  JobRegistry* jobs = nullptr;
  Observer* observer = nullptr;

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
    if (dry.depends_on_skipped) {
      d["note"] =
          "verification stopped at a step that depends on one which cannot run "
          "inside a transaction block -- a partitioned index recipe attaches "
          "the child indexes its concurrent builds would have made, and a dry "
          "run cannot make them. Those steps are unverified rather than wrong.";
    }
    if (dry.timed_out_at >= 0) {
      d["timedOutAtStep"] = dry.timed_out_at;
      d["note"] =
          "the dry run stopped at a step that does real work -- a scan, most "
          "likely -- rather than holding the earlier steps' locks for the "
          "length of it. A planning call must never block writes. Everything "
          "from that step on is unverified; raise "
          "dry_run_statement_timeout_ms if you want it checked, knowing what "
          "that costs.";
    }
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

// --- startMigration --------------------------------------------------------
//
// Returns immediately with a job id. The work outlives the call, because the
// stdio loop must stay answerable while a migration runs -- that is the whole
// reason the job registry exists.
inline json start_migration(ToolContext& ctx, const json& args) {
  if (ctx.jobs == nullptr) {
    return json{{"error", "this server has no job registry"},
                {"hint", "startMigration is unavailable in this build."}};
  }
  const auto planned = plan_migration_tool(ctx, args);
  if (planned.contains("error")) return planned;
  if (!planned.value("ok", false)) {
    json out = planned;
    out["accepted"] = false;
    out["hint"] = "The plan was refused; nothing was started.";
    return out;
  }
  if (planned.value("draft", false)) {
    return json{{"error", "a draft plan cannot be executed"},
                {"hint",
                 "This plan was produced with skipTrustChecks, so its "
                 "signatures were never verified. Sign the spec and plan again."}};
  }

  const auto& cfg = ctx.connection(args);
  if (ctx.jobs->running_count() >= cfg.executor.max_concurrent_jobs) {
    return json{{"error", "the concurrency limit is reached"},
                {"hint", "max_concurrent_jobs is " +
                             std::to_string(cfg.executor.max_concurrent_jobs) +
                             "; wait for a job to finish or raise it in "
                             "[executor]."},
                {"running", ctx.jobs->running_count()}};
  }

  const auto spec = parse_spec(args["spec"]);
  Ledger status_ledger(cfg, ctx.cache);
  const auto signer = status_ledger.verify_signer(spec);
  if (!signer.verified) {
    return json{{"error", signer.reason}, {"hint", "Call validateSpec."}};
  }

  auto job = ctx.jobs->create(new_job_id());
  job->spec_id = spec.id;
  job->spec_digest = spec.digest;
  job->signer_key_id = signer.key_id;
  job->connection = cfg.name;
  job->plan = planned;
  job->plan_digest = planned.value("planDigest", "");

  // One lock per spec and one per target table, so two different specs
  // touching the same table serialise.
  std::vector<long long> keys{advisory_key("laswell:spec:" + spec.id)};
  std::set<std::string> targets;
  for (const auto& in : spec.intents) targets.insert(in.qualified_table());
  for (const auto& t : targets) keys.push_back(advisory_key("laswell:table:" + t));
  job->lock_key = keys.front();

  auto ledger = std::make_shared<Ledger>(cfg, nullptr);
  long long migration_id = 0;
  try {
    migration_id = ledger->record_migration(spec, signer.key_id);
  } catch (const std::exception& e) {
    job->state = JobState::kFailed;
    return json{{"error", std::string("could not record the migration: ") + e.what()},
                {"hint",
                 "laswell.migration.signer_key_id references trusted_key, so "
                 "an untrusted signer cannot open a job. Call validateSpec."}};
  }

  std::string conflict;
  if (!ledger->open_job(job->job_id, migration_id, planned,
                        job->plan_digest, planned.value("budget", json::object()),
                        0, keys, &conflict)) {
    job->state = JobState::kFailed;
    return json{{"error", conflict},
                {"hint", "Call jobStatus to see the job that holds it."}};
  }

  if (ctx.observer) ctx.observer->start();
  job->worker = std::thread([cfg, job, ledger] {
    Executor(cfg, job, ledger.get()).run();
  });

  return json{{"jobId", job->job_id},
              {"accepted", true},
              {"specId", spec.id},
              {"specDigest", spec.digest},
              {"planDigest", job->plan_digest},
              {"signerKeyId", signer.key_id},
              {"note",
               "the work runs in the background; poll jobStatus for progress, "
               "contention and an ETA. The planDigest above is the plan that "
               "is being executed -- it must equal what planMigration showed."}};
}

// --- jobStatus -------------------------------------------------------------
inline json job_snapshot(const Job& j, const ExecutorConfig& e) {
  json out{{"jobId", j.job_id},
           {"specId", j.spec_id},
           {"specDigest", j.spec_digest},
           {"signerKeyId", j.signer_key_id},
           {"planDigest", j.plan_digest},
           {"state", to_string(j.state.load())}};

  const auto now = std::chrono::steady_clock::now();
  const auto end = j.has_finished ? j.finished : now;
  out["elapsedS"] =
      std::chrono::duration<double>(end - j.started).count();

  out["contention"] = json{
      {"directWaiters", j.pacing.direct_waiters.load()},
      {"transitiveWaiters", j.pacing.transitive_waiters.load()},
      {"blockingWaiterPid", j.pacing.blocking_waiter_pid.load()},
      {"oldestWaitMs", j.pacing.oldest_wait_ms.load()},
      {"inflictedBlockedMs", j.pacing.inflicted_blocked_ms.load()},
      {"throttled", j.pacing.throttled.load()},
      {"paused", j.pacing.paused.load()},
      {"thresholds", {{"throttleWaiters", e.throttle_waiters},
                      {"pauseWaiters", e.pause_waiters},
                      {"resumeWaiters", e.resume_waiters}}},
      {"note",
       "counts are TRANSITIVE: a direct-blocker count understates a pile-up, "
       "because pg_blocking_pids() returns direct blockers only. This measures "
       "harm this job causes, and is blind to harm it causes without holding a "
       "lock -- I/O, WAL volume, replication lag. Read those through pg_licht."}};

  if (j.pacing.cic_lockers_total.load() > 0) {
    out["createIndex"] = json{
        {"lockersTotal", j.pacing.cic_lockers_total.load()},
        {"lockersDone", j.pacing.cic_lockers_done.load()},
        {"currentLockerPid", j.pacing.cic_current_locker_pid.load()},
        {"note",
         "a concurrent build waits for transactions that could see the table. "
         "One idle-in-transaction backend stalls it indefinitely while holding "
         "only ShareUpdateExclusiveLock, so nothing looks blocked."}};
  }

  out["workerPid"] = j.pacing.worker_pid.load();
  out["steps"] = j.steps;
  if (!j.backfill.empty()) out["backfill"] = j.backfill;
  if (!j.warnings.empty()) out["warnings"] = j.warnings;
  if (!j.error.is_null()) out["error"] = j.error;
  return out;
}

inline json job_status(ToolContext& ctx, const json& args) {
  if (ctx.jobs == nullptr) {
    return json{{"jobs", json::array()},
                {"error", "this server has no job registry"}};
  }
  const auto& cfg = ctx.connection(args);
  const auto wanted = args.value("jobId", "");

  json jobs = json::array();
  for (const auto& j : ctx.jobs->all()) {
    if (!wanted.empty() && j->job_id != wanted) continue;
    // A SNAPSHOT COPY, taken under the job's own lock and never a reference
    // into live state. This is the invariant that makes the concurrency story
    // auditable in one paragraph.
    std::lock_guard<std::mutex> lock(j->m);
    jobs.push_back(job_snapshot(*j, cfg.executor));
  }

  json out{{"jobs", std::move(jobs)},
           {"concurrency", {{"running", ctx.jobs->running_count()},
                            {"limit", cfg.executor.max_concurrent_jobs}}}};
  try {
    Ledger ledger(cfg, ctx.cache);
    out["interrupted"] = ledger.interrupted_jobs();
  } catch (const std::exception&) {
    // The in-memory answer is still worth returning.
  }
  return out;
}

// --- cancelJob -------------------------------------------------------------
inline json cancel_job(ToolContext& ctx, const json& args) {
  if (ctx.jobs == nullptr) {
    return json{{"error", "this server has no job registry"}};
  }
  const auto id = args.value("jobId", "");
  auto job = ctx.jobs->find(id);
  if (!job) {
    return json{{"error", "no such job: " + id},
                {"hint", "Call jobStatus to list the jobs this server knows."}};
  }
  if (is_terminal(job->state.load())) {
    return json{{"jobId", id},
                {"state", to_string(job->state.load())},
                {"note", "the job had already finished; nothing to cancel"}};
  }
  job->pacing.cancel_stop = true;
  return json{{"jobId", id},
              {"requested", true},
              {"note",
               "cancellation is cooperative: the job stops at its next batch "
               "boundary, after committing what it has done. The cursor is "
               "committed too, so a later job for the same spec digest resumes "
               "from it rather than starting over."}};
}

// --- listMigrations --------------------------------------------------------
inline json list_migrations(ToolContext& ctx, const json& args) {
  const auto dir = args.value("directory", "");
  if (dir.empty()) {
    return json{{"error", "listMigrations needs a \"directory\""},
                {"hint", "Point it at the folder holding the migration specs."}};
  }
  const auto& cfg = ctx.connection(args);
  MigrationRepository repo(cfg, ctx.cache);
  auto out = repo.scan(dir, args.value("deriveRelations", true));
  out["connection"] = cfg.name;
  out["database"] = cfg.dbname;
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

  tools.push_back(ToolDef{
      "listMigrations",
      "read a directory of specs and report what is pending against THIS "
      "database, in dependency order, with the groups that may run "
      "concurrently. Also flags any spec that was edited after it was applied "
      "-- the database no longer matches the file that claims to describe it.",
      [] {
        json props{{"directory",
                    {{"type", "string"},
                     {"description", "folder holding the migration specs"}}},
                   {"deriveRelations",
                    {{"type", "boolean"},
                     {"description",
                      "derive what each migration touches by planning it and "
                      "reading foreign keys; needed for concurrency advice. "
                      "Default true."}}}};
        props.update(detail::connection_property());
        return json{{"type", "object"},
                    {"properties", props},
                    {"required", json::array({"directory"})}};
      },
      [] { return json{{"type", "object"}}; },
      {true, false, true, false},
      [&ctx](const json& a) { return list_migrations(ctx, a); }});

  tools.push_back(ToolDef{
      "startMigration",
      "plan and then apply a signed specification. Returns a jobId "
      "immediately; the work runs in the background. The plan it executes is "
      "byte-identically what planMigration showed, which the matching "
      "planDigest proves.",
      plan_schema_in,
      [] {
        return json{{"type", "object"},
                    {"properties",
                     {{"jobId", {{"type", "string"}}},
                      {"planDigest", {{"type", "string"}}}}}};
      },
      {false, true, false, true},
      [&ctx](const json& a) { return start_migration(ctx, a); }});

  tools.push_back(ToolDef{
      "jobStatus",
      "report every migration this server is running: per-step progress, the "
      "backfill's commit-reason histogram, and the contention it is causing "
      "measured as backends TRANSITIVELY blocked by it. Also lists jobs that "
      "died with their connection.",
      [] {
        return json{{"type", "object"},
                    {"properties",
                     {{"jobId",
                       {{"type", "string"},
                        {"description", "one job; omit for all of them"}}},
                      {"connection", {{"type", "string"}}}}}};
      },
      [] { return json{{"type", "object"}}; },
      {true, false, true, false},
      [&ctx](const json& a) { return job_status(ctx, a); }});

  tools.push_back(ToolDef{
      "cancelJob",
      "ask a running migration to stop. Cooperative: it stops at the next "
      "batch boundary after committing what it has done, and the committed "
      "cursor lets a later job resume rather than start over.",
      [] {
        return json{{"type", "object"},
                    {"properties", {{"jobId", {{"type", "string"}}}}},
                    {"required", json::array({"jobId"})}};
      },
      [] { return json{{"type", "object"}}; },
      {false, false, true, false},
      [&ctx](const json& a) { return cancel_job(ctx, a); }});

  return tools;
}

}  // namespace pglaswell
