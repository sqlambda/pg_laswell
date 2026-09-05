#!/usr/bin/env bash
# S1 — snapshot export semantics.
#
# Claim under test: pg_export_snapshot() + SET TRANSACTION SNAPSHOT shares a
# SNAPSHOT (visibility), not transaction state. The importing session must NOT
# see the exporting transaction's uncommitted rows.
set -uo pipefail

CONN="port=5555 dbname=laswell_spike"
PSQL="psql -X -q -A -t"
export PSQLRC=/dev/null
D=$(mktemp -d)
trap 'rm -rf "$D"; kill %1 %2 2>/dev/null' EXIT

mkfifo "$D/a_in" "$D/b_in"

# Session A: the exporter. Holds a transaction open.
$PSQL "$CONN" < "$D/a_in" > "$D/a_out" 2>&1 &
exec 3>"$D/a_in"
# Session B: the importer.
$PSQL "$CONN" < "$D/b_in" > "$D/b_out" 2>&1 &
exec 4>"$D/b_in"

a() { echo "$1" >&3; sleep 0.4; }
b() { echo "$1" >&4; sleep 0.4; }

a "DROP TABLE IF EXISTS s1;"
a "CREATE TABLE s1(id int primary key, tag text);"
a "INSERT INTO s1 VALUES (1,'committed-before-export');"

echo "=== setup: 1 committed row ==="

a "BEGIN;"
a "INSERT INTO s1 VALUES (2,'UNCOMMITTED-by-A');"
a "SELECT 'A-sees:' || count(*) FROM s1;"
a "SELECT 'TOKEN:' || pg_export_snapshot();"
sleep 0.6

TOKEN=$(grep -o 'TOKEN:.*' "$D/a_out" | tail -1 | cut -d: -f2-)
echo "exported snapshot token: ${TOKEN}"
[ -z "$TOKEN" ] && { echo "FAILED to get token"; cat "$D/a_out"; exit 1; }

b "BEGIN ISOLATION LEVEL REPEATABLE READ;"
b "SET TRANSACTION SNAPSHOT '${TOKEN}';"
b "SELECT 'B-sees-while-A-open:' || count(*) FROM s1;"
b "SELECT 'B-sees-tags:' || coalesce(string_agg(tag,','),'<none>') FROM s1;"
sleep 0.6

echo "--- A commits ---"
a "COMMIT;"
sleep 0.6

b "SELECT 'B-sees-after-A-commit:' || count(*) FROM s1;"
sleep 0.6
b "COMMIT;"
b "SELECT 'B-sees-after-own-commit:' || count(*) FROM s1;"
sleep 0.6

a "\\q"
b "\\q"
exec 3>&-
exec 4>&-
wait 2>/dev/null

echo
echo "===== session A output ====="
grep -E 'A-sees|TOKEN|ERROR' "$D/a_out"
echo "===== session B output ====="
grep -E 'B-sees|ERROR' "$D/b_out"
