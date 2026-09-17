#pragma once

// The migration repository: a directory of signed specs, and what is pending.
//
// Two questions this answers, and the second is worth more than the first:
//
//  1. WHAT IS PENDING against this database. The ledger is keyed by digest, so
//     the comparison is exact.
//
//  2. WHETHER AN APPLIED SPEC HAS BEEN EDITED SINCE. A spec whose id was
//     applied under a DIFFERENT digest was changed after the fact, and the
//     database no longer matches the file that claims to describe it. Flyway
//     and Sqitch both consider this their most valuable check, and here it
//     falls out of digests for nothing.
//
// ORDERING AND PARALLELISM, and the asymmetry at the heart of it:
//
//   pg_laswell can prove two migrations CONFLICT -- they touch a shared
//   relation. It cannot prove they are INDEPENDENT. Triggers call functions
//   whose bodies the catalog does not expose, and business ordering is not
//   written anywhere a machine can read. So the tool reports what it derived,
//   names what it could not see, and the agent decides. Advisory locks refuse
//   an unsafe overlap regardless of that decision, so a wrong call is refused
//   rather than allowed to corrupt.

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "catalog.h"
#include "ledger.h"
#include "session.h"
#include "spec.h"

namespace pglaswell {

// What a migration touches, and how confident we are that the list is complete.
struct RelationSet {
  std::set<std::string> relations;   // schema.table
  std::vector<std::string> opaque;   // why the list may be incomplete
  bool complete() const { return opaque.empty(); }
};

// The hint tells an operator what to paste into psql, so the tag goes in as a
// SQL literal rather than between two apostrophes. A tag with an apostrophe in
// it produced advice that does not parse -- and advice that does not parse is
// the kind a tired operator edits until it runs.
inline std::string quote_sql_literal(const std::string& v) {
  std::string out = "'";
  for (const char c : v) {
    if (c == '\'') out += '\'';
    out += c;
  }
  return out + "'";
}

enum class RepoStatus {
  kPending,     // never applied here
  kApplied,     // this exact digest succeeded
  kModified,    // the id was applied under a DIFFERENT digest
  kInProgress,  // a job for this digest is running
  kFailed,      // the last job for this digest failed
  kUnreadable,  // the file does not parse
  // Not applicable here, and not a problem. A specification for another
  // environment is somebody else's migration; one whose release has not been
  // approved is this database's migration, waiting. Both are ordinary states of
  // a healthy repository, which is why neither is a `problem`: a deployment
  // that skips them has done its job correctly.
  kWrongEnvironment,
  kWrongDatabase,
  kHeldForRelease,
  // The specification names a connection this configuration does not define.
  // A repository problem rather than a gate: every other status answers "should
  // this run here", and this one says the question could not be asked.
  kUnknownConnection
};

inline const char* to_string(RepoStatus s) {
  switch (s) {
    case RepoStatus::kPending: return "pending";
    case RepoStatus::kApplied: return "applied";
    case RepoStatus::kModified: return "modified_after_apply";
    case RepoStatus::kInProgress: return "in_progress";
    case RepoStatus::kFailed: return "failed";
    case RepoStatus::kUnreadable: return "unreadable";
    case RepoStatus::kWrongEnvironment: return "wrong_environment";
    case RepoStatus::kWrongDatabase: return "wrong_database";
    case RepoStatus::kHeldForRelease: return "held_for_release";
    case RepoStatus::kUnknownConnection: return "unknown_connection";
  }
  return "unknown";
}

struct RepoEntry {
  std::string path;
  std::string spec_id;
  std::string digest;
  RepoStatus status = RepoStatus::kPending;
  std::vector<std::string> depends_on;
  RelationSet relations;
  std::string error;
  std::string hint;
  std::string applied_digest;  // set when status is kModified
  std::string connection;      // which configured connection it runs against
  std::string database;        // target.database, as the spec declares it
  std::string environment;     // target.environment, as the spec declares it
  std::string release;         // the release tag gating it, if any
};

// Derives what a spec actually touches.
//
// Three sources, and the third is a refusal to guess:
//
//   declared   the schema.table each intent names
//   planned    the relations PostgreSQL reports in the rendered SQL's plan --
//              which catches tables hidden inside opaque set/where expressions
//              that the spec never mentions
//   fk edges   both directions: writing a child takes a lock on the parent for
//              the constraint check, and vice versa
//
// Triggers are NOT resolved. The catalog says a trigger exists and names the
// function it calls; the function body is opaque, and the plan does not mention
// what that function writes -- verified 2026-09-05. So a target carrying
// triggers downgrades confidence and is named, rather than being silently
// treated as fully understood.
class RelationAnalyzer {
 public:
  RelationAnalyzer(ConnConfig cfg, ConnectionCache* cache)
      : cfg_(std::move(cfg)), cache_(cache) {}

