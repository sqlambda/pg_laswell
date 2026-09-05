#pragma once

// RFC 8785 (JSON Canonicalization Scheme) plus SHA-256.
//
// The whole signature story rests on this file. Two documents that a human
// would call identical must produce identical bytes, or reformatting a spec
// breaks its signature; and two documents that differ in any way a parser can
// see must produce different bytes, or a signature can be transplanted.
//
// JCS in full is: UTF-8 output, object members sorted by the UTF-16 code-unit
// order of their names, no insignificant whitespace, minimal string escaping,
// and ECMAScript Number::toString for numbers.
//
// That last clause is the error-prone one -- it is the shortest decimal string
// that round-trips to the same double, and getting it subtly wrong produces a
// canonicalizer that agrees with itself and with nobody else. This file
// sidesteps it entirely: FLOATING-POINT VALUES ARE REFUSED. Nothing a
// migration intent needs is a float, integers serialise unambiguously, and a
// rejected float is a loud parse error naming its JSON pointer rather than a
// signature that silently fails to verify on another implementation.

#include <openssl/evp.h>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace pglaswell {

using json = nlohmann::json;

namespace detail {

// Decodes one UTF-8 code point starting at `i`, advancing `i` past it.
// Throws on malformed input: a canonicalizer that guesses at broken UTF-8
// would produce bytes that depend on which implementation did the guessing.
inline std::uint32_t utf8_next(const std::string& s, std::size_t& i) {
  const auto byte = [&](std::size_t k) -> std::uint32_t {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(s[k]));
  };
  const std::uint32_t c0 = byte(i);
  auto need = [&](std::size_t n) {
    if (i + n >= s.size()) throw std::runtime_error("canonical: truncated UTF-8");
    for (std::size_t k = 1; k <= n; ++k) {
      if ((byte(i + k) & 0xC0u) != 0x80u) {
        throw std::runtime_error("canonical: malformed UTF-8 continuation");
      }
    }
  };
  if (c0 < 0x80u) { i += 1; return c0; }
  if ((c0 & 0xE0u) == 0xC0u) {
    need(1);
    const std::uint32_t cp = ((c0 & 0x1Fu) << 6) | (byte(i + 1) & 0x3Fu);
    i += 2;
    return cp;
  }
  if ((c0 & 0xF0u) == 0xE0u) {
    need(2);
    const std::uint32_t cp = ((c0 & 0x0Fu) << 12) |
                             ((byte(i + 1) & 0x3Fu) << 6) |
                             (byte(i + 2) & 0x3Fu);
    i += 3;
    return cp;
  }
  if ((c0 & 0xF8u) == 0xF0u) {
    need(3);
    const std::uint32_t cp = ((c0 & 0x07u) << 18) |
                             ((byte(i + 1) & 0x3Fu) << 12) |
                             ((byte(i + 2) & 0x3Fu) << 6) |
                             (byte(i + 3) & 0x3Fu);
    i += 4;
    return cp;
  }
  throw std::runtime_error("canonical: invalid UTF-8 lead byte");
}

// UTF-16 code units for a UTF-8 string. JCS sorts member names by these, not
// by bytes -- the two orders differ for anything above U+FFFF, because a
// surrogate pair begins with 0xD800..0xDBFF while the three-byte UTF-8 forms
// that sort after it in byte order encode code points below U+FFFF.
inline std::vector<std::uint16_t> utf16_units(const std::string& s) {
  std::vector<std::uint16_t> out;
  std::size_t i = 0;
  while (i < s.size()) {
    const std::uint32_t cp = utf8_next(s, i);
    if (cp <= 0xFFFFu) {
      out.push_back(static_cast<std::uint16_t>(cp));
    } else {
      const std::uint32_t v = cp - 0x10000u;
      out.push_back(static_cast<std::uint16_t>(0xD800u + (v >> 10)));
      out.push_back(static_cast<std::uint16_t>(0xDC00u + (v & 0x3FFu)));
    }
  }
  return out;
}

inline bool utf16_less(const std::string& a, const std::string& b) {
  return utf16_units(a) < utf16_units(b);
}

