#!/usr/bin/env bash
#
# The pg_cron and pgvector modules against real servers.
#
# Their refusals are unit-tested against hand-written readings, which proves
# the decision. Two things only a server can say are checked here:
#
#   1. that each refusal pre-empts an error the server REALLY raises -- the
#      refused statement is also run by hand, and the server's own message must
#      be the one the refusal quotes. A pgvector or pg_cron release that changes
#      a limit or lifts a restriction fails this, instead of leaving a refusal
#      that refuses what now works;
#   2. that what is NOT refused is accepted: the dry run executes it and rolls
#      it back, and comes back clean.
#
# Needs the two servers of examples/docker/extensions/compose.yml:
#   EXT_CENTRAL_URL  pg_cron kept in postgres  (postgresql://postgres:laswell@127.0.0.1:55450)
#   EXT_LOCAL_URL    pg_cron kept in docs      (postgresql://postgres:laswell@127.0.0.1:55451)
# Server URLs without a database name. SKIPs loudly without them.
set -uo pipefail

BIN="${1:-}"
MCP="${2:-}"
CENTRAL="${EXT_CENTRAL_URL:-}"
LOCAL="${EXT_LOCAL_URL:-}"
export PSQLRC=/dev/null

if [ -z "$BIN" ] || [ -z "$MCP" ]; then
  echo "usage: extensions-tests.sh <pg_laswell> <pg_laswell_mcp>" >&2
  exit 2
fi
for m in pg_cron pgvector; do
  if ! "$MCP" --version 2>/dev/null | grep -q "^modules:.*$m"; then
    echo "SKIP: this binary was built without the $m module"
    exit 0
  fi
done
if [ -z "$CENTRAL" ] || [ -z "$LOCAL" ]; then
  echo "SKIP: EXT_CENTRAL_URL and EXT_LOCAL_URL are unset; no servers to test against."
  echo "      docker compose -f examples/docker/extensions/compose.yml up -d --build"
  exit 0
fi

pass=0; fail=0
ok()  { echo "  ok   $1"; pass=$((pass+1)); }
bad() { echo "  FAIL $1"; echo "       $2" | head -c 2000; echo; fail=$((fail+1)); }
q()   { psql -X -q -A -t -v ON_ERROR_STOP=1 "$1" -c "$2" 2>&1; }

# plan <url> <intents json> -- planMigration, with its dry run.
plan() {
  "$MCP" --call planMigration --args "{\"spec\":{\"laswell_spec_version\":1,
     \"id\":\"ext-test\",\"description\":\"extensions test\",\"intents\":[$2]},
     \"skipTrustChecks\":true}" "$1" 2>&1
}
clean() {  # clean <label> <url> <intents>
  local out; out=$(plan "$2" "$3")
  if echo "$out" | grep -q '"ok":true' && ! echo "$out" | grep -q '"problems"'; then
    ok "$1"
  else
    bad "$1" "$out"
  fi
}
# refused <label> <url> <intents> <needle> [<raw sql> <server message>]
# With the last two, the raw statement is run as well, and the server must
# raise the message the refusal quotes.
refused() {
  local out; out=$(plan "$2" "$3")
  if ! echo "$out" | grep -q '"ok":false' || ! echo "$out" | grep -qF -- "$4"; then
    bad "$1 should be refused, naming: $4" "$out"
    return
  fi
  if [ -n "${5:-}" ]; then
    local raw; raw=$(q "$2" "BEGIN; $5; ROLLBACK;")
    if ! echo "$raw" | grep -qF -- "$6"; then
      bad "$1: the server no longer raises what the refusal quotes" \
          "expected [$6], got [$raw]"
      return
    fi
  fi
  ok "$1"
}

# --- pgvector -----------------------------------------------------------
echo "pgvector: what is refused is what pgvector raises"
q "$CENTRAL/postgres" "DROP DATABASE IF EXISTS ext_test WITH (FORCE)" >/dev/null
q "$CENTRAL/postgres" "CREATE DATABASE ext_test" >/dev/null
V="$CENTRAL/ext_test"
q "$V" "CREATE EXTENSION vector" >/dev/null
q "$V" "CREATE TABLE public.v (id bigint PRIMARY KEY, e vector(128), wide vector(2001),
        h halfvec(4001), s sparsevec(1000), u vector, t text)" >/dev/null
