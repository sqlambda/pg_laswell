# Installing pg_laswell

**Pre-release.** Packages are built and verified for seven targets, each one
installed and run inside the platform it targets before it is uploaded. They
are not yet published anywhere, so build them yourself or build from source —
see [BUILD.md](BUILD.md).

| target | artifact |
|---|---|
| Linux x86_64 (portable) | `pg_laswell-linux-x86_64.tar.gz` |
| Linux arm64 (portable) | `pg_laswell-linux-arm64.tar.gz` |
| Debian 13 x86_64 | `pg_laswell-linux-x86_64-debian13.deb` |
| Debian 13 arm64 | `pg_laswell-linux-arm64-debian13.deb` |
| Rocky/RHEL 9 x86_64 | `pg_laswell-linux-x86_64-rocky9.rpm` |
| Rocky/RHEL 9 arm64 | `pg_laswell-linux-arm64-rocky9.rpm` |
| macOS arm64 | `pg_laswell-macos-arm64.tar.gz` |

```bash
cmake -S cpp -B cpp/build -DCMAKE_BUILD_TYPE=Release
cmake --build cpp/build
cd cpp/build && cpack -G DEB      # or -G RPM, -G TGZ
```

Two limits worth stating rather than discovering. The **tarballs** are built on
the runner's own glibc and glibc is backward-compatible, not forward — so a
Linux tarball runs on that release and newer, never on an older one; build from
source there. The **macOS tarball** links Homebrew's libpq and openssl@3, so it
needs `brew install libpq openssl@3`; a Homebrew formula would declare those,
and there is not one yet.

This file records what installation involves, because three parts of it are
decisions rather than steps.

## What gets installed

| | |
|---|---|
| `bin/pg_laswell_mcp` | the binary; MCP over stdio, or `--call` for one tool |
| `share/man/man1/pg_laswell_mcp.1` | the authoritative reference — read it |
| `share/pg_laswell/bootstrap.sql` | run by hand, once, by a DBA |

**Dependencies are derived from the binary, never listed by hand.** The DEB
uses `dpkg-shlibdeps` and the RPM uses rpmbuild's soname scan, so the package
describes what was actually linked rather than what someone believed was
linked.

**Release packages link libpqxx statically**, built from a pinned source
release inside the target distribution's own container (`debian:trixie`,
`rockylinux:9`) — the same method as pg_licht. libpq stays dynamic: its C ABI
is stable and distributions patch it independently, so it should come from the
target's own repository. Release dependencies are then:

```
libc6 (>= 2.38), libgcc-s1 (>= 4.3), libpq5 (>= 10~~),
libssl3t64 (>= 3.0.0), libstdc++6 (>= 14)
```

A **local** `cpack` links your distribution's shared libpqxx instead and will
name it (`libpqxx-7.10` on Debian 13). That package is fine on the machine that
built it and is not what gets released — a soname like `libpqxx-7.10.so` is
provided by no RPM distribution and pins the deb to one Debian release.

Two failures this arrangement exists to prevent, both observed rather than
imagined. A hand-written `libpq5` was wrong in both directions: it missed
`libpqxx-7.10` and `libstdc++6 (>= 14)`, and named `libpq5` redundantly. And
`CPACK_DEBIAN_PACKAGE_SHLIBDEPS` silently needs the `file` utility — without it
CPack produces no package at all.

## 1. Bootstrap, by hand

`sql/bootstrap.sql` is run once by a superuser or owner role. pg_laswell never
runs it, and that is a privilege argument rather than a convenience one:

> If the tool created `laswell.trusted_key`, its role would own it — and a role
> that owns a table can `INSERT` into it, which means the migrating role could
> grant itself trust and the whole gate would collapse into a comment.

```bash
psql -v ON_ERROR_STOP=1 \
     -v laswell_role=laswell_runner \
     -v first_key_id=ed25519:9f2c41b7d0e6a85c \
     -v first_key_b64=<base64 of the raw 32-byte public key> \
     -v first_key_label=ops-prod \
     -f /usr/share/pg_laswell/bootstrap.sql
```

Re-running at the same schema version is a no-op. Against a different version
it raises rather than silently upgrading.

Afterwards, `checkPrivileges` will warn if the runtime role can still write
`laswell.trusted_key` — that is, if you ran the script *as* that role, which
defeats the point.

## 2. The connection budget

pg_laswell uses **three connections per job**, and the third is not negotiable:

| connection | why it is separate |
|---|---|
| worker | owns every transaction; commits explicitly |
| observer | short `READ ONLY` transactions, one per tick |
| coordination | holds the session advisory locks and writes the ledger |

A ledger write on the worker would be rolled back by a worker rollback, so a
failure would erase its own record. An advisory lock on the worker would be
dropped by a worker reconnect. Different requirements, different connections.

At the default `max_concurrent_jobs = 2` that is six, plus one cached read
connection and one for the process-wide observer — call it **eight**.

**Size `max_connections` against that, but understand which limit actually
matters.** The ceiling that decides whether an application survives being
blocked is *its own* connection-pool limit, not the server's: an application
with a 20-connection pool collapses at 20 blocked queries however generous
`max_connections` is. That limit is invisible from the server, so set it:

```ini
[executor]
app_pool_size = 20
```

Left at `0` the budget reports the server's ceiling and says so. Measured: one
blocked query pins exactly one connection, so `headroom / arrival_rate` is the
time you have.

## 3. Poolers

**The executor requires a direct connection.** Not a style preference — with
PgBouncer in transaction mode, a session GUC set by one client was readable by
a *different* client, and an advisory lock taken by one was visible as held to
another. `server_reset_query` is not run in transaction mode unless
`server_reset_query_always` is enabled, and it is off by default.

Since `pg_try_advisory_lock` is re-entrant within a session, two jobs sharing
one server connection would both believe they hold the same exclusive lock,
with no error anywhere.

Startup detection is not achievable, so the worker re-reads its backend PID at
the start of every transaction and aborts on any change. See the POOLERS
section of `man pg_laswell_mcp`.

The read-only tools — `checkPrivileges`, `validateSpec`, `planMigration`,
`listMigrations` — are pooler-safe.

## Attaching it to an agent

```bash
claude mcp add --transport stdio pg-laswell \
  -e DATABASE_URL="postgresql://user@host/dbname" \
  -- /usr/local/bin/pg_laswell_mcp
```

Attach [pg_licht](https://github.com/sqlambda/pg_licht) alongside it. pg_laswell
measures the harm *it* causes by holding locks; it is blind to I/O bandwidth,
WAL volume, replication lag and the vacuum debt a large backfill leaves. Those
are pg_licht's readings, and pg_laswell names them where they matter.

Because pg_licht is read-only *by invariant* and pg_laswell is not, the two can
be attached at different trust levels — which is worth more than the
deduplication that merging them would save.

## In a pipeline

CI is the one caller that is neither an agent nor a person. `--call` runs one
tool and **exits non-zero when it reports a problem**:

```bash
pg_laswell_mcp --call listMigrations \
  --args '{"directory":"db/migrations"}' || exit 1
```

That fails the build on a missing dependency, a dependency cycle, or — the case
worth having — a spec that was **edited after it was applied**, where the
database no longer matches the file that claims to describe it.

## Verifying an installation

```bash
pg_laswell_mcp --version
pg_laswell_mcp --call checkPrivileges "postgresql://..."
man pg_laswell_mcp
```

`checkPrivileges` is the one to read: it distinguishes *denied* from
*degraded*, and a degraded answer is easy to mistake for a healthy one.
