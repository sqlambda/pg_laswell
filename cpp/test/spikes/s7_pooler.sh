#!/usr/bin/env bash
# S7 — does a transaction-mode pooler flip pg_backend_pid() between
# transactions, and does a session advisory lock survive?
#
# This is the detection the executor uses to refuse to run behind a pooler.
set -uo pipefail
export PSQLRC=/dev/null
PSQL="psql -X -q -A -t"
D=$(mktemp -d); trap 'kill $(cat "$D/bouncer.pid" 2>/dev/null) 2>/dev/null; rm -rf "$D"' EXIT
PORT=6543

cat > "$D/users.txt" <<EOF
"daniel" ""
EOF

write_ini() {
cat > "$D/pgbouncer.ini" <<EOF
[databases]
laswell_spike = host=/var/run/postgresql port=5555 dbname=laswell_spike

[pgbouncer]
listen_addr = 127.0.0.1
listen_port = $PORT
auth_type = trust
auth_file = $D/users.txt
pool_mode = $1
server_reset_query = DISCARD ALL
max_client_conn = 20
default_pool_size = 5
logfile = $D/pgbouncer.log
pidfile = $D/bouncer.pid
unix_socket_dir =
EOF
}

probe() {
  local MODE=$1
  write_ini "$MODE"
  /usr/sbin/pgbouncer -d "$D/pgbouncer.ini" >/dev/null 2>&1
  sleep 1.5
  local C="host=127.0.0.1 port=$PORT dbname=laswell_spike user=daniel"

  echo "--- pool_mode = $MODE ---"
  # Two SEPARATE transactions on ONE client connection.
  local OUT
  OUT=$($PSQL "$C" <<'SQL' 2>&1
BEGIN; SELECT 'txn1-pid:' || pg_backend_pid(); COMMIT;
BEGIN; SELECT 'txn2-pid:' || pg_backend_pid(); COMMIT;
SQL
)
  echo "$OUT" | grep -E 'txn[12]-pid|ERROR|ERRO' | sed 's/^/    /'
  local P1 P2
  P1=$(echo "$OUT" | grep -o 'txn1-pid:.*' | cut -d: -f2)
  P2=$(echo "$OUT" | grep -o 'txn2-pid:.*' | cut -d: -f2)
  if [ -n "$P1" ] && [ -n "$P2" ]; then
    [ "$P1" = "$P2" ] && echo "    => backend pid STABLE ($P1)" \
                      || echo "    => backend pid CHANGED ($P1 -> $P2)  <-- detectable"
  fi

  # Does a SESSION advisory lock survive a transaction boundary?
  OUT=$($PSQL "$C" <<'SQL' 2>&1
BEGIN; SELECT 'took:' || pg_try_advisory_lock(987654321)::text; COMMIT;
BEGIN; SELECT 'still-held:' || (count(*) > 0)::text FROM pg_locks
        WHERE locktype='advisory' AND objid=987654321 AND pid=pg_backend_pid(); COMMIT;
SQL
)
  echo "$OUT" | grep -E 'took|still-held|ERROR|ERRO' | sed 's/^/    /'

  kill "$(cat "$D/bouncer.pid")" 2>/dev/null; sleep 0.8
}

echo "############ S7 — pooler detection ############"
probe session
echo
probe transaction
echo
echo "--- direct connection, for contrast ---"
$PSQL "port=5555 dbname=laswell_spike" <<'SQL' 2>&1 | sed 's/^/    /'
BEGIN; SELECT 'txn1-pid:' || pg_backend_pid(); COMMIT;
BEGIN; SELECT 'txn2-pid:' || pg_backend_pid(); COMMIT;
BEGIN; SELECT 'took:' || pg_try_advisory_lock(987654321)::text; COMMIT;
BEGIN; SELECT 'still-held:' || (count(*) > 0)::text FROM pg_locks
        WHERE locktype='advisory' AND objid=987654321 AND pid=pg_backend_pid(); COMMIT;
SQL
