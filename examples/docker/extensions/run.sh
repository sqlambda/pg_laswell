#!/usr/bin/env bash
# pgvector and pg_cron: one repository, two ways a server can keep pg_cron.
#
# pg_cron is installed in ONE database per server, the one cron.database_name
# names. Most servers keep it in `postgres` and schedule jobs for application
# databases from there; some keep it in the application database itself. The
# three signed specifications here do not choose: 0020 and 0030 target the
# connection named `cron`, and each configuration says where that is --
# `postgres` on one server, `docs` on the other. The same bytes apply to both.
#
#   docker compose -f examples/docker/extensions/compose.yml up -d --build
#   ./examples/docker/extensions/run.sh
#   docker compose -f examples/docker/extensions/compose.yml down -v
#
# Needs a binary built with the pg_cron and pgvector modules; the released
# package is one.
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
. "$here/../lib.sh"
need_binaries

CENTRAL_PORT=${CENTRAL_PORT:-55450}   # cron.database_name = postgres
LOCAL_PORT=${LOCAL_PORT:-55451}       # cron.database_name = docs
for p in "$CENTRAL_PORT" "$LOCAL_PORT"; do wait_for "$p"; done

say "1. Which binary is this"
run "$PG_LASWELL_MCP" --version
for m in pg_cron pgvector; do
  "$PG_LASWELL_MCP" --version | grep -q "^modules:.*$m" || {
    echo "  This binary was built without the $m module. Build one with"
    echo "  -DPGLASWELL_MODULES=\"citus;pg_cron;pgvector\"."
    exit 1
  }
done

say "2. Where each server keeps pg_cron"
for p in "$CENTRAL_PORT" "$LOCAL_PORT"; do
  echo "  port $p: cron.database_name = $(psql -X -t -A -c 'SHOW cron.database_name' "$(url "$p" postgres)")"
done

# A clean start on each server: a fresh `docs` (the application database),
# and pg_cron and the ledger gone from `postgres`, which cannot be dropped and
# recreated like `docs` can.
for p in "$CENTRAL_PORT" "$LOCAL_PORT"; do
  recreate_db "$p" docs
  psql -X -q -c "DROP EXTENSION IF EXISTS pg_cron CASCADE" -c "DROP SCHEMA IF EXISTS laswell CASCADE" \
    "$(url "$p" postgres)" >/dev/null 2>&1 || true
done
# A ledger wherever a specification is applied: docs on both, and postgres on
# the server where the `cron` connection points there.
kid=$(install_ledger "$CENTRAL_PORT" docs "$here/keys" ext)
install_ledger "$CENTRAL_PORT" postgres "$here/keys" ext >/dev/null
install_ledger "$LOCAL_PORT" docs "$here/keys" ext >/dev/null

