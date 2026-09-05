#!/usr/bin/env bash
# S11 — what does a lock pile-up behind a migration actually look like, and
#       which signal should a contention circuit breaker trigger on?
#
# Builds the classic outage shape from pg_licht's plan-schema-change prompt:
#   "A brief exclusive lock request waits behind a long transaction, and
#    everything arriving after it waits behind that -- which is how a
#    millisecond operation becomes an outage."
#
#   W  holds RowExclusiveLock (an ordinary UPDATE, still in transaction)
#   V1 requests AccessExclusiveLock            -> blocks on W
#   V2..VN request RowExclusiveLock            -> queue behind V1
#
# Questions:
#  (a) does the cheap observer query -- "who has W among its direct blockers"
#      -- see the whole pile-up, or only V1?
#  (b) does the recursive chain (pg_licht locks(pid), chain_depth) see more?
#  (c) what does inflicted blocked-time look like as a signal?
set -uo pipefail
CONN="port=5555 dbname=laswell_spike"
export PSQLRC=/dev/null
PSQL="psql -X -q -A -t"
NV=${NV:-6}                       # number of second-order waiters
D=$(mktemp -d)
cleanup() { for f in "$D"/*_in; do [ -e "$f" ] && echo "\\q" > "$f" 2>/dev/null; done
            sleep 0.5; pkill -P $$ 2>/dev/null; rm -rf "$D"; }
trap cleanup EXIT

$PSQL "$CONN" <<'SQL'
DROP TABLE IF EXISTS s11;
CREATE TABLE s11(id int primary key, n int);
INSERT INTO s11 SELECT g,0 FROM generate_series(1,100) g;
SQL

open_session() {                  # $1 = tag
  mkfifo "$D/$1_in"
  $PSQL "$CONN" < "$D/$1_in" > "$D/$1_out" 2>&1 &
  exec {fd}>"$D/$1_in"
  eval "FD_$1=$fd"
}
say() { local tag=$1; shift; local v="FD_$tag"; echo "$*" >&${!v}; }

# --- W: the "migration" worker, holding an ordinary write lock -------------
open_session w
say w "BEGIN;"
say w "UPDATE s11 SET n=n+1 WHERE id<=10;"
say w "SELECT 'WPID:' || pg_backend_pid();"
sleep 1.2
WPID=$(grep -o 'WPID:.*' "$D/w_out" | tail -1 | cut -d: -f2)
echo "worker pid = $WPID  (holds RowExclusiveLock on s11)"

# --- V1: the exclusive request that everything else queues behind ----------
open_session v1
say v1 "BEGIN;"
say v1 "LOCK TABLE s11 IN ACCESS EXCLUSIVE MODE;"
sleep 1.0
echo "v1 requested AccessExclusiveLock (blocked on worker)"

# --- V2..VN: ordinary writers, queued behind V1 ----------------------------
for i in $(seq 2 $NV); do
  open_session "v$i"
  say "v$i" "BEGIN;"
  say "v$i" "UPDATE s11 SET n=n+1 WHERE id=50;"
done
sleep 2.0
echo "v2..v$NV issued ordinary UPDATEs (queued behind v1)"
echo

# --- (a) the cheap observer query the executor runs every tick -------------
echo "=== (a) DIRECT: backends with the worker among their blockers ==="
$PSQL "$CONN" -c "
SELECT jsonb_pretty(jsonb_build_object(
   'waiter_count', count(*),
   'waiters', jsonb_agg(l.pid ORDER BY l.pid),
   'oldest_wait_s', round(extract(epoch FROM now()-min(l.waitstart))::numeric,2),
   'inflicted_blocked_s', round(sum(extract(epoch FROM now()-l.waitstart))::numeric,2)))
  FROM pg_locks l
 WHERE NOT l.granted AND ${WPID} = ANY(pg_blocking_pids(l.pid));"

# --- (b) the transitive chain, pg_licht locks(pid) style --------------------
echo "=== (b) TRANSITIVE: everything reachable in the blocking graph ==="
$PSQL "$CONN" -c "
WITH RECURSIVE blocked(pid, depth, path) AS (
    SELECT ${WPID}::int, 0, ARRAY[${WPID}::int]
  UNION ALL
    SELECT l.pid, b.depth+1, b.path || l.pid
      FROM blocked b
      JOIN pg_locks l ON NOT l.granted AND b.pid = ANY(pg_blocking_pids(l.pid))
     WHERE NOT l.pid = ANY(b.path) AND b.depth < 16
)
SELECT jsonb_pretty(jsonb_build_object(
   'total_blocked_backends', count(DISTINCT pid) FILTER (WHERE depth>0),
   'max_chain_depth', max(depth),
   'by_depth', jsonb_object_agg(depth::text, cnt)))
  FROM (SELECT depth, pid, count(*) OVER (PARTITION BY depth) cnt
          FROM blocked WHERE depth>0) s;"

# --- (c) total blocked-time inflicted, all waiters transitively ------------
echo "=== (c) ALL ungranted lock waits on this table, whatever the cause ==="
$PSQL "$CONN" -c "
SELECT jsonb_pretty(jsonb_build_object(
   'ungranted_waits', count(*),
   'total_blocked_s', round(sum(extract(epoch FROM now()-l.waitstart))::numeric,2),
   'modes', jsonb_agg(DISTINCT l.mode)))
  FROM pg_locks l
 WHERE NOT l.granted AND l.relation='s11'::regclass;"

echo "=== worker commits; the queue should drain ==="
say w "COMMIT;"
sleep 2.0
$PSQL "$CONN" -c "SELECT 'remaining ungranted waits: ' || count(*) FROM pg_locks l
                   WHERE NOT l.granted AND l.relation='s11'::regclass;"
