# pg_laswell

**Intelligent migration for PostgreSQL.** It decides *how* to apply a change by
measuring the database in front of it, and applies it without blocking the
application any longer than it has to.

Intelligent here is a claim with a specific meaning, not a slogan. It measures
before it chooses and names the reading that decided each choice. It paces on
contention rather than on a schedule. It refuses what it can prove wrong, and
says so plainly where it can prove nothing. It converges databases that have
drifted. And it never states a number it cannot derive.

**Status: pre-0.1.0.** The whole path works — repository, signing, planning,
dry run, paced execution, ledger — with 260 tests green on GCC and Clang, under
AddressSanitizer/UBSan and ThreadSanitizer. Packages build for seven targets —
deb, rpm and tarball, x86_64 and arm64, plus macOS arm64 — each installed and
run inside the platform it targets before upload. The manual is the
authoritative reference. Not yet published anywhere.

## Why

Migrations are usually hand-written SQL with a version string. That loses the
two things that decide whether a change is safe:

- **The safe method is a property of the table, not the statement.**
  `CREATE INDEX` is instant on an empty table and an outage at a billion rows.
  A script written elsewhere, against a database of a different size, cannot
  know which it faces.
- **Pacing depends on contention, not on row count.** A backfill batched at
  "10 000 rows per commit" holds locks for however long 10 000 rows happens to
  take today. It should commit when something starts *waiting* on it.

So a pg_laswell specification states a business-level **change intent** — "add
column `region` to `orders`", "backfill it from `warehouse`" — and the tool
reads the live catalog, table sizes, lock state and connection budget to derive
a deterministic plan. Good practice lives in the tool rather than in every
script an engineer writes.

It is deliberately PostgreSQL-only, so it can use
`pg_stat_progress_create_index`, `pg_blocking_pids()`, `NOT VALID` constraints
and transactional DDL without abstracting any of it away.

## The name

Laswell, the East Beast Lord — the same supreme hierarchy as Clevatess and
Vordein — is the administrator of the undocumented legacy database. The rest of
the team does not understand exactly how his internal system operates, but they
fear the impact of an outage and avoid any change that might provoke his wrath.

That is the problem this tool exists for. The database nobody will touch is not
usually fragile; it is *unmeasured*. pg_laswell's answer is to measure before it
acts, say which reading decided the method, and record what it did — so a change
becomes something you can read afterwards rather than something you feared
beforehand.

## Its sibling

[pg_licht](https://github.com/sqlambda/pg_licht) is read-only by invariant and
can only advise; its `plan-schema-change` prompt is effectively a written
specification for how to reason about a schema change. **pg_laswell is the half
that is allowed to act.** Use both: read load, locks and capacity through
pg_licht, and apply change through pg_laswell.

The division of labour is real and worth stating: pg_laswell measures the harm
*it* causes — backends transitively blocked by its own backend. It is blind to
harm it inflicts without holding a lock, such as I/O bandwidth, WAL volume and
replication lag. Those are pg_licht's readings.

## What it does

- **Signed specifications.** A spec is JSON, canonicalised with RFC 8785, and
  signed with Ed25519. The database itself holds the list of keys it trusts, so
  a permissive client configuration cannot talk a production database into
  accepting a development key.
- **Deterministic planning.** The same spec against the same observations
  produces a byte-identical plan, and the plan says which measurement decided
  each choice before anything runs.
- **Lock-aware pacing.** A backfill commits when a waiter appears or an interval
  elapses, not at a fixed row count, and backs off when it is causing a pile-up.
- **A ledger.** What ran, what it was signed by, what was measured, and how long
  each step took — readable from the database afterwards, during an incident.
- **A migration repository.** A directory of specs, classified against one
  database's ledger: what is pending, in `depends_on` order, and which groups
  may run concurrently. It also flags a spec that was **edited after it was
  applied** — the database no longer matches the file that claims to describe
  it.
- **Constraints as first-class changes.** `add_foreign_key`,
  `add_check_constraint` and `set_not_null` plan as `NOT VALID` + `VALIDATE` in
  separate transactions; `drop_constraint` refuses, before anything runs, a
  drop PostgreSQL would reject at execution — and never emits `CASCADE`.
- **Convergence.** The same spec creates an index where it is missing and
  renames one that was built by hand under a different name, so a repository
  drives every database to the same state rather than only the clean ones.
- **Agent operation.** It is an MCP server. Migrations return a job id
  immediately; `jobStatus` reports progress, contention and per-step results
  while they run.

Measured on 300 000 rows with an application contending: **48 of 63 commits
fired on a lock waiter and none on the interval**, finishing in the same wall
time as an uncontended run. Pacing on contention rather than on a schedule
costs nothing and bounds the harm.

Signing answers *"did this exact change come from someone we trust?"*, not
*"should this change be allowed?"*. The second question is `GRANT`'s job.

## Building

PostgreSQL 14+ headers, libpqxx, nlohmann/json, OpenSSL and GoogleTest, all
from system packages. See [BUILD.md](BUILD.md).

```bash
cmake -S cpp -B cpp/build
cmake --build cpp/build
ctest --test-dir cpp/build
```

## Poolers

The executor requires a **direct connection**. This is a departure from
pg_licht, which is deliberately pooler-safe, and the reason is measured rather
than stylistic: in PgBouncer's transaction mode, `server_reset_query` is not run
unless `server_reset_query_always` is enabled — and it is off by default. Session
state therefore leaks *between clients*, and since `pg_try_advisory_lock` is
re-entrant within a session, two jobs sharing one server connection would both
believe they hold the same exclusive lock, with no error anywhere.

See the POOLERS section of `man pg_laswell_mcp` for how this is detected.

## License

Apache 2.0 — see [LICENSE](LICENSE).
