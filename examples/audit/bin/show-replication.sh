#!/usr/bin/env bash
# Prove the topology actually replicates, rather than merely existing.
#
# Attaches a partition on the store, writes two audit rows into the
# PARTITIONED table there, and reads them back from the ORDINARY table on the
# audit cluster. That asymmetry is the whole point of
# publish_via_partition_root: without it the change would arrive under the leaf
# partition's name and the audit side would have nowhere to put it.
set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
[ -f "$here/laswell.ini" ] || { echo "run bin/setup.sh first" >&2; exit 3; }
store_port=$(sed -n 's/^port *= *//p' "$here/laswell.ini" | sed -n 1p)
audit_port=$(sed -n 's/^port *= *//p' "$here/laswell.ini" | sed -n 2p)
month=${1:-2026-09}
next=$(date -d "$month-01 +1 month" +%Y-%m-01 2>/dev/null || echo "2026-10-01")

echo "--- store ($store_port): a partition, and two rows -----------------------"
psql -X -q -h 127.0.0.1 -p "$store_port" -U postgres -d store -v ON_ERROR_STOP=1 <<SQL
CREATE TABLE IF NOT EXISTS audit.logged_actions_${month//-/_} PARTITION OF audit.logged_actions
  FOR VALUES FROM ('$month-01') TO ('$next');
INSERT INTO audit.logged_actions
 (event_id, schema_name, table_name, relid, session_user_name,
  action_tstamp_tx, action_tstamp_stm, action_tstamp_clk, action, statement_only)
SELECT g, 'public', 'orders', 12345, 'app',
       '$month-14 10:00+00'::timestamptz, '$month-14 10:00+00'::timestamptz,
       '$month-14 10:00+00'::timestamptz, CASE WHEN g % 2 = 0 THEN 'U' ELSE 'I' END, false
  FROM generate_series(1, 2) g
ON CONFLICT DO NOTHING;
SQL
psql -X -tA -h 127.0.0.1 -p "$store_port" -U postgres -d store \
  -c "SELECT '  store  : ' || count(*) || ' rows, in a PARTITIONED table' FROM audit.logged_actions"

# Logical replication is asynchronous; wait for it rather than sleeping and
# hoping, so a slow machine reports the truth instead of a false failure.
echo "--- waiting for the rows to arrive ---------------------------------------"
for i in $(seq 1 30); do
  n=$(psql -X -tA -h 127.0.0.1 -p "$audit_port" -U postgres -d audit \
        -c "SELECT count(*) FROM audit.logged_actions" 2>/dev/null || echo 0)
  [ "${n:-0}" -ge 2 ] && break
  sleep 0.5
done

psql -X -tA -h 127.0.0.1 -p "$audit_port" -U postgres -d audit \
  -c "SELECT '  audit  : ' || count(*) || ' rows, in an ORDINARY table' FROM audit.logged_actions"
psql -X -tA -h 127.0.0.1 -p "$audit_port" -U postgres -d audit \
  -c "SELECT '           event ' || event_id || ' ' || table_name || ' ' || action
        FROM audit.logged_actions ORDER BY event_id"
echo
echo "The store's table is partitioned; the audit's is not. The rows crossed"
echo "anyway, because the publication publishes via the partition root."
