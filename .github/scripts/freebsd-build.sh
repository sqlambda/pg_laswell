#!/bin/sh
# Build, package, install and run pg_laswell on FreeBSD -- inside the VM that
# vmactions/freebsd-vm boots on a Linux runner, because GitHub has no FreeBSD
# runners. Called by tests.yml (every pull request, so a FreeBSD break is found
# before a tag) and by release.yml (which publishes the package).
#
# Usage: freebsd-build.sh <artifact-name> [tests]
#   artifact-name  the .pkg is renamed to <artifact-name>.pkg
#   tests          also build and run the suite that needs no database
#
# POSIX sh: the VM's /bin/sh, not bash.
#
# Why this exists at all: the pgshard lab runs FreeBSD 15.1 guests several times
# a day and still gave the FreeBSD build no coverage, because with no package to
# install it ran pg_laswell from the host. A package a guest can `pkg add` is
# what turns such a lab into coverage.
set -eu

ARTIFACT="${1:?usage: freebsd-build.sh <artifact-name> [tests]}"
TESTS="${2:-}"

freebsd-version
# The same packages BUILD.md lists. googletest only when the suite is built.
PKGS="cmake ninja pkgconf curl postgresql18-client nlohmann-json"
[ -n "$TESTS" ] && PKGS="$PKGS googletest"
# shellcheck disable=SC2086
env ASSUME_ALWAYS_YES=yes pkg install -y $PKGS

# libpqxx exactly as every other target builds it: pinned, checksummed,
# static, into /usr/local.
sh .github/scripts/build-libpqxx.sh "${PQXX_VERSION}" "${PQXX_SHA256}" ""
# Its CMake install writes libpqxx.pc to /usr/local/lib/pkgconfig, and
# FreeBSD's pkgconf searches /usr/local/libdata/pkgconfig -- so without this
# the configure below reports "Package 'libpqxx' not found" right after the
# install succeeded (the first CI run, 2026-10-03).
PKG_CONFIG_PATH="/usr/local/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export PKG_CONFIG_PATH

TESTING=OFF
[ -n "$TESTS" ] && TESTING=ON
cmake -S cpp -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING="$TESTING" \
      "-DPGLASWELL_MODULES=citus;pg_cron;pgvector;timescaledb"
if [ -n "$TESTS" ]; then
  cmake --build build
  ./build/pg_laswell_mcp_test
else
  cmake --build build --target pg_laswell pg_laswell_mcp
fi
strip build/pg_laswell build/pg_laswell_mcp

# Static libpqxx is the point of the source build; a silent fall back to a
# shared one would ship a package that installs and cannot start.
for b in pg_laswell pg_laswell_mcp; do
  readelf -d "build/$b" | grep NEEDED
  if readelf -d "build/$b" | grep -q pqxx; then
    echo "libpqxx was linked dynamically into $b" >&2
    exit 1
  fi
done

# A native package. pkg records the shared libraries the binaries need
# (shlibs_required) by itself, from the ELF -- derived, as the deb's and rpm's
# dependencies are, never listed by hand.
(cd build && cpack -G FREEBSD)
mv build/pg-laswell-*.pkg "./${ARTIFACT}.pkg"
pkg info -F "./${ARTIFACT}.pkg"
pkg info -R -F "./${ARTIFACT}.pkg" 2>/dev/null || true

# Installed, and run -- metadata is not evidence a package works.
pkg add "./${ARTIFACT}.pkg"
for f in /usr/local/bin/pg_laswell /usr/local/bin/pg_laswell_mcp \
         /usr/local/share/man/man1/pg_laswell.1.gz /usr/local/share/man/man7/pg_laswell_timescaledb.7.gz \
         /usr/local/share/pg_laswell/bootstrap.sql; do
  [ -e "$f" ] || [ -e "${f%.gz}" ] || { echo "missing from the package: $f" >&2; exit 1; }
done
pg_laswell_mcp --version
pg_laswell_mcp --version | grep -qx 'modules: citus,pg_cron,pgvector,timescaledb'
SPEC='{"spec":{"laswell_spec_version":1,"id":"x","description":"d","intents":[{"kind":"add_column","schema":"s","table":"t","column":"c","type":"text","nullable":true,"comment":"c"}]}}'
pg_laswell_mcp --call getSpecDigest --args "$SPEC" | grep -q '"digest"'
pg_laswell --version
set +e; pg_laswell >/dev/null 2>&1; rc=$?; set -e
[ "$rc" = 3 ] || { echo "pg_laswell with no --repo exited $rc, expected 3" >&2; exit 1; }
echo "FreeBSD $(freebsd-version): ${ARTIFACT}.pkg built, installed, and both binaries answered"