  RelationSet analyze(const Spec& spec, const json& plan) {
    RelationSet out;
    // conflict_keys(), not qualified_table(): an object intent carries `name`,
    // not `table`, so the old seed produced "public." for every type in a
    // schema -- serialising unrelated migrations while missing real conflicts.
    for (const auto& in : spec.intents) {
      for (const auto& k : conflict_keys(in)) out.relations.insert(k);
    }

    ReadSession s(cfg_, std::nullopt, cache_, 2000);
    const int version = s.server_version();

    // Relations the planner actually resolves, including ones no intent named.
    if (version >= 160000 && plan.contains("steps")) {
      for (const auto& step : plan["steps"]) {
        for (const auto& raw : step.value("sql", json::array())) {
          add_planned(s, raw.get<std::string>(), out);
        }
      }
    } else if (version < 160000) {
      out.opaque.push_back(
          "EXPLAIN (GENERIC_PLAN) needs PostgreSQL 16; on this server the "
          "relations hidden inside set/where expressions cannot be derived");
    }

    // Foreign keys, both ways: these are real locks, not a guess.
    const auto declared = out.relations;
    for (const auto& rel : declared) add_fk_edges(s, rel, out);

    // Triggers: named, never resolved.
    for (const auto& rel : declared) add_trigger_warning(s, rel, out);
    return out;
  }

 private:
  static std::string strip(const std::string& q) {
    auto end = q.find_last_not_of(" \n\t\r;");
    return end == std::string::npos ? std::string() : q.substr(0, end + 1);
  }

  void add_planned(ReadSession& s, const std::string& raw, RelationSet& out) {
    const auto stmt = strip(raw);
    if (stmt.empty()) return;
    const auto head = stmt.substr(0, stmt.find_first_of(" \n"));
    const bool query = stmt.rfind("WITH", 0) == 0 || head == "UPDATE" ||
                       head == "INSERT" || head == "SELECT" || head == "DELETE" ||
                       head == "MERGE";
    if (!query) return;
    try {
      const auto r =
          s.txn().exec("EXPLAIN (GENERIC_PLAN, VERBOSE, FORMAT JSON) " + stmt);
      if (r.empty() || r[0][0].is_null()) return;
      collect(json::parse(r[0][0].as<std::string>()), out.relations);
    } catch (const pqxx::sql_error&) {
      // A statement that cannot be planned yet -- because an earlier migration
      // creates what it needs -- is not a failure here. It is a gap in the
      // evidence, and saying so is the point.
      out.opaque.push_back(
          "a statement could not be planned in isolation, so the relations it "
          "touches are not derived: it depends on schema an earlier migration "
          "creates");
      s.txn().abort();
      s.renew();
    }
  }

  static void collect(const json& node, std::set<std::string>& into) {
    if (node.is_array()) {
      for (const auto& n : node) collect(n, into);
      return;
    }
    if (!node.is_object()) return;
    if (node.contains("Relation Name")) {
      const auto rel = node["Relation Name"].get<std::string>();
      const auto schema = node.value("Schema", "");
      into.insert(schema.empty() ? rel : schema + "." + rel);
    }
    for (auto it = node.begin(); it != node.end(); ++it) collect(it.value(), into);
  }

  void add_fk_edges(ReadSession& s, const std::string& rel, RelationSet& out) {
    const auto dot = rel.find('.');
    if (dot == std::string::npos) return;
    try {
      const auto r = pqxx_exec(
          s.txn(),
          "SELECT DISTINCT n.nspname || '.' || c.relname"
          "  FROM pg_constraint k"
          "  JOIN pg_class c ON c.oid = CASE WHEN k.conrelid = $1::regclass"
          "                                  THEN k.confrelid ELSE k.conrelid END"
          "  JOIN pg_namespace n ON n.oid = c.relnamespace"
          " WHERE k.contype = 'f'"
          "   AND (k.conrelid = $1::regclass OR k.confrelid = $1::regclass)",
          pqxx::params{rel});
      for (const auto& row : r) out.relations.insert(row[0].as<std::string>());
    } catch (const pqxx::sql_error&) {
      s.txn().abort();
      s.renew();
    }
  }

