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

# ct.dist is distributed on its own key. A backfill confines each batch to one
# distribution value, so here every batch would be one row: refused, naming why
# and what runs instead. (A table distributed on a DIFFERENT column is walked --
# see the grouped walk below.)
out=$(plan_for dist)
if echo "$out" | grep -q '"ok":false' && echo "$out" | grep -q 'which is the walk key'; then
  ok "a table distributed on its walk key is refused, and the refusal names why"
else
  bad "a table distributed on its walk key should be refused" "$out"
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
expect_form "backfill on a table distributed on its key" refuse "which is the walk key" \
  '{"kind":"backfill","schema":"ct","table":"dist","key":"id","set":{"flag":"true"},"where":"flag IS NULL"}'
expect_form "delete_rows by predicate on a table distributed on its key" refuse "which is the walk key" \
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
DELETE FROM laswell.migration WHERE spec_id = '9990-ct-ref';"
# THE §6 REFUSALS, each a Citus error found beforehand. Every expectation below
# was measured against Citus 13 before the refusal was written.
echo "citus: refusals before execution"
q "CREATE TABLE ct.fa (tenant_id bigint NOT NULL, id bigint NOT NULL, other bigint,
                       PRIMARY KEY (tenant_id, id))" >/dev/null
q "CREATE TABLE ct.fb (tenant_id bigint NOT NULL, id bigint NOT NULL, other bigint,
                       PRIMARY KEY (tenant_id, id))" >/dev/null
q "CREATE TABLE ct.fc (tenant_id bigint NOT NULL, id bigint NOT NULL, other bigint,
                       PRIMARY KEY (tenant_id, id))" >/dev/null
q "SELECT create_distributed_table('ct.fa','tenant_id')" >/dev/null
q "SELECT create_distributed_table('ct.fb','tenant_id', colocate_with => 'ct.fa')" >/dev/null
q "SELECT create_distributed_table('ct.fc','tenant_id', colocate_with => 'none')" >/dev/null
q "CREATE TABLE ct.fi (id int PRIMARY KEY)" >/dev/null

fk_intent() {  # fk_intent <child> <cols> <parent> <refs>
  echo "{\"kind\":\"add_foreign_key\",\"schema\":\"ct\",\"table\":\"$1\",\"name\":\"k_$1\",
         \"columns\":$2,\"references_schema\":\"ct\",\"references_table\":\"$3\",
         \"references_columns\":$4}"
}
expect_form "a foreign key between colocated tables, same ordinal" allow "" \
  "$(fk_intent fb '["tenant_id","other"]' fa '["tenant_id","id"]')"
expect_form "a foreign key between tables that are not colocated" refuse "not colocated" \
  "$(fk_intent fc '["tenant_id","other"]' fa '["tenant_id","id"]')"
expect_form "a foreign key pairing the distribution columns out of position" refuse "same position" \
  "$(fk_intent fb '["other","tenant_id"]' fa '["tenant_id","id"]')"
expect_form "a foreign key from a reference table to a distributed one" refuse "reference or local" \
  "$(fk_intent ref '["id"]' fa '["tenant_id"]')"
expect_form "colocating an int column with a bigint group" refuse "same type exactly" \
  '{"kind":"citus_distribute_table","schema":"ct","table":"fi","distribution_column":"id","colocate_with":"ct.fa","comment":"x"}'

