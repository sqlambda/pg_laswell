# Building pg_laswell

The source root is `cpp/`, not the repository root. Every invocation is
therefore `-S cpp`.

## Dependencies

All from system packages. There is no vcpkg, conan, FetchContent or submodule:
the dependency list is the thing an operator has to install, so it is kept
short and boring.

| | Debian/Ubuntu | Fedora/RHEL |
|---|---|---|
| libpqxx | `libpqxx-dev` | `libpqxx-devel` |
| libpq | `libpq-dev` | `libpq-devel` |
| nlohmann/json | `nlohmann-json3-dev` | `json-devel` |
| OpenSSL | `libssl-dev` | `openssl-devel` |
| GoogleTest | `libgtest-dev` | `gtest-devel` |
| mandoc *(optional)* | `mandoc` | `mandoc` |
| valgrind *(optional)* | `valgrind` | `valgrind` |

OpenSSL is linked for `OpenSSL::Crypto` only — Ed25519 signature verification,
never TLS. It adds no new *runtime* package: `libpq5` already depends on
libcrypto, which is why libsodium was not used despite its nicer API.

## Build and test

```bash
cmake -S cpp -B cpp/build
cmake --build cpp/build -j
ctest --test-dir cpp/build --output-on-failure
```

`ctest` registers three entries: the gtest binary, a Valgrind-wrapped run of it
(auto-registered when valgrind is present and no sanitizer is active), and a
`mandoc -Tlint` of the man page.

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
cmake -S cpp -B cpp/build -DCMAKE_BUILD_TYPE=Release
cmake --build cpp/build --target pg_laswell_mcp
cd cpp/build && cpack -G DEB    # or RPM
```

The man page is checked in rather than generated: release containers carry no
doc toolchain, and the release build names a single target rather than `all`,
so a generator target would never run and CPack would fail on a missing file.