  void add_trigger_warning(ReadSession& s, const std::string& rel,
                           RelationSet& out) {
    try {
      const auto r = pqxx_exec(
          s.txn(),
          "SELECT string_agg(t.tgname || ' -> ' || p.proname || '()', ', ')"
          "  FROM pg_trigger t JOIN pg_proc p ON p.oid = t.tgfoid"
          " WHERE t.tgrelid = $1::regclass AND NOT t.tgisinternal",
          pqxx::params{rel});
      if (!r.empty() && !r[0][0].is_null()) {
        out.opaque.push_back(
            rel + " carries triggers (" + r[0][0].as<std::string>() +
            "); what a trigger function writes is not visible in the catalog or "
            "the plan, so relations it touches are not in this set");
      }
    } catch (const pqxx::sql_error&) {
      s.txn().abort();
      s.renew();
    }
  }

  ConnConfig cfg_;
  ConnectionCache* cache_ = nullptr;
};

// A directory of specs, each classified against the ledger of the database it
// targets -- which need not be the same database for all of them.
//
// One repository, many databases. A specification names a connection in
// `target.connection` and is classified, gated and applied there; one that
// names none uses the caller's. depends_on crosses databases freely, because
// the ordering question -- "has that one been applied yet" -- is answerable
// wherever it ran, and the ledger that answers it lives in ITS database.
//
// What does NOT cross is atomicity. A change is single-database; it may
// require a change on another. PostgreSQL has no cross-database transaction,
// so the specification stays the unit, and each unit is atomic where
// atomicity actually exists.
class MigrationRepository {
 public:
  MigrationRepository(const Registry& registry, std::string default_connection,
                      ConnectionCache* cache)
      : registry_(&registry),
        default_connection_(std::move(default_connection)),
        cache_(cache) {}

 private:
  // Everything one database says about itself, read ONCE per listing. Two
  // entries targeting the same connection answer against the same reading,
  // which a per-entry query could not promise.
  struct View {
    bool reachable = false;
    std::string error;
    std::string database;
    std::string environment;
    std::set<std::string> releases_ready;
    json applied = json::object();
  };

  const View& view_for(const std::string& connection) {
    const auto it = views_.find(connection);
    if (it != views_.end()) return it->second;
    View v;
    try {
      const auto& cfg = registry_->get(connection);
      Ledger ledger(cfg, cache_);
      const auto st = ledger.status();
      v.database = st.database;
      v.environment = st.environment;
      v.releases_ready = st.releases_ready;
      v.applied = applied_index(cfg);
      v.reachable = true;
    } catch (const std::exception& e) {
      // Unreachable is reported by the tools that need it; here it means
      // nothing is labelled, nothing approved and nothing applied, which HOLDS
      // every gated spec rather than releasing it.
      v.error = e.what();
    }
    return views_.emplace(connection, std::move(v)).first->second;
  }

 public:

  json scan(const std::string& dir, bool derive_relations) {
    namespace fs = std::filesystem;
    std::vector<RepoEntry> entries;
    std::vector<std::string> problems;

    if (!fs::exists(dir) || !fs::is_directory(dir)) {
      return json{{"error", "not a directory: " + dir},
                  {"hint", "Point `directory` at the folder holding the "
                           "migration specs."}};
    }

    // Sorted, so the answer is stable regardless of filesystem order.
    std::vector<std::string> files;
    for (const auto& e : fs::directory_iterator(dir)) {
      if (e.is_regular_file() && e.path().extension() == ".json") {
        files.push_back(e.path().string());
      }
    }
    std::sort(files.begin(), files.end());

    std::map<std::string, Spec> parsed;
    // spec id -> the first file that claimed it. An id is the name every other
    // spec refers to in depends_on and the name the ledger records, so two
    // files claiming one id is not a preference to resolve but a question with
    // two answers.
    struct FirstSeen {
      std::string path;
      std::string digest;
    };
    std::map<std::string, FirstSeen> claimed;

    for (const auto& path : files) {
      RepoEntry entry;
      entry.path = path;
      try {
        std::ifstream in(path);
        const auto doc = json::parse(in);
        const auto spec = parse_spec(doc);
        entry.spec_id = spec.id;
        entry.digest = spec.digest;

        // Last-wins was the wrong answer twice over: the loser vanished from
        // the dependency order, and nothing said so. Harmless while one team
        // keeps one directory; read several together and two projects that
        // both wrote 0001-init collide, and one of them quietly stops
        // existing. The duplicate is not listed as its own entry -- a second
        // pending row under one id would put the id in the order twice -- so
        // the problem message carries both paths instead.
        const auto prior = claimed.find(spec.id);
        if (prior != claimed.end()) {
          problems.push_back(
              prior->second.digest == spec.digest
                  ? "\"" + spec.id + "\" is claimed by two files with identical "
                    "content: " + prior->second.path + " and " + path +
                    ". One of them is a copy; delete it."
                  : "\"" + spec.id + "\" is claimed by two DIFFERENT "
                    "specifications: " + prior->second.path + " (" +
                    prior->second.digest.substr(0, 12) + "…) and " + path +
                    " (" + spec.digest.substr(0, 12) +
                    "…). An id is what depends_on refers to and what the ledger "
                    "records, so it cannot mean two things. Rename one.");
          continue;
        }
        claimed.emplace(spec.id, FirstSeen{path, spec.digest});
        entry.depends_on = spec.depends_on;
        entry.database = spec.target_database;
        entry.environment = spec.target_environment;
        entry.release = spec.release;
        // Routed before it is classified, because WHICH ledger answers "has
        // this been applied" is the first question, not a detail of the
        // answer.
        entry.connection = spec.target_connection.empty()
                               ? default_connection_
                               : spec.target_connection;
        parsed[spec.id] = spec;
        if (!registry_->has(entry.connection)) {
          entry.status = RepoStatus::kUnknownConnection;
          entry.error = "\"" + spec.id + "\" targets connection \"" +
                        entry.connection +
                        "\", which this configuration does not define";
          entry.hint =
              "Add a [" + entry.connection +
              "] section naming its host, port, dbname and user, or correct "
              "target.connection. A specification cannot be classified at all "
              "until it is known which database's ledger to ask.";
          entries.push_back(std::move(entry));
          continue;
        }
        const auto& view = view_for(entry.connection);
        classify(entry, view.applied);
        gate(entry, view);
      } catch (const SpecError& e) {
        entry.status = RepoStatus::kUnreadable;
        entry.error = e.what();
        entry.hint = e.hint();
      } catch (const std::exception& e) {
        entry.status = RepoStatus::kUnreadable;
        entry.error = e.what();
      }
      entries.push_back(std::move(entry));
    }

    // A file that does not parse is a fault in the REPOSITORY, not a property
    // of one migration, so it belongs in `problems` and not only on its own
    // entry. Two callers depend on that and both were wrong without it: the
    // documented CI gate is "problems is non-empty", which would have passed
    // over a spec nobody can read; and pg_laswell would apply the healthy half
    // of a repository whose remainder is unknown -- and what is unknown
    // includes where that file sat in the dependency order.
    for (const auto& e : entries) {
      if (e.status == RepoStatus::kUnknownConnection) {
        problems.push_back(e.error + ". " + e.hint);
        continue;
      }
      if (e.status != RepoStatus::kUnreadable) continue;
      problems.push_back(
          e.path + " does not parse, so what it changes and where it belongs in "
          "the order are both unknown: " +
          (e.error.empty() ? std::string("no detail") : e.error));
    }

    // Anything a ledger knows that the directory does not -- asked of EVERY
    // database this listing touched, and answered with the name of the one
    // that knows, because "applied here" is now a question with several
    // possible heres.
    //
    // The default connection is read even when no specification names it: it
    // is where an unrouted specification would go, so its ledger is part of
    // this repository's story whether or not a file currently points there.
    if (registry_->has(default_connection_)) (void)view_for(default_connection_);
    std::set<std::string> on_disk;
    for (const auto& e : entries) {
      if (!e.spec_id.empty()) on_disk.insert(e.spec_id);
    }
    for (const auto& [conn, view] : views_) {
      for (auto it = view.applied.begin(); it != view.applied.end(); ++it) {
        if (on_disk.count(it.key()) == 0) {
          problems.push_back(
              "the ledger of \"" + conn + "\" records \"" + it.key() +
              "\" as applied, and no file in the repository has that id. It "
              "was applied from somewhere else, or the file was deleted.");
        }
      }
    }

    if (derive_relations) derive(entries, parsed);
    return assemble(entries, parsed, problems);
  }