# citus_distribute_function. On Citus 11+ every CREATE FUNCTION is propagated
# and recorded in pg_dist_object, which the first reading took to mean "already
# distributed" -- the kind reported satisfied for every function and routed
# nothing. A function created a moment ago must plan an APPLY.
q "CREATE OR REPLACE FUNCTION ct.fn(tid bigint) RETURNS bigint LANGUAGE plpgsql AS \$\$ BEGIN RETURN tid; END \$\$" >/dev/null
q "CREATE OR REPLACE FUNCTION ct.fn_text(tid text) RETURNS bigint LANGUAGE plpgsql AS \$\$ BEGIN RETURN 1; END \$\$" >/dev/null
fn_intent() {  # fn_intent <name> <arguments> <distribution_argument>
  echo "{\"kind\":\"citus_distribute_function\",\"schema\":\"ct\",\"name\":\"$1\",
         \"arguments\":\"$2\",\"distribution_argument\":\"$3\",
         \"colocate_with\":\"ct.fa\",\"comment\":\"x\"}"
}
out=$(intent_plan "$(fn_intent fn bigint tid)")
if echo "$out" | grep -q '"ok":true' && echo "$out" | grep -q '"action":"apply"'; then
  ok "a function Citus merely knows about is distributed, not reported satisfied"
else
  bad "a propagated-only function should plan an apply" "$out"
fi
expect_form "a distribution argument the function does not have" refuse "is not an argument of" \
  "$(fn_intent fn bigint nope)"
expect_form "a text argument on a bigint group" refuse "every call" \
  "$(fn_intent fn_text text tid)"

# THE LIFECYCLE OF A DISTRIBUTED TABLE. Each case plans through planMigration,
# whose dry run EXECUTES the Citus call and rolls it back -- so an "ok" with no
# problems proves Citus accepted the emitted statement, not only that the
# planner liked it.
echo "citus: the lifecycle kinds"

q "CREATE TABLE ct.la (id bigint PRIMARY KEY, t bigint NOT NULL)" >/dev/null
q "SELECT create_distributed_table('ct.la','id', shard_count => 4, colocate_with => 'none')" >/dev/null
q "CREATE TABLE ct.lb (id bigint PRIMARY KEY REFERENCES ct.la(id))" >/dev/null
q "SELECT create_distributed_table('ct.lb','id', colocate_with => 'ct.la')" >/dev/null
q "CREATE TABLE ct.lr (id bigint PRIMARY KEY)" >/dev/null
q "SELECT create_reference_table('ct.lr')" >/dev/null
q "INSERT INTO ct.lr VALUES (1)" >/dev/null
# A local table referencing the reference table: Citus quietly adds it to its
# metadata, and its rows are its only rows.
q "CREATE TABLE ct.ll (id bigint PRIMARY KEY, r bigint REFERENCES ct.lr(id))" >/dev/null
q "INSERT INTO ct.ll VALUES (1, 1)" >/dev/null
q "CREATE TABLE ct.lp (id bigint PRIMARY KEY)" >/dev/null

clean_apply() {  # clean_apply <label> <intent>
  local out; out=$(intent_plan "$2")
  if echo "$out" | grep -q '"ok":true' && echo "$out" | grep -q '"action":"apply"' \
     && ! echo "$out" | grep -q '"problems"'; then
    ok "$1 plans, and its dry run ran the call cleanly"
  else
    bad "$1 should plan an apply with a clean dry run" "$out"
  fi
}
satisfied() {  # satisfied <label> <intent>
  local out; out=$(intent_plan "$2")
  if echo "$out" | grep -q '"ok":true' && echo "$out" | grep -q '"action":"satisfied"' \
     && ! echo "$out" | grep -q '"action":"apply"'; then
    ok "$1 is satisfied, and no call is made"
  else
    bad "$1 should be satisfied" "$out"
  fi
}
alter() { echo "{\"kind\":\"citus_alter_distributed_table\",\"schema\":\"ct\",\"table\":\"la\"$1}"; }

expect_form "a shard count change in a shared group, cascade unsaid" refuse "cascade_to_colocated" \
  "$(alter ',"shard_count":8')"
clean_apply "a cascaded shard count change" "$(alter ',"shard_count":8,"cascade_to_colocated":true')"
# Measured, and the reason this is refused rather than warned: Citus succeeds,
# warns, and the key is gone.
expect_form "leaving the group while lb references la" refuse "would be dropped" \
  "$(alter ',"colocate_with":"none"')"
