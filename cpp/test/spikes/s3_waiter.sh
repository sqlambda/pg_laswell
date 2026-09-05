#!/usr/bin/env bash
# S3 — lock-waiter detection latency, and whether pg_locks.waitstart is usable.
#
# Claim under test: the observer's "is anyone blocked by me" query detects a
# waiter well within commit_interval_ms (default 1000ms). If detection costs
# seconds, the lock-waiter trigger buys nothing over time-based commit.
set -uo pipefail

CONN="port=5555 dbname=laswell_spike"
PSQL="psql -X -q -A -t"
export PSQLRC=/dev/null
D=$(mktemp -d)
trap 'rm -rf "$D"; kill %1 %2 2>/dev/null' EXIT

$PSQL "$CONN" <<'SQL'
DROP TABLE IF EXISTS s3;
CREATE TABLE s3(id int primary key, n int);
INSERT INTO s3 SELECT g, 0 FROM generate_series(1,1000) g;
SQL

mkfifo "$D/w_in" "$D/v_in"
$PSQL "$CONN" < "$D/w_in" > "$D/w_out" 2>&1 &
exec 3>"$D/w_in"
$PSQL "$CONN" < "$D/v_in" > "$D/v_out" 2>&1 &
exec 4>"$D/v_in"
w() { echo "$1" >&3; }
v() { echo "$1" >&4; }

# Worker: open a transaction holding RowExclusiveLock on s3.
w "BEGIN;"
w "UPDATE s3 SET n = n + 1 WHERE id <= 10;"
w "SELECT 'WPID:' || pg_backend_pid();"
sleep 1
WPID=$(grep -o 'WPID:.*' "$D/w_out" | tail -1 | cut -d: -f2-)
echo "worker pid: $WPID"

# The observer query from the plan: who is waiting on a lock THIS pid holds.
OBS="SELECT coalesce(jsonb_build_object(
        'waiter_count', count(*),
        'waiters', jsonb_agg(l.pid),
        'oldest_wait_s', round(extract(epoch FROM now() - min(l.waitstart))::numeric, 4),
        'waitstart_null', bool_or(l.waitstart IS NULL)
      )::text, '{}')
     FROM pg_locks AS l
    WHERE NOT l.granted AND ${WPID} = ANY(pg_blocking_pids(l.pid));"

echo "--- baseline (no waiter) ---"
$PSQL "$CONN" -c "$OBS"

# Time how long a single observer poll takes (its own cost matters: it runs
# every observer_tick_ms).
echo "--- observer query cost, 10 runs, no waiter ---"
S=$(date +%s%N)
for _ in $(seq 10); do $PSQL "$CONN" -c "$OBS" >/dev/null; done
E=$(date +%s%N)
echo "  ~$(( (E-S)/10000000 ))ms per poll (includes psql connect; in-process is far less)"

# Now create the waiter and measure detection latency.
echo "--- issuing LOCK TABLE ... IN SHARE MODE from the victim ---"
v "BEGIN;"
sleep 0.3
START=$(date +%s%N)
v "LOCK TABLE s3 IN SHARE MODE;"

DETECTED=""
for i in $(seq 1 200); do
  OUT=$($PSQL "$CONN" -c "$OBS" 2>/dev/null)
  if echo "$OUT" | grep -q '"waiter_count": *[1-9]'; then
    NOW=$(date +%s%N)
    DETECTED=$(( (NOW-START)/1000000 ))
    echo "  DETECTED after ${DETECTED}ms (poll #$i)"
    echo "  $OUT"
    break
  fi
  sleep 0.02
done
[ -z "$DETECTED" ] && echo "  NOT DETECTED within the polling window"

echo "--- worker commits; waiter should clear ---"
w "COMMIT;"
sleep 0.8
$PSQL "$CONN" -c "$OBS"

w "\\q"; v "\\q"
exec 3>&-; exec 4>&-
wait 2>/dev/null