 private:
  // spec_id -> {digest -> best state}
  json applied_index(const ConnConfig& cfg) {
    ReadSession s(cfg, std::nullopt, cache_, 2000);
    const auto probe =
        s.txn().exec("SELECT to_regclass('laswell.migration') IS NOT NULL");
    if (probe.empty() || !probe[0][0].as<bool>()) return json::object();

    const auto r = s.txn().exec(R"SQL(
      SELECT COALESCE(JSONB_OBJECT_AGG(spec_id, entry), '{}'::jsonb) FROM (
        SELECT m.spec_id,
               JSONB_BUILD_OBJECT(
                 'digests', JSONB_AGG(DISTINCT m.spec_digest),
                 'succeeded', COALESCE(JSONB_AGG(DISTINCT m.spec_digest)
                                FILTER (WHERE j.state = 'succeeded'), '[]'::jsonb),
                 'running', COALESCE(JSONB_AGG(DISTINCT m.spec_digest)
                              FILTER (WHERE j.finished_at IS NULL), '[]'::jsonb),
                 'failed', COALESCE(JSONB_AGG(DISTINCT m.spec_digest)
                             FILTER (WHERE j.state = 'failed'), '[]'::jsonb)
               ) AS entry
          FROM laswell.migration m
          LEFT JOIN laswell.job j ON j.migration_id = m.migration_id
         GROUP BY m.spec_id) AS s
    )SQL");
    if (r.empty() || r[0][0].is_null()) return json::object();
    return json::parse(r[0][0].as<std::string>());
  }

  // Has any database this listing touched recorded this spec id as applied?
  //
  // Every database, not one: depends_on crosses databases freely, and a
  // dependency the repository does not contain brings no target.connection of
  // its own to narrow the search with. The ledger that knows is the one in the
  // database where it ran, which is exactly the set already read here.
  //
  // Succeeded, specifically. A dependency that is running, failed or merely
  // recorded has not happened yet, and the caller must keep waiting or stop.
  bool applied_somewhere(const std::string& spec_id) const {
    for (const auto& [conn, view] : views_) {
      (void)conn;
      if (!view.applied.contains(spec_id)) continue;
      if (!view.applied[spec_id].value("succeeded", json::array()).empty()) {
        return true;
      }
    }
    return false;
  }

  static bool contains(const json& arr, const std::string& v) {
    for (const auto& x : arr) {
      if (x.get<std::string>() == v) return true;
    }
    return false;
  }

  void classify(RepoEntry& e, const json& applied) {
    if (!applied.contains(e.spec_id)) {
      e.status = RepoStatus::kPending;
      return;
    }
    const auto& a = applied[e.spec_id];
    if (contains(a.value("running", json::array()), e.digest)) {
      e.status = RepoStatus::kInProgress;
      return;
    }
    if (contains(a.value("succeeded", json::array()), e.digest)) {
      e.status = RepoStatus::kApplied;
      return;
    }
    // The id was applied, but not THIS content. The file changed after it was
    // applied, so the database no longer matches the document that claims to
    // describe it.
    const auto& succeeded = a.value("succeeded", json::array());
    if (!succeeded.empty()) {
      e.status = RepoStatus::kModified;
      e.applied_digest = succeeded[0].get<std::string>();
      e.error = "\"" + e.spec_id + "\" was applied here as " +
                e.applied_digest.substr(0, 12) + "…, and this file is " +
                e.digest.substr(0, 12) + "…";
      e.hint =
          "An applied migration was edited. The database does not match this "
          "file. Do not re-apply it: write a NEW spec for the difference. The "
          "bytes that were applied are in laswell.migration.canonical_bytes if "
          "you need to see what changed.";
      return;
    }
    if (contains(a.value("failed", json::array()), e.digest)) {
      e.status = RepoStatus::kFailed;
      e.hint = "A prior job for this exact spec failed. Re-running resumes from "
               "the committed cursor if there is one.";
      return;
    }
    e.status = RepoStatus::kPending;
  }

