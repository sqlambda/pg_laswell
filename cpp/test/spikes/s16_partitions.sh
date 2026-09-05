#!/usr/bin/env bash
#
# S16 -- attaching and detaching partitions without an outage.
#
# Partition rotation is how time-series data is actually retired, and both
# halves have a trap: ATTACH validates unless you prove it need not, and DETACH
# has a concurrent form that is not a drop-in replacement.
set -uo pipefail
CONN="port=5555 dbname=laswell_spike16"
export PSQLRC=/dev/null
PSQL() { psql -X -q -v ON_ERROR_STOP=1 "$CONN" "$@"; }
RAW()  { psql -X -q "$CONN" "$@"; }

dropdb -p 5555 --if-exists laswell_spike16
createdb -p 5555 laswell_spike16
PSQL <<'SQL'
CREATE TABLE events(id bigint, at timestamptz NOT NULL, pad text)
  PARTITION BY RANGE (at);
CREATE TABLE events_2025 PARTITION OF events
  FOR VALUES FROM ('2025-01-01') TO ('2026-01-01');
CREATE INDEX events_at ON events(at);
INSERT INTO events SELECT g, '2025-06-01'::timestamptz + (g||' seconds')::interval,
       repeat('x',100) FROM generate_series(1,300000) g;

-- Two candidates for attaching, identical except for one CHECK constraint.
CREATE TABLE cand_bare(LIKE events INCLUDING DEFAULTS);
INSERT INTO cand_bare SELECT g, '2026-06-01'::timestamptz + (g||' seconds')::interval,
       repeat('x',100) FROM generate_series(1,300000) g;
CREATE TABLE cand_checked(LIKE events INCLUDING DEFAULTS);
INSERT INTO cand_checked SELECT g, '2027-06-01'::timestamptz + (g||' seconds')::interval,
       repeat('x',100) FROM generate_series(1,300000) g;
ALTER TABLE cand_checked ADD CONSTRAINT ck
  CHECK (at >= '2027-01-01' AND at < '2028-01-01');
ANALYZE;
SQL

echo "=== 1. ATTACH without a matching CHECK: does it scan? ==="
PSQL -c "\timing on" -c "ALTER TABLE events ATTACH PARTITION cand_bare FOR VALUES FROM ('2026-01-01') TO ('2027-01-01')" 2>&1 | grep -i "tempo\|time"

echo "=== 2. ATTACH WITH a matching CHECK: ==="
PSQL -c "\timing on" -c "ALTER TABLE events ATTACH PARTITION cand_checked FOR VALUES FROM ('2027-01-01') TO ('2028-01-01')" 2>&1 | grep -i "tempo\|time"

echo
echo "=== 3. locks taken by ATTACH ==="
PSQL <<'SQL'
CREATE TABLE cand3(LIKE events INCLUDING DEFAULTS);
ALTER TABLE cand3 ADD CONSTRAINT ck3 CHECK (at >= '2028-01-01' AND at < '2029-01-01');
BEGIN;
ALTER TABLE events ATTACH PARTITION cand3 FOR VALUES FROM ('2028-01-01') TO ('2029-01-01');
SELECT c.relname, l.mode FROM pg_locks l JOIN pg_class c ON c.oid=l.relation
 WHERE l.pid=pg_backend_pid() AND c.relname IN ('events','cand3','events_2025')
 ORDER BY 1,2;
COMMIT;
SQL

echo
echo "=== 4. did ATTACH have to build the parent's index on the new partition? ==="
PSQL -c "SELECT indexrelid::regclass AS idx, indisvalid FROM pg_index WHERE indrelid='cand_bare'::regclass"

echo
echo "=== 5. plain DETACH: locks ==="
PSQL <<'SQL'
BEGIN;
ALTER TABLE events DETACH PARTITION cand3;
SELECT c.relname, l.mode FROM pg_locks l JOIN pg_class c ON c.oid=l.relation
 WHERE l.pid=pg_backend_pid() AND c.relname IN ('events','cand3') ORDER BY 1,2;
ROLLBACK;
SQL

echo
echo "=== 6. DETACH CONCURRENTLY: can it run in a transaction? ==="
RAW -c "BEGIN; ALTER TABLE events DETACH PARTITION cand3 CONCURRENTLY; COMMIT;" 2>&1 | head -3
echo "--- and outside one: ---"
RAW -c "ALTER TABLE events DETACH PARTITION cand3 CONCURRENTLY" 2>&1 | head -2
PSQL -c "SELECT relispartition FROM pg_class WHERE relname='cand3'"

echo
echo "=== 7. what locks does DETACH CONCURRENTLY take? (observed from another session) ==="
PSQL -c "SELECT 'see the docs: ShareUpdateExclusiveLock on parent, AccessExclusiveLock on the partition briefly' AS note"

echo
echo "=== 8. the DEFAULT partition trap ==="
PSQL <<'SQL'
CREATE TABLE events_default PARTITION OF events DEFAULT;
INSERT INTO events_default SELECT g, '2030-06-01'::timestamptz + (g||' seconds')::interval,
       repeat('x',100) FROM generate_series(1,200000) g;
CREATE TABLE cand4(LIKE events INCLUDING DEFAULTS);
ALTER TABLE cand4 ADD CONSTRAINT ck4 CHECK (at >= '2029-01-01' AND at < '2030-01-01');
SQL
echo "--- attaching with a DEFAULT partition present, candidate fully CHECKed: ---"
PSQL -c "\timing on" -c "ALTER TABLE events ATTACH PARTITION cand4 FOR VALUES FROM ('2029-01-01') TO ('2030-01-01')" 2>&1 | grep -i "tempo\|time"
echo "  (the default partition must be scanned to prove it holds no row belonging to the new range)"

dropdb -p 5555 laswell_spike16
echo "spike database dropped."
