# Runnable examples, on containers

Every arrangement the landing page describes, as something you can run.

```sh
cmake -S cpp -B cpp/build && cmake --build cpp/build   # the binaries
docker compose -f examples/docker/compose.yml up -d
./examples/docker/run-all.sh                            # or one at a time
docker compose -f examples/docker/compose.yml down -v
```

Needs `psql`, `openssl`, `python3` and `docker compose`.

## Three clusters, not one database

`compose.yml` starts **three** PostgreSQL servers on 55432, 55433 and 55434.
Three, because two arrangements cannot be shown inside one cluster at all:
logical replication needs a publisher and a subscriber (a subscription to a
publication in the same cluster deadlocks against its own walsender), and a
sharded fleet where every shard's database is called `app` is not expressible
where names are unique. A second *database* would prove neither.

The image is `postgres:latest` deliberately. These exist to be run against
whatever PostgreSQL is current, and an example pinned to the version its author
happened to have stops being evidence about the version you have. Every script
prints the server version it found.

## The examples

| | What it demonstrates |
|---|---|
| `single-cluster/` | The baseline. One directory, one ledger, and a dry run that is honest about being per specification. |
| `ci-gate/` | Four exit codes a pipeline can branch on, and the three ways drift is caught. |
| `release-tag/` | A change that waits until *this* database approves its release, recorded with who and when. |
| `environments/` | One repository, three environments. The declaration is signed; the label is in the database. |
| `two-roles/` | Two connection names, one database — and why that is not two databases. |
| `dba-and-app/` | Two teams, one database, neither refused for the other's absence. |
| `adopting-a-database/` | Ten years of DDL nobody scripted, and how tracking begins anyway. |
| `rolling-baseline/` | A new base backup retires a lineage without unsaying that it ran. |
| `sharded-fleet/` | Three databases all called `app`, on three servers, migrating concurrently. |
| `publisher-subscriber/` | A dependency that crosses databases: the subscription waits for the publication. |
| `citus/` | One repository for plain PostgreSQL and a Citus cluster, told apart by epoch: the cluster registered from a signed specification, tables distributed and colocated, procedures routed to their shard. Borrowed from the banking/journal variant of pgshard, a lab that benchmarks the same schema on plain PostgreSQL and on Citus clusters of one to six workers. Needs its own coordinator and two workers: `docker compose -f examples/docker/citus/compose.yml up -d`, then `./examples/docker/citus/run.sh`. Not part of `run-all.sh`; CI runs it in the `citus` job. |
| `extensions/` | pgvector and pg_cron. Three signed specifications — a table with an HNSW index, pg_cron, and a nightly purge job — applied unchanged to a server that keeps pg_cron in `postgres` and to one that keeps it in the application database: the specifications target a connection called `cron`, and each configuration says where that is. Then a job scheduled from the wrong database, and an index pgvector would refuse, both refused before anything runs. Needs PostgreSQL with both extensions: `docker compose -f examples/docker/extensions/compose.yml up -d --build`, then `./examples/docker/extensions/run.sh`. Not part of `run-all.sh`; CI runs it in the `extensions` job. |
| `timescale/` | TimescaleDB, one repository on both editions. A table made a hypertable and indexed runs everywhere; the columnstore, its policy, a continuous aggregate and a retention policy sit in the epoch `timescale-license`, which only the Timescale License server opens, so the Apache server holds them. Then 1.2 million rows and an index on them, planned one chunk at a time -- never `CONCURRENTLY`, which TimescaleDB refuses. Needs the three official images: `docker compose -f examples/docker/timescale/compose.yml up -d`, then `./examples/docker/timescale/run.sh`. Not part of `run-all.sh`; CI runs it in the `timescaledb` job. |

Two arrangements on the page have no directory of their own, deliberately. *A
large estate, decoupled* is `dba-and-app/` with more teams — the same mechanism,
and a third directory would demonstrate nothing the second does not. *An agent
writes, a person approves* is the split every one of these already uses:
`pg_laswell_mcp` produces the canonical bytes, `openssl` signs them (see
`lib.sh`), and `pg_laswell` applies what was signed. pg_laswell **verifies**
signatures and never creates them, which is why signing lives outside the tool.

## `lib.sh`

Every example needs the same four things and none of them is the point of any
example: reach a cluster, install the ledger, make a signing key the database
trusts, and sign a directory. They live here so each script is about the
arrangement it exists to show.
