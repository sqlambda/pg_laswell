#!/usr/bin/env bash
#
# S19 -- the ALTER forms for objects pg_laswell could already create.
#
# One question decides the shape of alter_domain: does adding a domain
# constraint scan, and can that scan be deferred the way a table CHECK can?
set -uo pipefail
CONN="port=5555 dbname=laswell_spike19"
export PSQLRC=/dev/null
PSQL() { psql -X -q -v ON_ERROR_STOP=1 "$CONN" "$@"; }

dropdb -p 5555 --if-exists laswell_spike19
createdb -p 5555 laswell_spike19
PSQL <<'SQL'
CREATE DOMAIN pos AS int CHECK (VALUE > 0);
CREATE TABLE big(a pos, b pos);
INSERT INTO big SELECT g, g FROM generate_series(1,500000) g;
CREATE TABLE big2(c pos);
INSERT INTO big2 SELECT g FROM generate_series(1,500000) g;
ANALYZE;
SQL
echo "=== 1. ALTER DOMAIN ADD CONSTRAINT: 1.5M values over 3 columns in 2 tables ==="
PSQL -c "\timing on" -c "ALTER DOMAIN pos ADD CONSTRAINT lt CHECK (VALUE < 10000000)" 2>&1 | grep -i "tempo\|time"
echo "=== 2. the same, NOT VALID ==="
PSQL -c "\timing on" -c "ALTER DOMAIN pos ADD CONSTRAINT lt2 CHECK (VALUE < 20000000) NOT VALID" 2>&1 | grep -i "tempo\|time"
echo "=== 3. and validating it afterwards ==="
PSQL -c "\timing on" -c "ALTER DOMAIN pos VALIDATE CONSTRAINT lt2" 2>&1 | grep -i "tempo\|time"

echo
echo "=== 4. ALTER POLICY: which lock? ==="
PSQL <<'SQL'
CREATE TABLE t(id int, tenant text);
ALTER TABLE t ENABLE ROW LEVEL SECURITY;
CREATE POLICY p ON t USING (tenant = current_user);
BEGIN;
ALTER POLICY p ON t USING (tenant = 'x');
SELECT c.relname, l.mode FROM pg_locks l JOIN pg_class c ON c.oid=l.relation
 WHERE l.pid=pg_backend_pid() AND c.relname='t';
ROLLBACK;
SQL

echo
echo "=== 5. ALTER COLUMN SET DEFAULT: rewrite? ==="
PSQL -c "SELECT relfilenode AS before FROM pg_class WHERE oid='big'::regclass"
PSQL -c "ALTER TABLE big ALTER COLUMN b SET DEFAULT 7"
PSQL -c "SELECT relfilenode AS after FROM pg_class WHERE oid='big'::regclass"

dropdb -p 5555 laswell_spike19
echo "spike database dropped."
