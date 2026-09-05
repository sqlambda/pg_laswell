#!/usr/bin/env bash
# S9 — should the OBSERVER connection import a snapshot / hold a long
# REPEATABLE READ transaction?  Checks, inside ONE long RR transaction:
#   (a) is pg_locks live?                       (shared memory, not MVCC)
#   (b) are CUMULATIVE statistics live?         (stats_fetch_consistency)
#   (c) what does it cost in held-back xmin?
set -uo pipefail
CONN="port=5555 dbname=laswell_spike"
export PSQLRC=/dev/null
PSQL="psql -X -q -A -t"
D=$(mktemp -d); trap 'rm -rf "$D"; kill %1 %2 2>/dev/null' EXIT

$PSQL "$CONN" <<'SQL'
DROP TABLE IF EXISTS s9;
CREATE TABLE s9(id int primary key, n int);
INSERT INTO s9 SELECT g,0 FROM generate_series(1,500) g;
SELECT pg_stat_force_next_flush();
SQL

mkfifo "$D/o_in" "$D/v_in"
$PSQL "$CONN" < "$D/o_in" > "$D/o_out" 2>&1 &
exec 3>"$D/o_in"
$PSQL "$CONN" < "$D/v_in" > "$D/v_out" 2>&1 &
exec 4>"$D/v_in"
o() { echo "$1" >&3; sleep 0.4; }
v() { echo "$1" >&4; sleep 0.4; }

echo "### Observer opens ONE long REPEATABLE READ transaction and never leaves it"
o "BEGIN TRANSACTION ISOLATION LEVEL REPEATABLE READ;"
o "SELECT 'obs-pid:' || pg_backend_pid();"
o "SELECT 'A locks-at-start:' || count(*) FROM pg_locks WHERE relation='s9'::regclass;"
o "SELECT 'B seqscans-at-start:' || coalesce(seq_scan,0)::text FROM pg_stat_user_tables WHERE relname='s9';"
sleep 0.5

echo "### World changes: 5 seq scans, then a victim takes AccessExclusiveLock"
for _ in 1 2 3 4 5; do $PSQL "$CONN" -c "SELECT count(*) FROM s9;" >/dev/null; done
$PSQL "$CONN" -c "SELECT pg_stat_force_next_flush();" >/dev/null
sleep 0.8
v "BEGIN;"
v "LOCK TABLE s9 IN ACCESS EXCLUSIVE MODE;"
sleep 1.0

echo "### The SAME observer transaction re-reads. Does it see the new reality?"
o "SELECT 'A locks-now:' || count(*) FROM pg_locks WHERE relation='s9'::regclass;"
o "SELECT 'B seqscans-now:' || coalesce(seq_scan,0)::text FROM pg_stat_user_tables WHERE relname='s9';"
o "SELECT 'C stats_fetch_consistency=' || current_setting('stats_fetch_consistency');"
o "SELECT 'D observer holds xmin: ' || coalesce(age(backend_xmin)::text,'NONE') FROM pg_stat_activity WHERE pid=pg_backend_pid();"
sleep 1.0

echo "### CONTRAST: a fresh short transaction, which is what the design uses"
$PSQL "$CONN" -c "SELECT 'A locks-fresh:' || count(*) FROM pg_locks WHERE relation='s9'::regclass;"
$PSQL "$CONN" -c "SELECT 'B seqscans-fresh:' || coalesce(seq_scan,0)::text FROM pg_stat_user_tables WHERE relname='s9';"

v "COMMIT;"; v "\\q"
o "COMMIT;"; o "\\q"
exec 3>&-; exec 4>&-; wait 2>/dev/null

echo
echo "===== observer transcript (one long REPEATABLE READ transaction) ====="
grep -E '^(obs-pid|A |B |C |D )|ERROR|ERRO' "$D/o_out"
