#!/usr/bin/env bash
#
# S21 -- row-level DML: what an insert/update/delete/merge/copy intent would
# have to DECIDE, measured on 18.6 rather than assumed.
#
# Nine questions. Six of them changed the design; the results are quoted in
# planner_dml.h beside the code each one justifies.
#
#   1. Does MERGE fit the executor's paced keyset shape?          YES
#   2. Does MERGE ... RETURNING advance the cursor reliably?      NO -- see A
#   3. Does ON CONFLICT need a matching unique index?             YES, exactly
#   4. What do explicit ids do to an identity sequence?           leave it behind
#   5. Do VALUES rows need explicit casts?                        YES, on row 1
#   6. Is COPY pace-able?                                         NO, all-or-nothing
#   7. Does RLS refuse, or silently narrow?                       BOTH, differently
#
set -uo pipefail
CONN="port=5555 dbname=laswell_spike21"
export PSQLRC=/dev/null
PSQL() { psql -X -q -v ON_ERROR_STOP=0 "$CONN" "$@"; }
Q() { psql -X -At -v ON_ERROR_STOP=0 "$CONN" "$@"; }

dropdb -p 5555 --if-exists laswell_spike21
createdb -p 5555 laswell_spike21
echo "=== 0. server version ==="; Q -c "SHOW server_version"

PSQL <<'SQL'
CREATE TABLE tgt(id bigint PRIMARY KEY, v int);
CREATE TABLE src(id bigint PRIMARY KEY, v int);
INSERT INTO src SELECT g, g FROM generate_series(1,100) g;
CREATE TABLE nokey(id bigint, v int);
INSERT INTO nokey VALUES (1,1),(1,2);
SQL

echo
echo "=== 1. MERGE in the paced keyset shape the executor requires ==="
echo "-- WITH batch AS (...) MERGE ... RETURNING: accepted on 18.6"
Q -c "WITH batch AS (SELECT id FROM src WHERE id > 0 ORDER BY id LIMIT 10)
      MERGE INTO tgt USING (SELECT src.id, src.v FROM src JOIN batch b ON b.id=src.id) s
        ON tgt.id = s.id
        WHEN MATCHED THEN UPDATE SET v = s.v
        WHEN NOT MATCHED THEN INSERT (id,v) VALUES (s.id,s.v)
      RETURNING s.id" 2>&1 | tail -2
echo "-- MERGE inside a data-modifying CTE: also accepted"
Q -c "WITH m AS (MERGE INTO tgt USING (SELECT 1::bigint id,1 v) s ON tgt.id=s.id
                 WHEN MATCHED THEN UPDATE SET v=s.v RETURNING tgt.id)
      SELECT count(*) FROM m" 2>&1 | tail -1
echo "-- MERGE without a target alias, so the project's own-name convention holds"
Q -c "MERGE INTO tgt USING (SELECT 1::bigint id, 7 v) s ON tgt.id = s.id
       WHEN MATCHED THEN UPDATE SET v = s.v RETURNING tgt.id" 2>&1 | tail -2

echo
echo "=== A. THE CURSOR HAZARD: MERGE ... RETURNING returns nothing for a"
echo "===    source row that matched no WHEN clause ==="
PSQL <<'SQL' >/dev/null 2>&1
CREATE TABLE m_t(id bigint PRIMARY KEY, v int);
CREATE TABLE m_s(id bigint PRIMARY KEY, v int);
INSERT INTO m_t SELECT g,0 FROM generate_series(1,5) g;
INSERT INTO m_s SELECT g,g FROM generate_series(1,10) g;
SQL
echo "-- batch = source ids 6..10, none of which match the target; only WHEN MATCHED"
Q -c "WITH batch AS (SELECT * FROM m_s WHERE m_s.id > 5 ORDER BY m_s.id LIMIT 5)
      MERGE INTO m_t USING batch s ON m_t.id = s.id
        WHEN MATCHED THEN UPDATE SET v = s.v
      RETURNING s.id" 2>&1
echo "   ^ EMPTY. The executor reads affected==0 as 'no rows left' and declares"
echo "     the step COMPLETE having merged nothing. A silent no-op."
echo "-- the fix: name the batch from an outer SELECT, MERGE as a side effect"
Q -c "WITH batch AS (SELECT * FROM m_s WHERE m_s.id > 5 ORDER BY m_s.id LIMIT 5),
           m AS (MERGE INTO m_t USING batch s ON m_t.id = s.id
                  WHEN MATCHED THEN UPDATE SET v = s.v RETURNING m_t.id)
      SELECT batch.id FROM batch ORDER BY batch.id" 2>&1 | tr '\n' ' '; echo
echo "-- and an unreferenced data-modifying CTE still runs (v becomes 999)"
Q -c "WITH batch AS (SELECT * FROM m_s WHERE m_s.id > 0 ORDER BY m_s.id LIMIT 3),
           ins AS (INSERT INTO m_t (id,v) SELECT batch.id, 999 FROM batch
                   ON CONFLICT (id) DO UPDATE SET v = 999 RETURNING m_t.id)
      SELECT batch.id FROM batch ORDER BY batch.id" 2>&1 | tr '\n' ' '; echo
