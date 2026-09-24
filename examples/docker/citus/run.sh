#!/usr/bin/env bash
# A Citus cluster: distribute a table, colocate a second with it, add a
# reference table -- signed specifications, planned against a real coordinator.
#
# Needs its own cluster, not the one ../compose.yml starts:
#
#   docker compose -f examples/docker/citus/compose.yml up -d
#   ./examples/docker/citus/run.sh
#   docker compose -f examples/docker/citus/compose.yml down -v
#
# And a binary built WITH the module -- `cmake -DPGLASWELL_MODULES=citus`. A
# PostgreSQL-only build refuses every citus_* kind as unknown, which is correct
# and is shown in step 1 rather than hidden.
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
. "$here/../lib.sh"
need_binaries

COORDINATOR_PORT=${COORDINATOR_PORT:-55440}
WORKER1_PORT=${WORKER1_PORT:-55441}
WORKER2_PORT=${WORKER2_PORT:-55442}
for p in "$COORDINATOR_PORT" "$WORKER1_PORT" "$WORKER2_PORT"; do wait_for "$p"; done
DB=citus_example

say "1. Which binary is this"
run "$PG_LASWELL_MCP" --version
if ! "$PG_LASWELL_MCP" --version | grep -q '^modules:.*citus'; then
  echo "  This binary was built without the citus module, so it refuses every"
  echo "  citus_* kind as unknown. Build one with -DPGLASWELL_MODULES=citus."
  exit 1
fi

say "2. A fresh database on every node, with Citus in it"
# pg_dist_node is PER DATABASE: each database on a coordinator has its own node
# list. So a new database needs the extension on all three nodes and the workers
# registered in it -- the cluster being up is not enough.
# Coordinator first: dropping it removes the metadata that points at the workers.
for p in "$COORDINATOR_PORT" "$WORKER1_PORT" "$WORKER2_PORT"; do
  recreate_db "$p" "$DB"
  psql -X -q -c "CREATE EXTENSION citus" "$(url "$p" "$DB")" >/dev/null
done
psql -X -q "$(url "$COORDINATOR_PORT" "$DB")" >/dev/null <<'SQL'
SELECT citus_set_coordinator_host('coordinator', 5432);
SELECT citus_add_node('worker1', 5432);
SELECT citus_add_node('worker2', 5432);
SQL
psql -X -t -A -c "SELECT count(*) || ' workers registered' FROM pg_dist_node
                   WHERE noderole = 'primary' AND groupid <> 0" \
  "$(url "$COORDINATOR_PORT" "$DB")" | sed 's/^/  /'

kid=$(install_ledger "$COORDINATOR_PORT" "$DB" "$here/keys" citus)
export DATABASE_URL="$(url "$COORDINATOR_PORT" "$DB")"
sign_dir "$here/migrations" "$here/keys/citus.key.pem" "$kid"

say "3. What is pending"
run "$PG_LASWELL" --repo "$here/migrations" --status

say "4. Apply"
echo "  Citus refusals are checked before anything runs -- a unique key that"
echo "  does not include the distribution column, a colocation target of another"
echo "  type -- because each is an error Citus would raise halfway through."
run "$PG_LASWELL" --repo "$here/migrations"

say "5. What Citus now records"
psql -X -q -c "\pset border 2" -c \
  "SELECT logicalrelid::text AS table,
          CASE partmethod WHEN 'h' THEN 'distributed' WHEN 'n' THEN 'reference' END AS kind,
          colocationid AS colocation_group
     FROM pg_dist_partition ORDER BY 1" \
  "$DATABASE_URL"

say "6. Run it again: nothing to do"
run "$PG_LASWELL" --repo "$here/migrations"