  // Applicability, asked only of something that would otherwise run.
  //
  // Deliberately AFTER classify and only over the runnable states: an applied
  // migration stays applied whatever environment this is, and a spec that was
  // edited after being applied is a problem in every environment. Re-labelling
  // those would hide a real fault behind a routine one.
  //
  // Both answers come from the DATABASE, never from the specification alone or
  // from configuration beside it -- the laswell.environment and laswell.release
  // tables, which the migrating role can read and cannot write. That is the
  // trusted_key argument applied to a second question: a permissive client
  // configuration must not be able to talk a production database into running
  // the development migration, and an environment asserted by whoever launched
  // the tool would be exactly that.
  void gate(RepoEntry& e, const View& view) {
    if (e.status != RepoStatus::kPending && e.status != RepoStatus::kFailed) {
      return;
    }

    // The database's own name first: it is the most concrete of the three and
    // needs nothing to have been set up. It is also the weakest -- see
    // LedgerStatus::database -- so it is a check the author asked for rather
    // than a substitute for the environment label.
    if (!e.database.empty() && !view.database.empty() && e.database != view.database) {
      e.status = RepoStatus::kWrongDatabase;
      e.error = "\"" + e.spec_id + "\" targets database \"" + e.database +
                "\" and this is \"" + view.database + "\"";
      e.hint = "Nothing is wrong: this specification names another database by "
               "name, so it will never be applied to this one. A database name "
               "proves less than an environment label -- a dump restored "
               "elsewhere keeps neither -- so target.environment is the "
               "stronger statement if what you mean is \"production only\".";
      return;
    }

    if (!e.environment.empty()) {
      if (view.environment.empty()) {
        // Cannot be checked, so it is not assumed to pass. An unlabelled
        // database is a database that has not said what it is, and a spec that
        // names an environment is asking a question it cannot answer.
        e.status = RepoStatus::kWrongEnvironment;
        e.error = "\"" + e.spec_id + "\" targets environment \"" +
                  e.environment + "\" and this database is not labelled";
        e.hint =
            "Label it as the owner of the laswell schema: INSERT INTO "
            "laswell.environment(name) VALUES ('...') ON CONFLICT (only_one) "
            "DO UPDATE SET name = EXCLUDED.name, set_at = now(), "
            "set_by = current_user;";
        return;
      }
      if (e.environment != view.environment) {
        e.status = RepoStatus::kWrongEnvironment;
        e.error = "\"" + e.spec_id + "\" targets environment \"" +
                  e.environment + "\" and this database is \"" + view.environment +
                  "\"";
        e.hint = "Nothing is wrong: this specification is for another "
                 "database. It will stay listed here and will never be applied "
                 "to this one.";
        return;
      }
    }

    if (!e.release.empty() && view.releases_ready.count(e.release) == 0) {
      e.status = RepoStatus::kHeldForRelease;
      e.error = "\"" + e.spec_id + "\" is held: release \"" + e.release +
                "\" is not marked ready in this database";
      e.hint =
          "Approve it as the owner of the laswell schema: INSERT INTO "
          "laswell.release(tag, ready, marked_ready_at, marked_by, note) "
          "VALUES (" + quote_sql_literal(e.release) +
          ", true, now(), current_user, '...') "
          "ON CONFLICT (tag) DO UPDATE SET ready = EXCLUDED.ready, "
          "marked_ready_at = EXCLUDED.marked_ready_at, "
          "marked_by = EXCLUDED.marked_by, note = EXCLUDED.note;";
    }
  }

  void derive(std::vector<RepoEntry>& entries,
              const std::map<std::string, Spec>& parsed) {
    // Per connection, because relations are only comparable within the
    // database that holds them: "shop.orders" in one database and
    // "shop.orders" in another are different tables that happen to be spelled
    // the same, and treating them as one would serialise work that cannot
    // conflict -- or worse, let the name coincidence stand in for evidence.
    for (auto& e : entries) {
      if (e.status == RepoStatus::kUnreadable || e.status == RepoStatus::kApplied ||
          e.status == RepoStatus::kUnknownConnection) {
        continue;
      }
      const auto& cfg = registry_->get(e.connection);
      RelationAnalyzer analyzer(cfg, cache_);
      Catalog cat(cfg, cache_);
      const auto it = parsed.find(e.spec_id);
      if (it == parsed.end()) continue;
      try {
        std::vector<std::string> schemas, tables;
        for (const auto& in : it->second.intents) {
          schemas.push_back(in.schema());
          tables.push_back(in.table());
        }
        const auto obs = cat.observe(schemas, tables);
        const auto plan = plan_migration(it->second, obs, cfg.executor);
        e.relations = analyzer.analyze(it->second, plan.to_json());
      } catch (const std::exception& ex) {
        e.relations.opaque.push_back(
            std::string("relations could not be derived: ") + ex.what());
      }
    }
  }

