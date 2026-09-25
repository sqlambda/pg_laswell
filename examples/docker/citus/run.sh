#!/usr/bin/env bash
# One repository, two kinds of server: the same signed specifications build a
# schema on plain PostgreSQL and on a Citus cluster, and on Citus they also
# register the cluster's nodes, distribute the tables and route the procedures.
#
# The specifications are borrowed from the banking/journal variant of pgshard,
# a lab that benchmarks the same schema on plain PostgreSQL and on Citus
# clusters of one to six workers. Epochs are what let one repository serve both targets: a
# specification naming no epoch runs everywhere, and one in epoch `citus` or
# `plain` runs only where that epoch is open.
#
# Needs its own cluster, not the one ../compose.yml starts:
#
#   docker compose -f examples/docker/citus/compose.yml up -d
#   ./examples/docker/citus/run.sh
#   docker compose -f examples/docker/citus/compose.yml down -v
#
# And a binary built WITH the module -- the released package is. A
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
PLAIN=plain_example

say "1. Which binary is this"
run "$PG_LASWELL_MCP" --version
if ! "$PG_LASWELL_MCP" --version | grep -q '^modules:.*citus'; then
  echo "  This binary was built without the citus module, so it refuses every"
  echo "  citus_* kind as unknown. Build one with -DPGLASWELL_MODULES=citus."
  exit 1
fi

say "2. Two fresh databases: one with Citus on every node, one without"
# pg_dist_node is PER DATABASE: each database on a coordinator has its own node
# list. So a new database needs the extension on all three nodes -- and, until
# 0040 runs, an EMPTY node list, which pg_laswell refuses to distribute into.
# Coordinator first: dropping it removes the metadata that points at the workers.
for p in "$COORDINATOR_PORT" "$WORKER1_PORT" "$WORKER2_PORT"; do
  recreate_db "$p" "$DB" 2>&1 | grep -v 'partially supports CREATE DATABASE\|does not propagate\|manually create' >&2 || true
  psql -X -q -c "CREATE EXTENSION citus" "$(url "$p" "$DB")" >/dev/null
done
# The plain target lives on the coordinator's server with NO citus extension in
# it: to pg_laswell, and to PostgreSQL, it is an ordinary database.
recreate_db "$COORDINATOR_PORT" "$PLAIN" 2>&1 | grep -v 'partially supports CREATE DATABASE\|does not propagate\|manually create' >&2 || true

# One configuration per target. Only the Citus one says which hosts are its
# workers: the signed specification 0040 says only that the workers are the
# configured set, so another cluster runs the same bytes with its own list here.
write_config() {  # write_config <file> <dbname> [citus.workers]
  { echo "[$2]"
    echo "host     = 127.0.0.1"
    echo "port     = $COORDINATOR_PORT"
    echo "dbname   = $2"
    echo "user     = postgres"
    echo "password = $PGPASSWORD_DEFAULT"
    [ -n "${3:-}" ] && echo "citus.workers = $3"
  } > "$1"
  chmod 600 "$1"
}
citus_cfg=$(mktemp); plain_cfg=$(mktemp)
trap 'rm -f "$citus_cfg" "$plain_cfg"' EXIT
write_config "$citus_cfg" "$DB" "worker1:5432, worker2:5432"
write_config "$plain_cfg" "$PLAIN"
echo "  $DB:    $(grep '^citus.workers' "$citus_cfg")   (configuration, not specification)"
echo "  $PLAIN: no Citus, no workers"

# The same key, trusted by both databases; each opens the epoch it is.
kid=$(install_ledger "$COORDINATOR_PORT" "$DB" "$here/keys" citus)
install_ledger "$COORDINATOR_PORT" "$PLAIN" "$here/keys" citus >/dev/null
open_epoch "$COORDINATOR_PORT" "$DB" citus "A Citus cluster: distribution, routing, and Citus's order for foreign keys."
open_epoch "$COORDINATOR_PORT" "$PLAIN" plain "A single PostgreSQL: foreign keys straight after the tables."
export DATABASE_URL="$(url "$COORDINATOR_PORT" "$DB")"   # for psql below
sign_dir "$here/migrations" "$here/keys/citus.key.pem" "$kid"

say "3. What each target will run"
echo "  Twelve specifications. The ones in epoch citus are held on the plain"
echo "  database, and the one in epoch plain is held on the Citus one."
run "$PG_LASWELL" --config "$plain_cfg" --repo "$here/migrations" --status
run "$PG_LASWELL" --config "$citus_cfg" --repo "$here/migrations" --status

say "4. The plain target"
run "$PG_LASWELL" --config "$plain_cfg" --repo "$here/migrations"

say "5. The Citus target: rehearse the whole chain, then roll it back"
echo "  A plain --dry-run plans each specification against the database as it is"
echo "  NOW: here, 0050 would be refused, because no node is registered yet."
echo "  The chain rehearses them in order, each on top of the last -- including the"
echo "  Citus calls, which are executed, not merely planned -- and commits nothing."
echo "  The rebalance moves shards on connections of its own and cannot be rolled"
echo "  back, so it is listed as unverified rather than run."
run "$PG_LASWELL" --config "$citus_cfg" --repo "$here/migrations" --dry-run=chain
psql -X -t -A -c "SELECT count(*) || ' nodes and ' ||
                         (SELECT count(*) FROM pg_dist_partition) ||
                         ' Citus tables after the rehearsal'
                    FROM pg_dist_node" "$DATABASE_URL" | sed 's/^/  /'

say "6. The Citus target: apply"
echo "  Citus refusals are checked before anything runs -- a unique key that"
echo "  does not include the distribution column, a colocation target of another"
echo "  type, a table distributed before any node exists, a procedure routed on"
echo "  an argument that cannot reach its group -- because each is an error Citus"
echo "  would raise halfway through, or worse, would not raise at all."
run "$PG_LASWELL" --config "$citus_cfg" --repo "$here/migrations"

say "7. The same schema, two shapes"
echo "  plain_example:"
psql -X -q -c "\\pset border 2" -c \
  "SELECT c.relname AS table, con.conname AS foreign_key
     FROM pg_class c LEFT JOIN pg_constraint con ON con.conrelid = c.oid AND con.contype = 'f'
    WHERE c.relnamespace = 'public'::regnamespace AND c.relkind = 'r' ORDER BY 1" \
  "$(url "$COORDINATOR_PORT" "$PLAIN")"
echo "  citus_example:"
psql -X -q -c "\\pset border 2" -c \
  "SELECT nodename, nodeport, groupid, shouldhaveshards FROM pg_dist_node ORDER BY groupid" \
  "$DATABASE_URL"
psql -X -q -c "\\pset border 2" -c \
  "SELECT logicalrelid::text AS table,
          CASE partmethod WHEN 'h' THEN 'distributed' WHEN 'n' THEN 'reference' END AS kind,
          colocationid AS colocation_group
     FROM pg_dist_partition ORDER BY 1" \
  "$DATABASE_URL"
psql -X -q -c "\\pset border 2" -c \
  "SELECT p.proname AS procedure, o.distribution_argument_index + 1 AS routed_on_argument,
          o.colocationid AS colocation_group
     FROM pg_dist_object o JOIN pg_proc p ON p.oid = o.objid
    WHERE o.classid = 'pg_proc'::regclass AND o.distribution_argument_index IS NOT NULL
    ORDER BY 1" \
  "$DATABASE_URL"

say "8. Run both again: nothing to do"
run "$PG_LASWELL" --config "$plain_cfg" --repo "$here/migrations"
run "$PG_LASWELL" --config "$citus_cfg" --repo "$here/migrations"