# Server messages are matched in English; the cluster's lc_messages decides.
q "$V" "ALTER DATABASE ext_test SET lc_messages = 'C'" >/dev/null

idx() {  # idx <name> <method> <columns json> [with json] [unique]
  local w=""; [ -n "${4:-}" ] && w=",\"with\":$4"
  local u=""; [ -n "${5:-}" ] && u=",\"unique\":true"
  echo "{\"kind\":\"create_index\",\"schema\":\"public\",\"table\":\"v\",\"name\":\"$1\",
         \"method\":\"$2\",\"columns\":$3$w$u,\"comment\":\"t\"}"
}

clean "an hnsw index within the limits, with storage parameters, plans and its dry run builds it" \
  "$V" "$(idx v_e_hnsw hnsw '[{"name":"e","opclass":"vector_cosine_ops"}]' '{"m":24,"ef_construction":96}')"
clean "an ivfflat index on vector uses the default class" \
  "$V" "$(idx v_e_ivf ivfflat '["e"]' '{"lists":10}')"
refused "vector wider than 2000 on hnsw" \
  "$V" "$(idx v_w hnsw '[{"name":"wide","opclass":"vector_l2_ops"}]')" \
  "more than 2000 dimensions for hnsw index" \
  "CREATE INDEX ON public.v USING hnsw (wide vector_l2_ops)" \
  "column cannot have more than 2000 dimensions for hnsw index"
refused "halfvec wider than 4000 on ivfflat" \
  "$V" "$(idx v_h ivfflat '[{"name":"h","opclass":"halfvec_l2_ops"}]')" \
  "more than 4000 dimensions for ivfflat index" \
  "CREATE INDEX ON public.v USING ivfflat (h halfvec_l2_ops)" \
  "column cannot have more than 4000 dimensions for ivfflat index"
refused "a vector column without dimensions" \
  "$V" "$(idx v_u hnsw '[{"name":"u","opclass":"vector_l2_ops"}]')" \
  "column does not have dimensions" \
  "CREATE INDEX ON public.v USING hnsw (u vector_l2_ops)" \
  "column does not have dimensions"
refused "an hnsw column with no operator class" \
  "$V" "$(idx v_n hnsw '["e"]')" \
  "names no operator class" \
  "CREATE INDEX ON public.v USING hnsw (e)" \
  "has no default operator class for access method \"hnsw\""
refused "an operator class for another type" \
  "$V" "$(idx v_m hnsw '[{"name":"e","opclass":"halfvec_l2_ops"}]')" \
  "does not accept data type vector" \
  "CREATE INDEX ON public.v USING hnsw (e halfvec_l2_ops)" \
  "operator class \"halfvec_l2_ops\" does not accept data type vector"
refused "sparsevec on ivfflat" \
  "$V" "$(idx v_s ivfflat '[{"name":"s","opclass":"sparsevec_l2_ops"}]')" \
  "which ivfflat cannot index" \
  "CREATE INDEX ON public.v USING ivfflat (s sparsevec_l2_ops)" \
  "operator class \"sparsevec_l2_ops\" does not exist for access method \"ivfflat\""
refused "a unique hnsw index" \
  "$V" "$(idx v_q hnsw '[{"name":"e","opclass":"vector_l2_ops"}]' '' unique)" \
  "does not support unique indexes" \
  "CREATE UNIQUE INDEX ON public.v USING hnsw (e vector_l2_ops)" \
  "access method \"hnsw\" does not support unique indexes"
refused "a storage parameter hnsw does not have" \
  "$V" "$(idx v_f hnsw '[{"name":"e","opclass":"vector_l2_ops"}]' '{"fillfactor":70}')" \
  "unrecognized parameter" \
  "CREATE INDEX ON public.v USING hnsw (e vector_l2_ops) WITH (fillfactor = 70)" \
  "unrecognized parameter \"fillfactor\""
refused "m above its bound" \
  "$V" "$(idx v_b hnsw '[{"name":"e","opclass":"vector_l2_ops"}]' '{"m":101,"ef_construction":400}')" \
  "between 2 and 100" \
  "CREATE INDEX ON public.v USING hnsw (e vector_l2_ops) WITH (m = 101, ef_construction = 400)" \
  "value 101 out of bounds for option \"m\""