satisfied "an alter asking for the shard count la already has" "$(alter ',"shard_count":4')"

satisfied "undistributing a table Citus does not record" \
  '{"kind":"citus_undistribute_table","schema":"ct","table":"lp"}'
expect_form "undistributing one end of a key" refuse "cascade_via_foreign_keys" \
  '{"kind":"citus_undistribute_table","schema":"ct","table":"la"}'
clean_apply "undistributing both ends of a key" \
  '{"kind":"citus_undistribute_table","schema":"ct","table":"la","cascade_via_foreign_keys":true}'

clean_apply "adding a plain local table to metadata" \
  '{"kind":"citus_add_local_table_to_metadata","schema":"ct","table":"lp"}'
satisfied "adding a table Citus already manages as local" \
  '{"kind":"citus_add_local_table_to_metadata","schema":"ct","table":"ll"}'
expect_form "adding a reference table to metadata as local" refuse "already a reference table" \
  '{"kind":"citus_add_local_table_to_metadata","schema":"ct","table":"lr"}'

# Measured: the call truncates with CASCADE, and ct.ll's own rows went with it.
expect_form "truncating local data that would cascade into ct.ll" refuse "ct.ll" \
  '{"kind":"citus_truncate_local_data","schema":"ct","table":"lr"}'
clean_apply "truncating the local copy under a distributed table" \
  '{"kind":"citus_truncate_local_data","schema":"ct","table":"la"}'
if [ "$(q "SELECT count(*) FROM ct.ll")" = 1 ]; then
  ok "ct.ll still has its row: nothing above committed"
else
  bad "ct.ll lost its row" "$(q "SELECT count(*) FROM ct.ll")"
fi

# THE GROUPED WALK, through plain `backfill`.
#
# The author writes backfill whatever the server runs. Core asks the Citus
# module one question -- must row-locking batches on this table be confined to
# one value of a column? -- and for a distributed table the answer is its
# distribution column, so core walks one value at a time and FOR UPDATE becomes
# single-shard. Correctness is the acceptance test, not speed: the measured gain
# is ~16%, and the reason to have it is that the alternative is no walk at all.
echo "citus: the grouped walk"

q "DROP TABLE IF EXISTS ct.tw" >/dev/null
# The shape this needs, and the shape the tenant convention already produces: the
# distribution column first in the primary key, so the key is unique WITHIN one
# value of it. A unique index on the key alone cannot exist here -- Citus refuses
# one that does not contain the distribution column.
q "CREATE TABLE ct.tw (tenant_id bigint NOT NULL, id bigint NOT NULL,
                       n int NOT NULL DEFAULT 0, PRIMARY KEY (tenant_id, id))" >/dev/null
q "SELECT create_distributed_table('ct.tw','tenant_id')" >/dev/null
q "INSERT INTO ct.tw SELECT t, (t-1)*100 + g, 0 FROM generate_series(1,8) t,
                                                    generate_series(1,100) g" >/dev/null

out=$(intent_plan '{"kind":"backfill","schema":"ct","table":"tw","key":"id","set":{"n":"tw.n + 1"},"where":"n = 0"}')
if echo "$out" | grep -q '"ok":true' && echo "$out" | grep -q '"batch_mode":"grouped"' \
   && echo "$out" | grep -q '"confined_by":"citus"'; then
  ok "a plain backfill on a distributed table plans as a grouped walk, and says the citus reading decided"
else
  bad "backfill should plan grouped here, confined by the citus reading" "$out"
fi

# delete_rows by predicate walks the target the same way, and groups the same way.
out=$(intent_plan '{"kind":"delete_rows","schema":"ct","table":"tw","key":"id","where":"n < 0"}')
if echo "$out" | grep -q '"ok":true' && echo "$out" | grep -q '"batch_mode":"grouped"'; then
  ok "delete_rows by predicate on a distributed table plans as a grouped walk"
else
  bad "delete_rows by predicate should plan grouped here" "$out"
