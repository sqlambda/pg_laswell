#!/usr/bin/env bash
# Two teams, one database, and neither refused for the other's absence.
#
# The problem this exists to show: the check that catches drift asks "does every
# migration this database records have a file here?" -- and for a deployment that
# is DELIBERATELY partial, that question has no honest answer. The application
# team does not have the DBA's files and never will.
#
# An epoch is a lineage of tracked history. Scoped by epoch the question becomes
# answerable: each deployment answers for the lineages it brought, and says
# nothing about the others. The DBA's manifest then declares itself COMPLETE,
# which widens the check back to the whole database for the one deployment that
# really does carry everything.
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
. "$here/../lib.sh"

need_binaries
wait_for "$ALPHA_PORT"
DB=dba_and_app
say "PostgreSQL $(server_version "$ALPHA_PORT") on 127.0.0.1:$ALPHA_PORT"

recreate_db "$ALPHA_PORT" "$DB"
kid=$(install_ledger "$ALPHA_PORT" "$DB" "$here/keys" shared)
export DATABASE_URL="$(url "$ALPHA_PORT" "$DB")"

# Both lineages are opened by the schema owner. This is the point of the
# privilege split: opening an epoch NARROWS the drift check, so a role that
# could open its own could excuse its own drift.
open_epoch "$ALPHA_PORT" "$DB" app "The application team's lineage."
open_epoch "$ALPHA_PORT" "$DB" dba "The DBA's lineage."

sign_dir "$here/app" "$here/keys/shared.key.pem" "$kid"
sign_dir "$here/dba" "$here/keys/shared.key.pem" "$kid"

say "1. The DBA deploys everything, from a manifest that says so"
run "$PG_LASWELL" --manifest "$here/manifest.json"

say "2. The application deploys its own directory alone"
echo "  Its two migrations are already applied, and -- the part that matters --"
echo "  the DBA's migration is NOT reported as a deleted file."
run "$PG_LASWELL" --repo "$here/app" --status

say "3. What each side can see of the other"
psql -X -q -c "\\pset border 2" -c "
  SELECT epoch, spec_id
    FROM laswell.migration ORDER BY epoch, spec_id" "$DATABASE_URL"
echo "  One ledger, two lineages. The application team never checked out dba/."

say "4. The scoping is not just silence"
echo "  Inside the application's OWN lineage, a missing file is still drift."
echo "  Hide app-0002 and the application's own deployment refuses:"
mv "$here/app/0002-status-index.json" "$here/0002.hidden"
set +e
"$PG_LASWELL" --repo "$here/app" --status
echo "  exit $? -- a repository problem."
set -e
mv "$here/0002.hidden" "$here/app/0002-status-index.json"

say "5. The contrast the whole design turns on"
echo "  Now hide one of the DBA's files instead, and look at the SAME database"
echo "  through the two deployments."
mv "$here/dba/0001-retention.json" "$here/dba0001.hidden"

echo
echo "  a) the application, which never had that file and does not claim to:"
set +e
"$PG_LASWELL" --repo "$here/app" --status
echo "     exit $? -- silent, because dba/ is not its lineage to answer for."

echo
echo "  b) the DBA manifest, which declares itself complete:"
"$PG_LASWELL" --manifest "$here/manifest.json" --status
echo "     exit $? -- the one deployment that claims the whole database is the"
echo "     one that catches it."
set -e
mv "$here/dba0001.hidden" "$here/dba/0001-retention.json"

say "Done. Clean up with:"
echo "  docker compose -f examples/docker/compose.yml down -v"
