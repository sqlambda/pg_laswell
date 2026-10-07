#!/usr/bin/env bash
# TimescaleDB: one repository, both editions.
#
# 0010 makes a hypertable and indexes it, and runs on either edition. 0020 and
# 0030 use the columnstore, policies and a continuous aggregate, which only the
# Timescale License edition has; they are in the epoch `timescale-license`,
# which only that server opens -- so the Apache server holds them, and neither
# is refused for what it lacks.
#
# Then the hazard the module exists for: an index on a hypertable large enough
# that core would otherwise reach for CREATE INDEX CONCURRENTLY, which
# TimescaleDB refuses.
#
#   docker compose -f examples/docker/timescale/compose.yml up -d
#   ./examples/docker/timescale/run.sh
#   docker compose -f examples/docker/timescale/compose.yml down -v
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
. "$here/../lib.sh"
need_binaries

TSL_PORT=${TSL_PORT:-55470}
APACHE_PORT=${APACHE_PORT:-55471}
for p in "$TSL_PORT" "$APACHE_PORT"; do wait_for "$p"; done

say "1. Which binary is this"
run "$PG_LASWELL_MCP" --version
"$PG_LASWELL_MCP" --version | grep -q '^modules:.*timescaledb' || {
  echo "  This binary was built without the timescaledb module."
  exit 1
}

say "2. Two servers, two editions"
for p in "$TSL_PORT" "$APACHE_PORT"; do
  echo "  port $p: TimescaleDB $(psql -X -t -A -c "SELECT extversion FROM pg_extension WHERE extname='timescaledb'" "$(url "$p")"), license $(psql -X -t -A -c 'SHOW timescaledb.license' "$(url "$p")")"
done

for p in "$TSL_PORT" "$APACHE_PORT"; do
  recreate_db "$p" metrics
  # IF NOT EXISTS: the timescale images install it in template1 already.
  psql -X -q -c "CREATE EXTENSION IF NOT EXISTS timescaledb" "$(url "$p" metrics)" >/dev/null
done
kid=$(install_ledger "$TSL_PORT" metrics "$here/keys" ts)
install_ledger "$APACHE_PORT" metrics "$here/keys" ts >/dev/null
open_epoch "$TSL_PORT" metrics timescale-license "The Timescale License edition: columnstore, policies, continuous aggregates."

build=$here/.build
rm -rf "$build"; mkdir -p "$build"
cp "$here/migrations"/*.json "$build/"
sign_dir "$build" "$here/keys/ts.key.pem" "$kid"

say "3. The Apache edition: the hypertable, and the rest held"
run "$PG_LASWELL" --repo "$build" "$(url "$APACHE_PORT" metrics)"

say "4. The Timescale License edition: all of it"
run "$PG_LASWELL" --repo "$build" "$(url "$TSL_PORT" metrics)"
psql -X -q -c "\\pset border 2" -c \
  "SELECT proc_name, hypertable_name, schedule_interval FROM timescaledb_information.jobs
    WHERE job_id >= 1000 ORDER BY 1" "$(url "$TSL_PORT" metrics)"

say "5. An index on a large hypertable"
echo "  1.2 million readings: over 100 MB with their index, in chunks. The parent reads"
echo "  0 pages, so without the module core would size the build as tiny -- and"
echo "  for a larger table it would plan CREATE INDEX CONCURRENTLY, which"
echo "  TimescaleDB refuses on the hypertable. It accepts one on each chunk, so"
echo "  the index is created on the parent only and then built chunk by chunk,"
echo "  blocking no write. The first steps of the plan:"
psql -X -q -c "INSERT INTO public.readings SELECT now() - (g || ' seconds')::interval, g % 50, random()
               FROM generate_series(1, 1200000) g" -c "ANALYZE public.readings" "$(url "$APACHE_PORT" metrics)"
"$PG_LASWELL_MCP" --call planMigration --args '{"spec":{"laswell_spec_version":1,
   "id":"value-index","description":"an index on value",
   "intents":[{"kind":"create_index","schema":"public","table":"readings",
     "name":"readings_value_idx","columns":["value"],"comment":"Outliers."}]},
   "skipTrustChecks":true}' "$(url "$APACHE_PORT" metrics)" \
  | python3 -c 'import json,sys
d = json.load(sys.stdin)
for s in d["steps"][:3]:
    print("  why: ", s["why"])
    print("  lock:", s["lock"])
    for q in s["sql"]: print("  sql: ", q)
print("  ... and", len(d["steps"]) - 3, "more steps: a build and a check for each chunk")'

say "6. Run both again: nothing to do"
run "$PG_LASWELL" --repo "$build" "$(url "$APACHE_PORT" metrics)"
run "$PG_LASWELL" --repo "$build" "$(url "$TSL_PORT" metrics)"

say "7. A policy changed by hand"
echo "  A policy is a row: anyone with the rights can pause or remove it, with no"
echo "  migration involved. The ledger says what each was last declared to be, so"
echo "  any plan against this database says when one no longer is. Here the"
echo "  retention job is paused by hand, and an unrelated plan is asked for:"
psql -X -q -t -A -c "SELECT alter_job(job_id, scheduled => false) IS NOT NULL
                       FROM timescaledb_information.jobs WHERE proc_name = 'policy_retention'" \
  "$(url "$TSL_PORT" metrics)" >/dev/null
notes=$("$PG_LASWELL_MCP" --call planMigration --args '{"spec":{"laswell_spec_version":1,
   "id":"unrelated","description":"an unrelated schema",
   "intents":[{"kind":"create_schema","schema":"other","comment":"Unrelated."}]},
   "skipTrustChecks":true}' "$(url "$TSL_PORT" metrics)" \
  | python3 -c 'import json,sys
for a in json.load(sys.stdin).get("advisories", []): print("  note:", a)')
echo "$notes"
echo "$notes" | grep -q "retention policy differs from what an applied specification declared: the job is paused" || {
  echo "  The paused policy was not noticed."
  exit 1
}
psql -X -q -t -A -c "SELECT alter_job(job_id, scheduled => true) IS NOT NULL
                       FROM timescaledb_information.jobs WHERE proc_name = 'policy_retention'" \
  "$(url "$TSL_PORT" metrics)" >/dev/null