refused "m = 40 with the default ef_construction" \
  "$V" "$(idx v_d hnsw '[{"name":"e","opclass":"vector_l2_ops"}]' '{"m":40}')" \
  "less than 2 * m" \
  "CREATE INDEX ON public.v USING hnsw (e vector_l2_ops) WITH (m = 40)" \
  "ef_construction must be greater than or equal to 2 * m"

echo "pgvector: storage parameters are part of what makes two indexes the same"
q "$V" "CREATE INDEX v_by_hand ON public.v USING hnsw (e vector_cosine_ops) WITH (m = 16)" >/dev/null
out=$(plan "$V" "$(idx v_named hnsw '[{"name":"e","opclass":"vector_cosine_ops"}]' '{"m":16}')")
if echo "$out" | grep -q 'RENAME TO \\"v_named\\"'; then
  ok "the same parameters, read back from reloptions, adopt the hand-built index by renaming it"
else
  bad "an equivalent index with the same parameters should be renamed" "$out"
fi
out=$(plan "$V" "$(idx v_named hnsw '[{"name":"e","opclass":"vector_cosine_ops"}]' '{"m":24,"ef_construction":96}')")
if echo "$out" | grep -q 'different storage parameters' && echo "$out" | grep -q 'CREATE INDEX'; then
  ok "other parameters build a second index, and say so"
else
  bad "different parameters should build, with a warning" "$out"
fi

echo "pgvector: an expression key that is a cast is checked as the type it casts to"
clean "a vector wider than 2000, indexed as halfvec through a cast" \
  "$V" "$(idx v_cast hnsw '[{"expression":"wide::halfvec(2001)","opclass":"halfvec_l2_ops"}]')"
refused "a cast to a halfvec wider than 4000" \
  "$V" "$(idx v_castw hnsw '[{"expression":"h::halfvec(4001)","opclass":"halfvec_l2_ops"}]')" \
  "more than 4000 dimensions" \
  "CREATE INDEX ON public.v USING hnsw ((h::halfvec(4001)) halfvec_l2_ops)" \
  "column cannot have more than 4000 dimensions for hnsw index"

echo "pgvector: advisories, from the server's own readings, outside the digest"
out=$(plan "$V" "$(idx v_ivf_empty ivfflat '["e"]')")
if echo "$out" | grep -q '"advisories"' && echo "$out" | grep -q 'reads as empty'; then
  ok "an ivfflat index on an empty table is an advisory"
else
  bad "an empty table should produce the ivfflat advisory" "$out"
fi
q "$V" "INSERT INTO public.v (id, e) SELECT g, (SELECT array_agg(random())::vector(128)
        FROM generate_series(1,128) WHERE g > 0) FROM generate_series(1,3000) g" >/dev/null
q "$V" "ANALYZE public.v" >/dev/null
q "$CENTRAL/postgres" "ALTER DATABASE ext_test SET maintenance_work_mem = '1MB'" >/dev/null
# The server's own setting is 1 MB; the graph needs about 4 MB. With no ceiling
# configured, the build is raised to what it needs, within shared_buffers
# divided by max_concurrent_jobs (128 MB / 2 here) -- so the step says so, and
# the dry run, which runs it with that setting, comes back clean.
out=$(plan "$V" "$(idx v_mem hnsw '[{"name":"e","opclass":"vector_l2_ops"}]')")
if echo "$out" | grep -q 'maintenance_work_mem raised to 4MB for this build' \
   && echo "$out" | grep -q '"memory_wanted_by":"pgvector"' && ! echo "$out" | grep -q '"problems"'; then
  ok "an hnsw build is given the memory its graph needs, sized from rows, dims and m"
else
  bad "3000 rows of vector(128) should be raised to about 4 MB from 1 MB" "$out"
