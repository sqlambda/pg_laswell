# Runnable examples, on containers

Each arrangement the landing page describes, as something you can run.

```sh
docker compose -f examples/docker/compose.yml up -d
./examples/docker/dba-and-app/run.sh
docker compose -f examples/docker/compose.yml down -v
```

Needs the binaries built (`cmake -S cpp -B cpp/build && cmake --build cpp/build`),
plus `psql`, `openssl` and `python3`.

## Three clusters, not one database

`compose.yml` starts **three** PostgreSQL servers on 55432, 55433 and 55434.
Three because two of the arrangements cannot be shown inside one cluster at all:
logical replication needs a publisher and a subscriber, and a sharded fleet where
every shard's database is called `app` is not expressible where names are unique.
A second *database* would prove neither.

The image is `postgres:latest` deliberately. These exist to be run against
whatever PostgreSQL is current, and an example pinned to the version its author
happened to have stops being evidence about the version you have. Every script
prints the server version it found.

## What each example shows

| Example | The thing it demonstrates |
|---|---|
| `dba-and-app/` | Two teams, one database, neither refused for the other's absence. An epoch scopes the drift check; a complete manifest widens it back. |

More to follow, one per arrangement on the landing page.

## `lib.sh`

Every example needs the same four things and none of them are the point of any
example: reach a cluster, install the ledger, make a signing key the database
trusts, and sign a directory of specifications. They live here so each example's
own script is about the arrangement it exists to show.

pg_laswell **verifies** signatures and never creates them, so signing is done
here with `openssl` over the canonical bytes `getSpecDigest` returns. That split
is the point rather than an inconvenience: the tool cannot sign for you.
