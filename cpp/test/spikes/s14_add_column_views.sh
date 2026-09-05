#!/usr/bin/env bash
#
# S14 -- adding a column under a view stack, and how to expose it.
#
# S13 measured what BLOCKS a column change. This measures the opposite case,
# because the two behave nothing alike and conflating them would produce a plan
# that is far more destructive than it needs to be.
set -uo pipefail
CONN="port=5555 dbname=laswell_spike14"
export PSQLRC=/dev/null
PSQL() { psql -X -q -v ON_ERROR_STOP=1 "$CONN" "$@"; }
RAW()  { psql -X -q "$CONN" "$@"; }

dropdb -p 5555 --if-exists laswell_spike14
createdb -p 5555 laswell_spike14
PSQL <<'SQL'
CREATE ROLE laswell_s14_reader;
CREATE TABLE base(id bigint PRIMARY KEY, amount int);
CREATE VIEW v_star AS SELECT * FROM base;
CREATE VIEW v1 AS SELECT id, amount FROM base;
CREATE VIEW v2 AS SELECT id, amount FROM v1;
CREATE MATERIALIZED VIEW mv1 AS SELECT id FROM v1;
COMMENT ON VIEW v1 IS 'v1 doc';
COMMENT ON COLUMN v1.amount IS 'amount doc';
ALTER VIEW v1 SET (security_barrier = true);
GRANT SELECT ON v1 TO laswell_s14_reader;
GRANT SELECT (id) ON v1 TO laswell_s14_reader;
CREATE FUNCTION noop() RETURNS trigger LANGUAGE plpgsql AS $$BEGIN RETURN NEW; END$$;
CREATE TRIGGER v1_ins INSTEAD OF INSERT ON v1 FOR EACH ROW EXECUTE FUNCTION noop();
ALTER VIEW v1 OWNER TO laswell_s14_reader;
SQL

echo "=== 1. ADD COLUMN under a full view stack ==="
RAW -c "ALTER TABLE base ADD COLUMN region text" && echo "  succeeded -- views never block it"

echo "=== 2. does a SELECT * view pick the new column up? ==="
PSQL -c "SELECT string_agg(attname, ',' ORDER BY attnum) AS v_star_columns
           FROM pg_attribute WHERE attrelid='v_star'::regclass
            AND attnum>0 AND NOT attisdropped"

echo "=== 3. CREATE OR REPLACE VIEW to expose it: what survives ==="
PSQL -c "CREATE OR REPLACE VIEW v1 AS SELECT id, amount, region FROM base"
PSQL <<'SQL'
SELECT 'view comment' AS thing, coalesce(obj_description('v1'::regclass),'** GONE **') AS after
UNION ALL SELECT 'column comment', coalesce(col_description('v1'::regclass,2),'** GONE **')
UNION ALL SELECT 'grants', coalesce((SELECT array_to_string(relacl::text[],',') FROM pg_class WHERE oid='v1'::regclass),'** GONE **')
UNION ALL SELECT 'column grants', coalesce((SELECT 'present' FROM information_schema.column_privileges WHERE table_name='v1' AND grantee='laswell_s14_reader' LIMIT 1),'** GONE **')
UNION ALL SELECT 'security_barrier', coalesce((SELECT array_to_string(reloptions,',') FROM pg_class WHERE oid='v1'::regclass),'** GONE **')
UNION ALL SELECT 'INSTEAD OF trigger', coalesce((SELECT string_agg(tgname,',') FROM pg_trigger WHERE tgrelid='v1'::regclass AND NOT tgisinternal),'** GONE **')
UNION ALL SELECT 'owner', (SELECT pg_get_userbyid(relowner) FROM pg_class WHERE oid='v1'::regclass)
UNION ALL SELECT 'dependent v2', coalesce((SELECT 'still there' FROM pg_class WHERE relname='v2'),'** GONE **')
UNION ALL SELECT 'dependent mv1', coalesce((SELECT 'still there' FROM pg_class WHERE relname='mv1'),'** GONE **');
SQL

echo "=== 4. what CREATE OR REPLACE refuses ==="
for q in "SELECT id, region, amount FROM base" \
         "SELECT id, amount FROM base" \
         "SELECT id, amount::bigint, region FROM base" \
         "SELECT id AS ident, amount, region FROM base"; do
  printf '  %-48s -> ' "$q"
  RAW -c "CREATE OR REPLACE VIEW v1 AS $q" 2>&1 | head -1 | sed 's/^ERRO:  /REFUSED: /'
done

echo "=== 5. CREATE OR REPLACE for a materialized view? ==="
RAW -c "CREATE OR REPLACE MATERIALIZED VIEW mv1 AS SELECT id FROM v1" 2>&1 | head -1

PSQL -c "DROP OWNED BY laswell_s14_reader" >/dev/null 2>&1
dropdb -p 5555 laswell_spike14
psql -X -q "port=5555 dbname=postgres" -c "DROP ROLE IF EXISTS laswell_s14_reader" >/dev/null 2>&1
echo "spike database dropped."
