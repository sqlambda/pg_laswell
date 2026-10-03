#!/usr/bin/env bash
#
# The TimescaleDB module against real servers: both editions, and the oldest
# supported line.
#
# What a fixed reading cannot prove, checked here:
#   1. each refusal pre-empts an error the server REALLY raises -- the refused
#      statement is also run by hand, and must fail with the quoted message;
#   2. what the planner emits for an index on a hypertable is accepted: the SQL
#      is taken out of the plan and run by hand, because a per-chunk build
#      cannot run in the dry run's transaction;
#   3. what is not refused is accepted by the dry run, which executes it.
#
#   TS_TSL_URL     Timescale License build   (postgresql://postgres:laswell@127.0.0.1:55470)
#   TS_APACHE_URL  Apache build (-oss)       (postgresql://postgres:laswell@127.0.0.1:55471)
#   TS_OLDEST_URL  2.28.3 on PostgreSQL 15   (postgresql://postgres:laswell@127.0.0.1:55472)
# Server URLs without a database name. SKIPs loudly without them.
set -uo pipefail

BIN="${1:-}"
MCP="${2:-}"
export PSQLRC=/dev/null

if [ -z "$BIN" ] || [ -z "$MCP" ]; then
  echo "usage: timescaledb-tests.sh <pg_laswell> <pg_laswell_mcp>" >&2
  exit 2
fi
if ! "$MCP" --version 2>/dev/null | grep -q '^modules:.*timescaledb'; then
  echo "SKIP: this binary was built without the timescaledb module"
  exit 0
fi
if [ -z "${TS_TSL_URL:-}" ] || [ -z "${TS_APACHE_URL:-}" ] || [ -z "${TS_OLDEST_URL:-}" ]; then
  echo "SKIP: TS_TSL_URL, TS_APACHE_URL and TS_OLDEST_URL are unset."
  echo "      docker compose -f examples/docker/timescale/compose.yml up -d"
  exit 0
fi

pass=0; fail=0
ok()  { echo "  ok   $1"; pass=$((pass+1)); }
bad() { echo "  FAIL $1"; echo "       $2" | head -c 2000; echo; fail=$((fail+1)); }
q()   { psql -X -q -A -t -v ON_ERROR_STOP=1 "$1" -c "$2" 2>&1; }

