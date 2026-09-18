#!/usr/bin/env bash
# The baseline: one cluster, one directory, one ledger.
#
# Start here. Everything else on the landing page is this with one thing added.
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
. "$here/../lib.sh"
need_binaries; wait_for "$ALPHA_PORT"
DB=simple
say "PostgreSQL $(server_version "$ALPHA_PORT")"
recreate_db "$ALPHA_PORT" "$DB"
kid=$(install_ledger "$ALPHA_PORT" "$DB" "$here/keys" simple)
export DATABASE_URL="$(url "$ALPHA_PORT" "$DB")"
sign_dir "$here/migrations" "$here/keys/simple.key.pem" "$kid"

say "1. What is pending, and in what order"
run "$PG_LASWELL" --repo "$here/migrations" --status

say "2. Plan it without touching anything"
echo "  Each plan is tried against the real schema in a transaction that is"
echo "  rolled back. Per specification -- 0002 depends on a table 0001 has not"
echo "  created yet, so it says so rather than pretending."
set +e; "$PG_LASWELL" --repo "$here/migrations" --dry-run; echo "  exit $?"; set -e

say "3. Apply"
run "$PG_LASWELL" --repo "$here/migrations"

say "4. The ledger records what ran, with the bytes that were verified"
psql -X -q -c "\pset border 2" -c \
  "SELECT spec_id, left(spec_digest, 12) AS digest, epoch FROM laswell.migration ORDER BY spec_id" \
  "$DATABASE_URL"

say "5. Run it again: nothing to do"
run "$PG_LASWELL" --repo "$here/migrations"