Q -c "SELECT 'm_t ids 1..3 v: '||string_agg(v::text,',' ORDER BY id) FROM m_t WHERE id<=3"

echo
echo "=== 2. MERGE and unique keys: the target duplicate is the SILENT one ==="
echo "-- duplicate TARGET rows, one source row: both updated, no error"
PSQL <<'SQL'
MERGE INTO nokey t USING (SELECT 1::bigint id, 9 v) s ON t.id = s.id
  WHEN MATCHED THEN UPDATE SET v = s.v
  WHEN NOT MATCHED THEN INSERT (id,v) VALUES (s.id,s.v);
SELECT 'nokey rows: '||count(*)||' vals: '||string_agg(v::text,',' ORDER BY v) FROM nokey;
SQL
echo "-- duplicate SOURCE rows: a hard error, so only the target case needs refusing"
Q -c "MERGE INTO tgt USING (VALUES (2::bigint,1),(2::bigint,2)) s(id,v) ON tgt.id=s.id
       WHEN MATCHED THEN UPDATE SET v=s.v" 2>&1 | tail -2

echo
echo "=== 3. INSERT ... ON CONFLICT: the arbiter must match an index exactly ==="
echo "-- no unique index at all"
Q -c "INSERT INTO nokey(id,v) VALUES (5,5) ON CONFLICT (id) DO NOTHING" 2>&1 | tail -1
PSQL <<'SQL' >/dev/null 2>&1
CREATE TABLE part(id int, live boolean);
CREATE UNIQUE INDEX part_u ON part(id) WHERE live;
INSERT INTO part VALUES (1,true);
SQL
echo "-- a PARTIAL unique index, and an arbiter that omits its predicate"
Q -c "INSERT INTO part VALUES (1,true) ON CONFLICT (id) DO NOTHING" 2>&1 | tail -1
echo "-- the same arbiter carrying the predicate: accepted"
Q -c "INSERT INTO part VALUES (1,true) ON CONFLICT (id) WHERE live DO NOTHING; SELECT 'ok'" 2>&1 | tail -1

echo
echo "=== 4. identity and generated columns ==="
PSQL <<'SQL' >/dev/null 2>&1
CREATE TABLE ida(id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY, v int);
CREATE TABLE idd(id bigint GENERATED BY DEFAULT AS IDENTITY PRIMARY KEY, v int);
CREATE TABLE gen(id int PRIMARY KEY, a int, b int GENERATED ALWAYS AS (a*2) STORED);
SQL
echo "-- GENERATED ALWAYS, explicit id"
Q -c "INSERT INTO ida(id,v) VALUES (1,1)" 2>&1 | tail -2
echo "-- the same with OVERRIDING SYSTEM VALUE: accepted"
Q -c "INSERT INTO ida(id,v) OVERRIDING SYSTEM VALUE VALUES (1,1) RETURNING id" 2>&1 | tail -1
echo "-- a stored generated column named in the INSERT"
Q -c "INSERT INTO gen(id,a,b) VALUES (1,1,2)" 2>&1 | tail -2
echo
echo "-- THE SEQUENCE GAP: explicit ids into GENERATED BY DEFAULT"
PSQL -c "INSERT INTO idd(id,v) VALUES (1,1),(2,2),(3,3),(4,4),(5,5)" >/dev/null 2>&1
Q -c "SELECT 'sequence last_value after 5 explicit inserts: '||COALESCE(last_value::text,'NULL')
        FROM pg_sequences WHERE sequencename='idd_id_seq'"
echo "-- so the application's very next insert dies:"
Q -c "INSERT INTO idd(v) VALUES (99)" 2>&1 | tail -2
echo "-- what the migration must emit instead, and where to find the sequence:"
Q -c "SELECT 'setval('||quote_literal(pg_get_serial_sequence('idd','id'))||', '||max(id)||')' FROM idd"
echo "-- the catalog columns that make all four of these predictable:"
Q -c "SELECT attname||'  attidentity='||quote_nullable(attidentity::text)||'  attgenerated='||quote_nullable(attgenerated::text)
        FROM pg_attribute WHERE attrelid='idd'::regclass AND attnum>0"
Q -c "SELECT attname||'  attidentity='||quote_nullable(attidentity::text)||'  attgenerated='||quote_nullable(attgenerated::text)
        FROM pg_attribute WHERE attrelid='gen'::regclass AND attnum>0"