fi
raw=$(q "$V" "SET maintenance_work_mem = '1MB'; SET max_parallel_maintenance_workers = 0;
              BEGIN; CREATE INDEX ON public.v USING hnsw (e vector_l2_ops); ROLLBACK;")
if echo "$raw" | grep -q 'hnsw graph no longer fits into maintenance_work_mem'; then
  ok "and pgvector agrees the graph does not fit"
else
  bad "pgvector should report the graph outgrowing maintenance_work_mem" "$raw"
fi

# --- pg_cron ------------------------------------------------------------
echo "pg_cron: both layouts"
for url in "$CENTRAL/postgres" "$LOCAL/docs"; do
  q "$url" "CREATE EXTENSION IF NOT EXISTS pg_cron" >/dev/null
  q "$url" "SELECT cron.unschedule(jobid) FROM cron.job WHERE jobname LIKE 'ext-test-%'" >/dev/null
done
q "$LOCAL/postgres" "ALTER DATABASE docs SET lc_messages = 'C'" >/dev/null
q "$CENTRAL/postgres" "ALTER DATABASE postgres SET lc_messages = 'C'" >/dev/null
job() {  # job <name> [extra json fields]
  echo "{\"kind\":\"pg_cron_schedule\",\"name\":\"ext-test-$1\",\"schedule\":\"*/5 * * * *\",
         \"command\":\"SELECT 1\"${2:+,$2}}"
}
gone() {  # gone <url> <name> -- the dry run must leave no job behind
  [ "$(q "$1" "SELECT count(*) FROM cron.job WHERE jobname = 'ext-test-$2'")" = "0" ]
}

clean "pg_cron in docs: a job for docs is a plain schedule, rehearsed by execution" \
  "$LOCAL/docs" "$(job local)"
gone "$LOCAL/docs" local && ok "and the rehearsal left nothing in cron.job" \
  || bad "the rehearsal should leave no job" "$(q "$LOCAL/docs" "SELECT * FROM cron.job")"
clean "pg_cron in postgres: a job for docs is scheduled from postgres" \
  "$CENTRAL/postgres" "$(job central '"database":"ext_test"')"
gone "$CENTRAL/postgres" central && ok "and the rehearsal left nothing in cron.job" \
  || bad "the rehearsal should leave no job" "$(q "$CENTRAL/postgres" "SELECT * FROM cron.job")"
refused "pg_cron in postgres: a job scheduled from another database" \
  "$V" "$(job wrong)" "keeps its jobs in database postgres"
refused "creating pg_cron outside cron.database_name" \
  "$V" '{"kind":"create_extension","name":"pg_cron"}' \
  "can only create extension in database postgres" \
  "CREATE EXTENSION pg_cron" "can only create extension in database postgres"

out=$(plan "$LOCAL/docs" "$(job bad '"schedule":"not a schedule"')")
if echo "$out" | grep -q 'invalid schedule'; then
  ok "a schedule pg_cron cannot read is caught by the dry run"
else
  bad "the dry run should run cron.schedule and report the invalid schedule" "$out"
fi

q "$LOCAL/docs" "SELECT cron.schedule('ext-test-kept', '*/5 * * * *', 'SELECT 1')" >/dev/null
out=$(plan "$LOCAL/docs" "$(job kept)")
if echo "$out" | grep -q '"action":"satisfied"'; then
  ok "a job already scheduled as declared is satisfied"
else
  bad "an identical job should be satisfied" "$out"
fi
clean "unscheduling it plans, and its dry run runs cron.unschedule" \
  "$LOCAL/docs" '{"kind":"pg_cron_unschedule","name":"ext-test-kept"}'
q "$LOCAL/docs" "SELECT cron.unschedule('ext-test-kept')" >/dev/null

echo "pg_cron: the command is checked without being run"
q "$LOCAL/docs" "CREATE TABLE IF NOT EXISTS public.cmd_t (id int, ts timestamptz)" >/dev/null
q "$LOCAL/docs" "CREATE OR REPLACE PROCEDURE public.cmd_purge(days int) LANGUAGE sql AS \$\$ DELETE FROM public.cmd_t WHERE ts < now() - make_interval(days => days) \$\$" >/dev/null
q "$CENTRAL/postgres" "CREATE DATABASE cmd_db" >/dev/null 2>&1 || true
q "$CENTRAL/cmd_db" "CREATE TABLE IF NOT EXISTS public.cmd_t (id int, ts timestamptz)" >/dev/null
cmd() {  # cmd <name> <command> [extra]
  echo "{\"kind\":\"pg_cron_schedule\",\"name\":\"ext-test-$1\",\"schedule\":\"0 3 * * *\",
         \"command\":\"$2\"${3:+,$3}}"
}
problem() {  # problem <label> <url> <intent> <needle>
  local out; out=$(plan "$2" "$3")
  if echo "$out" | grep -q '"problems"' && echo "$out" | grep -qF -- "$4"; then ok "$1"
  else bad "$1 -- expected the dry run to report: $4" "$out"; fi
}
clean "a command that can run passes, in the job's own database" \
  "$LOCAL/docs" "$(cmd c1 'CALL public.cmd_purge(7)')"
problem "a procedure that does not exist is found before it is scheduled" \
  "$LOCAL/docs" "$(cmd c2 'CALL public.cmd_nope(7)')" "procedure public.cmd_nope(integer) does not exist"
problem "a table that does not exist" \
  "$LOCAL/docs" "$(cmd c3 'DELETE FROM public.cmd_nope')" 'relation \"public.cmd_nope\" does not exist'
problem "a syntax error" \
  "$LOCAL/docs" "$(cmd c4 'SELEC 1')" 'syntax error at or near \"SELEC\"'
clean "a job that runs in another database is checked there, over a second connection" \
  "$CENTRAL/postgres" "$(cmd c5 'DELETE FROM public.cmd_t WHERE ts < now()' '"database":"cmd_db"')"
problem "and a command that cannot work there is found there" \
  "$CENTRAL/postgres" "$(cmd c6 'DELETE FROM public.cmd_nope' '"database":"cmd_db"')" "in database cmd_db"
clean "validate_command false schedules it unchecked" \
  "$LOCAL/docs" "$(cmd c7 'CALL public.cmd_nope(7)' '"validate_command":false')"
[ "$(q "$LOCAL/docs" "SELECT count(*) FROM cron.job WHERE jobname LIKE 'ext-test-c%'")" = 0 ] \
  && ok "and none of it left a job, or a procedure, behind" \
  || bad "the checks should leave nothing" "$(q "$LOCAL/docs" "SELECT jobname FROM cron.job")"
q "$CENTRAL/postgres" "DROP DATABASE cmd_db WITH (FORCE)" >/dev/null

echo "pg_cron: a role that is not a superuser"
q "$CENTRAL/postgres" "DROP ROLE IF EXISTS ext_runner" >/dev/null
q "$CENTRAL/postgres" "CREATE ROLE ext_runner LOGIN PASSWORD 'laswell'" >/dev/null
q "$CENTRAL/postgres" "GRANT USAGE ON SCHEMA cron TO ext_runner" >/dev/null
RUNNER=$(echo "$CENTRAL" | sed 's#//postgres:laswell@#//ext_runner:laswell@#')
# The finding this section exists for: cron.database_name is readable only
# with pg_read_all_settings, and the pg_cron reading is taken for EVERY plan
# on a server that loads it. An ordinary specification must still plan.
q "$CENTRAL/postgres" "GRANT CREATE ON SCHEMA public TO ext_runner" >/dev/null
q "$V" "GRANT CREATE, USAGE ON SCHEMA public TO ext_runner" >/dev/null
clean "a role that cannot read cron.database_name still plans an ordinary specification" \
  "$RUNNER/ext_test" '{"kind":"create_table","schema":"public","table":"r_plain","comment":"t",
    "primary_key":["id"],"columns":[{"name":"id","type":"bigint","nullable":false,"comment":"t"}]}'
