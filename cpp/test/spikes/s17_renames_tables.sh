#!/usr/bin/env bash
#
# S17 -- renames, and dropping a table that things depend on.
#
# The interesting question for a rename is what FOLLOWS it. PostgreSQL stores
# dependencies by OID, so the catalog should repair itself -- but "should" is
# not a measurement, and the answer decides whether rename_column needs the
# whole S13 view-rebuild machinery or none of it.
set -uo pipefail
CONN="port=5555 dbname=laswell_spike17"
export PSQLRC=/dev/null
PSQL() { psql -X -q -v ON_ERROR_STOP=1 "$CONN" "$@"; }
RAW()  { psql -X -q "$CONN" "$@"; }

dropdb -p 5555 --if-exists laswell_spike17
createdb -p 5555 laswell_spike17
PSQL <<'SQL'
CREATE TABLE parent(code text PRIMARY KEY);
CREATE TABLE t(id bigint PRIMARY KEY, code text REFERENCES parent(code),
               amount int CHECK (amount > 0));
INSERT INTO parent VALUES ('a'),('b');
INSERT INTO t SELECT g, 'a', g FROM generate_series(1,1000) g;
CREATE INDEX t_amount ON t(amount);
CREATE VIEW v1 AS SELECT id, amount FROM t;
CREATE VIEW v2 AS SELECT id, amount * 2 AS doubled FROM v1;
CREATE MATERIALIZED VIEW mv1 AS SELECT id, amount FROM t;
SQL

echo "=== 1. rename a COLUMN that views read: does anything break? ==="
RAW -c "ALTER TABLE t RENAME COLUMN amount TO value" 2>&1 | head -2
echo "--- what do the views say now? ---"
PSQL -c "SELECT pg_get_viewdef('v1'::regclass, true) AS v1_after_rename"
PSQL -c "SELECT count(*) AS v2_still_queryable FROM v2"
PSQL -c "SELECT count(*) AS mv1_still_there FROM mv1"

echo
echo "=== 2. and the index, the check constraint, the FK? ==="
PSQL -c "SELECT indexdef FROM pg_indexes WHERE tablename='t' AND indexname='t_amount'"
PSQL -c "SELECT conname, pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid='t'::regclass ORDER BY conname"

echo
echo "=== 3. locks taken by a column rename ==="
PSQL <<'SQL'
BEGIN;
ALTER TABLE t RENAME COLUMN value TO amount;
SELECT c.relname, l.mode FROM pg_locks l JOIN pg_class c ON c.oid=l.relation
 WHERE l.pid=pg_backend_pid() AND c.relname IN ('t','v1','v2','mv1') ORDER BY 1,2;
COMMIT;
SQL

echo
echo "=== 4. rename a TABLE that a view reads ==="
RAW -c "ALTER TABLE t RENAME TO t_renamed" 2>&1 | head -2
PSQL -c "SELECT pg_get_viewdef('v1'::regclass, true) AS v1_after_table_rename"
PSQL -c "SELECT count(*) AS v2_still_works FROM v2"
echo "--- did the indexes and constraints follow? ---"
PSQL -c "SELECT count(*) AS indexes_on_renamed FROM pg_indexes WHERE tablename='t_renamed'"
PSQL -c "ALTER TABLE t_renamed RENAME TO t"

echo
echo "=== 5. renaming a constraint, and its index ==="
PSQL -c "ALTER TABLE t RENAME CONSTRAINT t_amount_check TO t_amount_positive"
PSQL -c "SELECT conname FROM pg_constraint WHERE conrelid='t'::regclass AND contype='c'"
echo "--- does renaming a unique CONSTRAINT rename its index too? ---"
PSQL -c "ALTER TABLE t ADD CONSTRAINT t_code_uq UNIQUE (code)"
PSQL -c "ALTER TABLE t RENAME CONSTRAINT t_code_uq TO t_code_unique"
PSQL -c "SELECT con.conname, con.conindid::regclass AS index_name FROM pg_constraint con WHERE con.conname='t_code_unique'"

echo
echo "=== 6. DROP TABLE with views and an FK pointing at it ==="
RAW -c "DROP TABLE parent" 2>&1 | head -4
RAW -c "DROP TABLE t" 2>&1 | head -4

dropdb -p 5555 laswell_spike17
echo "spike database dropped."
