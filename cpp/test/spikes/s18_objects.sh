#!/usr/bin/env bash
#
# S18 -- the object types pg_laswell could not express at all.
#
# The question is not "can we emit CREATE X" -- obviously we can. It is whether
# each one has something to DECIDE, because a kind with nothing to decide is a
# psql wrapper and the repository is the only argument for it.
set -uo pipefail
CONN="port=5555 dbname=laswell_spike18"
export PSQLRC=/dev/null
PSQL() { psql -X -q -v ON_ERROR_STOP=1 "$CONN" "$@"; }
RAW()  { psql -X -q "$CONN" "$@"; }

dropdb -p 5555 --if-exists laswell_spike18
createdb -p 5555 laswell_spike18
PSQL -c "CREATE ROLE laswell_s18 NOLOGIN" 2>/dev/null || true

echo "=== 1. ALTER TYPE ... ADD VALUE inside a transaction block ==="
PSQL -c "CREATE TYPE mood AS ENUM ('ok','bad')"
RAW -c "BEGIN; ALTER TYPE mood ADD VALUE 'great'; COMMIT;" 2>&1 | head -2
echo "--- can the new value be USED in the same transaction? ---"
RAW -c "BEGIN; ALTER TYPE mood ADD VALUE 'awful'; SELECT 'awful'::mood; COMMIT;" 2>&1 | head -3
echo "--- and a type created in the SAME transaction? ---"
RAW -c "BEGIN; CREATE TYPE t2 AS ENUM ('a'); ALTER TYPE t2 ADD VALUE 'b'; SELECT 'b'::t2; COMMIT;" 2>&1 | head -3
echo "--- is ADD VALUE reversible? ---"
RAW -c "ALTER TYPE mood DROP VALUE 'great'" 2>&1 | head -2

echo
echo "=== 2. CREATE OR REPLACE FUNCTION: what survives? ==="
PSQL <<'SQL'
CREATE FUNCTION f(a int) RETURNS int LANGUAGE sql AS 'SELECT a';
COMMENT ON FUNCTION f(int) IS 'the comment';
GRANT EXECUTE ON FUNCTION f(int) TO laswell_s18;
ALTER FUNCTION f(int) OWNER TO laswell_s18;
SQL
PSQL -c "CREATE OR REPLACE FUNCTION f(a int) RETURNS int LANGUAGE sql AS 'SELECT a * 2'"
PSQL -c "SELECT coalesce(obj_description('f(int)'::regprocedure),'** GONE **') AS comment,
                coalesce(array_to_string(proacl::text[],','),'** GONE **') AS grants,
                pg_get_userbyid(proowner) AS owner
           FROM pg_proc WHERE oid='f(int)'::regprocedure"
echo "--- can CREATE OR REPLACE change the return type? ---"
RAW -c "CREATE OR REPLACE FUNCTION f(a int) RETURNS bigint LANGUAGE sql AS 'SELECT a::bigint'" 2>&1 | head -2
echo "--- does replacing a function change its OID (and so break dependents)? ---"
PSQL -c "SELECT 'f(int)'::regprocedure::oid AS oid_after_replace"

echo
echo "=== 3. CREATE OR REPLACE TRIGGER (PG14+) ==="
PSQL <<'SQL'
CREATE TABLE t(id int);
CREATE FUNCTION trg_f() RETURNS trigger LANGUAGE plpgsql AS $$BEGIN RETURN NEW; END$$;
CREATE TRIGGER trg BEFORE INSERT ON t FOR EACH ROW EXECUTE FUNCTION trg_f();
SQL
RAW -c "CREATE OR REPLACE TRIGGER trg AFTER INSERT ON t FOR EACH ROW EXECUTE FUNCTION trg_f()" 2>&1 | head -2
echo "--- locks taken by CREATE TRIGGER ---"
PSQL <<'SQL'
BEGIN;
CREATE TRIGGER trg2 AFTER UPDATE ON t FOR EACH ROW EXECUTE FUNCTION trg_f();
SELECT c.relname, l.mode FROM pg_locks l JOIN pg_class c ON c.oid=l.relation
 WHERE l.pid=pg_backend_pid() AND c.relname='t';
ROLLBACK;
SQL

echo
echo "=== 4. dropping a type/function/sequence that something uses ==="
PSQL -c "CREATE TABLE uses_mood(m mood)"
RAW -c "DROP TYPE mood" 2>&1 | head -3
PSQL -c "CREATE VIEW uses_f AS SELECT f(1) AS v"
RAW -c "DROP FUNCTION f(int)" 2>&1 | head -3
PSQL -c "CREATE SEQUENCE s; CREATE TABLE uses_s(id int DEFAULT nextval('s'))"
RAW -c "DROP SEQUENCE s" 2>&1 | head -3

echo
echo "=== 5. CREATE SCHEMA / EXTENSION: locks and idempotence ==="
PSQL <<'SQL'
BEGIN;
CREATE SCHEMA sch;
SELECT count(*) AS locks_on_anything_existing FROM pg_locks l
  JOIN pg_class c ON c.oid=l.relation WHERE l.pid=pg_backend_pid();
COMMIT;
SQL
PSQL -c "SELECT name, default_version, installed_version FROM pg_available_extensions WHERE name IN ('pg_trgm','btree_gist') ORDER BY name"
RAW -c "BEGIN; CREATE EXTENSION pg_trgm; COMMIT;" 2>&1 | head -2
PSQL -c "SELECT extname, extversion FROM pg_extension WHERE extname='pg_trgm'"
echo "--- extension objects land in a schema; which? ---"
PSQL -c "SELECT n.nspname AS extension_schema FROM pg_extension e JOIN pg_namespace n ON n.oid=e.extnamespace WHERE e.extname='pg_trgm'"

echo
echo "=== 6. a domain, and adding a constraint to it ==="
PSQL -c "CREATE DOMAIN positive AS int CHECK (VALUE > 0)"
PSQL -c "CREATE TABLE uses_domain(v positive)"
PSQL -c "INSERT INTO uses_domain VALUES (1)"
RAW -c "INSERT INTO uses_domain VALUES (-1)" 2>&1 | head -2
echo "--- does adding a domain constraint scan existing tables? ---"
PSQL -c "\timing on" -c "ALTER DOMAIN positive ADD CONSTRAINT lt100 CHECK (VALUE < 100)" 2>&1 | grep -i "tempo\|time"

PSQL -c "DROP OWNED BY laswell_s18" >/dev/null 2>&1
dropdb -p 5555 laswell_spike18
psql -X -q "port=5555 dbname=postgres" -c "DROP ROLE IF EXISTS laswell_s18" >/dev/null 2>&1
echo "spike database dropped."