  // Topological levels from depends_on, then parallel-safe subsets within a
  // level. Only relation overlap can DENY parallelism; only complete evidence
  // can ALLOW it.
  json assemble(const std::vector<RepoEntry>& entries,
                const std::map<std::string, Spec>& parsed,
                std::vector<std::string> problems) {
    std::map<std::string, const RepoEntry*> by_id;
    std::vector<std::string> runnable;
    for (const auto& e : entries) {
      if (!e.spec_id.empty()) by_id[e.spec_id] = &e;
      if (e.status == RepoStatus::kPending || e.status == RepoStatus::kFailed) {
        runnable.push_back(e.spec_id);
      }
      if (e.status == RepoStatus::kModified) {
        problems.push_back(e.error + " -- " + e.hint);
      }
    }

    // A dependency the repository does not contain is not automatically a
    // refusal. The question depends_on asks is "has that one run", and the
    // thing that answers it is a LEDGER, not a directory listing -- so a
    // dependency any database in this listing records as succeeded is
    // satisfied whether or not its file is here.
    //
    // That distinction is what makes a partial repository possible at all. One
    // project's directory holds one project's specs; a spec in it may depend on
    // something that ran from a directory this deployment will never see. Read
    // from the files, that is unanswerable and the whole run is refused. Read
    // from the ledger, it is simply true.
    //
    // Unsatisfied is still a refusal, and the message now says both places
    // that were looked in, because "I cannot find it" is only useful when it
    // says where it looked.
    for (const auto& id : runnable) {
      for (const auto& d : by_id[id]->depends_on) {
        if (by_id.count(d) != 0) continue;
        if (applied_somewhere(d)) continue;
        problems.push_back("\"" + id + "\" depends on \"" + d +
                           "\", which is not in this repository and which no "
                           "database in this listing records as applied");
      }
    }

    std::map<std::string, int> level;
    std::set<std::string> placed;
    json groups = json::array();
    auto remaining = runnable;

    while (!remaining.empty()) {
      std::vector<std::string> ready;
      std::vector<std::string> blocked;  // why each remaining spec is not ready
      for (const auto& id : remaining) {
        bool ok = true;
        for (const auto& d : by_id[id]->depends_on) {
          const auto it = by_id.find(d);
          if (it == by_id.end()) continue;  // reported separately as missing
          // An applied dependency is satisfied. One still pending in this batch
          // must go first. One in any OTHER state blocks, and saying which
          // state matters -- see below.
          if (it->second->status == RepoStatus::kApplied) continue;
          if (placed.count(d) > 0) continue;
          ok = false;
          const bool runnable_dep = it->second->status == RepoStatus::kPending ||
                                    it->second->status == RepoStatus::kFailed;
          if (!runnable_dep) {
            blocked.push_back(
                "\"" + id + "\" depends on \"" + d + "\", which is " +
                to_string(it->second->status) +
                " -- not applied and not runnable, so nothing that depends on "
                "it can be ordered");
          }
        }
        if (ok) ready.push_back(id);
      }
      if (ready.empty()) {
        // Distinguish a genuine cycle from a dependency stuck in a bad state.
        // Reporting the latter as a cycle sends the reader hunting for
        // something that does not exist.
        if (!blocked.empty()) {
          for (auto& b : blocked) problems.push_back(std::move(b));
        } else {
          std::string names;
          for (const auto& id : remaining) {
            if (!names.empty()) names += ", ";
            names += id;
          }
          problems.push_back(
              "the depends_on graph has a cycle among [" + names +
              "]; none of them can be ordered");
        }
        break;
      }
      groups.push_back(parallel_subsets(ready, by_id));
      for (const auto& id : ready) placed.insert(id);
      remaining.erase(std::remove_if(remaining.begin(), remaining.end(),
                                     [&](const std::string& id) {
                                       return placed.count(id) > 0;
                                     }),
                      remaining.end());
    }

    json list = json::array();
    for (const auto& e : entries) {
      json j{{"path", e.path},
             {"specId", e.spec_id},
             {"digest", e.digest},
             {"status", to_string(e.status)},
             {"connection", e.connection},
             {"dependsOn", e.depends_on}};
      if (!e.relations.relations.empty()) {
        j["relations"] = e.relations.relations;
        j["relationsComplete"] = e.relations.complete();
      }
      if (!e.relations.opaque.empty()) j["opaque"] = e.relations.opaque;
      if (!e.error.empty()) j["error"] = e.error;
      if (!e.hint.empty()) j["hint"] = e.hint;
      if (!e.applied_digest.empty()) j["appliedDigest"] = e.applied_digest;
      list.push_back(std::move(j));
    }
    (void)parsed;

    return json{
        {"migrations", std::move(list)},
        {"order", std::move(groups)},
        {"problems", std::move(problems)},
        {"note",
         "`order` is a list of levels from depends_on; each level lists groups "
         "that may run concurrently. pg_laswell can prove two migrations "
         "CONFLICT -- a shared relation -- but cannot prove they are "
         "INDEPENDENT: trigger function bodies are invisible to the catalog "
         "and to the plan, and business ordering is nowhere a machine can read "
         "it. A group marked mayRunConcurrently:false with no shared relation "
         "is asking you to decide. Advisory locks refuse an unsafe overlap "
         "whatever you decide, so a wrong call is refused, not corrupting."}};
  }

