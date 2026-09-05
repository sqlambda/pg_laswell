#!/usr/bin/env bash
#
# One bug shape, five times, in one project.
#
#   for (auto& [k, v] : obj.value("key", json::object()).items())
#
# `value()` returns a TEMPORARY json; `.items()` holds a reference into it that
# dies at the end of the full expression, and the loop then walks freed memory.
# GCC's -Wdangling-reference catches it, but only sometimes and only on some
# compilers -- it has caught it here every time so far, and that is luck rather
# than a guarantee. This makes it impossible instead: bind the temporary to a
# named local first.
#
# Runs as a ctest entry, so it fails the build rather than a review.
set -uo pipefail
SRC="${1:?usage: no-dangling-items.sh <src dir>}"

# A .value(...) call whose closing paren is followed directly by .items(),
# .begin() or .end() -- allowing for a line break inside the arguments.
hits=$(perl -0777 -ne '
  while (/\.value\s*\([^;]*?\)\s*\.\s*(items|begin|end)\s*\(/gs) {
    my $pre = substr($_, 0, pos($_));
    my $line = ($pre =~ tr/\n//) + 1;
    print "$ARGV:$line\n";
  }' "$SRC"/*.h "$SRC"/*.cpp 2>/dev/null)

if [ -n "$hits" ]; then
  echo "A temporary from .value() is being iterated. Bind it to a named local"
  echo "first, or the loop walks freed memory:"
  echo "$hits" | sed 's/^/  /'
  echo
  echo "  const json x = obj.value(\"key\", json::object());"
  echo "  for (const auto& [k, v] : x.items()) { ... }"
  exit 1
fi
echo "ok   no temporary from .value() is iterated"