fi

# The apply must be SINGLE SHARD. That is the entire mechanism: Citus allows
# FOR UPDATE on one shard and refuses it across many, so a batch that is not
# confined cannot lock and the walk cannot exist.
tasks=$(q "EXPLAIN (COSTS OFF) UPDATE ct.tw SET n = n + 1
            WHERE ct.tw.tenant_id = 3 AND ct.tw.id = ANY('{\"201\"}'::bigint[]) AND (n = 0)" \
        | grep -oE 'Task Count: [0-9]+' | grep -oE '[0-9]+')
if [ "$tasks" = "1" ]; then ok "a confined batch is one task, not one per shard"
else bad "a confined batch should be Task Count 1, got '${tasks:-none}'" ""; fi

# EXACTLY ONCE, which is what a counter proves and a boolean cannot: a row
# updated twice reads n = 2, and a boolean set twice is indistinguishable from a
# boolean set once.
walk_repo=$(mktemp -d)
cat > "$walk_repo/9991-ct-shard.json" <<'JSON'
{"laswell_spec_version":1,"id":"9991-ct-shard",
 "description":"Backfill a distributed table one shard at a time.",
 "intents":[{"kind":"backfill","schema":"ct","table":"tw",
             "key":"id","set":{"n":"tw.n + 1"},"where":"n = 0"}]}
JSON
bytes2=$("$MCP" --call getSpecDigest --args "{\"spec\":$(cat "$walk_repo/9991-ct-shard.json")}" \
         "$CITUS_URL" 2>/dev/null | python3 -c 'import json,sys;print(json.load(sys.stdin)["canonicalBytes"],end="")')
printf '%s' "$bytes2" > "$walk_repo/.bytes"
sig2=$(openssl pkeyutl -sign -inkey "$key_dir/k.pem" -rawin -in "$walk_repo/.bytes" 2>/dev/null | base64 -w0)
python3 - "$walk_repo/9991-ct-shard.json" "$kid" "$sig2" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
d["signatures"] = [{"key_id": sys.argv[2], "algorithm": "ed25519", "signature": sys.argv[3]}]
json.dump(d, open(sys.argv[1], "w"), indent=2)
PY
rm -f "$walk_repo/.bytes"

out=$("$BIN" --repo "$walk_repo" --repo "$repo" \
        --repo "$(dirname "$0")/../../examples/docker/citus/migrations" "$CITUS_URL" 2>&1)
left=$(q "SELECT count(*) FROM ct.tw WHERE n = 0")
twice=$(q "SELECT count(*) FROM ct.tw WHERE n <> 1")
if [ "$left" = "0" ] && [ "$twice" = "0" ]; then
  ok "800 rows over 8 tenants, each updated exactly once"
else
  bad "not exactly once (n=0: $left, n<>1: $twice)" "$out"
fi

# RESUMING MID-SHARD, through the executor.
#
# An earlier version of this case drove the SQL by hand and passed -- while the
# executor could not resume at all: its staleness check cast the cursor as
# bigint, a shard cursor is ["4","400"], the cast threw, and a check that throws
# is read as "stale". Every retry restarted from the top, and because the
# predicate hides rows already done, the outcome looked identical. The case
# below makes one shard fail, commits per batch so earlier shards stay done,
# retries, and reads WHERE the retry started.
q "UPDATE ct.tw SET n = 0" >/dev/null
q "$(printf "DELETE FROM laswell.backfill_cursor WHERE job_id IN (SELECT job_id FROM laswell.job WHERE migration_id IN (SELECT migration_id FROM laswell.migration WHERE spec_id = '9991-ct-shard'));\nDELETE FROM laswell.step WHERE job_id IN (SELECT job_id FROM laswell.job WHERE migration_id IN (SELECT migration_id FROM laswell.migration WHERE spec_id = '9991-ct-shard'));\nDELETE FROM laswell.job WHERE migration_id IN (SELECT migration_id FROM laswell.migration WHERE spec_id = '9991-ct-shard');\nDELETE FROM laswell.migration WHERE spec_id = '9991-ct-shard';")" >/dev/null
q "ALTER TABLE ct.tw ADD CONSTRAINT tw_block_t5 CHECK (NOT (tenant_id = 5 AND n > 0))" >/dev/null

# A commit per batch, or the walk is one transaction and the failure takes every
# row with it -- correct, and not the case under test. --config replaces the
# connection source, so the section is built from CITUS_URL.
paced_ini=$(mktemp); chmod 600 "$paced_ini"
python3 - "$CITUS_URL" > "$paced_ini" <<'PY'
import sys, urllib.parse as u
p = u.urlparse(sys.argv[1])
print("[executor]\nbatch_rows = 25\nbatch_cap_rows = 25\n"
      "commit_interval_ms = 1\nobserver_tick_ms = 1\n")
print("[citus]")
print(f"host = {p.hostname}\nport = {p.port or 5432}\n"
      f"dbname = {p.path.lstrip('/')}\nuser = {p.username}")
if p.password: print(f"password = {p.password}")
PY
repos=(--repo "$walk_repo" --repo "$repo" --repo "$(dirname "$0")/../../examples/docker/citus/migrations")

"$BIN" --config "$paced_ini" "${repos[@]}" >/dev/null 2>&1; first=$?
q "ALTER TABLE ct.tw DROP CONSTRAINT tw_block_t5" >/dev/null
out=$("$BIN" --config "$paced_ini" "${repos[@]}" 2>&1); second=$?
rm -f "$paced_ini"

resumed=$(q "SELECT s.detail->>'resumedFrom' FROM laswell.step s JOIN laswell.job j USING (job_id)
              JOIN laswell.migration m USING (migration_id)
             WHERE m.spec_id = '9991-ct-shard' AND j.state = 'succeeded'")
still=$(q "SELECT count(*) FROM ct.tw WHERE n = 0")
twice=$(q "SELECT count(*) FROM ct.tw WHERE n <> 1")
if [ "$first" != 0 ] && [ "$second" = 0 ] && [ "$resumed" = '["4","400"]' ] \
   && [ "$still" = 0 ] && [ "$twice" = 0 ]; then
  ok "a walk that failed on tenant 5 resumed from $resumed and finished, none twice"
else
  bad "retry did not resume (first=$first second=$second resumedFrom=${resumed:-none} left=$still n<>1=$twice)" "$out"
fi

rm -rf "$walk_repo"
cleanup_sql="$cleanup_sql
DELETE FROM laswell.backfill_cursor WHERE job_id IN (
  SELECT job_id FROM laswell.job WHERE migration_id IN (
    SELECT migration_id FROM laswell.migration WHERE spec_id = '9991-ct-shard'));
DELETE FROM laswell.step WHERE job_id IN (
  SELECT job_id FROM laswell.job WHERE migration_id IN (
    SELECT migration_id FROM laswell.migration WHERE spec_id = '9991-ct-shard'));
DELETE FROM laswell.job WHERE migration_id IN (
  SELECT migration_id FROM laswell.migration WHERE spec_id = '9991-ct-shard');
DELETE FROM laswell.migration WHERE spec_id = '9991-ct-shard';
-- The key LAST, and this ordering is load-bearing. laswell.migration references
-- laswell.trusted_key, so deleting the key while any migration row still points
-- at it violates the FK -- and because these statements share one implicit
-- transaction, that rolled the WHOLE cleanup back. The symptom was a suite that
-- alternated pass and fail: a run that cleaned up left the next one clean, and a
-- run that did not left a spec the next run's drift check refused.
DELETE FROM laswell.trusted_key WHERE key_id = '$kid';"
q "$cleanup_sql" >/dev/null
q "DROP SCHEMA IF EXISTS ct CASCADE" >/dev/null

echo "citus: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
