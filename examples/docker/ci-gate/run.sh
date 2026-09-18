#!/usr/bin/env bash
# A pipeline step that changes nothing and can still fail the build.
#
# Four exit codes, each meaning something a pipeline can branch on. The one
# that matters is 2: the database records a migration no file here describes,
# which is drift -- caught before anything reaches a database.
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
. "$here/../lib.sh"
need_binaries; wait_for "$ALPHA_PORT"
DB=gate
say "PostgreSQL $(server_version "$ALPHA_PORT")"
recreate_db "$ALPHA_PORT" "$DB"
kid=$(install_ledger "$ALPHA_PORT" "$DB" "$here/keys" gate)
export DATABASE_URL="$(url "$ALPHA_PORT" "$DB")"
# Worked on a COPY. This example's whole job is to edit an applied
# specification, and the first version edited the file in migrations/ -- so the
# second run started from an already-edited file, applied it in that state, and
# the "edit after apply" became a no-op. It still exited 0 and still looked
# fine. run-all.sh now greps for the line that IS the demonstration, and that
# is how this was found.
build=$here/.build
rm -rf "$build"; mkdir -p "$build"
cp "$here/migrations"/*.json "$build/"
sign_dir "$build" "$here/keys/gate.key.pem" "$kid"

probe() {  # probe <label>
  set +e; "$PG_LASWELL" --repo "$build" --status >/dev/null 2>&1
  printf '  exit %d  %s\n' "$?" "$1"; set -e
}

say "1. Clean: nothing pending, nothing wrong"
"$PG_LASWELL" --repo "$build" >/dev/null
probe "everything applied"

say "2. Drift: the applied file is edited afterwards"
python3 - "$build/0001-orders.json" <<'PY'
import json, sys
p = sys.argv[1]; d = json.load(open(p)); d.pop("signatures", None)
d["description"] = "The orders table (edited after it was applied)."
json.dump(d, open(p, "w"), indent=2)
PY
sign_dir "$build" "$here/keys/gate.key.pem" "$kid"
set +e; "$PG_LASWELL" --repo "$build" --status; echo "  exit $?"; set -e
echo "  The database no longer matches the file claiming to describe it."

say "3. Drift the other way: the applied file is deleted"
mv "$build/0001-orders.json" "$here/0001.hidden"
set +e; "$PG_LASWELL" --repo "$build" --status; echo "  exit $?"; set -e
mv "$here/0001.hidden" "$build/0001-orders.json"

say "4. A configuration problem is its own code"
set +e; DATABASE_URL= "$PG_LASWELL" --repo "$build" --status >/dev/null 2>&1
echo "  exit $? -- no connection, which is 3 and not 2"; set -e

say "In a pipeline"
cat <<'YAML'
  - name: migrations must match the database
    run: pg_laswell --repo ./migrations --status
YAML
