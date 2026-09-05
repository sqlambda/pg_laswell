#pragma once

// Configuration: the connection registry, the trust policy, and the executor's
// tuning knobs.
//
// Parsing is deliberately loud. Every failure throws naming `path:lineno` and
// the offending key, and nothing is ever silently defaulted -- a typo that
// disables a safety knob must not look like a working configuration. This is
// pg_licht's discipline and the reason for it is stronger here: a mis-parsed
// `lock_timeout_ms` in pg_licht returns a slow answer, whereas here it is an
// ALTER TABLE queueing behind a long transaction with the whole application
// piling up behind that.

#include <sys/stat.h>

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace pglaswell {

inline constexpr int kDefaultStatementTimeoutMs = 120000;

// Executor tuning. Every one of these is configuration rather than a
// `constexpr` for a specific reason: the tests drive the identical code paths
// with tiny values, so pacing behaviour can be exercised in seconds instead of
// minutes. A `constexpr` here would mean either a slow suite or an untested
// path, and both are worse than a config key.
struct ExecutorConfig {
  // Pacing. batch_rows bounds how long ONE STATEMENT runs, and therefore
  // worst-case waiter latency. commit_interval_ms bounds how long ONE
  // TRANSACTION runs, and therefore how long locks accumulate. Conflating the
  // two is the design error to avoid: a fixed row count on a wide table can
  // produce an arbitrarily long transaction.
  int batch_rows = 1000;
  int commit_interval_ms = 1000;
  int batch_cap_rows = 10000;  // backstop against pathological row widths

  int lock_timeout_ms = 3000;
  int observer_tick_ms = 250;
  int max_concurrent_jobs = 2;

  // Contention circuit breaker, on the TRANSITIVE count of backends blocked by
  // us. Measured 2026-09-05 (spike S11): a direct-blocker count sees 1 waiter
  // where the real pile-up is 6, because pg_blocking_pids() returns direct
  // blockers only and second-order waiters are invisible to it.
  int throttle_waiters = 4;
  int pause_waiters = 8;
  int resume_waiters = 1;
  int contention_dwell_ticks = 3;
  int max_waiter_wait_ms = 5000;  // then cancel the statement (spike S6)
  int max_pause_s = 300;          // then abort; the cursor is committed

  // The connection budget (spike S12). One blocked query pins exactly one
  // connection, so T_exhaust = headroom / arrival_rate is computable -- but
  // the ceiling that matters is the APPLICATION's pool, not max_connections,
  // because the application collapses when its own pool fills and that limit
  // is invisible to the server. Zero means "not configured", and is reported
  // as such rather than guessed.
  int app_pool_size = 0;
  int app_statement_timeout_ms = 0;
  int safety_percent = 25;  // use at most this share of the budget
};

struct ConnConfig {
  std::string name;
  std::string conninfo;  // assembled libpq conninfo
  // Non-secret echo fields. A password is parsed into conninfo but never
  // retained here, so nothing that prints a ConnConfig can leak one.
  std::string service, host, port, dbname, user;
  int statement_timeout_ms = kDefaultStatementTimeoutMs;
  ExecutorConfig executor;
};

struct TrustedKey {
  std::string key_id;
  std::string label;
  std::vector<unsigned char> public_key;  // raw 32 bytes
};

struct TrustPolicy {
  // Key ids this machine will even attempt. A spec signed by anything else is
  // rejected here, offline, before a connection is opened -- so a
  // wrong-environment mistake costs a millisecond rather than a round trip to
  // production. This is NOT the authoritative gate; laswell.trusted_key in the
  // target database is, precisely because it survives a wrong or stale copy of
  // this file.
  std::vector<std::string> accept;
  std::map<std::string, TrustedKey> keys;

  // Whether a local policy exists at all. With DATABASE_URL and no config
  // file there is nothing to check locally, and that is NOT the same as "this
  // machine accepts nothing" -- gate 1 is a fail-fast convenience, gate 2 in
  // the database is the boundary. Conflating the two would make the
  // single-connection form unable to run anything, which is a papercut
  // masquerading as a security property.
  bool configured() const { return !accept.empty(); }

  bool accepts(const std::string& key_id) const {
    for (const auto& k : accept) {
      if (k == key_id) return true;
    }
    return false;
  }
};

namespace detail {

inline std::string trim(const std::string& s) {
  const auto b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return {};
  const auto e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

// Strips an inline `;` or `#` comment, honouring nothing clever: a value that
// needs one of those characters is not a case this file has, and pretending to
// support quoting would be a promise the parser does not keep.
inline std::string strip_comment(const std::string& s) {
  const auto p = s.find_first_of(";#");
  return p == std::string::npos ? s : s.substr(0, p);
}

inline std::string where(const std::string& path, std::size_t lineno) {
  return path + ":" + std::to_string(lineno);
}

inline int positive_int(const std::string& value, const std::string& key,
                        const std::string& w) {
  try {
    std::size_t used = 0;
    const long v = std::stol(value, &used);
    if (used != value.size() || v <= 0 || v > 2147483647L) {
      throw std::invalid_argument("range");
    }
    return static_cast<int>(v);
  } catch (const std::exception&) {
    throw std::runtime_error(w + ": " + key + " must be a positive integer, got \"" +
                             value + "\"");
  }
}

// Kept separate from positive_int rather than adding a flag. Letting one of
// them accept 0 silently would turn a typo into a disabled safety feature, and
// the two call sites mean genuinely different things: 0 is "no ceiling" for a
// timeout and "not configured" for the connection budget.
inline int non_negative_int(const std::string& value, const std::string& key,
                            const std::string& w) {
  try {
    std::size_t used = 0;
    const long v = std::stol(value, &used);
    if (used != value.size() || v < 0 || v > 2147483647L) {
      throw std::invalid_argument("range");
    }
    return static_cast<int>(v);
  } catch (const std::exception&) {
    throw std::runtime_error(w + ": " + key +
                             " must be a non-negative integer, got \"" + value +
                             "\"");
  }
}

inline std::vector<std::string> split_list(const std::string& value,
                                           const std::string& key,
                                           const std::string& w) {
  std::vector<std::string> out;
  if (trim(value).empty()) {
    throw std::runtime_error(w + ": " + key + " is empty");
  }
  // Split by hand rather than with std::getline. getline does not produce a
  // final empty token, so "a," parses as the single entry "a" and a trailing
  // comma -- a typo, and usually one where a second key was meant to follow --
  // goes unreported. A config that reads as a list of two and behaves as a
  // list of one is exactly the silent failure this parser exists to prevent.
  std::size_t start = 0;
  while (true) {
    const auto comma = value.find(',', start);
    const auto piece =
        trim(value.substr(start, comma == std::string::npos
                                     ? std::string::npos
                                     : comma - start));
    if (piece.empty()) {
      throw std::runtime_error(w + ": " + key + " has an empty entry");
    }
    out.push_back(piece);
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return out;
}

inline std::string quote_conninfo(const std::string& v) {
  bool needs = v.empty();
  for (const char c : v) {
    if (c == ' ' || c == '\t' || c == '\'' || c == '\\') needs = true;
  }
  if (!needs) return v;
  std::string out = "'";
  for (const char c : v) {
    if (c == '\'' || c == '\\') out += '\\';
    out += c;
  }
  out += "'";
  return out;
}

}  // namespace detail

// Applies one `key = value` pair to an ExecutorConfig. Returns false if the key
// is not an executor key, so the caller can decide whether that is an error.
inline bool apply_executor_key(ExecutorConfig& e, const std::string& key,
                               const std::string& value, const std::string& w) {
  if (key == "batch_rows") { e.batch_rows = detail::positive_int(value, key, w); return true; }
  if (key == "commit_interval_ms") { e.commit_interval_ms = detail::positive_int(value, key, w); return true; }
  if (key == "batch_cap_rows") { e.batch_cap_rows = detail::positive_int(value, key, w); return true; }
  if (key == "lock_timeout_ms") { e.lock_timeout_ms = detail::positive_int(value, key, w); return true; }
  if (key == "observer_tick_ms") { e.observer_tick_ms = detail::positive_int(value, key, w); return true; }
  if (key == "max_concurrent_jobs") { e.max_concurrent_jobs = detail::positive_int(value, key, w); return true; }
  if (key == "throttle_waiters") { e.throttle_waiters = detail::positive_int(value, key, w); return true; }
  if (key == "pause_waiters") { e.pause_waiters = detail::positive_int(value, key, w); return true; }
  if (key == "resume_waiters") { e.resume_waiters = detail::non_negative_int(value, key, w); return true; }
  if (key == "contention_dwell_ticks") { e.contention_dwell_ticks = detail::positive_int(value, key, w); return true; }
  if (key == "max_waiter_wait_ms") { e.max_waiter_wait_ms = detail::positive_int(value, key, w); return true; }
  if (key == "max_pause_s") { e.max_pause_s = detail::positive_int(value, key, w); return true; }
  if (key == "app_pool_size") { e.app_pool_size = detail::non_negative_int(value, key, w); return true; }
  if (key == "app_statement_timeout_ms") { e.app_statement_timeout_ms = detail::non_negative_int(value, key, w); return true; }
  if (key == "safety_percent") { e.safety_percent = detail::positive_int(value, key, w); return true; }
  return false;
}

// Checks the internal consistency of an ExecutorConfig. A configuration that
// parses but cannot behave is worse than one that fails to parse, because the
// failure surfaces during a migration rather than at startup.
inline void validate_executor(const ExecutorConfig& e, const std::string& w) {
  if (e.batch_cap_rows < e.batch_rows) {
    throw std::runtime_error(w + ": batch_cap_rows (" +
                             std::to_string(e.batch_cap_rows) +
                             ") is below batch_rows (" +
                             std::to_string(e.batch_rows) +
                             "), so the cap would fire on every batch");
  }
  if (e.pause_waiters < e.throttle_waiters) {
    throw std::runtime_error(w + ": pause_waiters (" +
                             std::to_string(e.pause_waiters) +
                             ") is below throttle_waiters (" +
                             std::to_string(e.throttle_waiters) +
                             "), so the breaker would pause before throttling");
  }
  if (e.resume_waiters >= e.pause_waiters) {
    // Without hysteresis the breaker flaps: it pauses at N, resumes at N, and
    // pauses again on the next tick.
    throw std::runtime_error(w + ": resume_waiters (" +
                             std::to_string(e.resume_waiters) +
                             ") must be below pause_waiters (" +
                             std::to_string(e.pause_waiters) +
                             ") or the breaker will flap");
  }
  if (e.observer_tick_ms > e.commit_interval_ms) {
    throw std::runtime_error(w + ": observer_tick_ms (" +
                             std::to_string(e.observer_tick_ms) +
                             ") exceeds commit_interval_ms (" +
                             std::to_string(e.commit_interval_ms) +
                             "), so a waiter could not be seen before the "
                             "interval commit it was meant to trigger");
  }
  if (e.safety_percent > 100) {
    throw std::runtime_error(w + ": safety_percent must be 1..100, got " +
                             std::to_string(e.safety_percent));
  }
}

class Registry {
 public:
  // Parses an INI file. Section names are connection names, except for the
  // three reserved forms: [trust], [executor], and [key <key_id>].
  static Registry from_ini(const std::string& path,
                           const std::string& app_name) {
    Registry r;
    r.check_permissions(path);

    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open config file " + path);

    std::string line, section;
    std::size_t lineno = 0;
    std::map<std::string, std::map<std::string, std::string>> sections;
    std::vector<std::string> order;

    while (std::getline(in, line)) {
      ++lineno;
      const auto text = detail::trim(detail::strip_comment(line));
      if (text.empty()) continue;

      if (text.front() == '[') {
        if (text.back() != ']') {
          throw std::runtime_error(detail::where(path, lineno) +
                                   ": unterminated section header");
        }
        section = detail::trim(text.substr(1, text.size() - 2));
        if (section.empty()) {
          throw std::runtime_error(detail::where(path, lineno) +
                                   ": empty section name");
        }
        if (sections.count(section) != 0) {
          throw std::runtime_error(detail::where(path, lineno) +
                                   ": duplicate section [" + section + "]");
        }
        sections[section] = {};
        order.push_back(section);
        continue;
      }

      if (section.empty()) {
        throw std::runtime_error(detail::where(path, lineno) +
                                 ": key outside any section");
      }
      const auto eq = text.find('=');
      if (eq == std::string::npos) {
        throw std::runtime_error(detail::where(path, lineno) +
                                 ": expected key = value");
      }
      const auto key = detail::trim(text.substr(0, eq));
      const auto value = detail::trim(text.substr(eq + 1));
      if (key.empty()) {
        throw std::runtime_error(detail::where(path, lineno) + ": empty key");
      }
      if (sections[section].count(key) != 0) {
        throw std::runtime_error(detail::where(path, lineno) +
                                 ": duplicate key " + key + " in [" + section +
                                 "]");
      }
      sections[section][key] = value;
    }

    const std::string w = path;
    for (const auto& name : order) {
      const auto& kv = sections[name];
      if (name == "trust") {
        r.parse_trust(kv, w);
      } else if (name == "executor") {
        for (const auto& [k, v] : kv) {
          if (!apply_executor_key(r.executor_, k, v, w)) {
            throw std::runtime_error(w + ": unknown key " + k +
                                     " in [executor]");
          }
        }
        validate_executor(r.executor_, w);
      } else if (name.rfind("key ", 0) == 0) {
        r.parse_key(detail::trim(name.substr(4)), kv, w);
      } else {
        r.parse_connection(name, kv, app_name, w);
      }
    }

    // Every accepted key id must have a corresponding [key ...] section, or
    // the policy names a key this machine cannot check a signature against.
    for (const auto& id : r.trust_.accept) {
      if (r.trust_.keys.count(id) == 0) {
        throw std::runtime_error(w + ": [trust] accepts " + id +
                                 " but there is no [key " + id + "] section");
      }
    }
    return r;
  }

  // The single-connection form, from DATABASE_URL or argv.
  static Registry from_url(const std::string& url, const std::string& app_name) {
    Registry r;
    ConnConfig c;
    c.name = "default";
    c.conninfo = url + " application_name=" + detail::quote_conninfo(app_name);
    c.executor = r.executor_;
    r.conns_["default"] = c;
    r.order_.push_back("default");
    r.default_name_ = "default";
    return r;
  }

  const ConnConfig& get(const std::string& name) const {
    const auto it = conns_.find(name);
    if (it == conns_.end()) {
      throw std::runtime_error("no such connection: " + name);
    }
    return it->second;
  }
  bool has(const std::string& name) const { return conns_.count(name) != 0; }
  const std::string& default_name() const { return default_name_; }
  const std::vector<std::string>& order() const { return order_; }
  const TrustPolicy& trust() const { return trust_; }
  const ExecutorConfig& executor() const { return executor_; }

 private:
  // Same rule as ~/.pgpass, and for both of the reasons this file carries.
  //
  // It holds CREDENTIALS: dbname, user and password, like any other migration
  // tool's configuration, so it must not be readable by anyone else. It also
  // holds POLICY -- which signing keys this machine will accept -- and a policy
  // anyone can rewrite is not a policy, so it must not be writable either.
  //
  // Checking 0077 covers both: it refuses any group or other permission at all.
  // An earlier version checked only 0022, on the mistaken belief that this file
  // held nothing secret. That was wrong, and it was wrong in the quiet
  // direction -- a world-readable password would have passed.
  void check_permissions(const std::string& path) const {
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) {
      throw std::runtime_error("cannot stat config file " + path);
    }
    if ((st.st_mode & static_cast<mode_t>(0077)) != 0) {
      throw std::runtime_error(
          "config file " + path +
          " is group- or world-accessible; it may hold a password, and it "
          "declares which signing keys are trusted. Run: chmod 600 " + path);
    }
  }

  void parse_trust(const std::map<std::string, std::string>& kv,
                   const std::string& w) {
    for (const auto& [k, v] : kv) {
      if (k == "accept") {
        trust_.accept = detail::split_list(v, k, w);
      } else {
        throw std::runtime_error(w + ": unknown key " + k + " in [trust]");
      }
    }
  }

  void parse_key(const std::string& key_id,
                 const std::map<std::string, std::string>& kv,
                 const std::string& w) {
    if (key_id.empty()) {
      throw std::runtime_error(w + ": [key ...] section with no key id");
    }
    TrustedKey k;
    k.key_id = key_id;
    for (const auto& [key, v] : kv) {
      if (key == "label") {
        k.label = v;
      } else if (key == "public_key") {
        k.public_key = decode_public_key(v, key_id, w);
      } else {
        throw std::runtime_error(w + ": unknown key " + key + " in [key " +
                                 key_id + "]");
      }
    }
    if (k.public_key.empty()) {
      throw std::runtime_error(w + ": [key " + key_id +
                               "] has no public_key");
    }
    if (k.label.empty()) {
      // A key with no human name is a key nobody can talk about during an
      // incident.
      throw std::runtime_error(w + ": [key " + key_id + "] has no label");
    }
    trust_.keys[key_id] = std::move(k);
  }

  // Accepts base64 of either a raw 32-byte Ed25519 public key or a 44-byte
  // SPKI DER wrapper (what `openssl pkey -pubout` emits). Both are common and
  // refusing one would be a papercut with no safety benefit.
  static std::vector<unsigned char> decode_public_key(const std::string& b64,
                                                      const std::string& key_id,
                                                      const std::string& w) {
    const auto raw = base64_decode(b64);
    if (raw.size() == 32) return raw;
    static const unsigned char kSpkiPrefix[12] = {
        0x30, 0x2a, 0x30, 0x05, 0x06, 0x03, 0x2b, 0x65, 0x70, 0x03, 0x21, 0x00};
    if (raw.size() == 44 &&
        std::equal(kSpkiPrefix, kSpkiPrefix + 12, raw.begin())) {
      return std::vector<unsigned char>(raw.begin() + 12, raw.end());
    }
    throw std::runtime_error(
        w + ": [key " + key_id +
        "] public_key must be base64 of a 32-byte raw Ed25519 key or its "
        "44-byte SPKI form, got " +
        std::to_string(raw.size()) + " bytes");
  }

 public:
  static std::string base64_encode(const std::vector<unsigned char>& in) {
    static const char* kAlpha =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < in.size(); i += 3) {
      const unsigned int b0 = in[i];
      const unsigned int b1 = (i + 1 < in.size()) ? in[i + 1] : 0u;
      const unsigned int b2 = (i + 2 < in.size()) ? in[i + 2] : 0u;
      const unsigned int t = (b0 << 16) | (b1 << 8) | b2;
      out += kAlpha[(t >> 18) & 0x3Fu];
      out += kAlpha[(t >> 12) & 0x3Fu];
      out += (i + 1 < in.size()) ? kAlpha[(t >> 6) & 0x3Fu] : '=';
      out += (i + 2 < in.size()) ? kAlpha[t & 0x3Fu] : '=';
    }
    return out;
  }

  // Strict base64: rejects any character outside the alphabet rather than
  // skipping it. A lenient decoder would silently accept a truncated or
  // corrupted key and produce a key id that simply never matches, which is a
  // much harder failure to read than "that is not base64".
  static std::vector<unsigned char> base64_decode(const std::string& in) {
    auto value = [](char c) -> int {
      if (c >= 'A' && c <= 'Z') return c - 'A';
      if (c >= 'a' && c <= 'z') return c - 'a' + 26;
      if (c >= '0' && c <= '9') return c - '0' + 52;
      if (c == '+') return 62;
      if (c == '/') return 63;
      return -1;
    };
    std::string s;
    for (const char c : in) {
      if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
      s += c;
    }
    if (s.size() % 4 != 0) {
      throw std::runtime_error("base64: length is not a multiple of 4");
    }
    std::vector<unsigned char> out;
    out.reserve(s.size() / 4 * 3);
    for (std::size_t i = 0; i < s.size(); i += 4) {
      int n[4];
      int pad = 0;
      for (int j = 0; j < 4; ++j) {
        const char c = s[i + static_cast<std::size_t>(j)];
        if (c == '=') {
          if (j < 2) throw std::runtime_error("base64: misplaced padding");
          n[j] = 0;
          ++pad;
        } else {
          n[j] = value(c);
          if (n[j] < 0) {
            throw std::runtime_error(std::string("base64: invalid character '") +
                                     c + "'");
          }
          if (pad > 0) throw std::runtime_error("base64: data after padding");
        }
      }
      const unsigned int triple =
          (static_cast<unsigned int>(n[0]) << 18) |
          (static_cast<unsigned int>(n[1]) << 12) |
          (static_cast<unsigned int>(n[2]) << 6) | static_cast<unsigned int>(n[3]);
      out.push_back(static_cast<unsigned char>((triple >> 16) & 0xFF));
      if (pad < 2) out.push_back(static_cast<unsigned char>((triple >> 8) & 0xFF));
      if (pad < 1) out.push_back(static_cast<unsigned char>(triple & 0xFF));
    }
    return out;
  }

 private:
  void parse_connection(const std::string& name,
                        const std::map<std::string, std::string>& kv,
                        const std::string& app_name, const std::string& w) {
    ConnConfig c;
    c.name = name;
    c.executor = executor_;  // section order matters: [executor] first wins
    std::vector<std::string> parts;

    for (const auto& [k, v] : kv) {
      // Non-libpq keys are CONSUMED here rather than passed through. Anything
      // unrecognised would otherwise land in the conninfo and libpq would
      // reject the whole string at connect time -- a failure that surfaces on
      // the first tool call rather than at startup.
      if (k == "statement_timeout_ms") {
        c.statement_timeout_ms = detail::non_negative_int(v, k, w);
        continue;
      }
      if (apply_executor_key(c.executor, k, v, w)) continue;

      if (k == "options") {
        // PgBouncer rejects `options` as an unsupported startup parameter, and
        // this project already refuses to run its executor behind a
        // transaction-mode pooler for stronger reasons. Passing it would make
        // the failure a connect error at the worst moment.
        throw std::runtime_error(
            w + ": [" + name +
            "] sets `options`, which a pooler rejects as an unsupported "
            "startup parameter. Set GUCs per transaction instead.");
      }
      if (k == "service") c.service = v;
      if (k == "host") c.host = v;
      if (k == "port") c.port = v;
      if (k == "dbname") c.dbname = v;
      if (k == "user") c.user = v;
      parts.push_back(k + "=" + detail::quote_conninfo(v));
    }
    validate_executor(c.executor, w);

    if (parts.empty()) {
      throw std::runtime_error(w + ": [" + name +
                               "] has no connection parameters");
    }
    parts.push_back("application_name=" + detail::quote_conninfo(app_name));
    std::string conninfo;
    for (std::size_t i = 0; i < parts.size(); ++i) {
      if (i != 0) conninfo += " ";
      conninfo += parts[i];
    }
    c.conninfo = conninfo;

    conns_[name] = std::move(c);
    order_.push_back(name);
    if (default_name_.empty()) default_name_ = name;
  }

  std::map<std::string, ConnConfig> conns_;
  std::vector<std::string> order_;
  std::string default_name_;
  TrustPolicy trust_;
  ExecutorConfig executor_;
};

}  // namespace pglaswell
