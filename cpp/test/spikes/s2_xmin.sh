#!/usr/bin/env bash
# S2 — cost of holding an imported snapshot: does it block vacuum?
#
# Claim under test: a session holding an imported (or any long-lived) snapshot
# holds back backend_xmin, so dead tuples produced after it started are NOT
# removable by VACUUM until it ends.
set -uo pipefail

CONN="port=5555 dbname=laswell_spike"
PSQL="psql -X -q -A -t"
export PSQLRC=/dev/null
D=$(mktemp -d)
trap 'rm -rf "$D"; kill %1 %2 2>/dev/null' EXIT

mkfifo "$D/a_in" "$D/b_in"
$PSQL "$CONN" < "$D/a_in" > "$D/a_out" 2>&1 &
exec 3>"$D/a_in"
$PSQL "$CONN" < "$D/b_in" > "$D/b_out" 2>&1 &
exec 4>"$D/b_in"
a() { echo "$1" >&3; sleep 0.4; }
b() { echo "$1" >&4; sleep 0.4; }

$PSQL "$CONN" <<'SQL'
DROP TABLE IF EXISTS s2;
CREATE TABLE s2(id int primary key, pad text);
INSERT INTO s2 SELECT g, repeat('x',100) FROM generate_series(1,50000) g;
SQL
echo "=== 50 000 rows seeded ==="

# A opens the exporting transaction and exports a snapshot.
a "BEGIN;"
a "SELECT 'TOKEN:' || pg_export_snapshot();"
sleep 0.6
TOKEN=$(grep -o 'TOKEN:.*' "$D/a_out" | tail -1 | cut -d: -f2-)
echo "token: $TOKEN"

# B imports it and just sits there -- the "pre-image observer".
b "BEGIN ISOLATION LEVEL REPEATABLE READ;"
b "SET TRANSACTION SNAPSHOT '${TOKEN}';"
b "SELECT 'B-holds:' || count(*) FROM s2;"
sleep 0.6

# Now generate dead tuples from a third, independent session.
$PSQL "$CONN" -c "DELETE FROM s2 WHERE id <= 25000;" >/dev/null

echo
echo "=== backend_xmin while the observer holds the snapshot ==="
$PSQL "$CONN" -c "SELECT pid || ' | xmin_age=' || coalesce(age(backend_xmin)::text,'-') || ' | ' || left(coalesce(query,''),40) FROM pg_stat_activity WHERE datname='laswell_spike' AND backend_xmin IS NOT NULL ORDER BY pid;"

echo
echo "=== VACUUM VERBOSE with the observer OPEN ==="
$PSQL "$CONN" -c "VACUUM (VERBOSE) s2;" 2>&1 | grep -Ei 'removable|removed|dead|frozen' | head -8

# Close the observer and the exporter.
b "COMMIT;"; b "\\q"
a "COMMIT;"; a "\\q"
exec 3>&-; exec 4>&-
wait 2>/dev/null
sleep 0.5

echo
echo "=== VACUUM VERBOSE with the observer CLOSED ==="
$PSQL "$CONN" -c "VACUUM (VERBOSE) s2;" 2>&1 | grep -Ei 'removable|removed|dead|frozen' | head -8