plan() {  # plan <url> <intents json>
  "$MCP" --call planMigration --args "{\"spec\":{\"laswell_spec_version\":1,
     \"id\":\"ts-test\",\"description\":\"timescaledb test\",\"intents\":[$2]},
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
refused() {
  local out; out=$(plan "$2" "$3")
  if ! echo "$out" | grep -q '"ok":false' || ! echo "$out" | grep -qF -- "$4"; then
    bad "$1 should be refused, naming: $4" "$out"
    return
  fi
  if [ -n "${5:-}" ]; then
    local raw; raw=$(q "$2" "BEGIN; $5; ROLLBACK;")
    if ! echo "$raw" | grep -qF -- "$6"; then
      bad "$1: the server no longer raises what the refusal quotes" "expected [$6], got [$raw]"
      return
    fi
  fi
  ok "$1"
}
# The SQL of a plan's steps, one statement per line.
plan_sql() {
  plan "$1" "$2" | python3 -c 'import json,sys
d=json.load(sys.stdin)
for s in d.get("steps", []):
    for q in s.get("sql", []): print(q)'
}

idx() {  # idx <name> <columns json> [unique]
  echo "{\"kind\":\"create_index\",\"schema\":\"public\",\"table\":\"m\",\"name\":\"$1\",
         \"columns\":$2${3:+,\"unique\":true},\"comment\":\"t\"}"
}

for edition in tsl apache oldest; do
  case $edition in
    tsl) SERVER=$TS_TSL_URL ;; apache) SERVER=$TS_APACHE_URL ;; oldest) SERVER=$TS_OLDEST_URL ;;
  esac
  echo "timescaledb ($edition): $(q "$SERVER/postgres" "SELECT version()" | cut -d' ' -f1-2)"
  q "$SERVER/postgres" "DROP DATABASE IF EXISTS ts_test WITH (FORCE)" >/dev/null
  q "$SERVER/postgres" "CREATE DATABASE ts_test" >/dev/null
  U="$SERVER/ts_test"
  q "$U" "ALTER DATABASE ts_test SET lc_messages = 'C'" >/dev/null
  q "$U" "CREATE EXTENSION IF NOT EXISTS timescaledb" >/dev/null
  q "$U" "CREATE TABLE public.m (id bigint NOT NULL, ts timestamptz NOT NULL, dev int NOT NULL, v float8)" >/dev/null
  q "$U" "CREATE TABLE public.full_t (ts timestamptz NOT NULL, v int)" >/dev/null
  q "$U" "INSERT INTO public.full_t SELECT now() - (g || ' min')::interval, g FROM generate_series(1, 5000) g" >/dev/null
  q "$U" "ANALYZE public.full_t" >/dev/null

  hyper='{"kind":"timescaledb_create_hypertable","schema":"public","table":"m","time_column":"ts","chunk_time_interval":"1 day"}'
  clean "an empty table becomes a hypertable, rehearsed by execution" "$U" "$hyper"
  refused "a table with rows, without migrate_data" "$U" \
    '{"kind":"timescaledb_create_hypertable","schema":"public","table":"full_t","time_column":"ts"}' \
    "is not empty" \
    "SELECT create_hypertable('public.full_t', by_range('ts'))" \
    "table \"full_t\" is not empty"
  clean "with migrate_data, its rows move (and roll back)" "$U" \
    '{"kind":"timescaledb_create_hypertable","schema":"public","table":"full_t","time_column":"ts","migrate_data":true}'
  [ "$(q "$U" "SELECT count(*) FROM ONLY public.full_t")" = 5000 ] \
    && ok "the rehearsal left the rows where they were" \
    || bad "the rehearsal should roll the migration back" "$(q "$U" "SELECT count(*) FROM ONLY public.full_t")"

  q "$U" "SELECT create_hypertable('public.m', by_range('ts', INTERVAL '1 day'))" >/dev/null
  q "$U" "INSERT INTO public.m SELECT g, now() - (g || ' seconds')::interval, g % 10, random()
          FROM generate_series(1, 1200000) g" >/dev/null
  q "$U" "ANALYZE public.m" >/dev/null
  out=$(plan "$U" "$hyper")
  echo "$out" | grep -q '"action":"satisfied"' && ok "an existing hypertable is satisfied" \
    || bad "an existing hypertable should be satisfied" "$out"

  # The hazard. 1 200 000 rows is over 64 MiB -- the plain-build
  # ceiling only by the hypertable's own size: the parent reads 0 pages. (At
  # 400 000 rows, 23 MiB, a plain build is the right plan, and is what came out.)
  sql=$(plan_sql "$U" "$(idx m_dev '["dev"]')")
  if echo "$sql" | grep -q 'transaction_per_chunk' && ! echo "$sql" | grep -q CONCURRENTLY; then
    ok "an index on a large hypertable is planned per chunk, not concurrently"
  else
    bad "a large hypertable's index should be per chunk" "$sql"
  fi
  raw=$(echo "$sql" | grep '^CREATE INDEX' | psql -X -q "$U" 2>&1)
  [ -z "$raw" ] && ok "and the server builds it as planned" || bad "the planned per-chunk build should run" "$raw"
  raw=$(q "$U" "CREATE INDEX CONCURRENTLY m_cic ON public.m (v)")
  echo "$raw" | grep -qF "hypertables do not support concurrent index creation" \
    && ok "while the concurrent build it avoids is refused" \
    || bad "CIC on a hypertable should be refused" "$raw"

  refused "a unique index without the time column" "$U" "$(idx m_u '["id"]' 1)" \
    "cannot create a unique index without the column" \
    "CREATE UNIQUE INDEX m_u ON public.m (id)" \
    "cannot create a unique index without the column \"ts\""
  sql=$(plan_sql "$U" "$(idx m_uts '["id","ts"]' 1)")
  if echo "$sql" | grep -q '^CREATE UNIQUE INDEX "m_uts"' && ! echo "$sql" | grep -q 'CONCURRENTLY\|transaction_per_chunk'; then
    ok "a unique index with it is planned plain, the only build there is"
  else
    bad "a unique index on a hypertable should be a plain build" "$sql"
  fi
  raw=$(q "$U" "CREATE UNIQUE INDEX m_uts ON public.m (id, ts) WITH (timescaledb.transaction_per_chunk)")
  echo "$raw" | grep -qF "cannot use timescaledb.transaction_per_chunk with UNIQUE" \
    && ok "because the per-chunk build refuses UNIQUE" || bad "per-chunk UNIQUE should be refused" "$raw"

  sql=$(plan_sql "$U" '{"kind":"drop_index","schema":"public","table":"m","name":"m_dev"}')
  if echo "$sql" | grep -q '^DROP INDEX "public"."m_dev";'; then ok "a drop is plain"; else bad "a hypertable index drop should be plain" "$sql"; fi
  raw=$(q "$U" "DROP INDEX CONCURRENTLY public.m_dev")
  echo "$raw" | grep -qF "DROP INDEX CONCURRENTLY does not support dropping multiple objects" \
    && ok "because a concurrent drop is refused" || bad "DROP INDEX CONCURRENTLY should be refused" "$raw"

  if [ "$edition" = apache ]; then
    refused "the columnstore on the Apache build" "$U" \
      '{"kind":"timescaledb_set_columnstore","schema":"public","table":"m","segment_by":["dev"]}' \
      "license" \
      "ALTER TABLE public.m SET (timescaledb.enable_columnstore = true)" \
      'not supported under the current "apache" license'
    refused "a retention policy on the Apache build" "$U" \
      '{"kind":"timescaledb_add_retention_policy","schema":"public","table":"m","drop_after":"90 days","acknowledge_data_loss":true}' \
      "license" \
      "SELECT add_retention_policy('public.m', drop_after => INTERVAL '90 days')" \
      'is not supported under the current "apache" license'
    continue
  fi

  refused "a columnstore policy before the columnstore" "$U" \
    '{"kind":"timescaledb_add_columnstore_policy","schema":"public","table":"m","after":"7 days"}' \
    "columnstore not enabled" \
    "CALL add_columnstore_policy('public.m', after => INTERVAL '7 days')" \
    "columnstore not enabled on hypertable"
  cs='{"kind":"timescaledb_set_columnstore","schema":"public","table":"m","segment_by":["dev"],"order_by":["ts DESC"]}'
  pol='{"kind":"timescaledb_add_columnstore_policy","schema":"public","table":"m","after":"7 days"}'
  clean "the columnstore and its policy, in one specification" "$U" "$cs,$pol"
  clean "a retention policy" "$U" \
    '{"kind":"timescaledb_add_retention_policy","schema":"public","table":"m","drop_after":"90 days","acknowledge_data_loss":true}'
  cagg='{"kind":"timescaledb_create_continuous_aggregate","schema":"public","name":"m_hourly","query":"SELECT time_bucket('"'"'1 hour'"'"', ts) AS bucket, dev, avg(v) FROM public.m GROUP BY 1, 2"}'
  capol='{"kind":"timescaledb_add_continuous_aggregate_policy","schema":"public","name":"m_hourly","start_offset":"3 days","end_offset":"1 hour","schedule_interval":"1 hour"}'
  clean "a continuous aggregate and its refresh policy" "$U" "$cagg,$capol"

  # Apply the columnstore for real, then what it forbids.
  q "$U" "ALTER TABLE public.m SET (timescaledb.enable_columnstore = true, timescaledb.segmentby = 'dev', timescaledb.orderby = 'ts DESC')" >/dev/null
  out=$(plan "$U" "$cs")
  echo "$out" | grep -q '"action":"satisfied"' && ok "the columnstore, read back, is satisfied" \
    || bad "an identical columnstore should be satisfied" "$out"
  q "$U" "CALL add_columnstore_policy('public.m', after => INTERVAL '3 days')" >/dev/null
  sql=$(plan_sql "$U" "$pol")
  if echo "$sql" | grep -q remove_columnstore_policy && echo "$sql" | grep -q "add_columnstore_policy"; then
    ok "a policy with another interval is replaced"
  else
    bad "a changed policy should be removed and added" "$sql"
  fi
  refused "set_not_null with the columnstore" "$U" \
    '{"kind":"set_not_null","schema":"public","table":"m","column":"v"}' \
    "refuses VALIDATE CONSTRAINT" \
    "ALTER TABLE public.m ADD CONSTRAINT v_nn CHECK (v IS NOT NULL) NOT VALID; ALTER TABLE public.m VALIDATE CONSTRAINT v_nn" \
    "operation not supported on hypertables that have columnstore enabled"
  q "$U" "SELECT compress_chunk(c) FROM show_chunks('public.m', older_than => INTERVAL '2 days') c" >/dev/null
  refused "a type change with chunks in the columnstore" "$U" \
    '{"kind":"alter_column_type","schema":"public","table":"m","column":"v","type":"numeric"}' \
    "compressed chunks" \
    "ALTER TABLE public.m ALTER COLUMN v TYPE numeric" \
    "operation not supported on hypertables with compressed chunks"
done

echo
echo "timescaledb: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
