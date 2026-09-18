#!/usr/bin/env bash
# Rolling the baseline forward when a new base backup is taken.
#
# One lineage holds every change since the last base backup. A new backup is
# taken -- it already CONTAINS those changes -- so that lineage stops being
# something to track and a new one begins. Retiring is not deleting: the old
# migrations stay applied, so a change in the new lineage may still depend on
# one of them, and the files can be deleted without the drift check objecting.
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
. "$here/../lib.sh"

need_binaries
wait_for "$ALPHA_PORT"
DB=baseline
say "PostgreSQL $(server_version "$ALPHA_PORT")"

recreate_db "$ALPHA_PORT" "$DB"
psql -X -q -v ON_ERROR_STOP=1 -c \
  "CREATE TABLE orders(id bigint PRIMARY KEY)" "$(url "$ALPHA_PORT" "$DB")"
kid=$(install_ledger "$ALPHA_PORT" "$DB" "$here/keys" baseline)
export DATABASE_URL="$(url "$ALPHA_PORT" "$DB")"
open_epoch "$ALPHA_PORT" "$DB" since-2026-06 "Changes since the June base backup."
sign_dir "$here/since-backup" "$here/keys/baseline.key.pem" "$kid"
sign_dir "$here/after-backup" "$here/keys/baseline.key.pem" "$kid"

say "1. Everything since the last base backup, in one lineage"
run "$PG_LASWELL" --repo "$here/since-backup"

say "2. A new base backup is taken -- it already contains all of that"
echo "  So that lineage has nothing left to track. Retire it:"
run psql -X -q -c \
  "UPDATE laswell.epoch SET retired_at = now(), retired_by = current_user WHERE name = 'since-2026-06'" \
  "$DATABASE_URL"
open_epoch "$ALPHA_PORT" "$DB" since-2026-09 "Changes since the September base backup."

say "3. The retired lineage's files can now go"
echo "  The drift check does not hold a retired lineage to account for them:"
mkdir -p "$here/.retired"
mv "$here/since-backup"/*.json "$here/.retired/"
run "$PG_LASWELL" --repo "$here/after-backup" --status

say "4. But it was NOT unsaid"
echo "  0001-add-priority depends on 0001-add-channel, which lives in the"
echo "  retired lineage and whose file is gone. It still resolves, because"
echo "  retiring stops TRACKING without unsaying that anything ran:"
run "$PG_LASWELL" --repo "$here/after-backup"

psql -X -q -c "\pset border 2" -c \
  "SELECT m.epoch, m.spec_id, (e.retired_at IS NOT NULL) AS epoch_retired
     FROM laswell.migration m JOIN laswell.epoch e ON e.name = m.epoch
    ORDER BY m.epoch, m.spec_id" "$DATABASE_URL"

mv "$here/.retired"/*.json "$here/since-backup/" && rmdir "$here/.retired"
