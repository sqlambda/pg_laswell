#pragma once

// The ledger: gate 2 of trust, and the record of what ran.
//
// The client config (gate 1) fails fast and offline. This is the gate that
// actually decides, because it lives in the target database and therefore
// survives a wrong, stale or permissive copy of anyone's laswell.ini.
//
// Two things make it authoritative rather than decorative:
//
//  1. The signature is re-verified against the PUBLIC KEY BYTES STORED HERE,
//     not against the client's copy. A config that files an attacker's key
//     under a trusted id gets nowhere, because the bytes this database holds
//     are the ones that have to verify.
//
//  2. laswell.migration.signer_key_id REFERENCES laswell.trusted_key(key_id),
//     so a spec whose signer this database does not trust cannot create its
//     ledger row at all. That is a foreign key, not a code path: there is no
//     branch to forget.

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "planner.h"
#include "session.h"
#include "spec.h"
#include "trust.h"

namespace pglaswell {

inline constexpr int kLedgerSchemaVersion = 1;

struct LedgerStatus {
  bool installed = false;
  int version = 0;
  bool usable = false;
  std::vector<std::string> trusted_key_ids;
  std::string error;
  std::string hint;

  json to_json() const {
    json j{{"installed", installed},
           {"version", version},
           {"expectedVersion", kLedgerSchemaVersion},
           {"usable", usable},
           {"trustedKeyIds", trusted_key_ids}};
    if (!error.empty()) j["error"] = error;
    if (!hint.empty()) j["hint"] = hint;
    return j;
  }
};

class Ledger {
 public:
  explicit Ledger(const ConnConfig& cfg, ConnectionCache* cache = nullptr)
      : cfg_(cfg), cache_(cache) {}

  // Is the ledger present, at a version this binary understands, and which
  // keys does this database trust?
  //
  // to_regclass() rather than a direct query: it returns NULL for an absent
  // relation instead of raising, so "you have not bootstrapped" arrives as an
  // answer rather than as an exception. Those are very different things for an
  // operator to read, and collapsing them would be the same mistake as
  // reporting a privilege denial as an empty result.
  LedgerStatus status() {
    LedgerStatus st;
    ReadSession s(cfg_, std::nullopt, cache_, kLedgerLockTimeoutMs);

    const auto probe = s.txn().exec(
        "SELECT to_regclass('laswell.schema_version') IS NOT NULL,"
        "       to_regclass('laswell.trusted_key') IS NOT NULL");
    st.installed = !probe.empty() && probe[0][0].as<bool>() && probe[0][1].as<bool>();
    if (!st.installed) {
      st.error = "the laswell schema is not installed in this database";
      st.hint =
          "Run sql/bootstrap.sql once, as a superuser or owner role: "
          "psql -v ON_ERROR_STOP=1 -v laswell_role=<runtime role> "
          "-v first_key_id=... -v first_key_b64=... -v first_key_label=... "
          "-f bootstrap.sql. pg_laswell never installs it itself, so that the "
          "runtime role can never grant itself trust.";
      return st;
    }

    const auto r = s.txn().exec(R"SQL(
      SELECT JSONB_BUILD_OBJECT(
        'version', (SELECT MAX(version) FROM laswell.schema_version),
        'keys', COALESCE((SELECT JSONB_AGG(key_id ORDER BY key_id)
                            FROM laswell.trusted_key
                           WHERE revoked_at IS NULL), '[]'::jsonb))
    )SQL");
    if (!r.empty() && !r[0][0].is_null()) {
      const auto j = json::parse(r[0][0].as<std::string>());
      st.version = j.value("version", 0);
      for (const auto& k : j.value("keys", json::array())) {
        st.trusted_key_ids.push_back(k.get<std::string>());
      }
    }

    if (st.version != kLedgerSchemaVersion) {
      st.error = "the laswell ledger is at schema version " +
                 std::to_string(st.version) + ", this binary expects " +
                 std::to_string(kLedgerSchemaVersion);
      st.hint =
          "Use the pg_laswell binary that matches this ledger, or migrate the "
          "ledger deliberately with the bootstrap script that ships with the "
          "binary you intend to run.";
      return st;
    }
    // A database that trusts no keys is USABLE but will accept nothing. That
    // distinction matters: "you have not bootstrapped" and "your signer is not
    // trusted here" are different problems with different fixes, and an empty
    // key set is the second kind. Reporting it as a ledger fault would hide it
    // behind the wrong remedy, so it falls through to gate 2 and is reported
    // as the trust answer it is.
    st.usable = true;
    if (st.trusted_key_ids.empty()) {
      st.hint =
          "This database trusts no signing keys, so it will accept no spec. "
          "Add one: INSERT INTO laswell.trusted_key(key_id, public_key, label) "
          "VALUES ('ed25519:<16 hex>', decode('<base64>','base64'), '<name>'); "
          "run as a role that owns the table, not as the migrating role.";
    }
    return st;
  }

