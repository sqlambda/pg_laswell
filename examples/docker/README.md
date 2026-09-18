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
