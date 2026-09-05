#!/usr/bin/env bash
# S4 — CREATE INDEX CONCURRENTLY on a partitioned table, PG 18.6
# S5 — CIC failure leaves an INVALID index; DROP INDEX CONCURRENTLY cleans it
# S8 — ADD COLUMN ... DEFAULT <non-volatile> is metadata-only (relfilenode)
set -uo pipefail
CONN="port=5555 dbname=laswell_spike"
export PSQLRC=/dev/null
P() { psql -X -q -A -t "$CONN" "$@"; }

echo "############ S4 — CIC on a partitioned table ############"
P <<'SQL' 2>&1
DROP TABLE IF EXISTS s4 CASCADE;
CREATE TABLE s4(id bigint, region text, created_at timestamptz) PARTITION BY RANGE (created_at);
CREATE TABLE s4_a PARTITION OF s4 FOR VALUES FROM ('2020-01-01') TO ('2025-01-01');
CREATE TABLE s4_b PARTITION OF s4 FOR VALUES FROM ('2025-01-01') TO ('2030-01-01');
INSERT INTO s4 SELECT g, 'r'||(g%5), '2024-06-01'::timestamptz + (g||' min')::interval
  FROM generate_series(1,2000) g;
SQL
echo "-- attempt: CREATE INDEX CONCURRENTLY on the partitioned parent"
P -c "CREATE INDEX CONCURRENTLY s4_region_idx ON s4 (region);" 2>&1 | head -4
echo "-- resulting index state (parent + partitions):"
P -c "SELECT c.relname || ' | valid=' || i.indisvalid || ' | ready=' || i.indisready
        FROM pg_index i JOIN pg_class c ON c.oid=i.indexrelid
       WHERE c.relname LIKE 's4%idx%' ORDER BY 1;"
echo "-- for contrast: plain CREATE INDEX on the parent"
P -c "CREATE INDEX s4_created_idx ON s4 (created_at);" 2>&1 | head -3
P -c "SELECT c.relname || ' | valid=' || i.indisvalid
        FROM pg_index i JOIN pg_class c ON c.oid=i.indexrelid
       WHERE c.relname LIKE 's4%created%' ORDER BY 1;"

echo
echo "############ S5 — CIC failure leaves an INVALID index ############"
P <<'SQL' 2>&1
DROP TABLE IF EXISTS s5;
CREATE TABLE s5(id int primary key, dup int);
INSERT INTO s5 SELECT g, g % 100 FROM generate_series(1,5000) g;   -- dup has duplicates
SQL
echo "-- attempt: CREATE UNIQUE INDEX CONCURRENTLY on a column with duplicates"
P -c "CREATE UNIQUE INDEX CONCURRENTLY s5_dup_idx ON s5 (dup);" 2>&1 | head -4
echo "-- does the index survive, and is it valid?"
P -c "SELECT c.relname || ' | indisvalid=' || i.indisvalid || ' | indisready=' || i.indisready
        FROM pg_index i JOIN pg_class c ON c.oid=i.indexrelid WHERE c.relname='s5_dup_idx';"
echo "-- does it show up in pg_indexes (i.e. would a naive check miss it)?"
P -c "SELECT count(*)::text || ' row(s) in pg_indexes for s5_dup_idx' FROM pg_indexes WHERE indexname='s5_dup_idx';"
echo "-- DROP INDEX CONCURRENTLY on the invalid index:"
P -c "DROP INDEX CONCURRENTLY s5_dup_idx;" 2>&1 | head -3
P -c "SELECT count(*)::text || ' row(s) remain for s5_dup_idx' FROM pg_class WHERE relname='s5_dup_idx';"

echo
echo "############ S8 — ADD COLUMN with a non-volatile DEFAULT ############"
P <<'SQL' 2>&1
DROP TABLE IF EXISTS s8;
CREATE TABLE s8(id int primary key, pad text);
INSERT INTO s8 SELECT g, repeat('y',80) FROM generate_series(1,20000) g;
SQL
for CASE in "nullable_no_default:ADD COLUMN c1 text" \
            "stable_default:ADD COLUMN c2 text DEFAULT 'const'" \
            "notnull_stable_default:ADD COLUMN c3 int NOT NULL DEFAULT 42" \
            "volatile_default:ADD COLUMN c4 uuid DEFAULT gen_random_uuid()"; do
  NAME=${CASE%%:*}; DDL=${CASE#*:}
  BEFORE=$(P -c "SELECT relfilenode FROM pg_class WHERE relname='s8';")
  P -c "ALTER TABLE s8 $DDL;" >/dev/null 2>&1
  AFTER=$(P -c "SELECT relfilenode FROM pg_class WHERE relname='s8';")
  if [ "$BEFORE" = "$AFTER" ]; then VERDICT="METADATA-ONLY (relfilenode unchanged)"; else VERDICT="REWRITE (relfilenode $BEFORE -> $AFTER)"; fi
  printf '  %-24s %s\n' "$NAME" "$VERDICT"
done
