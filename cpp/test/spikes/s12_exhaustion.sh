#!/usr/bin/env bash
# S12 — does the connection-exhaustion model hold, and is it measurable?
#
# Model under test:
#   headroom  = max_connections - reserved - current_backends
#   R         = arrival rate of queries that block on us (backends/sec)
#   T_exhaust = headroom / R          -- seconds until nothing can connect
#
# A migration should continue only while its remaining work fits inside
# min(T_app, T_exhaust) with margin. This spike measures R and verifies that
# each blocked query really does pin one connection.
#
# DELIBERATELY BOUNDED: spawns at most MAXW writers, and refuses to run if
# that would consume more than a quarter of available headroom.
set -uo pipefail
CONN="port=5555 dbname=laswell_spike"
export PSQLRC=/dev/null
PSQL="psql -X -q -A -t"
MAXW=${MAXW:-16}
ARRIVAL_MS=${ARRIVAL_MS:-200}          # one new blocked writer every 200ms => R = 5/s
D=$(mktemp -d)
cleanup(){ [ -e "$D/w_in" ] && echo "\\q" > "$D/w_in" 2>/dev/null; sleep 0.3
           pkill -P $$ 2>/dev/null; rm -rf "$D"; }
trap cleanup EXIT

read -r MAXC RESV CUR <<<"$($PSQL "$CONN" -c "
  SELECT (SELECT setting::int FROM pg_settings WHERE name='max_connections') || ' ' ||
         ((SELECT setting::int FROM pg_settings WHERE name='superuser_reserved_connections')
         + (SELECT setting::int FROM pg_settings WHERE name='reserved_connections')) || ' ' ||
         (SELECT count(*) FROM pg_stat_activity);")"
HEADROOM=$(( MAXC - RESV - CUR ))
echo "max_connections=$MAXC  reserved=$RESV  in_use=$CUR  => headroom=$HEADROOM"
if [ $(( MAXW * 4 )) -gt "$HEADROOM" ]; then
  echo "REFUSING: $MAXW writers is more than a quarter of headroom ($HEADROOM)."; exit 1
fi
echo "spawning at most $MAXW blocked writers, one every ${ARRIVAL_MS}ms (R = $(echo "scale=1; 1000/$ARRIVAL_MS" | bc)/s)"
echo

$PSQL "$CONN" <<'SQL'
DROP TABLE IF EXISTS s12;
CREATE TABLE s12(id int primary key, n int);
INSERT INTO s12 SELECT g,0 FROM generate_series(1,100) g;
SQL

# --- the "migration": holds AccessExclusiveLock so every writer blocks -------
mkfifo "$D/w_in"
$PSQL "$CONN" < "$D/w_in" > "$D/w_out" 2>&1 &
exec 3>"$D/w_in"
echo "BEGIN;"                                    >&3
echo "LOCK TABLE s12 IN ACCESS EXCLUSIVE MODE;"  >&3
echo "SELECT 'WPID:' || pg_backend_pid();"       >&3
sleep 1.2
WPID=$(grep -o 'WPID:.*' "$D/w_out" | tail -1 | cut -d: -f2)
echo "migration pid = $WPID (holds AccessExclusiveLock)"
BASE=$($PSQL "$CONN" -c "SELECT count(*) FROM pg_stat_activity;")
echo "baseline backends = $BASE"
echo
printf '%6s %10s %10s %12s %14s\n' "t(s)" "backends" "waiters" "blocked_s" "T_exhaust(s)"

T0=$(date +%s%N)
for i in $(seq 1 $MAXW); do
  ( $PSQL "$CONN" -c "UPDATE s12 SET n=n+1 WHERE id=1;" >/dev/null 2>&1 ) &
  sleep "$(echo "scale=3; $ARRIVAL_MS/1000" | bc)"

  NOW=$(date +%s%N); ELAPSED=$(echo "scale=2; ($NOW-$T0)/1000000000" | bc)
  read -r NB NW BS <<<"$($PSQL "$CONN" -c "
    SELECT (SELECT count(*) FROM pg_stat_activity) || ' ' ||
           (SELECT count(*) FROM pg_locks l WHERE NOT l.granted AND l.relation='s12'::regclass) || ' ' ||
           (SELECT coalesce(round(sum(extract(epoch FROM now()-l.waitstart))::numeric,1),0)
              FROM pg_locks l WHERE NOT l.granted AND l.relation='s12'::regclass);")"
  GROWTH=$(( NB - BASE ))
  if [ "$GROWTH" -gt 0 ] && [ "$(echo "$ELAPSED > 0" | bc)" = 1 ]; then
    R=$(echo "scale=3; $GROWTH/$ELAPSED" | bc)
    HR=$(( MAXC - RESV - NB ))
    TE=$(echo "scale=1; $HR/$R" | bc 2>/dev/null || echo "-")
  else TE="-"; fi
  printf '%6s %10s %10s %12s %14s\n' "$ELAPSED" "$NB" "$NW" "$BS" "$TE"
done

echo
echo "=== does each blocked query pin exactly one connection? ==="
$PSQL "$CONN" -c "
SELECT 'blocked_waiters=' || (SELECT count(*) FROM pg_locks l
                               WHERE NOT l.granted AND l.relation='s12'::regclass)
    || '  backends_above_baseline=' || ((SELECT count(*) FROM pg_stat_activity) - $BASE);"

echo "=== releasing the lock; the queue should drain ==="
echo "COMMIT;" >&3
sleep 2.5
$PSQL "$CONN" -c "SELECT 'waiters_after_release=' || count(*) FROM pg_locks l
                   WHERE NOT l.granted AND l.relation='s12'::regclass;"
$PSQL "$CONN" -c "SELECT 'backends_after_release=' || count(*) FROM pg_stat_activity;"