  // Gate 2. Re-verifies the spec's signature against the key bytes THIS
  // DATABASE holds, and reports which key answered.
  SignatureCheck verify_signer(const Spec& spec) {
    SignatureCheck out;
    if (!spec.signatures.is_array() || spec.signatures.empty()) {
      out.reason = "the spec carries no signatures";
      return out;
    }

    ReadSession s(cfg_, std::nullopt, cache_, kLedgerLockTimeoutMs);
    std::string offered;
    for (const auto& sig : spec.signatures) {
      if (!sig.is_object() || !sig.contains("key_id") ||
          !sig["key_id"].is_string()) {
        continue;
      }
      const auto key_id = sig["key_id"].get<std::string>();
      if (!offered.empty()) offered += ", ";
      offered += key_id;

      const auto r = pqxx_exec(
          s.txn(),
          "SELECT public_key, label, revoked_at IS NOT NULL"
          "  FROM laswell.trusted_key WHERE key_id = $1",
          pqxx::params{key_id});
      if (r.empty()) continue;
      if (r[0][2].as<bool>()) {
        out.reason = "key " + key_id + " is revoked in this database";
        return out;
      }

      // The bytes come from the database, not from the caller's config. This
      // is what makes gate 2 stronger than gate 1 rather than a repeat of it.
      const auto bin = r[0][0].as<std::basic_string<std::byte>>();
      std::vector<unsigned char> pubkey;
      pubkey.reserve(bin.size());
      for (const auto b : bin) pubkey.push_back(static_cast<unsigned char>(b));

      std::vector<unsigned char> raw_sig;
      try {
        raw_sig = Registry::base64_decode(sig.value("signature", ""));
      } catch (const std::exception& e) {
        out.reason = std::string("signature for ") + key_id +
                     " is not valid base64: " + e.what();
        return out;
      }
      if (!verify_ed25519(pubkey, spec.canonical_bytes, raw_sig)) {
        out.reason = "signature by " + key_id +
                     " does not verify against the public key this database "
                     "holds for that id";
        return out;
      }
      out.verified = true;
      out.key_id = key_id;
      out.label = r[0][1].as<std::string>();
      return out;
    }

    const auto st = status();
    std::string trusted;
    for (const auto& k : st.trusted_key_ids) {
      if (!trusted.empty()) trusted += ", ";
      trusted += k;
    }
    out.reason = "the spec is signed by [" + offered +
                 "], and this database trusts [" +
                 (trusted.empty() ? std::string("nothing") : trusted) + "]";
    return out;
  }

  // Records the spec, if it is not already recorded. The signer foreign key is
  // the gate; an untrusted signer cannot get a row here at all.
  long long record_migration(const Spec& spec, const std::string& signer_key_id) {
    WriteSession w(cfg_);
    w.begin("pg_laswell/ledger/record-migration");
    const auto r = pqxx_exec(
        w.txn(),
        "INSERT INTO laswell.migration"
        "  (spec_id, spec_digest, canonical_bytes, signer_key_id, signature)"
        "  VALUES ($1, $2, convert_to($3, 'UTF8'), $4, decode($5, 'base64'))"
        "  ON CONFLICT (spec_digest) DO UPDATE SET spec_id = EXCLUDED.spec_id"
        "  RETURNING migration_id",
        // convert_to(..., 'UTF8') rather than any bytea quoting: RFC 8785
        // mandates UTF-8 output, so the canonical bytes ARE a UTF-8 string and
        // there is nothing to escape. One fewer encoding to get wrong.
        pqxx::params{spec.id, spec.digest, spec.canonical_bytes, signer_key_id,
                     spec.signatures[0].value("signature", "")});
    const auto id = r[0][0].as<long long>();
    w.commit();
    return id;
  }

  // A job with no finished_at whose advisory lock is absent from pg_locks died
  // with its connection. No heartbeat table, no timeout to tune, and no false
  // positive from a merely slow job: a session advisory lock is released by
  // the server the moment the backend goes away.
  json interrupted_jobs() {
    ReadSession s(cfg_, std::nullopt, cache_, kLedgerLockTimeoutMs);
    const auto r = s.txn().exec(R"SQL(
      SELECT COALESCE(JSONB_AGG(JSONB_BUILD_OBJECT(
               'jobId', j.job_id, 'state', j.state,
               'startedAt', j.started_at,
               'specDigest', m.spec_digest, 'specId', m.spec_id)), '[]'::jsonb)
        FROM laswell.job j
        JOIN laswell.migration m ON m.migration_id = j.migration_id
       WHERE j.finished_at IS NULL
         AND NOT EXISTS (SELECT 1 FROM pg_locks l
                          WHERE l.locktype = 'advisory'
                            AND ((l.classid::bigint << 32) | l.objid::bigint) = j.lock_key)
    )SQL");
    if (r.empty() || r[0][0].is_null()) return json::array();
    return json::parse(r[0][0].as<std::string>());
  }

 private:
  // Ledger reads must not queue behind a migration in flight, for the same
  // reason observation must not: a status call that hangs is worse than one
  // that reports contention.
  static constexpr int kLedgerLockTimeoutMs = 2000;

  ConnConfig cfg_;
  ConnectionCache* cache_ = nullptr;
};

}  // namespace pglaswell
