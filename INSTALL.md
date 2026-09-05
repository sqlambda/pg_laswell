# Installing pg_laswell

**Pre-release. There are no packages yet** — build from source, see
[BUILD.md](BUILD.md). This file records what installation involves, because
three parts of it are decisions rather than steps.

## What gets installed

| | |
|---|---|
| `bin/pg_laswell_mcp` | the binary; MCP over stdio, or `--call` for one tool |
| `share/man/man1/pg_laswell_mcp.1` | the authoritative reference — read it |
| `share/pg_laswell/bootstrap.sql` | run by hand, once, by a DBA |

The only shared runtime dependency is `libpq5`. libcrypto is not listed
separately because `libpq5` already depends on it; claiming it twice would mean
two differently-named package requirements per distribution for one library.

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
