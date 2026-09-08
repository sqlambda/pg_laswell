#!/usr/bin/env bash
#
# What survives a transaction-mode pooler, and what does not.
#
# pg_laswell's read-only tools are pooler-safe by the same discipline pg_licht
# holds to: everything is transaction-scoped. Its EXECUTOR is not, and refuses
# to run behind one. This script asserts both halves, and a third thing that is
# easy to forget:
#
#   IT ASSERTS THE LEAK IS STILL REAL.
#
# The refusal exists because PgBouncer does not run server_reset_query in
# transaction mode unless server_reset_query_always is enabled, and it is off by
# default -- so session state leaks BETWEEN CLIENTS. If that default ever
# changes, the refusal becomes unnecessary and this test is how we find out,
# rather than carrying a restriction nobody can justify any more.
set -euo pipefail

BIN="${1:?usage: run-pooled-tests.sh <path to pg_laswell_mcp>}"
PG_PORT="${PG_PORT:-5432}"
PG_HOST="${PG_HOST:-localhost}"
PG_USER="${PG_USER:-postgres}"
PG_PASS="${PG_PASS:-postgres}"
PG_DB="${PG_DB:-postgres}"
BOUNCER_PORT="${BOUNCER_PORT:-6432}"
PGBOUNCER="${PGBOUNCER:-pgbouncer}"

export PSQLRC=/dev/null
export PGPASSWORD="$PG_PASS"

WORK=$(mktemp -d)
cleanup() {
  [ -f "$WORK/bouncer.pid" ] && kill "$(cat "$WORK/bouncer.pid")" 2>/dev/null || true
  rm -rf "$WORK"
}
trap cleanup EXIT

pass=0; fail=0
ok()   { printf '  ok   %s\n' "$1"; pass=$((pass+1)); }
bad()  { printf '  FAIL %s\n' "$1"; fail=$((fail+1)); }

printf '"%s" "%s"\n' "$PG_USER" "$PG_PASS" > "$WORK/users.txt"
cat > "$WORK/pgbouncer.ini" <<EOF
[databases]
$PG_DB = host=$PG_HOST port=$PG_PORT dbname=$PG_DB

[pgbouncer]
listen_addr = 127.0.0.1
listen_port = $BOUNCER_PORT
auth_type = plain
auth_file = $WORK/users.txt
pool_mode = transaction
server_reset_query = DISCARD ALL
max_client_conn = 50
default_pool_size = 1
logfile = $WORK/pgbouncer.log
pidfile = $WORK/bouncer.pid
unix_socket_dir =
EOF

"$PGBOUNCER" --version || true
"$PGBOUNCER" -d "$WORK/pgbouncer.ini"

# Prove the pooler is reachable BEFORE running any probe through it.
#
# Without this the script ran its probes against a pooler nothing could connect
# to, and reported "session state did NOT leak -- PgBouncer's default may have
# changed" -- a conclusion about PgBouncer's behaviour drawn from a connection
# that never happened. That is worse than failing: it points the next reader at
# the wrong thing entirely, and it is exactly the mistake this project refuses
# to make elsewhere, where a reading that cannot be taken is reported as
# unavailable rather than guessed at.
reachable=no
for _ in $(seq 30); do
  if psql -X -q -c 'SELECT 1' \
       "host=127.0.0.1 port=$BOUNCER_PORT dbname=$PG_DB user=$PG_USER" >/dev/null 2>&1; then
    reachable=yes; break
  fi
  sleep 0.5
done
if [ "$reachable" != yes ]; then
  echo "  FAIL the pooler never accepted a connection, so nothing below was measured"
  echo "       psql said:"
  psql -X -q -c 'SELECT 1' \
    "host=127.0.0.1 port=$BOUNCER_PORT dbname=$PG_DB user=$PG_USER" 2>&1 | sed 's/^/         /'
  echo "       pgbouncer log:"
  sed 's/^/         /' "$WORK/pgbouncer.log" 2>/dev/null || echo "         (no log at $WORK/pgbouncer.log)"
  echo "       config:"
  sed 's/^/         /' "$WORK/pgbouncer.ini"
  exit 1
fi

POOLED="host=127.0.0.1 port=$BOUNCER_PORT dbname=$PG_DB user=$PG_USER password=$PG_PASS"
DIRECT="host=$PG_HOST port=$PG_PORT dbname=$PG_DB user=$PG_USER password=$PG_PASS"

echo "--- the assumption the refusal rests on ---"
# Set a session GUC on one client, end its transaction, then read it from a
# DIFFERENT client. With default_pool_size = 1 they share one server
# connection, so a leak is visible if server_reset_query did not run.
TOKEN="laswell_$RANDOM"
psql -X -q "$POOLED" -c "BEGIN; SET my.probe = '$TOKEN'; COMMIT;" >/dev/null 2>&1 || true
SEEN=$(psql -X -q -A -t "$POOLED" \
  -c "SELECT coalesce(current_setting('my.probe', true), '')" 2>/dev/null || echo '')
if [ "$SEEN" = "$TOKEN" ]; then
  ok "session state leaks between clients (server_reset_query_always is off)"
else
  bad "session state did NOT leak -- PgBouncer's default may have changed."
  echo "       If server_reset_query now runs in transaction mode, the"
  echo "       executor's refusal may no longer be necessary. Re-measure"
  echo "       before removing it: advisory-lock re-entrancy within one"
  echo "       session is the hazard, and DISCARD ALL is what would fix it."
fi

echo "--- read-only tools through the pooler ---"
for tool in checkPrivileges; do
  if "$BIN" --call "$tool" "$POOLED" >/dev/null 2>&1; then
    ok "$tool answers through a transaction-mode pooler"
  else
    bad "$tool failed through the pooler"
  fi
done

cat > "$WORK/args.json" <<'JSON'
{"spec":{"laswell_spec_version":1,"id":"pooled-probe","description":"d",
 "intents":[{"kind":"add_column","schema":"public","table":"t","column":"c",
             "type":"text","nullable":true,"comment":"c"}]}}
JSON
if "$BIN" --call getSpecDigest --args "@$WORK/args.json" "$POOLED" >/dev/null 2>&1; then
  ok "getSpecDigest answers through the pooler (it touches no database)"
else
  bad "getSpecDigest failed through the pooler"
fi

echo "--- and the same tools direct, as a control ---"
if "$BIN" --call checkPrivileges "$DIRECT" >/dev/null 2>&1; then
  ok "checkPrivileges answers on a direct connection"
else
  bad "checkPrivileges failed on a direct connection"
fi

echo
echo "$pass passed, $fail failed"
[ "$fail" = 0 ] || exit 1