// JCS string escaping: the two mandatory escapes, the five short forms, and
// \u00XX for everything else below 0x20. Every other code point is emitted as
// literal UTF-8 -- notably, non-ASCII is NOT \u-escaped.
//
// This decodes rather than walking bytes, so that it VALIDATES. An earlier
// version copied bytes directly and only validated incidentally, inside the
// key comparator -- which meant a single-key object never validated at all
// (std::sort does not call the comparator for one element) and a malformed
// value never validated under any circumstances. A canonicalizer that passes
// invalid UTF-8 through is producing bytes another implementation may reject
// or repair differently, which is precisely the disagreement it exists to
// prevent.
inline void escape_into(const std::string& s, std::string& out) {
  static const char* kHex = "0123456789abcdef";
  out += '"';
  std::size_t i = 0;
  while (i < s.size()) {
    const std::size_t start = i;
    const std::uint32_t cp = utf8_next(s, i);
    switch (cp) {
      case '"':  out += "\\\""; continue;
      case '\\': out += "\\\\"; continue;
      case '\b': out += "\\b";  continue;
      case '\f': out += "\\f";  continue;
      case '\n': out += "\\n";  continue;
      case '\r': out += "\\r";  continue;
      case '\t': out += "\\t";  continue;
      default: break;
    }
    if (cp < 0x20u) {
      out += "\\u00";
      out += kHex[(cp >> 4) & 0xFu];
      out += kHex[cp & 0xFu];
    } else {
      out.append(s, start, i - start);
    }
  }
  out += '"';
}

inline void canonicalize_into(const json& v, std::string& out,
                              const std::string& pointer) {
  switch (v.type()) {
    case json::value_t::null:
      out += "null";
      return;
    case json::value_t::boolean:
      out += v.get<bool>() ? "true" : "false";
      return;
    case json::value_t::number_float:
      // See the header comment. This is a refusal, not a limitation to work
      // around: it removes JCS's one genuinely error-prone clause.
      throw std::runtime_error(
          "canonical: floating-point values are not allowed in a spec (at " +
          (pointer.empty() ? std::string("/") : pointer) +
          "). Use an integer, or a string if the value is not a number.");
    case json::value_t::number_integer:
    case json::value_t::number_unsigned: {
      const auto n = v.is_number_unsigned()
                         ? static_cast<std::int64_t>(v.get<std::uint64_t>())
                         : v.get<std::int64_t>();
      // Beyond 2^53 a JSON number stops round-tripping through the double that
      // most clients parse it into, so a spec must not contain one. Large ids
      // belong in strings, which is already the rule for anything the ledger
      // emits.
      if (n > 9007199254740991LL || n < -9007199254740991LL) {
        throw std::runtime_error(
            "canonical: integer out of safe range at " + pointer +
            "; values beyond 2^53 must be strings");
      }
      out += std::to_string(n);
      return;
    }
    case json::value_t::string:
      escape_into(v.get<std::string>(), out);
      return;
    case json::value_t::array: {
      out += '[';
      std::size_t i = 0;
      for (const auto& e : v) {
        if (i != 0) out += ',';
        canonicalize_into(e, out, pointer + "/" + std::to_string(i));
        ++i;
      }
      out += ']';
      return;
    }
    case json::value_t::object: {
      std::vector<std::string> keys;
      keys.reserve(v.size());
      for (auto it = v.begin(); it != v.end(); ++it) keys.push_back(it.key());
      std::sort(keys.begin(), keys.end(), utf16_less);
      out += '{';
      for (std::size_t i = 0; i < keys.size(); ++i) {
        if (i != 0) out += ',';
        escape_into(keys[i], out);
        out += ':';
        canonicalize_into(v.at(keys[i]), out, pointer + "/" + keys[i]);
      }
      out += '}';
      return;
    }
    case json::value_t::binary:
    case json::value_t::discarded:
    default:
      throw std::runtime_error("canonical: unrepresentable value at " + pointer);
  }
}

}  // namespace detail

// RFC 8785 canonical form. Reformatting the input -- reordering keys, changing
// indentation, a jq round-trip -- must not change this output. A test asserts
// exactly that, because it is the property the whole signature scheme buys.
inline std::string canonicalize(const json& v) {
  std::string out;
  detail::canonicalize_into(v, out, "");
  return out;
}

inline std::vector<unsigned char> sha256(const std::string& data) {
  std::vector<unsigned char> digest(EVP_MAX_MD_SIZE);
  unsigned int len = 0;
  if (EVP_Digest(data.data(), data.size(), digest.data(), &len, EVP_sha256(),
                 nullptr) != 1) {
    throw std::runtime_error("sha256: EVP_Digest failed");
  }
  digest.resize(len);
  return digest;
}

inline std::string to_hex(const std::vector<unsigned char>& bytes) {
  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (const auto b : bytes) {
    out += kHex[(b >> 4) & 0xFu];
    out += kHex[b & 0xFu];
  }
  return out;
}

// The digest a human quotes and the ledger stores. Computed from the canonical
// bytes, never from the file as written.
inline std::string digest_hex(const json& v) {
  return to_hex(sha256(canonicalize(v)));
}

}  // namespace pglaswell
