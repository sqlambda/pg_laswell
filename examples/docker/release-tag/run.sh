#!/usr/bin/env bash
# A release tag as the trigger.
#
# The specification carries the tag and waits. Approving it is a privileged
# statement in the TARGET DATABASE, recorded with who and when -- an
# authorisation act, not a technical one. The tag is inside the signature, so
# nobody can retarget it on the way to production without signing again.
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
. "$here/../lib.sh"
need_binaries; wait_for "$ALPHA_PORT"
DB=released
say "PostgreSQL $(server_version "$ALPHA_PORT")"
recreate_db "$ALPHA_PORT" "$DB"
psql -X -q -v ON_ERROR_STOP=1 -c "CREATE TABLE customers(id bigint PRIMARY KEY)" "$(url "$ALPHA_PORT" "$DB")"
kid=$(install_ledger "$ALPHA_PORT" "$DB" "$here/keys" release)
export DATABASE_URL="$(url "$ALPHA_PORT" "$DB")"
sign_dir "$here/migrations" "$here/keys/release.key.pem" "$kid"

say "1. Nobody has approved 2026.09 here, so it waits"
run "$PG_LASWELL" --repo "$here/migrations" --status
echo "  Held is not an error: a deployment that skips it has done its job."
set +e; "$PG_LASWELL" --repo "$here/migrations" >/dev/null 2>&1; echo "  exit $? -- held, and still a successful run"; set -e

say "2. Approving it is an act with a name attached"
run psql -X -q -c \
  "INSERT INTO laswell.release(tag, ready, marked_ready_at, marked_by, note)
   VALUES ('2026.09', true, now(), current_user, 'CHG-4471 approved by change board')" \
  "$DATABASE_URL"
psql -X -q -c "\pset border 2" -c \
  "SELECT tag, ready, marked_by, note FROM laswell.release" "$DATABASE_URL"

say "3. Now it runs"
run "$PG_LASWELL" --repo "$here/migrations"
