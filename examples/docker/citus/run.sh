#!/usr/bin/env bash
# A Citus cluster: register its nodes, distribute a table, colocate a second
# with it, add a reference table -- all signed specifications, planned against a
# real coordinator. Nothing here registers a node by hand.
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
# list. So a new database needs the extension on all three nodes -- and, until
# 0005 runs, an EMPTY node list, which pg_laswell refuses to distribute into.
# Coordinator first: dropping it removes the metadata that points at the workers.
for p in "$COORDINATOR_PORT" "$WORKER1_PORT" "$WORKER2_PORT"; do
  recreate_db "$p" "$DB"
  psql -X -q -c "CREATE EXTENSION citus" "$(url "$p" "$DB")" >/dev/null
done

# THE CONFIGURATION says which hosts are this cluster's workers; the signed
# specification 0005 says only that the workers are the configured set. Another
# cluster runs the same specifications with its own list here.
config=$(mktemp); chmod 600 "$config"
trap 'rm -f "$config"' EXIT
cat > "$config" <<INI
[citus_example]
host     = 127.0.0.1
port     = $COORDINATOR_PORT
dbname   = $DB
user     = postgres
password = $PGPASSWORD_DEFAULT
citus.workers = worker1:5432, worker2:5432
INI
echo "  $(grep '^citus.workers' "$config")   (in the configuration, not the specification)"

kid=$(install_ledger "$COORDINATOR_PORT" "$DB" "$here/keys" citus)
export DATABASE_URL="$(url "$COORDINATOR_PORT" "$DB")"   # for psql below
sign_dir "$here/migrations" "$here/keys/citus.key.pem" "$kid"

say "3. What is pending"
run "$PG_LASWELL" --config "$config" --repo "$here/migrations" --status

say "4. Rehearse the whole chain, then roll it back"
echo "  A plain --dry-run plans each specification against the database as it is"
echo "  NOW: here, 0030 would be refused, because no node is registered yet."
echo "  The chain rehearses them in order, each on top of the last -- including the"
echo "  Citus calls, which are executed, not merely planned -- and commits nothing."
run "$PG_LASWELL" --config "$config" --repo "$here/migrations" --dry-run=chain
psql -X -t -A -c "SELECT count(*) || ' nodes and ' ||
                         (SELECT count(*) FROM pg_dist_partition) ||
                         ' distributed tables after the rehearsal'
                    FROM pg_dist_node" "$DATABASE_URL" | sed 's/^/  /'

say "5. Apply"
echo "  Citus refusals are checked before anything runs -- a unique key that"
echo "  does not include the distribution column, a colocation target of another"
echo "  type, a table distributed before any node exists -- because each is an"
echo "  error Citus would raise halfway through, or worse, would not raise at all."
run "$PG_LASWELL" --config "$config" --repo "$here/migrations"

say "6. What Citus now records"
psql -X -q -c "\pset border 2" -c \
  "SELECT nodename, nodeport, groupid, shouldhaveshards FROM pg_dist_node ORDER BY groupid" \
  "$DATABASE_URL"
psql -X -q -c "\pset border 2" -c \
  "SELECT logicalrelid::text AS table,
          CASE partmethod WHEN 'h' THEN 'distributed' WHEN 'n' THEN 'reference' END AS kind,
          colocationid AS colocation_group
     FROM pg_dist_partition ORDER BY 1" \
  "$DATABASE_URL"

say "7. Run it again: nothing to do"
run "$PG_LASWELL" --config "$config" --repo "$here/migrations"
