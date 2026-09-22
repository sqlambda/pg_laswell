#!/usr/bin/env bash
#
# What a fixed Observations literal cannot see.
#
# The Citus module's refusals are unit-tested against hand-written readings,
# which proves the DECISION and nothing about whether the statement the planner
# emits can actually be planned and run by Citus. Those are different questions,
# and the gap between them cost exactly one defect: the paced-walk guard's
# reference-table exception was correct, the walk ran correctly, and the dry run
# reported it as a failed plan because EXPLAIN GENERIC_PLAN is unsupported on
# any Citus table. A unit test could not have found that. This can.
#
# Needs a running Citus cluster. examples/docker/citus/compose.yml builds one;
# point CITUS_URL at its coordinator. SKIPs loudly when there is none, because a
# skip nobody notices is a test that silently stopped.
set -uo pipefail

BIN="${1:-}"
MCP="${2:-}"
CITUS_URL="${CITUS_URL:-}"
export PSQLRC=/dev/null

if [ -z "$BIN" ] || [ -z "$MCP" ]; then
  echo "usage: citus-tests.sh <pg_laswell> <pg_laswell_mcp>" >&2
  exit 2
fi
if ! "$MCP" --version 2>/dev/null | grep -q '^modules:.*citus'; then
  echo "SKIP: this binary was built without the citus module"
  echo "      cmake -DPGLASWELL_MODULES=citus"
  exit 0
fi
if [ -z "$CITUS_URL" ]; then
  echo "SKIP: CITUS_URL is unset; no Citus coordinator to test against."
  echo "      cd examples/docker/citus && docker compose up -d"
  echo "      CITUS_URL=postgresql://postgres:postgres@localhost:55440/app"
  exit 0
fi
if ! psql -qAt "$CITUS_URL" -c "SELECT 1 FROM pg_extension WHERE extname='citus'" 2>/dev/null | grep -q 1; then
  echo "SKIP: $CITUS_URL has no citus extension created."
  exit 0
fi

pass=0; fail=0
ok()   { echo "  ok   $1"; pass=$((pass+1)); }
bad()  { echo "  FAIL $1"; echo "       $2"; fail=$((fail+1)); }

q() { psql -qAt "$CITUS_URL" -c "$1" 2>&1; }

# One schema, three tables of identical shape differing only in distribution.
# That is the whole point: the only variable is what Citus made of the table.
q "DROP SCHEMA IF EXISTS ct CASCADE" >/dev/null
q "CREATE SCHEMA ct" >/dev/null
for t in dist ref local; do
  q "CREATE TABLE ct.$t (id bigint PRIMARY KEY, v int, flag boolean)" >/dev/null
done
q "SELECT create_distributed_table('ct.dist','id')" >/dev/null
q "SELECT create_reference_table('ct.ref')" >/dev/null
for t in dist ref local; do
  q "INSERT INTO ct.$t SELECT g, g, NULL FROM generate_series(1,500) g" >/dev/null
done

plan_for() {  # plan_for <table> -- prints the planMigration JSON
  "$MCP" --call planMigration --args "{\"spec\":{\"laswell_spec_version\":1,
     \"id\":\"ct-$1\",\"description\":\"paced walk over ct.$1\",
     \"intents\":[{\"kind\":\"backfill\",\"schema\":\"ct\",\"table\":\"$1\",
       \"key\":\"id\",\"set\":{\"flag\":\"true\"},\"where\":\"flag IS NULL\"}]},
     \"skipTrustChecks\":true}" "$CITUS_URL" 2>&1
}

echo "citus: the paced walk, per table type"

# A DISTRIBUTED table is refused at plan time, naming the Citus restriction.
# Multi-shard FOR UPDATE does not merely perform badly -- it cannot run.
out=$(plan_for dist)
if echo "$out" | grep -q '"ok":false' && echo "$out" | grep -q 'is distributed'; then
  ok "a distributed table is refused, and the refusal names why"
else
  bad "a distributed table should be refused" "$out"
fi

