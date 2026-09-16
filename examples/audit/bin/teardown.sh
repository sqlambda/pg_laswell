#!/usr/bin/env bash
# Stop and remove the throwaway cluster this example built. Touches nothing else.
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cluster=$here/.cluster
bin=${PGBIN:-}
if [ -z "$bin" ]; then
  for d in $(ls -d /usr/lib/postgresql/*/bin 2>/dev/null | sort -Vr); do
    [ -x "$d/pg_ctl" ] && { bin=$d; break; }
  done
fi
[ -d "$cluster" ] || { echo "nothing to remove"; exit 0; }
for d in "$cluster"/store "$cluster"/audit; do
  [ -d "$d" ] && "${bin:-/usr/lib/postgresql/18/bin}/pg_ctl" -D "$d" -m immediate -w stop >/dev/null 2>&1 || true
done
rm -rf "$cluster"
# The port went into the subscription spec and its signature; put the
# placeholder back so the repository is the same as it ships.
sed -i 's/port=[0-9][0-9]* dbname=store/port=__PORT__ dbname=store/' \
    "$here/migrations/0040-subscription.json" 2>/dev/null || true
echo "cluster removed"
