#!/usr/bin/env bash
# Apply the repository to the house database, then check what it produced.
#
#   bin/run.sh              status, dry run, apply, verify
#   bin/run.sh --status     report what is pending and stop
#   bin/run.sh --dry-run    plan everything, apply nothing
#
# Exit codes come from pg_laswell itself, so a pipeline can branch on them:
#   0 nothing pending or everything applied   1 refused, untrusted, or failed
#   2 a repository problem (drift, unreadable spec, broken depends_on)
#   3 a configuration problem
set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
laswell=${PGLASWELL:-$here/../../cpp/build/pg_laswell}
[ -x "$laswell" ] || { echo "no pg_laswell at $laswell -- build it, or set PGLASWELL" >&2; exit 3; }
[ -f "$here/laswell.ini" ] || { echo "no $here/laswell.ini -- run bin/setup.sh first" >&2; exit 3; }

run() { "$laswell" --repo "$here/migrations" -c "$here/laswell.ini" "$@"; }

case ${1-} in
  --status)  run --status;  exit $? ;;
  --dry-run) run --dry-run; exit $? ;;
  "")        ;;
  *) echo "unknown argument: $1" >&2; exit 2 ;;
esac

echo "=== status ============================================================"
run --status

echo
echo "=== dry run ==========================================================="
# Cross-specification dependencies cannot all be dry-run before the earlier
# ones have committed: a plan is measured against the REAL catalog, and a table
# an earlier specification creates is not in it yet. A REFUSED line here for a
# migration at level 2 or deeper is that, not a defect.
run --dry-run || true

echo
echo "=== apply ============================================================="
run

echo
echo "=== verify ============================================================"
# Read the connection back out of the file pg_laswell just used, so the checks
# run against the database that was actually migrated and as the role that
# migrated it -- not as whoever happens to be at the shell.
port=$(sed -n 's/^port *= *//p'     "$here/laswell.ini" | head -1)
host=$(sed -n 's/^host *= *//p'     "$here/laswell.ini" | head -1)
user=$(sed -n 's/^user *= *//p'     "$here/laswell.ini" | head -1)
PGPASSWORD=$(sed -n 's/^password *= *//p' "$here/laswell.ini" | head -1)
export PGPASSWORD
psql -q -h "${host:-127.0.0.1}" -p "${port:-5432}" -U "${user:-house_runner}" \
     -d house -v ON_ERROR_STOP=1 -P pager=off -f "$here/sql/verify.sql"