refused "and a job from it names the grant it needs" \
  "$RUNNER/ext_test" "$(job unreadable)" "GRANT pg_read_all_settings"
refused "schedule_in_database without the grant" \
  "$RUNNER/postgres" "$(job grant '"database":"ext_test"')" \
  "not granted to PUBLIC" \
  "SELECT cron.schedule_in_database('ext-test-raw', '* * * * *', 'SELECT 1', 'ext_test')" \
  "permission denied for function schedule_in_database"
refused "a job for another role" \
  "$RUNNER/postgres" "$(job other '"username":"postgres"')" \
  "must be superuser to create a job for another role"

echo "pg_cron: a job for another role, scheduled by a superuser"
q "$CENTRAL/postgres" "DROP ROLE IF EXISTS ext_nologin; CREATE ROLE ext_nologin NOLOGIN" >/dev/null
q "$CENTRAL/postgres" "DROP ROLE IF EXISTS ext_noconn; CREATE ROLE ext_noconn LOGIN" >/dev/null
q "$CENTRAL/postgres" "REVOKE CONNECT ON DATABASE ext_test FROM PUBLIC" >/dev/null
for case in "ext_absent|there is no such role|role \"ext_absent\" does not exist" \
            "ext_nologin|LOGIN attribute|role \"ext_nologin\" can not log in" \
            "ext_noconn|does not have CONNECT privilege on ext_test|User ext_noconn does not have CONNECT privilege on ext_test"; do
  IFS='|' read -r role needle server <<<"$case"
  refused "a job run as $role" \
    "$CENTRAL/postgres" "$(job role '"database":"ext_test","username":"'"$role"'"')" "$needle" \
    "SELECT cron.schedule_in_database('ext-test-raw', '0 3 * * *', 'SELECT 1', 'ext_test', '$role')" \
    "$server"
