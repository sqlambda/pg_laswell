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

# The line that IS the demonstration, per example. Running without failing is
# not the same as still showing anything: an example whose point has quietly
# stopped appearing would pass a check that only looks at the exit code.
expect_for() {
  case $1 in
    single-cluster)        echo "level 2: 0002-status-index" ;;
    ci-gate)               echo "was applied here as" ;;
    release-tag)           echo "held_for_release" ;;
    environments)          echo "for another database or environment" ;;
    two-roles)             echo "both touch public.orders" ;;
    dba-and-app)           echo "declares itself complete" ;;
    adopting-a-database)   echo "deliberately unknown" ;;
    rolling-baseline)      echo "epoch_retired" ;;
    sharded-fleet)         echo "3 concurrent" ;;
    publisher-subscriber)  echo "event 5" ;;
    *)                     echo "" ;;
  esac
}

failed=()
for e in "${EXAMPLES[@]}"; do
  printf '\n\033[1m=== %s ===\033[0m\n' "$e"
  if ! "$here/$e/run.sh" >"$here/.$e.log" 2>&1; then
    failed+=("$e")
    echo "FAILED -- last lines:"
    tail -12 "$here/.$e.log" | sed 's/^/    /'
    continue
  fi
  want=$(expect_for "$e")
  if [ -n "$want" ] && ! grep -qF "$want" "$here/.$e.log"; then
    failed+=("$e")
    echo "RAN BUT DID NOT DEMONSTRATE: expected to see \"$want\""
    tail -12 "$here/.$e.log" | sed 's/^/    /'
    continue
  fi
  echo "ok${want:+  (showed \"$want\")}"
  rm -f "$here/.$e.log"
done

printf '\n'
if [ ${#failed[@]} -eq 0 ]; then
  echo "all ${#EXAMPLES[@]} examples ran"
else
  echo "${#failed[@]} of ${#EXAMPLES[@]} failed: ${failed[*]}"
  echo "logs are in $here/.<name>.log"
  exit 1
fi
