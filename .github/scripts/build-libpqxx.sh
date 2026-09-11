#!/bin/sh
#
# Fetch, verify and build libpqxx from a pinned source release, statically.
#
# ONE copy, called by .github/actions/libpqxx (tests.yml) and by all four
# per-platform steps in release.yml. There were five, and the composite
# action's own description said it existed so the two workflows could not
# drift -- while release.yml did not use it and four inline copies matched its
# flags only because nobody had changed one yet. The flags are the thing that
# must not drift: -DBUILD_SHARED_LIBS=OFF is why the packages work at all, and
# a copy that quietly lost it would produce a deb that installs and cannot
# start, which this project has already shipped once.
#
# POSIX sh on purpose. The Debian and Rocky containers give a `run` step
# `sh -e`, not bash, which is how a here-string in a release check came to be a
# syntax error that killed the step before it checked anything.
#
# Usage: build-libpqxx.sh <version> <sha256> [install-prefix] [extra cmake args...]
#
#   install-prefix  empty for the system default (/usr/local), which is what
#                   release.yml wants; the action passes $HOME/pqxx so the
#                   result can be cached, since libpqxx.pc bakes in its prefix.
set -eu

VERSION="${1:?usage: build-libpqxx.sh <version> <sha256> [prefix] [cmake args...]}"
SHA256="${2:?the checksum is required, not optional}"
PREFIX="${3-}"
# Written as an if rather than `[ c ] && a || b`, which is the idiom that
# silently returns 1 when it is the last command a branch runs. Verified
# harmless here in dash, bash and sh -- and written this way so the next
# person does not have to verify it again.
if [ "$#" -ge 3 ]; then shift 3; else shift 2; fi

SRC="/tmp/libpqxx-${VERSION}"
BUILD="/tmp/libpqxx-build"
TARBALL="/tmp/libpqxx.tar.gz"

# To a file and verified before anything is unpacked, rather than piped
# straight into tar. The tarball is fetched by TAG, and a tag is a moving
# reference: whoever can move it can put anything into every binary this
# project ships, statically linked, with nothing in the build log to notice.
# curl -f catches a 404, not a substitution.
#
# If this fails on an unchanged version, do not paste in the new sum. GitHub
# generates these archives on demand and has changed their compression before,
# so a mismatch is either that or an attack, and only one of those is safe to
# wave through: check the upstream tag first.
curl -fsSL -o "$TARBALL" \
  "https://github.com/jtv/libpqxx/archive/refs/tags/${VERSION}.tar.gz"
if command -v sha256sum >/dev/null 2>&1; then
  echo "${SHA256}  ${TARBALL}" | sha256sum -c -
else
  echo "${SHA256}  ${TARBALL}" | shasum -a 256 -c -
fi
rm -rf "$SRC" "$BUILD"
tar -xzf "$TARBALL" -C /tmp

# nproc on Linux, sysctl on macOS, and 2 if neither answers -- a build that
# runs slowly beats one that fails parsing the job count.
JOBS="$( (nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2) | head -1)"

set -- -DCMAKE_BUILD_TYPE=Release \
       -DBUILD_SHARED_LIBS=OFF \
       -DSKIP_BUILD_TEST=ON \
       "$@"
if [ -n "$PREFIX" ]; then
  set -- "$@" -DCMAKE_INSTALL_PREFIX="$PREFIX"
fi

cmake -S "$SRC" -B "$BUILD" "$@"
cmake --build "$BUILD" -j"$JOBS"

# sudo only for a SYSTEM install by a non-root user -- which is what the
# Ubuntu and macOS runners do, while the Debian and Rocky containers are
# already root and have no sudo to call.
#
# Keyed on the prefix, not on the uid. An earlier draft of this script asked
# `id -u` and sudo'd whenever it was not root, which is correct for all four
# release targets and WRONG for the caller that supplies a prefix: the action
# installs into $HOME/pqxx so the result can be cached, and root-owned files
# under $HOME are files actions/cache cannot save. Caught by running it: the
# scratch prefix came out owned by root.
if [ -n "$PREFIX" ] || [ "$(id -u)" -eq 0 ]; then
  cmake --install "$BUILD"
else
  sudo cmake --install "$BUILD"
fi
