#!/usr/bin/env bash
#
# S15 -- adding a UNIQUE constraint or a PRIMARY KEY without a long lock.
#
# The claimed recipe is CREATE UNIQUE INDEX CONCURRENTLY, then ALTER TABLE ...
# ADD CONSTRAINT ... USING INDEX, on the grounds that the second step is
# catalog-only. This measures whether that is true, and what else it does that
# nobody mentions.
set -uo pipefail
CONN="port=5555 dbname=laswell_spike15"
export PSQLRC=/dev/null
PSQL() { psql -X -q -v ON_ERROR_STOP=1 "$CONN" "$@"; }
RAW()  { psql -X -q "$CONN" "$@"; }

dropdb -p 5555 --if-exists laswell_spike15
createdb -p 5555 laswell_spike15
PSQL <<'SQL'
CREATE TABLE t(id bigint, code text, amount int);
INSERT INTO t SELECT g, 'c'||g, g FROM generate_series(1,200000) g;
ANALYZE t;
CREATE TABLE dup(id bigint, code text);
INSERT INTO dup SELECT g, 'same' FROM generate_series(1,100) g;
SQL

echo "=== 1. locks taken by a PLAIN ADD CONSTRAINT ... UNIQUE ==="
PSQL <<'SQL'
BEGIN;
ALTER TABLE t ADD CONSTRAINT t_code_uq UNIQUE (code);
SELECT c.relname, l.mode FROM pg_locks l JOIN pg_class c ON c.oid = l.relation
 WHERE l.pid = pg_backend_pid() AND c.relname LIKE 't%' ORDER BY 1,2;
ROLLBACK;
SQL

echo
echo "=== 2. the two-step recipe: locks of each step ==="
PSQL -c "CREATE UNIQUE INDEX CONCURRENTLY t_code_idx ON t(code)"
echo "--- CIC done. Now the ADD CONSTRAINT ... USING INDEX: ---"
PSQL <<'SQL'
BEGIN;
ALTER TABLE t ADD CONSTRAINT t_code_uq UNIQUE USING INDEX t_code_idx;
SELECT c.relname, l.mode FROM pg_locks l JOIN pg_class c ON c.oid = l.relation
 WHERE l.pid = pg_backend_pid() AND c.relname LIKE 't%' ORDER BY 1,2;
COMMIT;
SQL

echo
echo "=== 3. what happened to the index name? ==="
PSQL -c "SELECT c.relname AS index_name, con.conname AS constraint_name
           FROM pg_constraint con JOIN pg_class c ON c.oid = con.conindid
          WHERE con.conrelid = 't'::regclass"

echo
echo "=== 4. PRIMARY KEY the same way -- does it need NOT NULL first? ==="
PSQL -c "CREATE UNIQUE INDEX CONCURRENTLY t_id_idx ON t(id)"
echo "--- attnotnull on id before: ---"
PSQL -c "SELECT attnotnull FROM pg_attribute WHERE attrelid='t'::regclass AND attname='id'"
RAW -c "ALTER TABLE t ADD CONSTRAINT t_pkey PRIMARY KEY USING INDEX t_id_idx" 2>&1 | head -3
echo "--- attnotnull on id after: ---"
PSQL -c "SELECT attnotnull FROM pg_attribute WHERE attrelid='t'::regclass AND attname='id'"

echo
echo "=== 5. is there a NOT VALID form for UNIQUE? ==="
RAW -c "ALTER TABLE t ADD CONSTRAINT t_amount_uq UNIQUE (amount) NOT VALID" 2>&1 | head -2

echo
echo "=== 6. a duplicate: where does it fail, and what is left behind? ==="
RAW -c "CREATE UNIQUE INDEX CONCURRENTLY dup_code_idx ON dup(code)" 2>&1 | head -2
PSQL -c "SELECT indexrelid::regclass AS leftover, indisvalid
           FROM pg_index WHERE indrelid='dup'::regclass"
echo "--- and can that invalid index still be used for the constraint? ---"
RAW -c "ALTER TABLE dup ADD CONSTRAINT dup_code_uq UNIQUE USING INDEX dup_code_idx" 2>&1 | head -2

echo
echo "=== 7. does USING INDEX accept a NON-unique index? ==="
PSQL -c "CREATE INDEX t_amount_idx ON t(amount)"
RAW -c "ALTER TABLE t ADD CONSTRAINT t_amount_uq UNIQUE USING INDEX t_amount_idx" 2>&1 | head -2

dropdb -p 5555 laswell_spike15
echo "spike database dropped."
