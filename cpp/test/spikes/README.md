# Phase 0 spikes

Throwaway scripts that settled design questions before any product code was
written. **Four of them changed the design** (S7, S10, S11 and S12), which is why they are
kept rather than discarded: the next PostgreSQL major is the moment someone
will need to re-run them, and reconstructing them from prose is worse than
re-running a script.

Results, with raw output and the decisions they forced, are in
`docs/ROADMAP.md` §2. That file is the record; these are the instruments.

## Running them

Each script creates whatever it needs inside a database named
`laswell_spike` and leaves it behind for inspection. Create it first, and drop
it when you are finished:

```bash
createdb -p 5555 laswell_spike
bash cpp/test/spikes/s1_snapshot.sh
...
dropdb -p 5555 --force laswell_spike
```

The connection string is hardcoded as `port=5555 dbname=laswell_spike` — these
are local instruments, not a portable suite, and a spike that quietly connects
somewhere else is worse than one that fails to connect at all. Edit `CONN` at
the top if your test cluster is elsewhere.

`s7_pooler.sh` and `s7b_pooler.sh` start their own PgBouncer on ports 6543/6544
with a config in a temp dir, and kill it via its own pidfile on exit. They do
not touch a system PgBouncer. **Do not "clean up" with `pkill -f pgbouncer`** —
that matches the system service too.

## What each one settles

| script | question | verdict |
|---|---|---|
| `s1_snapshot.sh` | does an imported snapshot see the exporter's uncommitted rows? | **no** — verification must run on the worker connection |
| `s2_xmin.sh` | what does holding a snapshot cost? | vacuum blocked database-wide for its whole life |
| `s3_waiter.sh` | how fast is lock-waiter detection, and is `waitstart` usable? | 27 ms including connect; yes |
| `s4_s5_s8.sh` | CIC on a partitioned table; CIC failure state; `ADD COLUMN` rewrite classes | refused; leaves `indisvalid=false` **visible in `pg_indexes`**; non-volatile defaults are metadata-only |
| `s7_pooler.sh` | does `pg_backend_pid()` detect a transaction-mode pooler? | **no** — false negatives at both ends of the pool-size range |
| `s7b_pooler.sh` | then what does transaction mode actually do? | leaks session state **between clients**; `server_reset_query_always` is off by default |
| `s9_observer.sh` | should the observer hold a long transaction? | **no** — locks stay live but cumulative statistics freeze |
| `s10_preimage.sh` | is snapshot export needed for a pre-image reader? | **no** — plain `REPEATABLE READ` suffices; and the snapshot pins at the first *statement*, not at `BEGIN` |
| `s11_contention.sh` | what does a lock pile-up behind a migration look like? | **direct query sees 1 waiter, transitive sees 6** — breaker must use the chain |
| `s11b_chain_cost.sh` | what does the transitive chain cost? | 0.118 ms vs 0.087 ms server-side — no cost argument for the weaker signal |
| `s12_exhaustion.sh` | does the connection-exhaustion budget hold? | **yes, 1:1** — one blocked query pins one connection; harm is *quadratic* in block duration |

## Still outstanding

**S6** — `pqxx::connection::cancel_query()` called from a second thread against
a worker running a long statement, under TSAN. It needs the C++ build, so it is
not a shell script and is not here yet. It gates only the escalation path
(cancel a batch whose waiter has been blocked past `kMaxWaiterWaitMs`); the
fallback if it fails is to drop escalation and lower `batch_rows`.
