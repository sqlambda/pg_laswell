#!/usr/bin/env bash
# Apply the repository across BOTH databases, then check what it produced.
#
#   bin/run.sh              status, dry run, apply, verify
#   bin/run.sh --status     report what is pending and stop
#   bin/run.sh --dry-run    plan everything, apply nothing
#
# One pg_laswell invocation, one repository, two databases. Each specification
# names its connection and is applied there; the subscription waits until the
# publication is recorded applied in the STORE's ledger.
#
# Exit codes come from pg_laswell itself, so a pipeline can branch on them:
#   0 nothing pending or everything applied   1 refused, untrusted, or failed
#   2 a repository problem (drift, unreadable spec, broken depends_on)
#   3 a configuration problem
set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
laswell=${PGLASWELL:-$here/../../cpp/build/pg_laswell}
[ -x "$laswell" ] || { echo "no pg_laswell at $laswell -- build it, or set PGLASWELL" >&2; exit 3; }
[ -f "$here/laswell.ini" ] || { echo "no laswell.ini -- run bin/setup.sh first" >&2; exit 3; }

mode=${1:-}
run() { "$laswell" --config "$here/laswell.ini" --repo "$here/migrations" "$@"; }

echo "--- what is pending, and where -------------------------------------------"
run --status

case "$mode" in
  --status)  exit 0 ;;
  --dry-run) echo; echo "--- dry run ---"; run --dry-run; exit $? ;;
esac

echo
echo "--- dry run --------------------------------------------------------------"
# Cross-specification dependencies cannot all be dry-run before the earlier
# ones have committed: a plan is measured against the REAL catalog, and a table
# an earlier specification creates is not in it yet. A REFUSED line here for a
# migration at level 2 or deeper is that, not a defect -- 0030 publishes a
# table 0020 has only planned, and 0040 subscribes to a publication 0030 has
# only planned.
run --dry-run || true

echo
echo "--- applying -------------------------------------------------------------"
run

echo
echo "--- verifying ------------------------------------------------------------"
# The AUDIT port: the second of the two in the file, since [store] comes first.
port=$(sed -n 's/^port *= *//p' "$here/laswell.ini" | sed -n 2p)
psql -X -q -h 127.0.0.1 -p "$port" -U postgres -d audit -v ON_ERROR_STOP=1 \
     -f "$here/sql/verify.sql"