  // Within one dependency level, which migrations may run at the same time.
  //
  // A shared relation DENIES concurrency and that is decidable. Nothing proves
  // independence, so incomplete evidence also denies it -- an opaque trigger on
  // one target could write any table at all, including the other migration's.
  // The reason is attached to the LEVEL, because "why can these two not run
  // together" is the question being asked, and answering it on the individual
  // migration leaves the reader to join the two facts themselves.
  static json parallel_subsets(const std::vector<std::string>& ready,
                               const std::map<std::string, const RepoEntry*>& by_id) {
    json groups = json::array();
    std::vector<std::string> undecided;
    std::vector<bool> taken(ready.size(), false);

    for (std::size_t i = 0; i < ready.size(); ++i) {
      if (taken[i]) continue;
      std::vector<std::string> group{ready[i]};
      std::set<std::string> rels = by_id.at(ready[i])->relations.relations;
      const bool complete_i = by_id.at(ready[i])->relations.complete();
      taken[i] = true;

      const auto& conn_i = by_id.at(ready[i])->connection;
      for (std::size_t k = i + 1; k < ready.size(); ++k) {
        if (taken[k]) continue;
        // Different databases cannot touch each other's tables, so two
        // specifications on different connections are independent by
        // construction rather than by evidence -- the one case where this
        // grants concurrency without having to prove anything. Nothing
        // crosses a database but the dependency the author declared, and a
        // dependency puts them in different LEVELS, never this list.
        if (by_id.at(ready[k])->connection != conn_i) {
          group.push_back(ready[k]);
          taken[k] = true;
          continue;
        }
        const auto& other = by_id.at(ready[k])->relations;
        bool overlaps = false;
        std::string shared;
        for (const auto& r : other.relations) {
          if (rels.count(r) > 0) {
            overlaps = true;
            shared = r;
          }
        }
        if (overlaps) {
          undecided.push_back(ready[i] + " and " + ready[k] +
                              " both touch " + shared +
                              ", so they must not run together");
          continue;
        }
        if (!complete_i || !other.complete()) {
          const auto& why = complete_i ? other.opaque
                                       : by_id.at(ready[i])->relations.opaque;
          undecided.push_back(
              ready[i] + " and " + ready[k] +
              " share no relation this analysis can see, but independence is "
              "NOT proven: " + (why.empty() ? std::string("evidence is incomplete")
                                            : why.front()) +
              ". An opaque trigger function can write any table, including the "
              "other migration's. Your call.");
          continue;
        }
        group.push_back(ready[k]);
        rels.insert(other.relations.begin(), other.relations.end());
        taken[k] = true;
      }

      json g{{"specs", group},
             {"relations", rels},
             {"evidenceComplete", complete_i}};
      groups.push_back(std::move(g));
    }

    json level{{"groups", std::move(groups)}};
    if (!undecided.empty()) level["undecided"] = undecided;
    level["provenConcurrent"] = level["groups"].size() == 1 &&
                                level["groups"][0]["specs"].size() > 1;
    return level;
  }

  const Registry* registry_ = nullptr;
  std::string default_connection_;
  std::map<std::string, View> views_;
  ConnectionCache* cache_ = nullptr;
};

}  // namespace pglaswell
