#!/usr/bin/env bash
# S10 — is pg_export_snapshot() even NEEDED for a pre-image reader?
#
# Claims under test:
#  (a) a plain BEGIN ISOLATION LEVEL REPEATABLE READ opened before the worker
#      starts already sees the pre-migration state -- no export/import needed.
#  (b) GOTCHA: in REPEATABLE READ the snapshot is taken at the FIRST STATEMENT,
#      not at BEGIN. A transaction that only issued BEGIN is not yet pinned.
#  (c) does the importer survive the EXPORTER committing? (the paced backfill
#      commits every ~1s, so the exporting transaction is short-lived)
set -uo pipefail
CONN="port=5555 dbname=laswell_spike"
export PSQLRC=/dev/null
PSQL="psql -X -q -A -t"
D=$(mktemp -d); trap 'rm -rf "$D"; kill %1 %2 %3 2>/dev/null' EXIT

$PSQL "$CONN" <<'SQL'
DROP TABLE IF EXISTS s10;
CREATE TABLE s10(id int primary key, amount numeric);
INSERT INTO s10 SELECT g, 100 FROM generate_series(1,10) g;   -- sum = 1000
SQL
echo "seeded: 10 rows, amount=100 each, sum=1000"

mkfifo "$D/p_in" "$D/q_in" "$D/w_in"
$PSQL "$CONN" < "$D/p_in" > "$D/p_out" 2>&1 &   # P: pinned (issues a query)
exec 3>"$D/p_in"
$PSQL "$CONN" < "$D/q_in" > "$D/q_out" 2>&1 &   # Q: BEGIN only, NOT pinned
exec 4>"$D/q_in"
$PSQL "$CONN" < "$D/w_in" > "$D/w_out" 2>&1 &   # W: the worker
exec 5>"$D/w_in"
p() { echo "$1" >&3; sleep 0.35; }
q() { echo "$1" >&4; sleep 0.35; }
w() { echo "$1" >&5; sleep 0.35; }

echo
echo "### P: BEGIN REPEATABLE READ *and issues a statement* (pins the snapshot)"
p "BEGIN TRANSACTION ISOLATION LEVEL REPEATABLE READ;"
p "SELECT 'P pinned-at:' || sum(amount) FROM s10;"

echo "### Q: BEGIN REPEATABLE READ only, NO statement yet (NOT pinned)"
q "BEGIN TRANSACTION ISOLATION LEVEL REPEATABLE READ;"
sleep 0.5

echo "### W: the worker mutates in SHORT transactions, exactly like a paced backfill"
w "BEGIN;"
w "SELECT 'W TOKEN:' || pg_export_snapshot();"
sleep 0.5
TOKEN=$(grep -o 'W TOKEN:.*' "$D/w_out" | tail -1 | sed 's/^W TOKEN://')
echo "    worker exported: $TOKEN"

# A third reader imports the worker's snapshot, then the worker COMMITS.
$PSQL "$CONN" > "$D/r_out" 2>&1 <<SQL &
BEGIN TRANSACTION ISOLATION LEVEL REPEATABLE READ;
SET TRANSACTION SNAPSHOT '${TOKEN}';
SELECT 'R imported-sees:' || sum(amount) FROM s10;
SELECT pg_sleep(3);
SELECT 'R after-exporter-committed-sees:' || sum(amount) FROM s10;
COMMIT;
SQL
sleep 0.8

w "UPDATE s10 SET amount = amount * 2;"
w "COMMIT;"
w "BEGIN;"
w "UPDATE s10 SET amount = amount + 5;"
w "COMMIT;"
sleep 0.5
echo "    worker done: sum should now be 10*(100*2+5) = 2050"
$PSQL "$CONN" -c "SELECT 'ACTUAL now:' || sum(amount) FROM s10;"

echo
echo "### Now ask P and Q what they see"
p "SELECT 'P sees:' || sum(amount) FROM s10;"
q "SELECT 'Q sees:' || sum(amount) FROM s10;"
sleep 0.5

p "COMMIT;"; p "\\q"
q "COMMIT;"; q "\\q"
w "\\q"
exec 3>&-; exec 4>&-; exec 5>&-
wait 2>/dev/null

echo
echo "===== RESULTS ====="
grep -E 'P (pinned-at|sees)' "$D/p_out"
grep -E 'Q sees'             "$D/q_out"
grep -E 'R '                 "$D/r_out"
