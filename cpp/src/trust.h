#pragma once

// Ed25519 signature verification, via OpenSSL EVP.
//
// This binary VERIFIES; it never signs. Signing is `openssl pkeyutl -sign` over
// the canonical bytes that `getSpecDigest` emits, which keeps the private key
// out of this process and out of the threat model entirely.
//
// OpenSSL rather than libsodium, whose API is nicer: libcrypto is already
// loaded into this process because libpq5 depends on libssl3t64, so using it
// adds no new runtime package on any platform. libsodium would add one, named
// differently per distro, forever. The trade is the glue below, once.

#include <openssl/evp.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "canonical.h"
#include "config.h"

namespace pglaswell {

namespace detail {

struct EvpPkeyDeleter {
  void operator()(EVP_PKEY* p) const noexcept { EVP_PKEY_free(p); }
};
struct EvpMdCtxDeleter {
  void operator()(EVP_MD_CTX* c) const noexcept { EVP_MD_CTX_free(c); }
};
using PkeyPtr = std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>;
using MdCtxPtr = std::unique_ptr<EVP_MD_CTX, EvpMdCtxDeleter>;

}  // namespace detail

// The key id is content-addressed: it is derived from the key bytes, so it
// cannot be reassigned to a different key. You cannot rotate the bytes under a
// name and have anything keep trusting it. Human-readable naming is the
// separate `label` field.
inline std::string key_id_for(const std::vector<unsigned char>& public_key) {
  if (public_key.size() != 32) {
    throw std::runtime_error("ed25519: public key must be 32 bytes, got " +
                             std::to_string(public_key.size()));
  }
  const std::string raw(reinterpret_cast<const char*>(public_key.data()),
                        public_key.size());
  return "ed25519:" + to_hex(sha256(raw)).substr(0, 16);
}

// Verifies a detached Ed25519 signature over `message`.
//
// One-shot, and it has to be. OpenSSL's Ed25519 does not support the streaming
// EVP_DigestVerifyUpdate path at all: EVP_DigestVerify() must be called once
// with the complete message, and an Update call on an Ed25519 context fails
// rather than accumulating. That is fine at spec sizes, and it is named here
// because reaching for the streaming API out of habit produces a verify that
// fails with an unhelpful error.
inline bool verify_ed25519(const std::vector<unsigned char>& public_key,
                           const std::string& message,
                           const std::vector<unsigned char>& signature) {
  if (public_key.size() != 32) return false;
  if (signature.size() != 64) return false;

  detail::PkeyPtr pkey(EVP_PKEY_new_raw_public_key(
      EVP_PKEY_ED25519, nullptr, public_key.data(), public_key.size()));
  if (!pkey) return false;

  detail::MdCtxPtr ctx(EVP_MD_CTX_new());
  if (!ctx) return false;

  // The null digest argument is correct for Ed25519: the algorithm specifies
  // its own hashing (SHA-512, internally), so there is no hash-then-sign step
  // for a caller to get wrong.
  if (EVP_DigestVerifyInit(ctx.get(), nullptr, nullptr, nullptr, pkey.get()) != 1) {
    return false;
  }
  const int rc = EVP_DigestVerify(
      ctx.get(), signature.data(), signature.size(),
      reinterpret_cast<const unsigned char*>(message.data()), message.size());
  // Exactly 1 is success. A negative return is an operational error and 0 is a
  // bad signature; both must be false here, and `rc > 0` would be wrong.
  return rc == 1;
}

struct SignatureCheck {
  bool verified = false;
  bool not_configured = false;  // gate 1 only: no local policy exists to check
  std::string key_id;
  std::string label;
  std::string reason;  // set only when verified is false
};

// Checks a spec's signatures against the local trust policy. This is gate 1 of
// two: it fails fast and offline, before any connection is opened, so pointing
// a development configuration at production costs a millisecond rather than a
// round trip. It is NOT the authoritative gate -- laswell.trusted_key in the
// target database is, precisely because it survives a wrong or stale copy of
// the local config.
inline SignatureCheck verify_against_policy(
    const TrustPolicy& policy, const std::string& canonical_bytes,
    const json& signatures) {
  SignatureCheck out;
  if (!policy.configured()) {
    // No [trust] section: there is no local policy to check against. Reported
    // as such rather than as a refusal, because gate 2 -- the database -- is
    // the gate that decides, and a machine with no config file must still be
    // able to run a migration the target database trusts.
    out.not_configured = true;
    out.reason =
        "no [trust] accept list is configured on this machine, so nothing is "
        "checked locally; the target database decides";
    return out;
  }
  if (!signatures.is_array() || signatures.empty()) {
    out.reason = "the spec carries no signatures";
    return out;
  }

  std::string tried;
  for (const auto& sig : signatures) {
    if (!sig.is_object() || !sig.contains("key_id") ||
        !sig["key_id"].is_string()) {
      out.reason = "a signature entry has no key_id";
      return out;
    }
    const auto key_id = sig["key_id"].get<std::string>();
    if (!tried.empty()) tried += ", ";
    tried += key_id;

    const auto algorithm = sig.value("algorithm", "ed25519");
    if (algorithm != "ed25519") {
      out.reason = "unsupported signature algorithm \"" + algorithm +
                   "\"; this build verifies ed25519 only";
      return out;
    }
    if (!policy.accepts(key_id)) continue;

    const auto it = policy.keys.find(key_id);
    if (it == policy.keys.end()) continue;

    // The key id must match the key bytes it is filed under. Without this a
    // config could file an attacker's key under a trusted id, and every check
    // below would pass.
    if (key_id_for(it->second.public_key) != key_id) {
      out.reason = "key " + key_id +
                   " does not match its own public key; the config's key id "
                   "is not the content address of the key bytes";
      return out;
    }
    if (!sig.contains("signature") || !sig["signature"].is_string()) {
      out.reason = "signature entry for " + key_id + " has no signature value";
      return out;
    }

    std::vector<unsigned char> raw_sig;
    try {
      raw_sig = Registry::base64_decode(sig["signature"].get<std::string>());
    } catch (const std::exception& e) {
      out.reason = std::string("signature for ") + key_id + " is not base64: " +
                   e.what();
      return out;
    }
    if (!verify_ed25519(it->second.public_key, canonical_bytes, raw_sig)) {
      out.reason = "signature by " + key_id +
                   " does not verify over the canonical bytes";
      return out;
    }
    out.verified = true;
    out.key_id = key_id;
    out.label = it->second.label;
    return out;
  }

  std::string accepted;
  for (const auto& k : policy.accept) {
    if (!accepted.empty()) accepted += ", ";
    accepted += k;
  }
  out.reason = "the spec is signed by [" + tried +
               "], and this machine accepts [" +
               (accepted.empty() ? std::string("nothing") : accepted) + "]";
  return out;
}

}  // namespace pglaswell
