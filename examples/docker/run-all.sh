#!/usr/bin/env bash
# Every example, in order, against the containers in compose.yml.
#
#   docker compose -f examples/docker/compose.yml up -d
#   ./examples/docker/run-all.sh
#   docker compose -f examples/docker/compose.yml down -v
set -uo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

# Ordered so the simplest is first: if this one fails, none of the others will
# tell you anything the first did not.
EXAMPLES=(
  single-cluster
  ci-gate
  release-tag
  environments
  two-roles
  dba-and-app
  adopting-a-database
  rolling-baseline
  sharded-fleet
  publisher-subscriber
)

failed=()
for e in "${EXAMPLES[@]}"; do
  printf '\n\033[1m=== %s ===\033[0m\n' "$e"
  if ! "$here/$e/run.sh" >"$here/.$e.log" 2>&1; then
    failed+=("$e")
    echo "FAILED -- last lines:"
    tail -12 "$here/.$e.log" | sed 's/^/    /'
  else
    echo "ok"
    rm -f "$here/.$e.log"
  fi
done

printf '\n'
if [ ${#failed[@]} -eq 0 ]; then
  echo "all ${#EXAMPLES[@]} examples ran"
else
  echo "${#failed[@]} of ${#EXAMPLES[@]} failed: ${failed[*]}"
  echo "logs are in $here/.<name>.log"
  exit 1
fi