# A REFERENCE table is allowed, and the DRY RUN must agree. This is the case
# that regressed: the decision was right and the verification could not verify
# it, so the author was told their specification was the first place to look.
out=$(plan_for ref)
if echo "$out" | grep -q '"ok":true' && ! echo "$out" | grep -q '"problems"'; then
  ok "a reference table plans and its dry run is clean"
else
  bad "a reference table should plan with a clean dry run" "$out"
fi

# A LOCAL table in a Citus database walks as it always did.
out=$(plan_for local)
if echo "$out" | grep -q '"ok":true' && ! echo "$out" | grep -q '"problems"'; then
  ok "a local table in a citus database plans and its dry run is clean"
else
  bad "a local table should plan with a clean dry run" "$out"
fi

# WHICH FORMS RUN ON A DISTRIBUTED TABLE, one case per form.
#
# The guard refused four kinds with one explanation about FOR UPDATE. Two of them
# work; a third fails for an unrelated reason. Only a real cluster can say which,
# which is why this list lives here: each expectation below was established by
# preparing and EXECUTING the emitted statement against a distributed table.
echo "citus: which write forms a distributed table accepts"

intent_plan() {  # intent_plan <json-intent>
  "$MCP" --call planMigration --args "{\"spec\":{\"laswell_spec_version\":1,
     \"id\":\"ct-form\",\"description\":\"form probe\",\"intents\":[$1]},
     \"skipTrustChecks\":true}" "$CITUS_URL" 2>&1
}

expect_form() {  # expect_form <label> <allow|refuse> <needle-if-refused> <intent>
  local label=$1 want=$2 needle=$3 intent=$4
  local out; out=$(intent_plan "$intent")
  if [ "$want" = allow ]; then
    if echo "$out" | grep -q '"ok":true'; then ok "$label runs on a distributed table"
    else bad "$label should run on a distributed table" "$out"; fi
  else
    if echo "$out" | grep -q '"ok":false' && echo "$out" | grep -q "$needle"; then
      ok "$label is refused, and the reason names $needle"
    else
      bad "$label should be refused naming $needle" "$out"
    fi
  fi
}

# Refused, and for the reason that is actually true of them: they walk the target
# by key and take FOR UPDATE, which Citus will not do across shards.
expect_form "backfill" refuse "walks the table by key" \
  '{"kind":"backfill","schema":"ct","table":"dist","key":"id","set":{"flag":"true"},"where":"flag IS NULL"}'
expect_form "delete_rows by predicate" refuse "walks the table by key" \
  '{"kind":"delete_rows","schema":"ct","table":"dist","key":"id","where":"v < 0"}'

# Refused for its OWN reason. Told it was about FOR UPDATE, an author would go
# looking for a way to avoid a row lock this statement never takes.
expect_form "merge_rows" refuse "MERGE" \
  '{"kind":"merge_rows","schema":"ct","table":"dist","key":"id","columns":["id","v"],"values":[[1,7]]}'

# Allowed, because they run. Their rows come from the specification, so they join
# to them and take no row locks -- the restriction above does not apply, and
# refusing them was refusing work that succeeds.
expect_form "update_rows" allow "" \
  '{"kind":"update_rows","schema":"ct","table":"dist","key":"id","columns":["id","v"],"values":[[1,7]]}'
expect_form "delete_rows by values" allow "" \
  '{"kind":"delete_rows","schema":"ct","table":"dist","key":"id","columns":["id"],"values":[[499]]}'
expect_form "insert_rows" allow "" \
  '{"kind":"insert_rows","schema":"ct","table":"dist","key":"id","columns":["id","v"],"values":[[9001,1]]}'
expect_form "copy_rows" allow "" \
  '{"kind":"copy_rows","schema":"ct","table":"dist","columns":["id","v"],"values":[[9002,1]]}'

