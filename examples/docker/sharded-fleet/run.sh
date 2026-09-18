#!/usr/bin/env bash
# Three shards, all called "app", on three different servers.
#
# This is the case a scheduler comparing connection NAMES gets wrong, and the
# case a scheduler comparing DATABASE names gets wrong too: every shard's
# database is called "app". Identity is the cluster's own system_identifier
# with the database's oid, so the three are proven different -- and different
# databases cannot touch each other's tables, which is the one place
# concurrency is granted without needing any evidence at all.
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
. "$here/../lib.sh"

need_binaries
for port in "$ALPHA_PORT" "$BETA_PORT" "$GAMMA_PORT"; do wait_for "$port"; done

say "Three servers, three databases, one name between them"
for port in "$ALPHA_PORT" "$BETA_PORT" "$GAMMA_PORT"; do
  psql -X -t -A -c "SELECT '  127.0.0.1:$port  database=' || current_database() ||
                           '  system_identifier=' || (SELECT system_identifier FROM pg_control_system())" \
       "$(url "$port")"
done
echo "  Same name. Different databases. Only the identifier can tell them apart."

DB=shard
for port in "$ALPHA_PORT" "$BETA_PORT" "$GAMMA_PORT"; do
  recreate_db "$port" "$DB"
  kid=$(install_ledger "$port" "$DB" "$here/keys" fleet)
done
cat > "$here/fleet.ini" <<INI
[shard-1]
host = 127.0.0.1
port = $ALPHA_PORT
dbname = $DB
user = postgres
password = laswell

[shard-2]
host = 127.0.0.1
port = $BETA_PORT
dbname = $DB
user = postgres
password = laswell

[shard-3]
host = 127.0.0.1
port = $GAMMA_PORT
dbname = $DB
user = postgres
password = laswell
INI
chmod 600 "$here/fleet.ini"

say "One specification per shard, routed by connection"
# Generated into a BUILD directory rather than edited in place. The first
# version rewrote migrations/ and deleted its own template, so running the
# example twice failed on a file it had consumed -- an example that works once
# is a demo, not a thing you can re-run.
build=$here/.build
rm -rf "$build"; mkdir -p "$build"
for shard in shard-1 shard-2 shard-3; do
  SHARD=$shard python3 - "$here/migrations/0001-sessions.json" "$build/0001-$shard.json" <<'PY'
import json, os, sys
src, dst = sys.argv[1:3]
doc = json.load(open(src))
doc.pop("signatures", None)
doc["id"] = f"0001-sessions-{os.environ['SHARD']}"
doc["target"] = {"connection": os.environ["SHARD"]}
json.dump(doc, open(dst, "w"), indent=2)
PY
done
sign_dir "$build" "$here/keys/fleet.key.pem" "$kid"

run "$PG_LASWELL" --config "$here/fleet.ini" --repo "$build" --status
echo
echo "  'concurrent' above is the whole point: three shards, no evidence"
echo "  required, because different databases cannot collide."

say "Apply them"
run "$PG_LASWELL" --config "$here/fleet.ini" --repo "$build"

say "Each shard has its own ledger"
for port in "$ALPHA_PORT" "$BETA_PORT" "$GAMMA_PORT"; do
  psql -X -t -A -c "SELECT '  127.0.0.1:$port applied: ' || string_agg(spec_id, ', ')
                      FROM laswell.migration" "$(url "$port" "$DB")"
done
