#!/usr/bin/env bash
# S7b — why did pid stay stable in transaction mode, and what DOES detect it?
#   (1) does session state survive a transaction boundary in transaction mode?
#   (2) does it LEAK to another client? (server_reset_query_always defaults off)
#   (3) does the pid flip once there is real contention?
set -uo pipefail
export PSQLRC=/dev/null
PSQL="psql -X -q -A -t"
D=$(mktemp -d); trap 'kill $(cat "$D/bouncer.pid" 2>/dev/null) 2>/dev/null; rm -rf "$D"' EXIT
PORT=6544
printf '"daniel" ""\n' > "$D/users.txt"
cat > "$D/pgbouncer.ini" <<EOF
[databases]
laswell_spike = host=/var/run/postgresql port=5555 dbname=laswell_spike
[pgbouncer]
listen_addr = 127.0.0.1
listen_port = $PORT
auth_type = trust
auth_file = $D/users.txt
pool_mode = transaction
server_reset_query = DISCARD ALL
max_client_conn = 20
default_pool_size = 1
logfile = $D/pgbouncer.log
pidfile = $D/bouncer.pid
unix_socket_dir =
EOF
/usr/sbin/pgbouncer -d "$D/pgbouncer.ini" >/dev/null 2>&1
sleep 1.5
C="host=127.0.0.1 port=$PORT dbname=laswell_spike user=daniel"

echo "### pool_mode=transaction, default_pool_size=1, server_reset_query=DISCARD ALL"
echo
echo "(1) does a SESSION GUC survive a transaction boundary on ONE client?"
$PSQL "$C" <<'SQL' 2>&1 | grep -E 'set-|read-|ERROR|ERRO' | sed 's/^/    /'
BEGIN; SET my.probe = 'laswell'; SELECT 'set-ok'; COMMIT;
BEGIN; SELECT 'read-back:' || coalesce(current_setting('my.probe', true),'<DISCARDED>'); COMMIT;
SQL

echo
echo "(2) does that state LEAK to a DIFFERENT client connection?"
$PSQL "$C" -c "SELECT 'other-client-sees:' || coalesce(current_setting('my.probe', true),'<clean>');" 2>&1 | sed 's/^/    /'

echo
echo "(3) advisory lock taken by client A -- does client B see it held?"
$PSQL "$C" -c "SELECT 'A-took:' || pg_try_advisory_lock(555111)::text;" 2>&1 | sed 's/^/    /'
$PSQL "$C" -c "SELECT 'B-sees-advisory-locks:' || count(*)::text FROM pg_locks WHERE locktype='advisory' AND objid=555111;" 2>&1 | sed 's/^/    /'

echo
echo "(4) with TWO concurrent clients and pool_size=1, does the pid flip?"
D2=$(mktemp -d)
mkfifo "$D2/h_in"
$PSQL "$C" < "$D2/h_in" > "$D2/h_out" 2>&1 &
exec 3>"$D2/h_in"
echo "BEGIN;" >&3; sleep 0.3
echo "SELECT 'holder-pid:' || pg_backend_pid();" >&3; sleep 0.6
$PSQL "$C" -c "SELECT 'other-pid:' || pg_backend_pid();" 2>&1 | sed 's/^/    /' &
OTHERPID=$!
sleep 2
echo "COMMIT;" >&3; sleep 0.5
echo "\\q" >&3
exec 3>&-
wait $OTHERPID 2>/dev/null
grep -E 'holder-pid' "$D2/h_out" | sed 's/^/    /'
rm -rf "$D2"

echo
echo "### pgbouncer log (reset query activity)"
grep -iE 'reset|discard' "$D/pgbouncer.log" | tail -5 | sed 's/^/    /' || echo "    (no reset/discard lines)"