# And the reference-table walk must actually MOVE THE ROWS. Planning cleanly is
# not the claim being made; finishing is.
echo "citus: the reference-table walk, executed"
key_dir=$(mktemp -d)
trap 'rm -rf "$key_dir"' EXIT
openssl genpkey -algorithm ed25519 -out "$key_dir/k.pem" 2>/dev/null
openssl pkey -in "$key_dir/k.pem" -pubout -outform DER 2>/dev/null | tail -c 32 > "$key_dir/k.raw"
kid="ed25519:$(sha256sum < "$key_dir/k.raw" | cut -c1-16)"
q "INSERT INTO laswell.trusted_key (key_id, algorithm, public_key, label)
   VALUES ('$kid','ed25519',decode('$(base64 -w0 < "$key_dir/k.raw")','base64'),'citus-tests')
   ON CONFLICT (key_id) DO NOTHING" >/dev/null

repo=$(mktemp -d); trap 'rm -rf "$key_dir" "$repo"' EXIT
cat > "$repo/9990-ct-ref.json" <<'JSON'
{"laswell_spec_version":1,"id":"9990-ct-ref",
 "description":"Backfill a Citus reference table.",
 "intents":[{"kind":"backfill","schema":"ct","table":"ref","key":"id",
             "set":{"flag":"true"},"where":"flag IS NULL"}]}
JSON
bytes=$("$MCP" --call getSpecDigest --args "{\"spec\":$(cat "$repo/9990-ct-ref.json")}" \
        "$CITUS_URL" 2>/dev/null | python3 -c 'import json,sys;print(json.load(sys.stdin)["canonicalBytes"],end="")')
printf '%s' "$bytes" > "$repo/.bytes"
sig=$(openssl pkeyutl -sign -inkey "$key_dir/k.pem" -rawin -in "$repo/.bytes" 2>/dev/null | base64 -w0)
python3 - "$repo/9990-ct-ref.json" "$kid" "$sig" <<'PY'
import json, sys
p, kid, sig = sys.argv[1], sys.argv[2], sys.argv[3]
d = json.load(open(p))
d["signatures"] = [{"key_id": kid, "algorithm": "ed25519", "signature": sig}]
json.dump(d, open(p, "w"), indent=2)
PY
rm -f "$repo/.bytes"

q "UPDATE ct.ref SET flag = NULL" >/dev/null
before=$(q "SELECT count(*) FROM ct.ref WHERE flag IS NULL")
# Every repository that built this database, or the drift check refuses -- which
# is itself correct, and is why the examples repo is passed alongside.
out=$("$BIN" --repo "$repo" --repo "$(dirname "$0")/../../examples/docker/citus/migrations" "$CITUS_URL" 2>&1)
after=$(q "SELECT count(*) FROM ct.ref WHERE flag IS NULL")
if [ "$before" = "500" ] && [ "$after" = "0" ]; then
  ok "the walk finished: $before rows to do, $after left"
else
  bad "the walk did not finish (before=$before after=$after)" "$out"
fi

# Leave the ledger as it was found, or the next run sees its own spec already
# applied, does not re-run the walk, and reports a failure that is really just
# yesterday's success. Innermost first: job and step are NO ACTION FKs onto the
# migration row, which is one onto the trusted key.
cleanup_sql="
DELETE FROM laswell.backfill_cursor WHERE job_id IN (
  SELECT job_id FROM laswell.job WHERE migration_id IN (
    SELECT migration_id FROM laswell.migration WHERE spec_id = '9990-ct-ref'));
DELETE FROM laswell.step WHERE job_id IN (
  SELECT job_id FROM laswell.job WHERE migration_id IN (
    SELECT migration_id FROM laswell.migration WHERE spec_id = '9990-ct-ref'));
DELETE FROM laswell.job WHERE migration_id IN (
  SELECT migration_id FROM laswell.migration WHERE spec_id = '9990-ct-ref');
DELETE FROM laswell.migration WHERE spec_id = '9990-ct-ref';
DELETE FROM laswell.trusted_key WHERE key_id = '$kid';"
q "$cleanup_sql" >/dev/null
q "DROP SCHEMA IF EXISTS ct CASCADE" >/dev/null

echo "citus: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