done
q "$CENTRAL/postgres" "GRANT CONNECT ON DATABASE ext_test TO PUBLIC" >/dev/null
q "$CENTRAL/postgres" "DROP ROLE ext_nologin; DROP ROLE ext_noconn" >/dev/null
q "$CENTRAL/postgres" "REVOKE USAGE ON SCHEMA cron FROM ext_runner" >/dev/null
q "$CENTRAL/postgres" "REVOKE CREATE ON SCHEMA public FROM ext_runner" >/dev/null
q "$V" "REVOKE CREATE, USAGE ON SCHEMA public FROM ext_runner" >/dev/null
q "$CENTRAL/postgres" "DROP ROLE ext_runner" >/dev/null
q "$CENTRAL/postgres" "DROP DATABASE ext_test WITH (FORCE)" >/dev/null

echo "pg_cron: the zone a schedule is read in, and how the job connects"
out=$(plan "$LOCAL/docs" "$(cmd zone 'SELECT 1')")
zone=$(q "$LOCAL/docs" "SHOW cron.timezone")
echo "$out" | grep -qF "the schedule is read in $zone (cron.timezone)" \
  && ok "the step names cron.timezone ($zone)" || bad "the step should name the zone $zone" "$out"
q "$CENTRAL/postgres" "CREATE DATABASE ext_conn" >/dev/null
# No job has run in ext_conn: nothing is known, and the plan says so.
out=$(plan "$CENTRAL/postgres" "$(cmd conn-unknown 'SELECT 1' '"database":"ext_conn"')")
echo "$out" | grep -qF "whether pg_cron can connect is not known here" \
  && ok "with no history, the connection is reported as unknown" \
  || bad "expected the unknown-connection note" "$out"
# Let a job run there, then ask again. Unscheduled by id: by name, pg_cron
# looks only among the caller's own jobs.
q "$CENTRAL/postgres" "SELECT cron.schedule_in_database('ext-test-conn-probe', '2 seconds', 'SELECT 1', 'ext_conn')" >/dev/null
for _ in $(seq 1 20); do
  [ "$(q "$CENTRAL/postgres" "SELECT count(*) FROM cron.job_run_details WHERE database = 'ext_conn' AND status = 'succeeded'")" -gt 0 ] && break
  sleep 1
done
q "$CENTRAL/postgres" "SELECT cron.unschedule(jobid) FROM cron.job WHERE jobname = 'ext-test-conn-probe'" >/dev/null
out=$(plan "$CENTRAL/postgres" "$(cmd conn-known 'SELECT 1' '"database":"ext_conn"')")
echo "$out" | grep -qF "a job has connected to ext_conn as postgres" \
  && ok "after a job has run there, the history is the evidence" \
  || bad "expected the connected note" "$out"
q "$CENTRAL/postgres" "DELETE FROM cron.job_run_details WHERE database = 'ext_conn'" >/dev/null
q "$CENTRAL/postgres" "DROP DATABASE ext_conn WITH (FORCE)" >/dev/null

echo
echo "extensions: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
