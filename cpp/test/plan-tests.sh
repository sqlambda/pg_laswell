#!/usr/bin/env bash
#
# Golden-file plan tests, in the shape of PostgreSQL's own regression suite.
#
#   plans/NAME.spec.json   the input
#   plans/NAME.obs.json    the observations to plan against (or shared.obs.json)
#   plans/expected/NAME.out the transcript, committed and reviewed
#
# and, on failure, plans/results/NAME.out beside a unified diff -- pg_regress's
# arrangement, for pg_regress's reason: the first thing you want when a test
# fails is the actual output in a file you can look at and copy.
#
# WHY THIS EXISTS ALONGSIDE THE UNIT TESTS. There are ninety-odd assertions in
# test_main.cpp of the form EXPECT_NE(w.find("some phrase"), npos). Each one
# checks that a warning still contains a fragment, and none of them shows a
# reader what the warning SAYS. A golden file shows the whole thing, and a
# change to it arrives as a diff somebody reads rather than as a substring that
# silently still matches.
#
# Errors are output, not failures -- as in pg_regress, where ERROR lines sit in
# the expected files. A refused spec is a behaviour worth reviewing, and it
# changes more often than a success does.
#
# Regenerate after an intended change:  PLAN_TESTS_ACCEPT=1 plan-tests.sh ...
set -uo pipefail

RENDER="${1:?usage: plan-tests.sh <render_plan binary> <plans dir>}"
DIR="${2:?usage: plan-tests.sh <render_plan binary> <plans dir>}"
ACCEPT="${PLAN_TESTS_ACCEPT:-}"

mkdir -p "$DIR/expected" "$DIR/results"
pass=0; fail=0; wrote=0

shopt -s nullglob
for spec in "$DIR"/*.spec.json; do
  name=$(basename "$spec" .spec.json)
  obs="$DIR/$name.obs.json"
  [ -f "$obs" ] || obs="$DIR/shared.obs.json"
  if [ ! -f "$obs" ]; then
    printf '  FAIL %-40s no observations file\n' "$name"; fail=$((fail+1)); continue
  fi

  actual="$DIR/results/$name.out"
  # stderr is folded in deliberately: a renderer that dies has produced output
  # too, and hiding it would make the diff say "empty" instead of why.
  "$RENDER" "$spec" "$obs" > "$actual" 2>&1
  rc=$?
  if [ "$rc" -gt 1 ]; then
    printf '  FAIL %-40s renderer exited %s\n' "$name" "$rc"
    sed 's/^/       /' "$actual" | head -5
    fail=$((fail+1)); continue
  fi

  expected="$DIR/expected/$name.out"
  if [ -n "$ACCEPT" ]; then
    cp "$actual" "$expected"; printf '  wrote %s\n' "$name"; wrote=$((wrote+1)); continue
  fi
  if [ ! -f "$expected" ]; then
    printf '  FAIL %-40s no expected output. Review %s and rerun with\n' "$name" "$actual"
    printf '       PLAN_TESTS_ACCEPT=1 to accept it.\n'
    fail=$((fail+1)); continue
  fi
  if diff -u "$expected" "$actual" > "$DIR/results/$name.diff" 2>&1; then
    printf '  ok   %s\n' "$name"; pass=$((pass+1)); rm -f "$DIR/results/$name.diff"
  else
    printf '  FAIL %-40s the plan changed:\n' "$name"
    sed 's/^/       /' "$DIR/results/$name.diff" | head -30
    fail=$((fail+1))
  fi
done

if [ -n "$ACCEPT" ]; then
  echo "$wrote expected file(s) written -- READ THE DIFF before committing them."
  exit 0
fi
echo
echo "$pass passed, $fail failed"
[ "$fail" = 0 ] || exit 1
