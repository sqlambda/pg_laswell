# Installing pg_laswell

**Pre-release. There are no packages yet** — build from source, see
[BUILD.md](BUILD.md).

This file records what installation will involve, because two parts of it are
decisions rather than steps.

## Connection budget

The executor uses **three connections per job**, and the third is not
negotiable:

| connection | why it is separate |
|---|---|
| worker | owns every transaction; commits explicitly |
| observer | short `READ ONLY` transactions, one per tick |
| coordination | holds the session advisory locks and writes the ledger |

A ledger write on the worker would be rolled back by a worker rollback, so a
failure would erase its own record. An advisory lock on the worker would be
dropped by a worker reconnect. Different requirements, different connections.

At the default `max_concurrent_jobs = 2` that is six connections, plus one
cached read connection for the catalog tools. Size `max_connections` with that
in mind — and note that the number that actually matters for safety is your
*application's* pool limit, not the server's, because the application collapses
when its own pool fills.

## Bootstrap

`sql/bootstrap.sql` is run **by hand, once, by a DBA or superuser**. It is
deliberately not something the binary does.

The reason is a privilege argument, not a convenience one. If the tool created
`laswell.trusted_key`, its role would own it — and a role that owns a table can
`INSERT` into it, which means the migrating role could grant itself trust and
the whole trust gate would collapse into a comment. The chicken-and-egg is not
solvable inside the binary; it is solved by putting the trust root outside it.

The script creates the `laswell` schema owned by the superuser, creates the
ledger tables with `COMMENT ON` on every table and column, inserts the first
trusted key, and grants the runtime role `SELECT` on `laswell.trusted_key` and
nothing more.

## Poolers

The executor requires a direct connection. See the POOLERS section of
`man pg_laswell_mcp`, and the README, for the measurements behind that.