build=$here/.build
rm -rf "$build"; mkdir -p "$build"
cp "$here/migrations"/*.json "$build/"
sign_dir "$build" "$here/keys/ext.key.pem" "$kid"

write_config() {  # write_config <file> <port> <cron dbname>
  { for section in "app docs" "cron $3"; do
      set -- "$1" "$2" "$3" $section
      echo "[$4]"
      echo "host     = 127.0.0.1"
      echo "port     = $2"
      echo "dbname   = $5"
      echo "user     = postgres"
      echo "password = $PGPASSWORD_DEFAULT"
      echo
    done
  } > "$1"
  chmod 600 "$1"
}
central_cfg=$here/central.ini; local_cfg=$here/local.ini
write_config "$central_cfg" "$CENTRAL_PORT" postgres
write_config "$local_cfg" "$LOCAL_PORT" docs
echo "  central.ini: [cron] dbname = postgres     local.ini: [cron] dbname = docs"
echo "  (configuration, not specification: the signed files name only 'cron')"

jobs_on() {  # jobs_on <port> <cron db>
  psql -X -q -c "\\pset border 2" -c \
    "SELECT jobname, schedule, database, username, active FROM cron.job ORDER BY jobname" \
    "$(url "$1" "$2")"
}

say "3. pg_cron in postgres: the job is scheduled from there, to run in docs"
run "$PG_LASWELL" --config "$central_cfg" --repo "$build" --status
run "$PG_LASWELL" --config "$central_cfg" --repo "$build"
jobs_on "$CENTRAL_PORT" postgres

say "4. pg_cron in docs: the same signed bytes, scheduled where the table is"
run "$PG_LASWELL" --config "$local_cfg" --repo "$build"
jobs_on "$LOCAL_PORT" docs

say "5. A job in the wrong database is refused before anything runs"
echo "  The same intent, in a specification targeting docs on the server that"
echo "  keeps pg_cron in postgres:"
"$PG_LASWELL_MCP" --call planMigration --args '{"spec":{"laswell_spec_version":1,
   "id":"misplaced","description":"a job scheduled from docs",
   "intents":[{"kind":"pg_cron_schedule","name":"purge-expired-documents",
     "schedule":"0 3 * * *","command":"DELETE FROM public.documents WHERE expires_at < now()"}]},
   "skipTrustChecks":true}' "$(url "$CENTRAL_PORT" docs)" 2>&1 \
  | python3 -c 'import json,sys; [print("  refused:", c) for c in json.load(sys.stdin)["conflicts"]]' \
  || true   # a refused plan exits 1, which is the point being shown

say "6. And an index pgvector would refuse, before a concurrent build starts"
"$PG_LASWELL_MCP" --call planMigration --args '{"spec":{"laswell_spec_version":1,
   "id":"too-wide","description":"an embedding wider than hnsw indexes",
   "intents":[{"kind":"add_column","schema":"public","table":"documents","column":"wide",
     "type":"vector(3072)","nullable":true,"comment":"A wider embedding."},
     {"kind":"create_index","schema":"public","table":"documents","name":"documents_wide_idx",
     "method":"hnsw","columns":[{"name":"wide","opclass":"vector_cosine_ops"}],
     "comment":"Nearest by the wide embedding."}]},
   "skipTrustChecks":true}' "$(url "$CENTRAL_PORT" docs)" 2>&1 \
  | python3 -c 'import json,sys; [print("  refused:", c) for c in json.load(sys.stdin)["conflicts"]]' \
  || true   # a refused plan exits 1, which is the point being shown

say "7. A job edited by hand is noticed on the next plan, whatever it is for"
echo "  Someone moves the purge to 05:00 with cron.alter_job, outside any"
echo "  specification. The ledger says 0030 declared 03:00, so the next plan"
echo "  against that database says so -- here, a plan for an unrelated schema:"
psql -X -q -c "SELECT cron.alter_job(jobid, schedule => '0 5 * * *') FROM cron.job
                WHERE jobname = 'purge-expired-documents'" "$(url "$CENTRAL_PORT" postgres)" >/dev/null
"$PG_LASWELL_MCP" --call planMigration --args '{"spec":{"laswell_spec_version":1,
   "id":"unrelated","description":"an unrelated change",
   "intents":[{"kind":"create_schema","schema":"reporting","comment":"Reports."}]},
   "skipTrustChecks":true}' "$(url "$CENTRAL_PORT" postgres)" 2>&1 \
  | python3 -c 'import json,sys; [print("  note:", a) for a in json.load(sys.stdin).get("advisories", [])]'
psql -X -q -c "SELECT cron.alter_job(jobid, schedule => '0 3 * * *') FROM cron.job
                WHERE jobname = 'purge-expired-documents'" "$(url "$CENTRAL_PORT" postgres)" >/dev/null
echo "  (put back to 03:00, so the run below has nothing to say)"

say "8. Run both again: nothing to do"
run "$PG_LASWELL" --config "$central_cfg" --repo "$build"
run "$PG_LASWELL" --config "$local_cfg" --repo "$build"
