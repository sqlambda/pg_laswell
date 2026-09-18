#!/usr/bin/env bash
# One repository, two databases, and a dependency that crosses between them.
#
# The subscriber cannot be created until the publication exists -- and the
# publication lives in a database the subscriber cannot see. pg_laswell holds
# 0040 until STORE'S OWN ledger records 0030 as applied, because "has that one
# run" is a question only that database can answer.
#
# Two clusters rather than two databases on one, deliberately: a subscription
# to a publication in the same cluster deadlocks against its own walsender.
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
. "$here/../lib.sh"
need_binaries; wait_for "$ALPHA_PORT"; wait_for "$BETA_PORT"
say "store on $ALPHA_PORT, archive on $BETA_PORT -- PostgreSQL $(server_version "$ALPHA_PORT")"

# The subscription holds a replication slot ON THE PUBLISHER, so dropping the
# publisher's database fails while it exists -- which is what made a second run
# of this script stop dead. Dropped first, and the slot goes with it.
psql -X -q -c "DROP SUBSCRIPTION IF EXISTS archive_from_store" \
  "$(url "$BETA_PORT" archive)" >/dev/null 2>&1 || true
recreate_db "$ALPHA_PORT" store
recreate_db "$BETA_PORT" archive
kid=$(install_ledger "$ALPHA_PORT" store "$here/keys" repl)
kid=$(install_ledger "$BETA_PORT" archive "$here/keys" repl)
# The subscriber dials the publisher by CONTAINER NAME over the compose network,
# which is why compose.yml sets POSTGRES_HOST_AUTH_METHOD=trust: pg_laswell
# refuses a password in a subscription's connection string, because the ledger
# records what ran and a redacted entry would no longer be what ran.
sign_dir "$here/migrations" "$here/keys/repl.key.pem" "$kid"

cat > "$here/repl.ini" <<INI
[store]
host = 127.0.0.1
port = $ALPHA_PORT
dbname = store
user = postgres
password = laswell

[archive]
host = 127.0.0.1
port = $BETA_PORT
dbname = archive
user = postgres
password = laswell
INI
chmod 600 "$here/repl.ini"

say "1. The order, derived from depends_on across two databases"
run "$PG_LASWELL" --config "$here/repl.ini" --repo "$here/migrations" --status

say "2. Apply"
run "$PG_LASWELL" --config "$here/repl.ini" --repo "$here/migrations"

say "3. Write on the store"
psql -X -q -v ON_ERROR_STOP=1 -c \
  "INSERT INTO events(id, payload) SELECT g, 'event ' || g FROM generate_series(1,5) g" \
  "$(url "$ALPHA_PORT" store)"

say "4. Read on the archive, which never ran an INSERT"
for i in $(seq 1 30); do
  n=$(psql -X -t -A -c "SELECT count(*) FROM events" "$(url "$BETA_PORT" archive)")
  [ "$n" -ge 5 ] && break; sleep 1
done
psql -X -q -c "\pset border 2" -c "SELECT id, payload FROM events ORDER BY id" "$(url "$BETA_PORT" archive)"
