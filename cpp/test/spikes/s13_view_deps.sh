#!/usr/bin/env bash
#
# S13 -- what a table change costs when views depend on it.
#
# The question is NOT "how do I drop and recreate a view". It is what is
# SILENTLY LOST when you do, because that is what makes this painful by hand:
# the recreate succeeds, nothing errors, and something that was there before is
# quietly gone.
set -uo pipefail
CONN="port=5555 dbname=laswell_spike13"
export PSQLRC=/dev/null
# The conninfo is one argument. Unquoted "$PSQL" splits it on the space and
# psql reads "dbname=..." as a username, which fails as a peer-auth error and
# sends you looking at pg_hba.conf instead of at the quoting.
PSQL() { psql -X -q -v ON_ERROR_STOP=1 "$CONN" "$@"; }
RAW()  { psql -X -q "$CONN" "$@"; }

dropdb -p 5555 --if-exists laswell_spike13
createdb -p 5555 laswell_spike13

PSQL <<'SQL'
CREATE ROLE laswell_reader13;
CREATE TABLE base(id bigint PRIMARY KEY, amount int, note text);
INSERT INTO base SELECT g, g, 'n'||g FROM generate_series(1,100) g;

-- A three-level stack, which is what a real schema looks like.
CREATE VIEW v1 AS SELECT id, amount, note FROM base;
CREATE VIEW v2 AS SELECT id, amount * 2 AS doubled FROM v1;
CREATE MATERIALIZED VIEW mv1 AS SELECT id, amount FROM v1;
CREATE UNIQUE INDEX mv1_id ON mv1(id);

-- Everything a hand-written drop/recreate tends to forget.
COMMENT ON VIEW v1 IS 'the comment on v1';
COMMENT ON COLUMN v1.amount IS 'the comment on v1.amount';
GRANT SELECT ON v1 TO laswell_reader13;
GRANT SELECT (id) ON v2 TO laswell_reader13;
ALTER VIEW v1 SET (security_barrier = true);
CREATE FUNCTION noop() RETURNS trigger LANGUAGE plpgsql AS $$BEGIN RETURN NEW; END$$;
CREATE TRIGGER v1_ins INSTEAD OF INSERT ON v1 FOR EACH ROW EXECUTE FUNCTION noop();
ALTER VIEW v1 OWNER TO laswell_reader13;
SQL

echo "=== 1. can we change the column type with views on it? ==="
RAW -c "ALTER TABLE base ALTER COLUMN amount TYPE bigint" 2>&1 | head -3

echo
echo "=== 2. and dropping the column? ==="
RAW -c "ALTER TABLE base DROP COLUMN note" 2>&1 | head -3

echo
echo "=== 3. what about a WIDENING that needs no rewrite? ==="
RAW -c "ALTER TABLE base ALTER COLUMN note TYPE varchar(200)" 2>&1 | head -3

echo
echo "=== 4. the recursive dependency query, deepest first ==="
PSQL <<'SQL'
WITH RECURSIVE deps AS (
  SELECT DISTINCT r.ev_class AS oid, 1 AS level
    FROM pg_depend d
    JOIN pg_rewrite r ON r.oid = d.objid AND r.ev_class <> d.refobjid
   WHERE d.refobjid = 'base'::regclass AND d.classid = 'pg_rewrite'::regclass
  UNION ALL
  SELECT DISTINCT r.ev_class, deps.level + 1
    FROM deps
    JOIN pg_depend d ON d.refobjid = deps.oid AND d.classid = 'pg_rewrite'::regclass
    JOIN pg_rewrite r ON r.oid = d.objid AND r.ev_class <> d.refobjid
)
SELECT max(level) AS drop_order, oid::regclass AS object,
       (SELECT relkind FROM pg_class WHERE oid = deps.oid) AS kind
  FROM deps GROUP BY oid ORDER BY drop_order DESC;
SQL

echo
echo "=== 5. what CASCADE takes with it (rolled back) ==="
PSQL <<'SQL'
BEGIN;
DROP VIEW v1 CASCADE;
SELECT count(*) AS views_left FROM pg_class WHERE relkind IN ('v','m')
   AND relnamespace = 'public'::regnamespace;
ROLLBACK;
SQL

echo
echo "=== 6. what is LOST after a naive drop + recreate ==="
PSQL <<'SQL'
DROP MATERIALIZED VIEW mv1;
DROP VIEW v2;
DROP VIEW v1;
ALTER TABLE base ALTER COLUMN amount TYPE bigint;
CREATE VIEW v1 AS SELECT id, amount, note FROM base;
CREATE VIEW v2 AS SELECT id, amount * 2 AS doubled FROM v1;
CREATE MATERIALIZED VIEW mv1 AS SELECT id, amount FROM v1;
SQL
PSQL <<'SQL'
SELECT 'view comment'   AS lost, coalesce(obj_description('v1'::regclass), '** GONE **') AS now
UNION ALL SELECT 'column comment', coalesce(col_description('v1'::regclass, 2), '** GONE **')
UNION ALL SELECT 'grants on v1',   coalesce(array_to_string((SELECT relacl FROM pg_class WHERE oid='v1'::regclass)::text[], ','), '** GONE **')
UNION ALL SELECT 'column grants',  coalesce((SELECT string_agg(privilege_type,',') FROM information_schema.column_privileges WHERE table_name='v2' AND grantee='laswell_reader13'), '** GONE **')
UNION ALL SELECT 'security_barrier', coalesce((SELECT array_to_string(reloptions,',') FROM pg_class WHERE oid='v1'::regclass), '** GONE **')
UNION ALL SELECT 'INSTEAD OF trigger', coalesce((SELECT string_agg(tgname,',') FROM pg_trigger WHERE tgrelid='v1'::regclass AND NOT tgisinternal), '** GONE **')
UNION ALL SELECT 'owner',          (SELECT pg_get_userbyid(relowner) FROM pg_class WHERE oid='v1'::regclass)
UNION ALL SELECT 'matview index',  coalesce((SELECT string_agg(indexrelid::regclass::text,',') FROM pg_index WHERE indrelid='mv1'::regclass), '** GONE **');
SQL

echo
echo "=== 7. is a view definition round-trippable? ==="
PSQL -c "SELECT pg_get_viewdef('v2'::regclass, true) AS v2_definition"

echo
echo "=== 8. locks taken while the stack is rebuilt ==="
PSQL <<'SQL'
BEGIN;
DROP VIEW v2;
SELECT DISTINCT c.relname, l.mode FROM pg_locks l
  JOIN pg_class c ON c.oid = l.relation
 WHERE l.pid = pg_backend_pid() AND c.relname IN ('base','v1','v2')
 ORDER BY 1;
ROLLBACK;
SQL

PSQL -c "DROP OWNED BY laswell_reader13" >/dev/null 2>&1
dropdb -p 5555 laswell_spike13
psql -X -q "port=5555 dbname=postgres" -c "DROP ROLE IF EXISTS laswell_reader13" >/dev/null 2>&1
echo
echo "spike database dropped."
