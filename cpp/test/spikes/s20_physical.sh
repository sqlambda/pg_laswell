#!/usr/bin/env bash
#
# S20 -- which ALTER TABLE forms rewrite the table.
#
# The whole batch is about predicting that, so it is measured by relfilenode
# rather than reasoned about. Note the two prerequisites the first run of this
# spike discovered by failing: identity needs the column NOT NULL first, and
# SET EXPRESSION only applies to a column that is ALREADY generated.
set -uo pipefail
CONN="port=5555 dbname=laswell_spike20"
export PSQLRC=/dev/null
PSQL() { psql -X -q -v ON_ERROR_STOP=1 "$CONN" "$@"; }

dropdb -p 5555 --if-exists laswell_spike20
createdb -p 5555 laswell_spike20
PSQL <<'SQL'
CREATE TABLE t(a int NOT NULL, b int, c text,
               gen int GENERATED ALWAYS AS (b*2) STORED);
INSERT INTO t(a,b,c) SELECT g,g,'x'||g FROM generate_series(1,300000) g;
ANALYZE;
CREATE FUNCTION rw(stmt text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE before oid; after oid; BEGIN
  SELECT relfilenode INTO before FROM pg_class WHERE oid='t'::regclass;
  EXECUTE stmt;
  SELECT relfilenode INTO after FROM pg_class WHERE oid='t'::regclass;
  RETURN CASE WHEN before = after THEN 'no rewrite' ELSE 'REWRITE' END;
EXCEPTION WHEN others THEN RETURN 'ERROR: '||SQLERRM; END$$;
SQL
for s in "ALTER TABLE t ALTER COLUMN a ADD GENERATED ALWAYS AS IDENTITY" \
         "ALTER TABLE t ALTER COLUMN a DROP IDENTITY" \
         "ALTER TABLE t ALTER COLUMN gen SET EXPRESSION AS (b * 3)" \
         "ALTER TABLE t ALTER COLUMN gen DROP EXPRESSION" \
         "ALTER TABLE t ALTER COLUMN b SET STATISTICS 500" \
         "ALTER TABLE t ALTER COLUMN c SET STORAGE EXTERNAL" \
         "ALTER TABLE t ALTER COLUMN c SET COMPRESSION lz4" \
         "ALTER TABLE t SET (fillfactor = 70)" \
         "ALTER TABLE t SET UNLOGGED" \
         "ALTER TABLE t SET LOGGED" \
         "ALTER TABLE t REPLICA IDENTITY FULL" \
         "ALTER TABLE t SET ACCESS METHOD heap"; do
  printf '  %-58s -> ' "${s#ALTER TABLE t }"
  PSQL -A -t -c "SELECT rw(\$\$$s\$\$)"
done
dropdb -p 5555 laswell_spike20
echo "spike database dropped."
