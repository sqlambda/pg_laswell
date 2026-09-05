#!/usr/bin/env bash
#
# The --call contract, tested at the process level.
#
# The exit code is the whole point of this mode and it is invisible from inside
# gtest, which sees return values rather than process status. So it is tested
# here, against the real binary, the same way pg_licht tests the env switches
# that its suite cannot reach.
#
# Usage: call-mode.sh <path to pg_laswell_mcp> [conninfo]
set -euo pipefail

BIN="${1:?usage: call-mode.sh <binary> [conninfo]}"
CONN="${2:-${DATABASE_URL:-}}"
PSQLRC=/dev/null
export PSQLRC

pass=0
fail=0
check() {  # check <expected exit> <description> -- <args...>
  local want="$1" desc="$2"; shift 3
  local got=0
  set +e
  if [ -n "$CONN" ]; then
    "$BIN" "$@" "$CONN" >/dev/null 2>&1
  else
    "$BIN" "$@" >/dev/null 2>&1
  fi
  got=$?
  set -e
  if [ "$got" = "$want" ]; then
    printf '  ok   %-52s exit=%s\n' "$desc" "$got"
    pass=$((pass + 1))
  else
    printf '  FAIL %-52s exit=%s want=%s\n' "$desc" "$got" "$want"
    fail=$((fail + 1))
  fi
}

echo "--- flags that need no database ---"
set +e
"$BIN" --version >/dev/null 2>&1; v=$?
"$BIN" --help    >/dev/null 2>&1; h=$?
"$BIN" --call    >/dev/null 2>&1; c=$?
"$BIN" --nonsense >/dev/null 2>&1; n=$?
set -e
[ "$v" = 0 ] && { echo "  ok   --version"; pass=$((pass+1)); } || { echo "  FAIL --version exit=$v"; fail=$((fail+1)); }
[ "$h" = 0 ] && { echo "  ok   --help"; pass=$((pass+1)); }    || { echo "  FAIL --help exit=$h"; fail=$((fail+1)); }
# --call with no tool name is a usage error, not a silent success.
[ "$c" = 1 ] && { echo "  ok   --call with no tool name is rejected"; pass=$((pass+1)); } \
             || { echo "  FAIL --call with no tool name exit=$c"; fail=$((fail+1)); }
[ "$n" = 1 ] && { echo "  ok   an unknown flag is rejected"; pass=$((pass+1)); } \
             || { echo "  FAIL unknown flag exit=$n"; fail=$((fail+1)); }

if [ -z "$CONN" ]; then
  echo "--- no conninfo given; skipping the database checks ---"
  echo "$pass passed, $fail failed"
  [ "$fail" = 0 ] || exit 1
  exit 0
fi

# Prove the database is reachable BEFORE running checks that expect failure.
# Without this, every "check 1" is satisfied by a connection error and the
# script reports 15 passes against a database that is not there -- which is
# exactly what it did while the conninfo was frozen at configure time.
if ! "$BIN" --call checkPrivileges "$CONN" >/dev/null 2>&1; then
  echo "  FAIL cannot reach the database, so no exit code below means anything"
  echo "       conninfo: $CONN"
  "$BIN" --call checkPrivileges "$CONN" 2>&1 | sed 's/^/       /' | head -3
  exit 1
fi

echo "--- exit codes that a pipeline gates on ---"
check 0 "a healthy tool answers"            -- --call checkPrivileges
check 1 "an unknown tool"                   -- --call nosuchTool
check 1 "a spec that does not parse"        -- --call validateSpec --args '{"spec":{"laswell_spec_version":1}}'
check 1 "a tool called with no arguments"   -- --call validateSpec

DIR=$(mktemp -d)
trap 'rm -rf "$DIR"' EXIT
cat > "$DIR/0001.json" <<'JSON'
{"laswell_spec_version":1,"id":"0001","description":"d",
 "intents":[{"kind":"add_column","schema":"s","table":"t","column":"c",
             "type":"text","nullable":true,"comment":"c"}]}
JSON
check 0 "listMigrations on a clean repository" -- --call listMigrations \
      --args "{\"directory\":\"$DIR\",\"deriveRelations\":false}"

# The case that makes listMigrations a build gate: a dependency that is not
# there. A repository with problems must fail a pipeline, not merely mention
# them.
cat > "$DIR/0002.json" <<'JSON'
{"laswell_spec_version":1,"id":"0002","description":"d","depends_on":["ghost"],
 "intents":[{"kind":"add_column","schema":"s","table":"t","column":"c2",
             "type":"text","nullable":true,"comment":"c"}]}
JSON
check 1 "listMigrations with a repository problem" -- --call listMigrations \
      --args "{\"directory\":\"$DIR\",\"deriveRelations\":false}"

echo "--- --args sources ---"
cat > "$DIR/args.json" <<'JSON'
{"spec":{"laswell_spec_version":1,"id":"x","description":"d",
 "intents":[{"kind":"add_column","schema":"s","table":"t","column":"c",
             "type":"text","nullable":true,"comment":"c"}]}}
JSON
check 0 "--args @file" -- --call getSpecDigest --args "@$DIR/args.json"
set +e
"$BIN" --call getSpecDigest --args @- "$CONN" < "$DIR/args.json" >/dev/null 2>&1
stdin_exit=$?
"$BIN" --call getSpecDigest --args "@$DIR/nonexistent" "$CONN" >/dev/null 2>&1
missing_exit=$?
set -e
[ "$stdin_exit" = 0 ] && { echo "  ok   --args @- reads stdin"; pass=$((pass+1)); } \
                      || { echo "  FAIL --args @- exit=$stdin_exit"; fail=$((fail+1)); }
[ "$missing_exit" = 1 ] && { echo "  ok   --args @missing-file is an error"; pass=$((pass+1)); } \
                        || { echo "  FAIL --args @missing-file exit=$missing_exit"; fail=$((fail+1)); }

# The writing tools, which gtest reaches only in-process. A pipeline that can
# START a migration but cannot tell whether it was accepted is worse than one
# that cannot start it at all.
echo "--- the writing tools ---"
check 1 "startMigration with an unsigned spec"  -- --call startMigration \
      --args '{"spec":{"laswell_spec_version":1,"id":"x","description":"d","intents":[{"kind":"add_column","schema":"public","table":"nope","column":"c","type":"text","nullable":true,"comment":"c"}]}}'
check 1 "cancelJob with an unknown job id"      -- --call cancelJob --args '{"jobId":"nope"}'
check 0 "jobStatus with nothing running"        -- --call jobStatus

# jobStatus must answer with a jobs array even when idle: a caller that cannot
# distinguish "no jobs" from "the call failed" has to guess.
if "$BIN" --call jobStatus "$CONN" 2>/dev/null | grep -q '"jobs"'; then
  echo "  ok   jobStatus reports an empty jobs array rather than nothing"; pass=$((pass+1))
else
  echo "  FAIL jobStatus did not carry a jobs array"; fail=$((fail+1))
fi

echo "--- the payload is on stdout and is JSON ---"
if "$BIN" --call checkPrivileges "$CONN" 2>/dev/null | head -1 | grep -q '^{'; then
  echo "  ok   stdout carries the payload"; pass=$((pass+1))
else
  echo "  FAIL stdout did not carry a JSON payload"; fail=$((fail+1))
fi

echo
echo "$pass passed, $fail failed"
[ "$fail" = 0 ] || exit 1
