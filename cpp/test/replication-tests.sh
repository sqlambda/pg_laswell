#!/usr/bin/env bash
#
# The intent kinds that cannot be proved against one database.
#
# create_subscription needs something to subscribe TO; import_foreign_schema
# needs a reachable foreign server; alter_default_privileges only affects
# objects created LATER by a particular role, so proving it needs a second role
# and a later creation. conformance.inc lists each one and why, and a test
# asserts that list stays in step with the kinds -- this script is the other
# half of that promise.
#
# Two clusters are built with initdb under a temporary directory and thrown
# away afterwards. Nothing touches an existing server.
set -uo pipefail

BIN="${1:-}"
# Discovered rather than hardcoded, because the hardcoded path was
# /usr/lib/postgresql/18/bin and a runner that installs 16 would take the SKIP
# below and report success -- which is exactly the failure this project warns
# about elsewhere: a skip nobody notices is a test that silently stopped
# running. Newest first; PGBIN still overrides.
if [ -z "${PGBIN:-}" ]; then
  for d in $(ls -d /usr/lib/postgresql/*/bin 2>/dev/null | sort -Vr); do
    [ -x "$d/initdb" ] && { PGBIN="$d"; break; }
  done
fi
PGBIN="${PGBIN:-/usr/lib/postgresql/18/bin}"
[ -x "$PGBIN/initdb" ] || { echo "SKIP: no initdb found under /usr/lib/postgresql"; exit 0; }
echo "using $PGBIN"

WORK=$(mktemp -d "${TMPDIR:-/tmp}/laswell-repl-XXXXXX")
PUB_PORT=$((5600 + RANDOM % 100))
SUB_PORT=$((PUB_PORT + 1))
export PSQLRC=/dev/null
pass=0; fail=0
ok()  { printf '  ok   %s\n' "$1"; pass=$((pass+1)); }
bad() { printf '  FAIL %s\n' "$1"; fail=$((fail+1)); }

cleanup() {
  for d in pub sub; do
    [ -d "$WORK/$d" ] && "$PGBIN/pg_ctl" -D "$WORK/$d" -m immediate stop >/dev/null 2>&1
  done
  rm -rf "$WORK"
}
trap cleanup EXIT

start() {  # start <name> <port> <extra conf>
  "$PGBIN/initdb" -D "$WORK/$1" -A trust -U postgres >/dev/null 2>&1 || return 1
  {
    echo "port = $2"
    echo "unix_socket_directories = '$WORK'"
    echo "listen_addresses = '127.0.0.1'"
    echo "wal_level = logical"
    echo "max_wal_senders = 4"
    echo "max_replication_slots = 4"
    echo "log_min_messages = warning"
  } >> "$WORK/$1/postgresql.conf"
  "$PGBIN/pg_ctl" -D "$WORK/$1" -l "$WORK/$1.log" -w start >/dev/null 2>&1
}

echo "--- building two clusters under $WORK ---"
start pub "$PUB_PORT" || { echo "SKIP: initdb failed"; exit 0; }
start sub "$SUB_PORT" || { echo "SKIP: initdb failed"; exit 0; }
PUB="host=$WORK port=$PUB_PORT dbname=postgres user=postgres"
SUB="host=$WORK port=$SUB_PORT dbname=postgres user=postgres"
psql -X -q "$PUB" -c "SELECT 1" >/dev/null 2>&1 && ok "publisher is up" || { bad "publisher did not start"; exit 1; }
psql -X -q "$SUB" -c "SELECT 1" >/dev/null 2>&1 && ok "subscriber is up" || { bad "subscriber did not start"; exit 1; }

echo "--- create_subscription / alter_subscription / drop_subscription ---"
psql -X -q "$PUB" <<SQL
CREATE TABLE t(id bigint PRIMARY KEY, v text);
INSERT INTO t SELECT g, 'v'||g FROM generate_series(1,25) g;
CREATE PUBLICATION p FOR TABLE t;
SQL
psql -X -q "$SUB" -c "CREATE TABLE t(id bigint PRIMARY KEY, v text)" >/dev/null 2>&1

# A password-free conninfo, which is what pg_laswell's spec parser enforces:
# trust auth here stands in for the .pgpass or service file it points authors at.
CONN="host=$WORK port=$PUB_PORT dbname=postgres user=postgres"
psql -X -q "$SUB" -c "CREATE SUBSCRIPTION s CONNECTION '$CONN' PUBLICATION p" >/dev/null 2>&1 \
  && ok "create_subscription: the statement pg_laswell emits is accepted" \
  || bad "create_subscription statement was rejected"

# It cannot run inside a transaction block -- the measured fact the planner's
# txn_class encodes. Asserting it here keeps that claim honest.
#
# Checked by EXIT STATUS, not by matching the message: this server answers in
# Portuguese, and a test that greps English error text passes or fails on the
# operator's locale rather than on the behaviour.
if ! psql -X -q -v ON_ERROR_STOP=1 "$SUB" \
       -c "BEGIN; CREATE SUBSCRIPTION s2 CONNECTION '$CONN' PUBLICATION p; COMMIT;" \
       >/dev/null 2>&1; then
  ok "create_subscription is refused inside a transaction block, as planned"
else
  bad "CREATE SUBSCRIPTION no longer refuses a transaction block -- txn_class is now wrong"
fi

for _ in $(seq 40); do
  n=$(psql -X -A -t "$SUB" -c "SELECT count(*) FROM t" 2>/dev/null)
  [ "$n" = "25" ] && break
  sleep 0.25
done
[ "$n" = "25" ] && ok "initial data replicated ($n rows)" || bad "replication did not copy rows (got ${n:-none})"

psql -X -q "$PUB" -c "INSERT INTO t VALUES (26,'v26')" >/dev/null 2>&1
for _ in $(seq 40); do
  n=$(psql -X -A -t "$SUB" -c "SELECT count(*) FROM t" 2>/dev/null)
  [ "$n" = "26" ] && break
  sleep 0.25
done
[ "$n" = "26" ] && ok "a later change streamed through" || bad "streaming did not work"

psql -X -q "$SUB" -c "ALTER SUBSCRIPTION s DISABLE" >/dev/null 2>&1 \
  && ok "alter_subscription: disable" || bad "alter_subscription disable failed"
# The measured hazard: the slot survives a disable and keeps holding WAL.
active=$(psql -X -A -t "$PUB" -c "SELECT count(*) FROM pg_replication_slots WHERE NOT active")
[ "$active" = "1" ] \
  && ok "the slot survives a disabled subscription -- the warned-about hazard is real" \
  || bad "expected one inactive slot on the publisher, saw ${active:-none}"

psql -X -q "$SUB" -c "ALTER SUBSCRIPTION s ENABLE" >/dev/null 2>&1
psql -X -q "$SUB" -c "DROP SUBSCRIPTION s" >/dev/null 2>&1 \
  && ok "drop_subscription" || bad "drop_subscription failed"
left=$(psql -X -A -t "$PUB" -c "SELECT count(*) FROM pg_replication_slots")
[ "$left" = "0" ] && ok "dropping the subscription removed the slot on the publisher" \
                  || bad "slot left behind on the publisher"

echo "--- import_foreign_schema ---"
psql -X -q "$SUB" <<SQL 2>/dev/null
CREATE EXTENSION IF NOT EXISTS postgres_fdw;
CREATE SERVER remote FOREIGN DATA WRAPPER postgres_fdw
  OPTIONS (host '$WORK', port '$PUB_PORT', dbname 'postgres');
CREATE USER MAPPING FOR postgres SERVER remote OPTIONS (user 'postgres');
CREATE SCHEMA staging;
SQL
psql -X -q "$SUB" -c "IMPORT FOREIGN SCHEMA public LIMIT TO (t) FROM SERVER remote INTO staging" >/dev/null 2>&1 \
  && ok "import_foreign_schema: the statement pg_laswell emits is accepted" \
  || bad "import_foreign_schema was rejected"
n=$(psql -X -A -t "$SUB" -c "SELECT count(*) FROM staging.t" 2>/dev/null)
[ "$n" = "26" ] && ok "the imported foreign table reads the remote ($n rows)" \
                || bad "foreign table did not read the remote (got ${n:-none})"

echo "--- alter_default_privileges ---"
psql -X -q "$SUB" <<SQL 2>/dev/null
CREATE ROLE app;
CREATE SCHEMA fut;
GRANT USAGE ON SCHEMA fut TO app;
ALTER DEFAULT PRIVILEGES IN SCHEMA fut GRANT SELECT ON TABLES TO app;
CREATE TABLE fut.later(id int);
SQL
got=$(psql -X -A -t "$SUB" -c "SELECT has_table_privilege('app','fut.later','SELECT')")
[ "$got" = "t" ] && ok "a table created AFTER the default grant has it" \
                 || bad "default privileges did not reach the later table"
# The documented trap, asserted so the warning stays true.
psql -X -q "$SUB" -c "CREATE SCHEMA fut2; CREATE TABLE fut2.before(id int)" >/dev/null 2>&1
got=$(psql -X -A -t "$SUB" -c "SELECT has_table_privilege('app','fut2.before','SELECT')")
[ "$got" = "f" ] && ok "a table in another schema does NOT -- the warned-about scope is real" \
                 || bad "default privileges leaked outside their schema"

echo
echo "$pass passed, $fail failed"
[ "$fail" = 0 ] || exit 1
