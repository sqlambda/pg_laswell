#!/usr/bin/env bash
#
# Fails unless each binary carries the hardening cmake/SafetyFlags.cmake asks
# for: PIE, full RELRO (GNU_RELRO + BIND_NOW), a non-executable stack, stack
# canaries, and at least one fortified libc call.
#
# The flags are requested in one file and the binary is produced by another
# toolchain entirely, so this reads the ELF rather than trusting the request.
# It found something on its first run: only one of the two binaries had been
# through the hardening function at all.
#
#   cpp/test/hardening-check.sh /path/to/pg_laswell /path/to/pg_laswell_mcp
#
# ELF only. Called for optimised builds without a sanitizer, which is the only
# configuration _FORTIFY_SOURCE applies to.
set -euo pipefail
# readelf translates its headers ("Tipo:" for "Type:").
export LC_ALL=C

fail=0
check() {  # check <label> <command...>
  local label="$1"; shift
  if "$@" >/dev/null 2>&1; then printf '  ok    %s\n' "$label"
  else printf '  FAIL  %s\n' "$label"; fail=1; fi
}

[ $# -ge 1 ] || { echo "usage: hardening-check.sh <binary>..." >&2; exit 2; }
for BIN in "$@"; do
  echo "$BIN"
  check "PIE (ELF type DYN)"   bash -c "readelf -h '$BIN' | grep -q 'Type:.*DYN'"
  check "GNU_RELRO"            bash -c "readelf -lW '$BIN' | grep -q GNU_RELRO"
  check "BIND_NOW"             bash -c "readelf -dW '$BIN' | grep -Eq 'BIND_NOW|FLAGS_1.*NOW'"
  check "non-executable stack" bash -c "readelf -lW '$BIN' | grep GNU_STACK | grep -qv 'RWE'"
  # readelf rather than nm -D: a stripped release binary keeps its dynamic
  # symbols, and this is the tool the standard names.
  check "stack protector"      bash -c "readelf -W --dyn-syms '$BIN' | grep -q __stack_chk_fail"
  check "FORTIFY_SOURCE"       bash -c "readelf -W --dyn-syms '$BIN' | grep -Eq '__[a-z_]+_chk'"
done
exit $fail
