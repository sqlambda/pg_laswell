#!/usr/bin/env bash
# S11b — what does the transitive blocking-chain query cost, relative to the
#        cheap direct one? Decides whether the observer can run it every tick
#        or must escalate to it.
set -uo pipefail
CONN="port=5555 dbname=laswell_spike"
export PSQLRC=/dev/null
PSQL="psql -X -q -A -t"
WPID=${WPID:-0}

DIRECT="SELECT count(*) FROM pg_locks l
         WHERE NOT l.granted AND ${WPID} = ANY(pg_blocking_pids(l.pid));"

CHAIN="WITH RECURSIVE blocked(pid, depth, path) AS (
    SELECT ${WPID}::int, 0, ARRAY[${WPID}::int]
  UNION ALL
    SELECT l.pid, b.depth+1, b.path || l.pid
      FROM blocked b
      JOIN pg_locks l ON NOT l.granted AND b.pid = ANY(pg_blocking_pids(l.pid))
     WHERE NOT l.pid = ANY(b.path) AND b.depth < 16)
 SELECT count(DISTINCT pid) FROM blocked WHERE depth > 0;"

bench() {                       # $1 = label, $2 = sql
  local t0 t1
  t0=$(date +%s%N)
  for _ in $(seq 20); do $PSQL "$CONN" -c "$2" >/dev/null; done
  t1=$(date +%s%N)
  printf '  %-28s %6.2f ms/call (incl. psql connect)\n' "$1" "$(echo "scale=2; ($t1-$t0)/20/1000000" | bc)"
}

echo "### query cost, quiet server (no waiters), 20 calls each"
bench "direct blockers" "$DIRECT"
bench "transitive chain" "$CHAIN"

echo
echo "### same, measured server-side with EXPLAIN ANALYZE (no client overhead)"
for LBL in DIRECT CHAIN; do
  SQL=$([ "$LBL" = DIRECT ] && echo "$DIRECT" || echo "$CHAIN")
  MS=$($PSQL "$CONN" -c "EXPLAIN (ANALYZE, TIMING ON, FORMAT JSON) $SQL" 2>/dev/null \
       | tr -d '\n' | grep -o '"Execution Time": *[0-9.]*' | grep -o '[0-9.]*$')
  printf '  %-28s %s ms\n' "$LBL" "${MS:-n/a}"
done

echo
echo "### how many backends exist right now (pg_locks scan size)"
$PSQL "$CONN" -c "SELECT 'pg_locks rows: ' || count(*) FROM pg_locks;"
$PSQL "$CONN" -c "SELECT 'backends: ' || count(*) FROM pg_stat_activity;"