echo
echo "=== 5. VALUES type inference: casts are REQUIRED, not cosmetic ==="
PSQL <<'SQL' >/dev/null 2>&1
CREATE TABLE ty(id uuid PRIMARY KEY, at timestamptz, amt numeric(10,2), tags text[], meta jsonb);
INSERT INTO ty VALUES ('11111111-1111-1111-1111-111111111111', now(), 1.00, '{a}', '{}');
SQL
echo "-- int/text happen to work, which is why this is easy to get wrong:"
Q -c "UPDATE m_t SET v = u.v FROM (VALUES (1,50),(2,60)) AS u(id,v) WHERE m_t.id = u.id RETURNING m_t.id" 2>&1 | tr '\n' ' '; echo
echo "-- uuid does not:"
Q -c "UPDATE ty SET amt = u.amt FROM (VALUES ('11111111-1111-1111-1111-111111111111','9.50')) AS u(id,amt)
       WHERE ty.id = u.id" 2>&1 | tail -2
echo "-- with a cast on the FIRST row only, rows 2+ inherit the type:"
Q -c "SELECT pg_typeof(a)::text||' / '||pg_typeof(b)::text FROM
       (VALUES ('11111111-1111-1111-1111-111111111111'::uuid,1::numeric),
               ('22222222-2222-2222-2222-222222222222',2)) AS v(a,b)" 2>&1
echo "-- arrays and jsonb travel through VALUES the same way:"
Q -c "UPDATE ty SET tags=u.tags, meta=u.meta FROM (VALUES
       ('11111111-1111-1111-1111-111111111111'::uuid,'{x,y}'::text[],'{\"k\":1}'::jsonb)) AS u(id,tags,meta)
      WHERE ty.id=u.id RETURNING ty.tags::text||' '||ty.meta::text" 2>&1 | tail -1
echo "-- DELETE ... USING (VALUES ...) has the identical requirement:"
Q -c "DELETE FROM ty USING (VALUES ('11111111-1111-1111-1111-111111111111')) AS k(id) WHERE ty.id=k.id" 2>&1 | tail -2

echo
echo "=== 6. COPY: all-or-nothing per statement, so it cannot be paced ==="
PSQL -c "CREATE TABLE cp(id int PRIMARY KEY, v int)" >/dev/null 2>&1
PSQL -c "\copy cp FROM PROGRAM 'seq 1 5 | sed ''s/\$/\t1/'''" >/dev/null 2>&1
Q -c "SELECT 'copied: '||count(*) FROM cp"
echo "-- a COPY whose 2nd of 3 rows violates the PK:"
Q -c "\copy cp FROM PROGRAM 'printf ''6\t1\n1\t1\n7\t1\n'''" 2>&1 | tail -1
Q -c "SELECT 'after the failed copy: '||count(*)||' -- rows 6 and 7 rolled back too' FROM cp"
echo "-- PG17/18 ON_ERROR ignore + LOG_VERBOSITY verbose:"
Q -c "\copy cp FROM PROGRAM 'printf ''8\t1\nbad\tx\n9\t1\n''' WITH (ON_ERROR ignore, LOG_VERBOSITY verbose)" 2>&1 | tail -2
Q -c "SELECT 'after ON_ERROR ignore: '||count(*) FROM cp"
echo "-- PG18 REJECT_LIMIT:"
Q -c "\copy cp FROM PROGRAM 'printf ''10\tz\n11\tz\n''' WITH (ON_ERROR ignore, REJECT_LIMIT 1)" 2>&1 | tail -2
echo "-- and COPY is NOT a fast path around constraints: FKs still fire"
PSQL <<'SQL' >/dev/null 2>&1
CREATE TABLE par(id int PRIMARY KEY);
CREATE TABLE chi(id int PRIMARY KEY, p int REFERENCES par(id));
SQL
Q -c "\copy chi FROM PROGRAM 'printf ''1\t99\n'''" 2>&1 | tail -2

echo
echo "=== 7. RLS: a WITH CHECK refuses loudly, a USING clause narrows silently ==="
PSQL <<'SQL' >/dev/null 2>&1
CREATE ROLE s21app LOGIN;
CREATE TABLE rls(id int PRIMARY KEY, tenant int);
ALTER TABLE rls ENABLE ROW LEVEL SECURITY;
CREATE POLICY p ON rls USING (tenant = 1) WITH CHECK (tenant = 1);
GRANT ALL ON rls TO s21app;
INSERT INTO rls VALUES (1,1),(2,2);
SQL
echo "-- INSERT violating WITH CHECK:"
Q -c "SET ROLE s21app; INSERT INTO rls VALUES (3,2)" 2>&1 | tail -1
echo "-- DELETE naming BOTH ids, as the policy-bound role:"
Q -c "SET ROLE s21app; DELETE FROM rls WHERE id IN (1,2) RETURNING id" 2>&1 | tail -2
Q -c "SELECT 'rows left, read as owner: '||count(*)||' -- id 2 was never deleted, and nothing said so' FROM rls"

echo
echo "=== 8. ctid addresses specific rows in a table with no unique key ==="
Q -c "UPDATE nokey SET v = 77 WHERE ctid = (SELECT min(ctid) FROM nokey) RETURNING ctid::text||' v='||v"

PSQL -c "DROP OWNED BY s21app" >/dev/null 2>&1
psql -X -q -p 5555 -d postgres -c "DROP ROLE IF EXISTS s21app" >/dev/null 2>&1
