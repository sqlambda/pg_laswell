# Building pg_laswell

The source root is `cpp/`, not the repository root. Every invocation is
therefore `-S cpp`.

## Dependencies

All from system packages but one. There is no vcpkg, conan, FetchContent or
submodule: the dependency list is the thing an operator has to install, so it is
kept short and boring.

| | Debian/Ubuntu | Fedora/RHEL |
|---|---|---|
| libpqxx **8.0.2**, built from source (below) | — | — |
| libpq | `libpq-dev` | `libpq-devel` |
| nlohmann/json | `nlohmann-json3-dev` | `json-devel` |
| OpenSSL | `libssl-dev` | `openssl-devel` |
| GoogleTest | `libgtest-dev` | `gtest-devel` |
| mandoc *(optional)* | `mandoc` | `mandoc` |
| valgrind *(optional)* | `valgrind` | `valgrind` |

### libpqxx: exactly 8.0.2

pg_laswell builds against one libpqxx, the version CI and every release build
from source, and CMake refuses any other. A distribution's package is whatever
that distribution froze (Debian 13 ships 7.10), and code that compiles against
two versions is written to what they share rather than to what the pinned one
can do: the first build of this against 7.10 compiled here and failed in CI,
where `sql_error::sqlstate()` returns a `string_view`.

Build it the way CI does, with the version and checksum CI pins
(`PQXX_VERSION` and `PQXX_SHA256` in `.github/workflows/tests.yml`), into a
prefix of your own; nothing is installed system-wide:

```bash
sh .github/scripts/build-libpqxx.sh 8.0.2 <PQXX_SHA256> $HOME/.local/opt/libpqxx-8.0.2
export CMAKE_PREFIX_PATH=$HOME/.local/opt/libpqxx-8.0.2
```

CMake reads `CMAKE_PREFIX_PATH` from the environment, so with it exported every
`cmake` command in this file finds the pinned libpqxx; `-DCMAKE_PREFIX_PATH=...`
on the command line does the same for one build tree. Without it, configuring
stops and says which libpqxx it found instead.

It is built as a static library, as in the release, so the binaries carry it and
need no libpqxx at run time.

OpenSSL is linked for `OpenSSL::Crypto` only — Ed25519 signature verification,
never TLS. It adds no new *runtime* package: `libpq5` already depends on
libcrypto, which is why libsodium was not used despite its nicer API.

## Build and test

```bash
cmake -S cpp -B cpp/build
cmake --build cpp/build -j
ctest --test-dir cpp/build --output-on-failure
```

`ctest` registers, with the Citus module built in:

| Entry | What it runs |
|---|---|
| `mcp_test` | the gtest binary |
| `pg_laswell_mcp_test_valgrind` | the same under Valgrind, when valgrind is present and no sanitizer is active |
| `manpage_lint` | `mandoc -Tlint` of every man page |
| `manpage_kinds` | `tools/check-manual.py`: every kind documented, on the right page |
| `no_dangling_items` | a source check for iterating a temporary JSON value, a bug shape found five times |
| `plans` | the plan renderer against committed transcripts |
| `replication` | the replication suite; it needs a second cluster and skips without one |
| `call_mode` | the binaries' `--call` mode |
| `citus_tests` | the live Citus suite, only in a build with the module; it skips without `CITUS_URL` |

A build without the module registers the same list minus `citus_tests`.

### Tests and the database

The pure layers — canonicalisation, signature verification, spec parsing and
the planner — have no database dependency and always run. `planner.h` must not
even include pqxx; that is asserted at compile time, and it is what makes plan
determinism testable against a fixed observation struct.

Database tests skip when `DATABASE_URL` is unset:

```bash
DATABASE_URL="port=5555 dbname=postgres" ctest --test-dir cpp/build
```

A skip nobody notices is a test that silently stopped running, so CI sets
`PGLASWELL_REQUIRE_DATABASE=1`, which turns that skip into a failure.

## Modules

A vendor module is compiled in, never loaded at runtime:

```bash
cmake -S cpp -B cpp/build -DPGLASWELL_MODULES=citus
```

The released package is built this way, and `pg_laswell --version` prints
`modules: citus`. Without the option the binary is PostgreSQL-only, and CI builds
both: the plain builds prove core needs no module, the module builds prove the
module. A module's kinds are refused by a binary without it, as a whole
specification, never skipped.

The Citus module's live suite needs a coordinator and two workers:

```bash
docker compose -f examples/docker/citus/compose.yml up -d
./examples/docker/citus/run.sh                   # the example, and a database the suite uses
CITUS_URL=postgresql://postgres:laswell@127.0.0.1:55440/citus_example \
  ctest --test-dir cpp/build -R citus_tests --output-on-failure
```

How a module is written is in `cpp/src/modules/README.md`.

## Warnings are errors

`-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wuninitialized
-Wshadow -Werror`, always, plus a hardened standard library
(`_GLIBCXX_ASSERTIONS` on GCC, `_LIBCPP_HARDENING_MODE_FAST` on Clang). Expect
to write explicit `static_cast`s for any `size_t`/`int`/`pqxx::result::size_type`
conversion.

Build with **both** compilers before proposing a change; they disagree about
`-Wconversion` in places, particularly around the OpenSSL C boundary:

```bash
cmake -S cpp -B cpp/build_clang -DCMAKE_CXX_COMPILER=clang++
cmake --build cpp/build_clang -j
```

## Sanitizers

One cache variable, not a flag per sanitizer. An unrecognised value is a
configure-time `FATAL_ERROR` rather than a silently ignored typo.

```bash
cmake -S cpp -B cpp/build_tsan -DPGLASWELL_SANITIZER=THREAD
cmake --build cpp/build_tsan -j && ctest --test-dir cpp/build_tsan
```

Accepted: `NONE`, `ADDRESS`, `UNDEFINED`, `ADDRESS_UNDEFINED`, `THREAD`.

**THREAD is a gate here, not belt-and-braces.** pg_licht is a single-threaded
`getline` loop; this project runs a worker thread per job plus one process-wide
observer thread against a shared `JobRegistry`, so a data race is a real
possibility rather than a theoretical one. The Valgrind ctest entry
self-disables under any sanitizer, because an instrumented binary under
Valgrind produces instrumentation conflicts rather than findings.

## Spikes

`cpp/test/spikes/` holds the shell scripts that settled design questions before
the code existed — four of them changed the design. They are instruments, not a
portable suite; see the README there. Do not "clean up" after them with
`pkill -f pgbouncer`, which matches the system service too.

## Packaging

```bash
cmake -S cpp -B cpp/build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
      -DPGLASWELL_MODULES=citus
cmake --build cpp/build --target pg_laswell pg_laswell_mcp
cd cpp/build && cpack -G DEB    # or RPM
```

That is what the release builds: one package, `pg-laswell`, with every module
compiled in, and each module's man page installed in section 7.

The man pages are checked in rather than generated: release containers carry no
doc toolchain, and the release build names its targets rather than `all`, so a
generator target would never run and CPack would fail on a missing file.
