# Changes

## Unreleased

Findings from a field report -- a 10-million-row table under 1,000 inserts
a second, where `set_not_null` stopped every insert for three seconds before its
job had started -- and the recipe that report was writing by hand.

### A NOT NULL column without a default, as one intent

`ADD COLUMN ... NOT NULL` without a default fails on the first existing row, so
`add_column` refused it and told the author to add the column nullable,
backfill it and set NOT NULL later. Written as separate intents, nothing covers
the rows inserted while the backfill runs unless the author also thinks of a
trigger.

`add_column` now takes `fill`, one SQL expression over the row's own columns,
and `after`, which says what becomes of the trigger:

```json
{"kind": "add_column", "schema": "public", "table": "messages",
 "column": "customer_id", "type": "bigint", "nullable": false,
 "fill": "(payload->>'customer')::bigint", "after": "drop_trigger",
 "comment": "The customer the message was sent to."}
```

The plan is the whole recipe: the column nullable and, in the same transaction,
a trigger that fills it on insert and update; a paced backfill of the rows
already there; the `set_not_null` recipe; and the trigger dropped or kept. One
expression serves the trigger and the backfill -- measured, the trigger
evaluates it as `SELECT (fill) INTO NEW.col FROM (SELECT NEW.*) AS table` -- so
they cannot disagree. `after` has no default: whether the application writes the
column itself is known only to the author.

Several such columns on one table, written as consecutive intents, are planned
as one recipe: one trigger, one backfill that sets them all, one validation
scan. One after the other they would rewrite every row once per column.
Measured on 6 million rows: one check over all the columns, validated once, is
enough for PostgreSQL to skip the scan for each, and both `SET NOT NULL` ran in
one statement in 4 ms against 344 ms for a bare one. They are not merged where
one expression names another column of the group, since the trigger fills in
order and a single `UPDATE` does not.

`fill` may be a list of sources, the first that gives a value winning: an
expression over the row, or `{"from", "on", "value"}` naming another table, how
its row is found and what is taken from it. A row found with a NULL falls
through to the next. The trigger evaluates the list as one `COALESCE`, which
stops at the first value (measured: the second table was probed for half the
rows, those the first had no value for). The backfill makes one joined pass per
source over the rows still null -- 17.5 ms a batch of 5,000 against 30 ms for
the per-row form, with one source -- and each pass is a step, so the ledger says
how many rows each source filled.

With a `default` as well, the recipe is shorter: the column is added NOT NULL
with its default, catalog-only, and the backfill writes only the rows whose
value differs from it. No trigger, no check, no validation scan, and the one
exclusive lock comes before the walk. A row is written only while it still
holds the default, so a value the application wrote is kept.

Consecutive columns with a default and a fill share one walk too: a row is
selected when any still holds its default and has another value to take, and
each column is written only where that is so for it. Found in the field as two
walks, every row rewritten twice.

Every part resumes from the catalog. A row the expression cannot fill stops the
job at the validation scan with the column nullable and the trigger in place,
and the same specification finishes once the row is repaired; the live test
does exactly that.

### A grouped walk no longer looks for the next group while holding row locks

A seventh finding, from running the change under load on Citus. A walk that
goes one group at a time -- a Citus distributed table, or any table whose key
is unique only within a tenant -- asks, at the end of a group, which group
comes next. It asked inside the transaction that still held the row locks of
the batches before. On a shard of 5 million rows that question took 0.6 to
1.5 s, and updates to rows the walk had just touched waited for it: single-row
updates went from 1.5 ms to 11.9 ms mean and 313 ms worst, with waits of up to
1.8 s seen on the workers. The walk commits when another session waits, but
only once the statement in flight returns.

Two changes. The walk now commits at the end of a group before it asks, as a
commit reason of its own, `group_end`. And the question is the next value after
this one and nothing more -- no predicate, no `DISTINCT` -- which is a probe of
the unique index the walk already requires: measured at 0.017 ms, against
359 ms for the earlier form to pass six finished groups. A group with nothing
left now costs one empty batch, taken with no lock held. The live test makes
every other commit trigger unreachable and counts the commits that remain.

### A key that is unique only with other columns is walked along the whole index

An eighth finding, from a probe on a TimescaleDB hypertable, whose primary key
must contain the time column. With the key as `(message_id, created_at)` a
backfill was refused, with advice no hypertable can follow (a unique index on
`message_id` alone, built concurrently). With it as `(created_at, message_id)`
it was accepted as a walk by group in which every timestamp was a group of one
row: 550 rows a second, about five hours for 10 million rows, with nothing in
the plan to say so -- and, since the end-of-group commit above, a commit per
row.

A backfill now walks such a key as what it is. Where the unique index that
contains `key` holds other columns too, and no module requires a batch to stay
inside one value of any of them, the walk follows all the index's columns
together in index order (`batch_mode: composite`), a full batch at a time
whatever the size of a "group". Measured on a hypertable of a million rows in
nine chunks: 0.44 ms for a batch of 1,000 whichever column leads, and the whole
table in 13 s. Every column of the index must be NOT NULL. The cursor is the
last row's whole key, and a pre-image is captured by it.

This applies to every table that was walked by group without being required
to, a plain tenant table keyed `(tenant_id, id)` included; the walk by group
remains for a Citus distributed table, where it is required. The end-of-group
commit now fires only once the transaction holds a batch of rows, and the
refusal no longer suggests an index: it says what the walk needs.

### The composite walk finds its partition

A tenth finding: on a hypertable the composite walk ran at about 40,000 rows a
second, a third of a plain table's rate. The batch's update named its rows by a
tuple, from which a partitioned table cannot tell which partitions they are in,
so it was planned against every chunk and probed them row by row. Each key
column is now also tested against its own array, which names no new row and
tells the planner where to look: 74,000 rows a second became 151,000 on a
hypertable of 18 chunks, and a plain table keyed `(tenant, id)` is unaffected.

### Citus: a distributed table is sized by its shards

From a probe of the same change on Citus 14: the coordinator's relation for a
distributed table is a shell -- 8192 bytes and 0 rows beside whatever its
shards hold -- and core decides how to build an index from the table's size.
So every index on a distributed table was built plainly, 346 MB as "size 0 B <
64 MiB ceiling", blocking writes on every shard for the length of the build and
presenting that as the safe choice. A backfill said "0 rows estimated" for a
million.

The Citus module now reads what the shards hold and answers core's question
about the index build, as TimescaleDB does for a hypertable. The sum is taken
from each node's own catalog over `run_command_on_workers`, not from
`citus_table_size`, which Citus refuses inside a transaction that has made
multi-shard modifications -- every chained dry run after its first write. It
matched `citus_table_size` to the byte. On the probe's table the plan now reads
"size 346.0 MiB >= 64 MiB ceiling -> concurrent build" and "1000000 rows
estimated".

### Citus: the trigger recipe is refused, and says what works instead

`add_column` with `fill` and no `default` creates a trigger, and Citus allows
none on a distributed or reference table unless `citus.enable_unsafe_triggers`
is on. The recipe failed in the dry run at `CREATE FUNCTION` with a Citus hint
about a setting, and the manual said it would fail somewhere else. It is now
refused at planning, quoting the measured errors, and names the form with a
`default`, which needs no trigger and works there.

### An exclusive step no longer queues the application behind its request

A third finding from the same report, still there once the rehearsal was fixed:
straight after a backfill autovacuum is on the table, and the next exclusive
step waited a second behind it with every session queued behind that request.

Reproduced on a 1.5 GB table with autovacuum running: asking for the exclusive
lock directly waited 1.05 s, and an insert arriving meanwhile took 1,027 ms.
Taking `LOCK TABLE ... IN SHARE UPDATE EXCLUSIVE MODE` first, in the same
transaction, waited the same second with the worst insert at 30 ms: that lock
conflicts with autovacuum and not with inserts and updates, and once it is held
the exclusive lock is granted almost at once. The exclusive steps of
`set_not_null`, `add_check_constraint`, `add_foreign_key` and the `fill` recipe
now begin with that statement, and the plan shows it.

The other cause of the same stall is a long application transaction, which the
weaker lock does not help with. There the exclusive statement runs with a 200 ms
lock timeout; when it gives up the transaction is rolled back and tried again,
for up to twenty times `lock_timeout_ms`. The application waits a fraction of a
second at a time, where it used to wait all of `lock_timeout_ms` before the step
failed anyway. A step records `lockAttempts`.

### The dry run no longer runs the scan of a split recipe

`set_not_null`, `add_check_constraint`, `add_foreign_key`, `attach_partition`
and a domain's check add a constraint `NOT VALID` and validate it in a
transaction of its own, so the lock of the first step is not held across the
scan of the second. The dry run applies every step in one rolled-back
transaction, so it held that lock across exactly that scan -- on the live
table, before every apply. The job honoured the recipe; the rehearsal in front
of it undid it.

The planner now marks those steps and the dry run leaves them out:
`VALIDATE CONSTRAINT`, and the `SET NOT NULL` of `set_not_null`, which is cheap
only once the scan has proved the rows. They are listed in `unverifiedSteps`,
and a new `unverifiedWhy` gives the reason for each. `--dry-run=chain`, which is
for an empty database or a restored copy, still runs them.

What this gives up, and what makes it acceptable: a violating row is found by
the job at its `VALIDATE`, with the `NOT VALID` constraint already committed,
instead of before anything ran. So the same specification applied again now
resumes there -- a constraint that exists and is not valid is validated, not
added a second time, and one that is valid is satisfied. Before, the second
attempt failed on the constraint's own name.

### The rehearsal's time is recorded

The dry run takes real locks before any job exists, and `laswell.job.started_at`
is after it, so three seconds of stall left no trace in the ledger. Its duration
is now in the result (`dryRun.durationMs`), which is the plan the job stores,
and `pg_laswell` prints it when the job starts, with the steps it did not run.

### Three locks the plan named wrongly

Read from `pg_locks` on PostgreSQL 18.6:

- Adding a `CHECK` constraint takes `AccessExclusiveLock`, `NOT VALID` or not.
  The plan said `ShareRowExclusiveLock` for `set_not_null`, `add_check_constraint`
  and `attach_partition`, under which a reader would not wait. It does.
- A `NOT VALID` foreign key takes `ShareRowExclusiveLock` on the referenced
  table as well as the referencing one. The plan said `RowShareLock` there,
  which is what the `VALIDATE` takes.

The steps now also say that the lock must be granted before it is brief: behind
a long transaction or an autovacuum it waits, and every session queues behind
it until `lock_timeout`.

## 0.1.3

Two gaps reported from pgshard while writing specifications for an audit
application, one wrong lock found fixing them, and three more modules in the one
package: pg_cron, pgvector and TimescaleDB. No ledger schema change.

### An object is documented by the intent that creates it

`comment` was accepted by thirteen core kinds and absent from the rest, and the
line was nearly but not quite "does this create a named object": `create_index`
required one and `create_trigger` refused one. A house rule of "every object
documented where it is created" took a second, `set_comment` intent restating
the schema, table and name of the first -- and a rename in one and not the
other documents the wrong object.

`comment` is now accepted, optionally, by every kind that creates a named object
and did not take one: `add_foreign_key`, `add_check_constraint`,
`add_unique_constraint`, `add_primary_key`, `create_trigger`, `create_policy`,
`create_rule`, `create_extension`, `create_publication` and
`create_subscription`. Optional, because requiring it would refuse every
repository written before it. It becomes the object's `COMMENT ON`, appended to
the step that leaves the object in place -- the VALIDATE after a `NOT VALID`
foreign key, the `ADD ... USING INDEX` after a concurrent unique build -- so it
commits with the object. `alter_publication` and `alter_subscription`, which
share a parser with the create kinds, refuse it rather than ignore it.
`add_enum_value` stays without one: PostgreSQL has no `COMMENT ON` for an enum
label.

`set_comment` also takes `RULE`, `PUBLICATION` and `SUBSCRIPTION`.

### A partition can be created, not only adopted

`create_table` took `partition_by`, which makes a parent, and `attach_partition`
adopted an existing table as a child. Nothing created a child, so a partition
was `create_table` restating every column of its parent -- seventeen, for the
audit table -- followed by an attach, and a restated copy is one that can drift.

`create_table` with `partition_of` creates it: `CREATE TABLE ... PARTITION OF`,
taking the parent's columns and keys, with one bound -- `from`/`to`, `values`,
`modulus`/`remainder` (new: hash) or `default`. Measured on PostgreSQL 18.6 and
stated in the plan:

- it takes **AccessExclusiveLock on the parent**, and on its DEFAULT partition
  if it has one: a SELECT on the parent waited behind it. `attach_partition`
  takes only ShareUpdateExclusiveLock on the parent, and the plan names it as
  the lighter route for a busy one;
- beside a DEFAULT partition, every row of the default is read to prove none
  belongs to the new bound (47 ms for 1M rows), and the plan states the
  default's row count. A row that does belong fails the statement, which the
  dry run executes, so it is found before anything commits;
- refused first where the reading shows it: a parent that is not partitioned,
  a bound of the wrong kind for the parent, a DEFAULT for a hash parent or a
  second DEFAULT, and a table of that name that exists and is not already its
  partition. One that already is, is satisfied.

`attach_partition` keeps its meaning, for a table that already exists.

### Storage parameters on an index

`create_index` takes `with`: the index's storage parameters, emitted as
`WITH (...)` -- btree `fillfactor`, gin `fastupdate`, brin `pages_per_range`,
and the ones an extension's method declares, such as HNSW's `m` and
`ef_construction` and IVFFlat's `lists`. Values are numbers, booleans or short
words. The parameters are now part of what makes two indexes the same: they are
read back from `pg_class.reloptions` (with `off` and `false` alike), and an
index matching in everything but its parameters is built as a second index,
with a warning naming both, instead of being adopted or renamed. One already
present under the declared name with other parameters is satisfied, and the plan
says so.

### pg_cron: scheduled jobs as intents

`pg_cron_schedule` and `pg_cron_unschedule`, in a module of their own. A job is
named, never numbered, and identified as pg_cron identifies it, by name and role.
Both ways a server keeps pg_cron are supported, and the specification does not
choose: in the application database, a job commits with the rest of the
specification and its ledger row; in a maintenance database (the default,
`postgres`), a specification targets a connection to it and names the database
each job runs in. The example applies one set of signed specifications to both.
A job scheduled from the wrong database, a `create_extension` of pg_cron
outside `cron.database_name`, a job for another role by a role that is not a
superuser, and `schedule_in_database` without its grant are refused before
anything runs, each quoting the error it pre-empts. A schedule pg_cron cannot
read is caught by the dry run, which executes the call and rolls it back.

Measured on the way: `cron.database_name` is readable only with the privileges
of `pg_read_all_settings`, and a role without them gets an error even from
`current_setting(..., true)`. The module's reading runs on every plan against a
server that loads pg_cron, so it checks first: an ordinary specification plans
for any role, and a pg_cron kind from such a role names the grant it needs.

The module reads, where its extension is not installed, what it needs to say
where it is: a module's `observe.h` now declares an absent reading too
(`nullptr` for Citus and pgvector).

### pgvector: the indexes it would refuse, refused first

No kinds of its own: a vector column is a `create_table` type, and an HNSW or
IVFFlat index is a `create_index` with a method, an operator class and `with`.
The module refuses what pgvector would: more dimensions than the method
indexes (2000 for `vector`, 4000 for `halfvec`, 64000 for `bit`), a column with
none, no operator class where the method has no default, a class for another
type, `sparsevec` on IVFFlat, UNIQUE or multicolumn, and storage parameters out
of bounds -- including `m` above half the default `ef_construction`. The
operator classes are read from the server. Said before an HNSW build starts,
because a concurrent build cannot be rehearsed and one refused at execution has
already scanned the table and left an invalid index.

An expression key is checked when it is an explicit cast -- `embedding::halfvec(3072)`,
`CAST(... AS ...)`, `binary_quantize(embedding)::bit(1536)` -- as the type it casts
to, which is how a vector wider than 2000 is indexed.

Three things pgvector does not refuse but a reader should know before a build
are now **advisories**: an IVFFlat index on an empty table, or one with fewer
rows than lists; an HNSW graph larger than the `maintenance_work_mem` the step
will run with, sized from rows, dimensions and `m` by a rule measured on 0.8.6
(the vector's bytes plus about 210 + 32·m a row, within 2% across four shapes),
naming the setting to raise; and a parallel HNSW build that would allocate more
shared memory than a container's default `/dev/shm`.

### Advisories: shown, and not hashed

A plan may now carry `advisories` beside its `warnings`, rendered as `note:`
lines by both binaries. They hold facts that move on their own -- a row count
autovacuum rewrites, a job's last run -- and are left out of `planDigest`, like
the budget, so planMigration and startMigration a minute apart still agree. A
plan with none serialises exactly as before. A module's guard may add them
through a second callback beside `refuse`; it still cannot reach a step.

### pg_cron, continued

- A job for another role is checked as pg_cron checks it: the role exists, may
  log in, and may connect to the job's database.
- **A job edited by hand is noticed on any plan against pg_cron's database**,
  not only when the specification that declared it is planned again. Each
  `pg_cron_schedule` step records what it declared; a module may now ask core
  for its own applied steps from the ledger, and pg_cron compares the newest
  declaration of each job with `cron.job`. The example moves a job by hand and
  shows the note on a plan for an unrelated schema.
- A job a specification touches whose last run failed is named, with the
  message, from the most recent 1000 runs.
- No `alter_job` kind, deliberately: `pg_cron_schedule` already changes a job in
  place and keeps its id, and `cron.alter_job` is not granted to PUBLIC.

Found by the live suite: on PostgreSQL 18, `to_regclass('laswell.job')` and
`has_table_privilege('laswell.job', ...)` RAISE for a role without USAGE on the
schema instead of answering no. The ledger reading looks the table up through
`pg_class` and tests by OID.

Both modules are tested against real servers in CI (the `extensions` job), and
every refusal is checked to pre-empt an error the server really raises: the
refused statement is run by hand too, and must fail with the quoted message.

### pg_cron: a job's command is checked before it is scheduled

pg_cron stores a job's command as text and first runs it at its schedule, so a
misspelt procedure was found at 03:00. The dry run now has PostgreSQL analyse
the command without running it -- as the body of a SQL-language procedure in
`pg_temp`, which is checked when it is created and never called. That reports a
syntax error, a missing table, a missing function or procedure, in PostgreSQL's
own words. Where the job runs in the database the specification targets, the
check is part of the rehearsal and sees what the same specification creates;
where it runs in another, the dry run checks there over a second connection.
`"validate_command": false` schedules a command unchecked.

A step may now carry statements for the rehearsal only (`rehearse_only`,
`rehearse_elsewhere`): run by the dry run, never by the real one.

### pg_cron: the zone a schedule is read in, and whether the job can connect

A cron expression names an hour, and which hour belongs to `cron.timezone` --
a server setting, GMT unless set, unrelated to the database's `TimeZone`. A
`pg_cron_schedule` step now says which zone its schedule is read in, as part of
the plan: the same specification on a server with another zone schedules
another moment, and its digest differs. A role that cannot read the setting is
told so.

pg_cron checks, when a job is scheduled, that its role can log in and may
connect; it cannot check that the role will be let in. It connects to
`cron.host` as the job's role, and a role `pg_hba.conf` refuses shows at the
first run as "connection failed" and nothing more. `pg_hba.conf` is not read --
matching its rules against a host name would be a guess -- so the plan says
what is known: that background workers make no connection; that a job has
lately run in that database as that role; that none has, so it is unknown; or,
as a warning, that the last one could not connect.

Found while measuring, and now in the man page: dropping a role that still has
a job makes the pg_cron launcher exit and restart every second, and no job runs
until the row is removed.

### pgvector: an HNSW build gets the memory it needs, within a deduced limit

pgvector now tells core how much `maintenance_work_mem` an HNSW graph needs, by
the measured rule its advisory already used, and core raises the build toward
it. A configured `maintenance_work_mem_mb` still decides outright. Without one,
the limit is deduced from `shared_buffers` -- the figure PostgreSQL's
documentation ties to about a quarter of the server's memory, so the one every
server is tuned by: all concurrent builds together get one more
`shared_buffers`, each `shared_buffers` divided by `max_concurrent_jobs`, never
less than the server's own setting. An untuned 128MB server is not raised. The
step says what it was raised to and why (`memory_wanted_by`), and the advisory
remains when the limit leaves the build short.

Found on the way, and fixed: **a step's `maintenance_work_mem` was applied only
outside a transaction.** The planner has always marked plain index builds and
`VALIDATE` steps with the configured value, and both the executor and the dry
run ran them inside a transaction with the server's setting instead. They now
`SET LOCAL` it for the step and put it back after; a test proves it with a CHECK
that validates only under the planned value.

### Warnings: eight more, and `-Werror` follows the source

Eight warnings join the seven: `-Wdouble-promotion`, `-Wnull-dereference`,
`-Wformat=2`, `-Wimplicit-fallthrough`, `-Wold-style-cast`, `-Wcast-qual`,
`-Wnon-virtual-dtor` and `-Woverloaded-virtual`. `-Wnull-dereference` fired
twice, in optimised GCC 14 builds only, and both times on something inlined from
a library header rather than on a null: reading `--args @file` through
`istreambuf_iterator`, which now goes through `rdbuf()`, and a JSON lookup
through an iterator's `operator->` in the Citus module, which now holds a
reference. Neither is suppressed.

`-Werror` is now `PGLASWELL_WERROR`: on by default in a git checkout, off when
there is no `.git` beside the sources -- a release tarball, built on someone
else's machine by a compiler that may be newer than any this project has seen,
whose new diagnostic must not lock a user out of a release that was clean when
it was cut. The warnings stay on in both. CI, the release workflow and the
FreeBSD build script pass it explicitly.

### Binary hardening, asserted on the binary

Both binaries are now built with stack canaries, stack-clash protection,
`_FORTIFY_SOURCE`, full RELRO, a non-executable stack, PIE, and CET on x86 --
each flag probed, since not every toolchain this builds on takes every one.
`cpp/test/hardening-check.sh` then reads the ELF and fails unless they reached
both binaries: in ctest, in the `packages` CI job and in the release. Requested
flags are an intention; the headers are the fact. Measured before: PIE and
partial RELRO from distribution defaults, and nothing else.

Found doing it: **only `pg_laswell_mcp` had ever been built with the warning
flags, `-Werror` and the sanitizers.** The deployment binary -- the one that
applies migrations -- had none, so the sanitizer jobs never instrumented it.
Both now go through the same two functions.

### The release workflow can be checked before a tag

- A pull request that changes the release workflow, its scripts or the CMake
  files now runs it: every package is built, installed and run, and nothing is
  published. Until now a release job was first exercised by a tag.
- The version inside each `.deb`, `.rpm` and FreeBSD `.pkg` is read back and
  must be the project's.
- libpqxx is built with, and cached per, the compiler of the job that links it.

### FreeBSD packages

Releases now carry `pg_laswell-freebsd14-amd64.pkg` and
`pg_laswell-freebsd15-amd64.pkg`, built in a FreeBSD VM on a Linux runner by
`.github/scripts/freebsd-build.sh`. The same script runs on every pull request
(the `freebsd` job): it builds with every module, runs the suite that needs no
database, packages with `cpack -G FREEBSD`, installs with `pkg add` and runs
both binaries. The reason: a lab that runs FreeBSD 15.1 guests several times a
day gave the FreeBSD build no coverage, because with no package to install it ran
pg_laswell from the host.

### Two refusals that now say more

- An unknown key names the binary's version, and says a later release may have
  added it -- the pgshard lab met a stale 0.1.0 refusing `target.connection` and
  read it as a typo.
- A password in a subscription's connection string: the refusal and
  `pg_laswell_mcp(1)` now say whose `~/.pgpass` the subscription reads -- the
  operating-system user the SUBSCRIBER's server runs as, on that host -- not the
  machine running pg_laswell.

### TimescaleDB

A module of nine kinds, and -- the reason it had to exist -- a correction to how
core plans an index on a hypertable. Measured on TimescaleDB 2.30.2 with
PostgreSQL 18 in both editions, and on 2.28.3 with PostgreSQL 15.

**Indexes on a hypertable.** A hypertable's parent holds no rows: relpages and
reltuples read 0 against the 125 MiB in its chunks. So core sized every index
build on one as tiny and built it plainly, holding ShareLock on the parent and
every chunk until it finished -- or, for a UNIQUE index or a table with waiters,
reached for `CREATE INDEX CONCURRENTLY`, which TimescaleDB refuses ("hypertables
do not support concurrent index creation"). Core now asks a module a second
question, beside the one about row locking: *how may an index be built and
dropped on this table?* TimescaleDB answers with facts -- no concurrent build or
drop, a per-chunk option, the hypertable's own size -- and core decides: a plain
build when it is small and quiet, `WITH (timescaledb.transaction_per_chunk)`
otherwise, one chunk at a time in its own transaction, and for a UNIQUE index,
which the per-chunk build refuses, the plain build with a warning that says what
it blocks. Drops, and the recovery of an invalid index, are plain. The step
records `index_build_by`. For any other table nothing changes, and a test says so.

**Kinds.** `timescaledb_create_hypertable` and
`timescaledb_set_chunk_time_interval`, in both editions; and in the Timescale
License edition only, `timescaledb_set_columnstore`, the columnstore and
retention policies (add and remove), `timescaledb_create_continuous_aggregate`
and its refresh policy. The edition is read from `timescaledb.license`, and the
Apache build -- what PGDG and Debian package, and the `-oss` images -- refuses
those first, quoting its own "not supported under the current \"apache\"
license". Which functions exist is read too; where only `add_compression_policy`
exists it is used. Intervals are compared as PostgreSQL keeps them, so "1 week"
is "7 days" and "168 hours" is not. A retention policy needs
`"acknowledge_data_loss": true`: it deletes data on a schedule from then on.

**Constraints with the columnstore.** Once the columnstore is enabled,
TimescaleDB refuses `VALIDATE CONSTRAINT` ("operation not supported on
hypertables that have columnstore enabled"), which is the step the recipes for
`set_not_null`, `add_check_constraint` and `add_foreign_key` end with: the `NOT
VALID` add would commit and the validation fail. Core asks a module a third
question -- *can a constraint on this table be validated in a step of its own?*
-- and where the answer is no, the statement that adds the constraint validates
it too: one step, in its own transaction. Measured with 137 of 140 chunks
converted: each of the three reads the converted chunks and fails on a violating
row that exists only there, so nothing is left unchecked. The cost is the lock,
held for the scan rather than for a catalog change -- AccessExclusiveLock on the
hypertable and every chunk for a check and for NOT NULL, ShareRowExclusiveLock
on them and on the referenced table for a foreign key -- and the step and a
warning say so, with the size it scans. The step records
`validated_in_one_step_by`. A hypertable without the columnstore, and every
other table, keeps the two-step recipe, and a test says so.

**A policy changed by hand.** A policy is a row TimescaleDB keeps, and
`remove_retention_policy`, a second `add_retention_policy` or `alter_job` changes
it with no migration involved. Each policy step now records what it declared,
and on any plan against the database the newest applied step for each policy is
compared with TimescaleDB's jobs: a policy that is gone, back after a
specification removed it, on another interval, or paused is shown as a `note:`.
An advisory, so not part of the plan digest. The example pauses the retention
job by hand and shows the note on an unrelated plan.

**Refusals for core kinds on a hypertable**, including one made a hypertable
earlier in the same specification: a unique key without the partitioning
column; `alter_column_type` once a chunk is in the columnstore. Each quotes the
error it pre-empts, and the live suite runs each refused statement to prove the
server still raises it.

The example applies one repository to both editions, the Timescale License
specifications held on the Apache server by their epoch. The dry run found a
defect before it shipped: a continuous aggregate is a view to `COMMENT ON`, not
a materialized view.

### A role the bootstrap did not name gets an answer, not an error

On PostgreSQL 18 every name-based probe of the ledger -- `to_regclass('laswell.x')`,
`has_table_privilege('laswell.x', ...)` -- RAISES "permission denied for schema
laswell" for a role without USAGE on the schema rather than answering. Measured
while testing pg_cron, and it was in core too: the ledger status, the repository
listing and `checkPrivileges` -- the tool whose job is exactly that answer --
all failed with the error instead of explaining it. They now look the ledger up
through `pg_class` by OID. The ledger status says "installed, but this role has
no USAGE on the laswell schema" and names the bootstrap option that grants it;
`checkPrivileges` reports `schemaUsable` and a `denied` note. Tested on
PostgreSQL 15 to 18 with such a role.

### The partition test counts the scan instead of timing it

The test proving that a validated CHECK lets `ATTACH PARTITION` skip its scan
asserted the attach was at least 3x faster. It failed on CI at 6.0 ms against
15.9 ms -- a fixed round-trip cost on both sides squeezes the ratio on a fast
runner while the scan stays exactly as skipped -- and it had to be skipped under
the sanitizers, taking the test's catalog checks with it. It now reads the
transaction's own statistics before and after the ATTACH: no scan with the
CHECK, one scan of 400 000 rows without. Deterministic, and it runs everywhere.

### CI

- The actions move to their Node 24 releases (checkout v7, upload-artifact v7,
  download-artifact v8, cache v6, upload-pages-artifact v5, deploy-pages v5).
  download-artifact v8 fails on a digest mismatch, which is the behaviour a
  release wants.
- Runners are pinned to `ubuntu-24.04` instead of `ubuntu-latest`, which moves
  to Ubuntu 26 on 2026-10-19: an image change should be a commit, not a
  surprise mid-release.
- A `macos` job builds with every module on Apple clang and libc++ and runs the
  suite that needs no database. The first macOS compile used to happen on a
  release tag, which is how a libc++-only error was first found.

### Citus: a failed job says what it left prepared

A multi-shard write is a two-phase commit, and one that loses its connection
between PREPARE and COMMIT leaves a prepared transaction on a worker, holding
its locks and that node's xmin horizon. The plan guard refuses the next plan
once such a transaction is older than twice `citus.recover_2pc_interval`; until
then nothing said so. Now, when a job fails on a cluster, the coordinator and
every worker are asked for the prepared transactions they hold, on a connection
of their own, and what is found goes into the job's error as `left_behind` --
node, gid, owner, age -- in the ledger and under the failure `pg_laswell`
prints. A reading only: nothing is resolved, and it does not claim the
transactions are this job's. Modules get this through a new optional file,
`after_failure.h`.

### Pacing on a Citus cluster, stated

`pg_laswell_citus(7)` now says plainly that the pacing defaults were derived on
single-node PostgreSQL and have not been re-derived for a cluster, what is
measured (a grouped-walk batch is a single-shard commit; worker waits reach the
breaker) and what is not (walks over reference tables, where each commit is a
two-phase commit across every node; larger worker counts).

### FreeBSD

pg_laswell builds and passes its suite on FreeBSD 14.5 and 15.1 (511 of 513
against PostgreSQL 18.6; the two skips need a second cluster and Citus), and
`cpack -G FREEBSD` makes a native package. One fix was needed: the threads
library is now linked by name. On Linux it had linked only because glibc 2.34
folded libpthread into libc; on FreeBSD every binary failed to link on
`pthread_create`. BUILD.md lists the packages.

### A comment's lock, measured

`set_comment` said "AccessShareLock -- a comment blocks nothing". Measured, a
comment on a table, column, index, view or sequence takes
**ShareUpdateExclusiveLock** on it: reads and writes pass, but VACUUM, ANALYZE,
`CREATE INDEX CONCURRENTLY` and other DDL on that relation wait. A comment on a
constraint, trigger, policy or rule takes AccessShareLock on its table and
ShareUpdateExclusiveLock on the object; on a schema, type, function, extension
or publication, ShareUpdateExclusiveLock on the object only. The plan now says
which.

## 0.1.2

Two arcs. Vendor modules, with Citus as the first: one package that speaks plain
PostgreSQL and Citus alike, seventeen Citus kinds, and the core fixes that
building them exposed, `--dry-run=chain` among them. And the multi-directory
work, end to end: a repository may be several directories, a specification
belongs to a lineage, and a manifest can say whether those directories are all
of a database's story. 506 tests, plus 55 live cases against a Citus coordinator
and two workers; 100 intent kinds, 83 core and 17 Citus. **The ledger schema
moves to version 3** -- run the bootstrap script shipped with this binary.

### One package, every module

The released `pg-laswell` is built with the Citus module compiled in, and
`--version` prints the set (`modules: citus`). A module is inert on a server
without its extension: its kinds carry the vendor's prefix (`citus_*`), its
reading is absent rather than empty, and for a table it has nothing to say
about, the build with it and the build without it emit the same statements,
which is tested. A PostgreSQL-only build is `cmake` with no
`PGLASWELL_MODULES`, and CI keeps building it. Modules are compiled in, never
loaded: a binary without one refuses a specification naming its kinds as a
whole, so two builds never disagree silently. Separate packages, or shared
libraries, wait until two modules actually conflict.

`cpp/src/modules/README.md` is the guide to writing the next one: every file a
module may provide and which are required, a step-by-step walkthrough pointing
at the working Citus instance of each piece, and **THE RULE**: a module may
*inform* core's SQL through a reading core interprets, and may never emit or
rewrite SQL for a core kind.

### A bulk change is the same specification on any server

The same `backfill`, and `delete_rows` by predicate, runs on plain
PostgreSQL in development and on a distributed table in staging. Core asks
each module one question (must row-locking batches on this table be confined
to one value of a column, and which?), and on a distributed table Citus answers
with the distribution column. Core then walks one value at a time, and a lock
that Citus refuses across shards is taken inside one. The specification names
no vendor. Refused only where the walk cannot be shaped: a distribution column
that *is* the walk key.

Underneath it, a paced batch is now a selection and an apply rather than one
statement with a CTE: its keys are selected and locked, then the change is
applied to exactly those keys. Batches are bounded by bytes of key as well as
by rows. A failed walk records where it stopped and resumes there.

### Citus: seventeen kinds, each measured before it was written

- **Distribution:** `citus_distribute_table` (the concurrent form chosen only
  when nothing rules it out, and the reading that decided named),
  `citus_create_reference_table`, `citus_distribute_function`.
- **Lifecycle:** `citus_alter_distributed_table`, `citus_undistribute_table`,
  `citus_add_local_table_to_metadata`, `citus_truncate_local_data`,
  `citus_update_table_statistics`.
- **The cluster:** `citus_set_coordinator_host`, `citus_add_node`,
  `citus_ensure_workers`, `citus_set_node_property`, `citus_disable_node`,
  `citus_activate_node`, `citus_remove_node`, `citus_drain_node`,
  `citus_rebalance_shards` (synchronous, so "applied" means balanced).

`citus_ensure_workers` takes the hosts from the connection's configuration
(`citus.workers = ...`), never from the specification, so one signed file
registers three workers on staging and twelve in production. The
specification can bound what the unsigned file may supply: `hosts_matching`,
`min_workers`, `max_workers`. Node membership is projected like table state,
so one specification can register the coordinator, set its property and add
the workers, in that order.

No kind moves or splits a shard by id: shard ids come from a sequence in each
database, so a signed specification naming one would mean a different shard
on every other target.

### Refusals that stop what Citus would accept and regret

Each is a behaviour measured on Citus 13.2, and most are cases where Citus
does *not* fail:

- **An empty cluster.** With nothing registered, distributing a table succeeds
  by registering the coordinator as `localhost` with every shard on it, and
  every worker added afterwards is refused. Refused before it happens.
- **A foreign key dropped by a rewrite.** Moving a table out of its colocation
  group while a distributed table references it succeeds with a warning, and
  the key is gone. Refused.
- **A truncate that cascades into real rows.** `truncate_local_data...`
  truncates with CASCADE, and emptied a local table that referenced the
  reference table. Refused.
- **A distributed function that routes nothing.** On Citus 11+, every
  function is recorded in `pg_dist_object`, so the kind read every function as
  already distributed and did nothing. It now compares how the function routes.
  A text argument on a bigint group is refused: Citus accepts that distribution
  and then fails every call.
- **A concurrent distribution that cannot finish.** It needs `wal_level =
  logical` on the coordinator too, and a failure leaves the table half
  converted. The concurrent form is chosen only when the reading rules both out.
- **A cluster that would diverge, or is not healthy:**
  - `citus.enable_ddl_propagation` off
  - a worker on another Citus version
  - prepared transactions older than twice Citus's own recovery interval, which
    hold locks and pin the xmin horizon

  All three refuse every plan, core kinds included.
- Colocation with a different distribution-column type (exactly, `int` against
  `bigint` included), foreign keys Citus cannot hold, keys that omit the
  distribution column, a drain or rebalance by logical replication on workers
  that cannot do it.

### Core, changed by what Citus exposed

- **`--dry-run=chain`.** Rehearses every pending specification in dependency
  order, each on top of the ones before it: one transaction per database,
  rolled back. It lifts most of the 0.1.2 "Known limit": a repository nobody has
  applied can now be checked. Paced batches and `CREATE INDEX CONCURRENTLY`
  still cannot run inside it, and are listed as unverified.
- **The dry run executes what must be executed.** A Citus call is a `SELECT`
  that changes the catalog, and was only ever planned. It is now run and rolled
  back, and a step that does not roll back (a rebalance, a drain) is skipped
  and says so rather than being run.
- **A dry run reports what the server said,** with SQLSTATE, message and
  statement, instead of inviting a defect report. A server error in the
  connection class on a healthy connection is reported as a problem rather than
  escaping as a tool error.
- **Pacing sees the workers.** Contention was read on the coordinator only,
  where a distributed write is quiet while every worker queues behind it. Waits
  on workers now count, and have an age the breaker can use.
- **A failed migration is unfinished work.** The deployment ignored one:
  listed with no status, never retried, exit 0. It is now retried, resuming
  where it stopped, and `--status` exits non-zero over it.
- **An index can say how it sorts, and what it is on.** A column may be an
  object: `direction`, `nulls`, `opclass`, or an `expression` in place of a
  name, plus `include`. The equivalent-index match compares ordering and
  operator classes, so an ascending index no longer satisfies a descending
  one. A plain string stays a string, so no existing signature changes.
- **Configuration:** a dotted key in a connection section
  (`citus.workers`) is a module setting for that connection. It is refused
  when read if no compiled-in module owns it.
- **Reference pages** read the checks a parser delegates to helpers. Two
  pages had called required keys optional.
- **The pooler diagnostic** says what it observed (the backend pid changed)
  rather than naming a pool mode it could not know.

### Building: libpqxx 8.0.2, exactly

CMake now requires the libpqxx CI and every release build from source, 8.0.2,
and stops naming the version it found otherwise. A distribution's package is
whatever that distribution froze (Debian 13 ships 7.10), and code compiling
against two versions is written to what they share: a build against 7.10
compiled locally and failed in CI, where `sqlstate()` is a `string_view`. The
7.x compatibility shim around `exec` is gone, and so is a GCC warning
suppression the 8.0.2 headers no longer need. BUILD.md shows how to build the
pinned version into a prefix of your own, with the script CI uses.

### CI

A `citus` job builds with the module, runs the example against a real
three-node cluster, and runs the live suite. The ASan/UBSan and TSan jobs
build with the module, so the shipped code is what they watch. Release builds
package with it.

### A repository may be several directories

A project per directory, read as one repository. The ledger is what permits it:
it records a specification's id, digest, canonical bytes and signature, and
never a file path, so which directory a migration came from is a listing concern
and not a state concern. Ordering needed nothing new -- the scheduler only ever
saw ids, relations and connections.

`--repo` is repeatable; `listMigrations` takes `directories`. Three things a
union makes newly possible to get wrong are refused rather than resolved: an id
claimed by two directories, one directory named twice, and a missing directory
(which now says which one). Sources are put in canonical order first, so the
order they are named in cannot change the listing.

### Epochs, and the partial repository

A specification belongs to a lineage, named in `target.epoch` and inside the
signature; naming none puts it in `default`, where a ledger upgraded from
version 2 keeps its whole history.

The check that catches drift -- this database records a migration applied that
no file here describes -- is unanswerable for a repository that is deliberately
partial. Scoped by epoch it becomes answerable: a deployment answers for the
lineages it brought and says nothing about the others. **That is what finally
makes a DBA repository and an application repository coexist without either
refusing the other's absence.**

`spec_digest` was globally unique, so the same bytes could never run in two
lineages of one database -- which is exactly what replaying a retired lineage
into a new one does. `UNIQUE (epoch, spec_digest)` replaces it, and the applied
index is keyed by epoch too: without that, a correct replay was reported as
edited-after-apply, a false drift alarm.

Epochs are opened by whoever owns the schema and never by the migrating role,
for the sharpest version of the reason environment and release are: opening one
NARROWS the check that catches drift, so a role that could open one could excuse
its own. Retiring a lineage stops tracking it without unsaying that it ran --
its migrations stay applied, so a live specification may still depend on one of
them, and its files can be deleted without objection.

### A manifest, and what `complete` asserts

A file naming the sources a deployment is made of, giving each a name that
messages use in place of its path, with directories resolved against the
manifest's own folder. Its `complete: true` is an assertion no single directory
can make about itself: these are ALL of this database's story, which widens the
drift check back to every epoch the ledger knows. It only ever widens, so
omitting it costs scope and never safety.

### Defects fixed

- **A dependency is satisfied by the ledger, not the directory.** `depends_on`
  asks "has that one run", and a ledger answers it -- but the check read the
  directory, so a dependency whose file was absent was refused however plainly
  the database recorded it succeeded.
- **Two files may not claim one spec id.** The listing kept the last file read,
  so the loser vanished from the dependency order and nothing said so. Silence
  was the defect, not the collision.
- **Grouping compares databases, not the names of connections.** The one place
  concurrency is granted without evidence compared connection NAMES, and nothing
  checked that two names denote two databases. Never corrupting -- advisory
  locks are scoped per database, exactly the semantics the scheduler assumed, so
  the overlap was refused -- but the plan was wrong. Identity is now the
  cluster's `system_identifier` with the database's oid, exact in both
  directions: a fleet of shards all called `app` still migrates concurrently,
  where comparing `current_database()` would have serialised every one of them.
- **A timestamp from autovacuum is not part of the plan.** `estimated_from` --
  when some other process last touched the statistics -- was inside
  `planDigest`, so autovacuum moved the receipt. `lock_waiters` went with it.
  Both are still shown, just not hashed.
- **A repository describing nothing is not agreement with everything.** Found by
  a worked example: scoping the drift check by epoch let an EMPTY directory pass
  silently against a database full of history. The two most catastrophic things
  that can be wrong -- the directory deleted, or the wrong path -- were the two
  that had stopped being caught.
- The deployment summary said "across N databases" while counting connections.

### A landing page that names every concept

The page now lists every idea the tool has -- thirty terms in six layers, each
with what it ACTUALLY is rather than a gloss, because `target` does four
unrelated jobs and saying so once beats four paragraphs elsewhere -- and then
nine ways people arrange it, each with a diagram, a numbered sequence, and a
link to a directory you can run. It states the dry-run limit below rather than
leaving a reader to discover it.

### Eleven runnable examples, and CI runs them

`examples/docker/` starts three `postgres:latest` containers and demonstrates
each arrangement: the baseline, a CI gate, release tags, environments, two roles
on one database, a DBA repository the application never sees, adopting a
database nobody scripted, rolling a baseline forward, a sharded fleet, and a
publisher with a subscriber. Three clusters because two of those cannot be shown
inside one.

They run in CI, and `run-all.sh` checks each one still prints the line that IS
its demonstration -- because an example that runs without failing is not the
same as one that still shows anything, and exactly that had already happened to
one of the ten.

The eleventh is `examples/docker/citus/`, on a coordinator and two workers of its
own: one repository, borrowed from the pgshard lab's banking/journal variant,
applied to a plain PostgreSQL and to a Citus cluster, told apart by epoch. CI
runs it in the `citus` job.

### Known limit, and how far it moved

`--dry-run` is per specification: one that needs a table an earlier PENDING
specification creates cannot be planned yet, and says so. `--dry-run=chain`
(above) is the repository-wide rehearsal that closes most of that. What it still
cannot run is what cannot run inside a transaction at all: paced batches, which
commit by design, an index built concurrently, and a Citus drain or rebalance.
Those are listed as unverified, never passed.

### Wave one, as published in v0.1.2-alpha1

**A dependency is satisfied by the ledger, not by the directory.** `depends_on`
asks "has that one run", and a ledger answers it -- but the check read the
directory listing, so a dependency whose file was absent was refused however
plainly the database recorded it succeeded. That is what makes a partial
repository impossible: one project's directory holds one project's specs, and a
spec in it may depend on something that ran from a directory this deployment
will never see. Every database in the listing is asked, because depends_on
crosses databases and a dependency the repository lacks brings no
target.connection of its own to narrow the search with.

Not sufficient on its own, and worth saying so: the same missing file also
trips the orphan check, which is still a refusal. Scoping that needs to know
what the listing is SUPPOSED to contain.

**Two files cannot claim one spec id.** The listing kept the last file read, so
the loser vanished from the dependency order and nothing said so. Silence was
the defect, not the collision -- a repository with a missing migration looked
exactly like a healthy one.

**Grouping compares databases, not the names of connections.** The one place
concurrency is granted with no evidence, on the reasoning that different
databases cannot touch each other's tables. Sound -- but it compared connection
NAMES, and nothing checks that two names denote two databases. A DDL role beside
an application role points both at one. Never corrupting: advisory locks are
scoped per database, exactly the semantics the scheduler assumed, so the overlap
was refused -- the operator just got a lock refusal where a scheduling decision
belonged.

The identity is now the cluster's `system_identifier` with the database's oid:
exact in both directions and unprivileged. The first attempt compared
`current_database()`, which is safe but blunt -- a fleet of shards all called
"app" would have serialised entirely, losing the concurrency routing exists to
provide. **Proved against two real clusters**, locally with docker and in CI
with a second service container, and the skip is fatal under
`PGLASWELL_REQUIRE_SECOND_CLUSTER` so it cannot quietly stop running.

**A timestamp from autovacuum is not part of the plan.** Two plans of one spec
seconds apart hashed differently, which surfaced as the Valgrind shards failing
the receipt while every fast job passed. Diffing the plans found one leaf:
`estimated_from`, which is GREATEST(last_vacuum, last_autovacuum, last_analyze,
last_autoanalyze) -- when some OTHER process last touched the statistics.
`lock_waiters` went with it. This is the budget argument one level down, and the
original fix simply had not reached into step detail. Both are still SHOWN, just
not hashed.

## 0.1.1

Everything below shipped as the pre-release `v0.1.0-alpha0` or was added after
it. 0.1.0 was never released as a final version: the alpha existed to exercise
packaging on all seven targets, and it did its job by failing on two of them.


The whole path works: repository, signing, planning, dry run, paced execution,
ledger. 383 tests green on GCC 14.2 and Clang 22 against real PostgreSQL 15,
16, 17 and 18, under AddressSanitizer/UBSan and ThreadSanitizer,
Valgrind-clean, plus a mandoc lint and a process-level `--call` contract
test.

- **A dry run says why a specification was refused when there is no plan to
  show.** A refusal that never reached a plan -- an untrusted signature, either
  trust gate -- carries `error` and `hint` rather than `conflicts`, and
  `--dry-run` printed the spec id, the word REFUSED, and an empty list.
- **A version 1 ledger is refused by version, not by exception.** `status()`
  read the two version 2 tables with `to_regclass` in a `WHERE`, which does
  nothing for a table named in the `FROM` -- PostgreSQL resolves that at parse
  time. Installing this binary before re-running `bootstrap.sql` therefore
  raised "relation laswell.environment does not exist" instead of saying which
  version the ledger is, and the repository listing, which swallows the
  exception, then called every gated specification's database unlabelled.
- **Paced row-level DML quotes its target.** The paced `INSERT`, `UPDATE`,
  `DELETE` and `MERGE` spliced the raw `schema.table` while the unpaced forms
  beside them used the quoted one, so every paced row-level kind was a syntax
  error on a reserved-word schema. Measured: `INSERT INTO user.t` fails at
  `user`, while `INSERT INTO cf.order` parses -- it is the schema that has to be
  quoted, which is why a case naming only a reserved table proves nothing.
- **`publish_via_partition_root` on `create_publication` and
  `alter_publication`.** Without it a partitioned table replicates its leaf
  partitions, and the subscriber needs a partition of each matching name; with
  it the change travels as the root table's, so the subscriber may be an
  ordinary table. A publisher keeping a rolling window and a subscriber keeping
  full history is the reason to want it. The plan warns either way, because
  neither arrangement errors until a row has nowhere to land.
- **A repository may span several databases.** A specification names
  `target.connection`, is classified against THAT database's ledger, and is
  applied there; `depends_on` crosses databases freely, so the subscription on
  one server waits for the publication on another. A change is always
  single-database -- PostgreSQL has no cross-database transaction, so a
  specification spanning two would be atomic in neither -- and the boundary
  stays at the specification, which keeps each half separately recorded and
  separately resumable. Observers are now one per connection: a job is watched
  in the database it runs in, or the contention breaker is watching nothing.
- **`max_concurrent_operations`: a ceiling on migration statements in flight**,
  across every job the process runs, answering a different question from
  `max_concurrent_jobs`. Jobs are how much work is underway; operations are how
  much of it is touching the server at this instant. Zero, the default,
  declares no ceiling. The observer, ledger and coordination connections are
  never counted.
- **`target.database` is enforced instead of ignored.** It was parsed, covered
  by the signature, and read by nothing, so naming a database gave exactly the
  protection of naming none. It is now compared to `current_database()` in the
  repository listing and in `startMigration`. A mismatch is reported as
  `wrong_database` and is not a failure, like `wrong_environment`. The man page
  now documents all three target gates, which were undocumented.
- **`preserve` is honoured by `update_rows`, `merge_rows` and `delete_rows`.**
  All three accepted the option and emitted nothing for it, and `update_rows`
  said in its plan that the previous values were preserved. One implementation
  now serves the four kinds: the side table, then a `preserved` CTE in the same
  statement as the change. A delete saves the whole row.
- **A unique index proves a key unique only when the key is the whole index.**
  `UNIQUE (id, tenant)` was accepted as proof that `id` is unique, and a
  `merge_rows` keyed on `id` updated two rows -- measured. Composite and partial
  unique indexes no longer count, for `merge_rows`, `update_rows` and
  `backfill`; the refusal names the index and says why.
- **Row-level DML: `insert_rows`, `update_rows`, `merge_rows`, `copy_rows`, and
  two new forms for `delete_rows`.** 84 kinds. Every PostgreSQL statement that
  writes rows except `TRUNCATE`, which stays deliberately absent. Each names its
  rows exactly one way, and the choice decides the pacing: a `values` count is
  known at planning time, so it runs in one transaction below
  `dml_single_txn_rows` and is paced above it; a `select` count is not derivable
  by a pure planner, so those are always paced rather than sized by a guess.
  Five of the six need no executor change at all — the paced runner was already
  generic over the statement.

  What the planner now refuses before anything runs, each from a reading and
  each measured on 18.6 (`cpp/test/spikes/s21_dml.sh`):

  - **A `merge_rows` whose key has no unique index.** `MERGE`'s `ON` is not a
    key constraint: against a table with two rows sharing a key, *one* source
    row updated *both* target rows — no error, nothing in the row count to
    notice. An `INSERT` would have hit the unique index; a `MERGE` has none.
  - **`ON CONFLICT` with no matching arbiter.** PostgreSQL does not fall back to
    the primary key, it refuses; and against a *partial* unique index the
    arbiter must repeat the index predicate. The refusal names the predicate to
    copy.
  - **A `GENERATED ALWAYS` identity column without `overriding`, and a `STORED`
    generated column at all.**
  - **Duplicate keys inside `values`**, which break three different ways
    depending on the statement.
  - **`when_not_matched_by_source: delete` when the statement would be paced** —
    it means "delete every target row the source does not mention", and a paced
    merge sees one batch at a time, so the first batch would delete everything
    outside it.

  Two findings changed the design rather than confirming it:

  - **A paced statement takes its cursor from the batch, never from the
    mutation's own `RETURNING`.** A paced `MERGE` whose batch matched nothing
    returned zero rows, which the executor reads as "the walk is finished" — the
    step would have reported success having merged nothing. The same trap waits
    for an update whose rows have gone and an insert whose every row hits
    `DO NOTHING`.
  - **A sequence is not advanced by explicit values**, so a seed that succeeds
    leaves the application's next insert failing on a duplicate key. Where
    explicit values are supplied for a column that owns a sequence, a second
    step emits the `setval` that closes the gap — deriving the number from the
    table at execution time, never from the spec, and writing nothing when the
    table is empty.

  **Version floor, gated.** `merge_rows` is the one kind that does not reach as
  far back as the rest: its paced form emits `MERGE ... RETURNING` and
  `when_not_matched_by_source` emits `WHEN NOT MATCHED BY SOURCE`, both
  PostgreSQL 17+, and measured against real 15, 16 and 17 clusters, both are a
  syntax error before 17. The planner refuses each below 17 and names the
  observed version — and refuses rather than quietly dropping to the unpaced
  form, because that would turn a bounded change into one long transaction
  holding its locks throughout, which is the harm this tool exists to prevent.
  The unpaced merge works from 15, as do the paced insert, update, delete and
  backfill.

  `copy_rows` is the one kind that is never paced and cannot be: a `COPY` whose
  second of three rows violates a constraint rolls back all three. It accepts no
  `WITH` options and **refuses `on_error`, `reject_limit` and `freeze` by name**
  rather than ignoring them — libpqxx offers one sanctioned way to send `COPY`
  data and it builds its own statement with no `WITH` clause. A spec asking for
  `ON_ERROR ignore` and silently getting a copy that stops on the first bad row
  would be worse than no copy at all.

- **Every identifier the planner emits is now quoted.** A column named `order`,
  `user`, `end`, `desc` or `limit` is legal PostgreSQL and matches the
  `[a-z0-9_]` shape `require_identifier` permits, so it reached the planner and
  came out as broken SQL. Measured on 18.6, the breakage is **by position, not
  by name**, which is why it survived: a schema or table named this way fails
  everywhere -- `CREATE TABLE user.order`, and even `SELECT ... FROM user.order`
  -- a column fails in every DDL position (`ADD COLUMN`, `ALTER COLUMN`,
  `DROP COLUMN`, `RENAME COLUMN`, an index column list, an `INSERT` column list,
  a `SET` clause, a `VALUES` alias list), and yet a fully qualified
  `schema.table.column` reference parses unquoted. Testing against ordinary
  schemas could never find it.

  The rule is now stated once, in `require_identifier`: **raw here, quoted
  there.** The raw name stays the observation key, the repository's concurrency
  key and the executor's advisory-lock key -- all three must agree with the
  catalog, which returns names unquoted. The quoted name is what reaches SQL,
  through `quote_identifier` and the new `quote_qualified`, which splits a
  `schema.table` key and quotes both halves.

  Verified by applying 18 intent kinds to a schema in which *every* identifier
  is a reserved word (`DatabaseTest.ReservedWordIdentifiersAreQuotedEverywhere\
  TheyAreEmitted`), and by a plan transcript. Every plan digest moves; the
  transcripts are the diff, and nothing but the quoting changed in them.

- **`planner.h` split.** `planner_base.h` holds `Step`, `Plan` and the rendering
  helpers; `planner_dml.h` holds the six row-level planners. `planner.h` keeps
  the single `plan_migration` entry point, and the purity check still covers the
  whole include graph. The six committed plan transcripts are byte-identical
  across the split.

- **New observations:** `attidentity`, `attgenerated` and the owning sequence
  per column, which is what makes the four insert refusals derivable rather than
  discovered at execution.

- **`--call <tool>`** runs one tool and **exits non-zero when it reports a
  problem**, which is what CI wants and the pipe alone did not give. Not a CLI:
  no subcommands, no per-tool argument parsing, no second surface to keep in
  step. `--args` takes literal JSON, `@file` or `@-`.
- **The man page is the authoritative reference** — every tool, the
  specification format, pacing, trust, bootstrap, poolers, the ledger, and how
  to operate with nothing but psql.
- deb, rpm and tarball build; the installed tree is verified end to end.

- MCP server over stdio, JSON-RPC 2.0, protocol revisions 2024-11-05 through
  2025-11-25. `initialize`, `ping`, `tools/list` and `tools/call` answer;
  notifications correctly produce no response.
- Tools are declared in **one** `ToolDef` vector, from which `tools/list` and
  dispatch are both derived. pg_licht keeps four parallel structures per tool
  synchronised only by tests; that cost is deliberately not inherited.
- `handle_request()` returns its response rather than writing it, so the
  transport is testable without redirecting `std::cout`, and so exactly one
  function may write to stdout while worker threads are running.
- Build: C++23, warnings-as-errors, hardened standard library, one
  `PGLASWELL_SANITIZER` cache variable, auto-registered Valgrind ctest entry,
  `mandoc -Tlint` of the man page as a ctest entry. Verified on GCC 14.2 and
  Clang 22.
- Database tests skip without `DATABASE_URL`; `PGLASWELL_REQUIRE_DATABASE=1`
  turns the skip into a failure. This differs from pg_licht, whose suite refuses
  to run at all without one, because pg_laswell's pure layers are the ones most
  worth running on a machine with no PostgreSQL.
- `config.h` — INI registry, trust policy, executor tuning. Every knob is
  configuration rather than `constexpr` so the tests can drive the identical
  code paths with tiny values. Inconsistent settings are refused at parse time:
  a `resume_waiters` at or above `pause_waiters` would make the breaker flap,
  and that is caught when the file is read rather than mid-migration.
- `canonical.h` — RFC 8785 (JCS) with SHA-256. Floats are refused outright,
  which removes JCS's one genuinely error-prone clause; integers beyond 2^53
  are refused too, since they stop round-tripping through the double most
  clients parse them into.
- `trust.h` — Ed25519 via OpenSSL EVP, one-shot as that algorithm requires.
  Key ids are the content address of the key, so an id cannot be reassigned to
  different bytes.
- `spec.h` — change intents, not desired state. Top-level keys are an
  allowlist, so a key outside the signed projection cannot be smuggled past
  verification. An unknown intent kind refuses the whole spec.
- `session.h` — `ReadSession` (read-only, always rolls back) and `WriteSession`,
  its writing counterpart. Every write transaction carries `lock_timeout` and
  `idle_in_transaction_session_timeout`, and re-reads `pg_backend_pid()` so a
  multiplexed connection aborts the job at the moment it is multiplexed.
- `catalog.h` — observation, bounded by `lock_timeout`. `pg_get_indexdef()`
  takes AccessShareLock and blocks behind an `ALTER TABLE`; without a bound the
  planner's own measurement would hang on the thing it is planning around.
  A blocked observation is reported as a fact, and the planner refuses.
- `planner.h` — pure, and `planner_purity_check.cpp` fails the build if pqxx
  ever reaches it. Each intent is planned against the catalog as its
  predecessors will leave it, so "add a column, then backfill it" works.
- `sql/bootstrap.sql` — run by hand by a DBA, never by the binary. The runtime
  role gets SELECT on `laswell.trusted_key` and nothing more, so it cannot
  grant itself trust. A CHECK constraint makes the key id the content address
  of the key, which even a superuser cannot forge.
- `ledger.h` — gate 2. Re-verifies against the key bytes the *database* holds,
  and `migration.signer_key_id` is a foreign key to `trusted_key`, so an
  untrusted signer cannot create a row at all: a constraint, not a code path.
- `tools.h` — `checkPrivileges`, `getSpecDigest`, `validateSpec`,
  `planMigration`. None of them execute anything.
- **The dry run.** `planMigration` applies the whole plan in a transaction and
  rolls it back, so later steps are checked against the schema earlier ones
  produce. Statement-by-statement checking cannot do this: a backfill
  referencing a column that step 0 adds does not parse until step 0 has run.
- `jobs.h` / `executor.h` — the executor. Three connections per job, one
  process-wide observer thread, and a job registry whose every read is a
  snapshot copy taken under one mutex.
- **Lock-aware pacing works.** Measured on 300 000 rows with an application
  contending: 48 of 63 commits were triggered by a lock waiter and **zero** by
  the 3-second interval, finishing in the same wall time as an uncontended run
  (which took 19 interval commits and no waiter commits).
- `startMigration` / `jobStatus` / `cancelJob`. Progress is published per
  batch, not per commit, so a long transaction is distinguishable from a stuck
  job — and committed rows are reported separately, because only those survive
  a crash.
- `repository.h` / `listMigrations` — a directory of signed specs, classified
  against one database's ledger. Reports what is pending, in `depends_on`
  order, with the groups that may run concurrently.
- **Edited-after-apply detection.** A spec whose id was applied under a
  different digest was changed after the fact: the database no longer matches
  the file that claims to describe it. Falls out of digests for nothing.
- **Dependencies are derived, not guessed.** `EXPLAIN (GENERIC_PLAN, VERBOSE)`
  reveals relations hidden inside opaque `set`/`where` expressions that no
  intent declared; foreign keys are followed both ways. Triggers are *named,
  never resolved* — a trigger function body is invisible to both the catalog
  and the plan, so it downgrades confidence instead of being guessed at.
- **Redundant-index detection.** The name check never caught an index
  identical to an existing one under a *different* name — a permanent cost paid
  on every write. Identical is now a conflict; prefix-redundancy warns, because
  wanting a narrower index is a judgement this tool cannot make.
- A backfill reports **no bloat figure**. Rows updated is not dead tuples:
  autovacuum reclaims some during the run, HOT updates may not bloat indexes at
  all, and other workload contributes. It points at `tableBloat` instead.
- **Convergence on index names.** When an equivalent index exists under a
  different name — the usual shape of a database where somebody built it by
  hand — the default is now to `ALTER INDEX … RENAME` rather than refuse, so
  one spec drives every database to the same state. Measured: that takes
  `ShareUpdateExclusiveLock` on the *index* and no lock on the table at all.
  `on_equivalent_index` selects `rename` (default), `adopt` or `refuse`.
  A constraint-backed index is never renamed, because renaming it renames the
  constraint too.
- **Three more intent kinds**, each earning its place by making a real
  planning decision rather than emitting one possible statement:
  `drop_index` (plain vs `CONCURRENTLY`, decided on contention not size),
  `set_not_null` and `add_foreign_key` (both `NOT VALID` + `VALIDATE`, in
  *separate transactions* — sharing one would hold the stronger lock across the
  scan and the recipe would buy nothing).
- An intent can now emit **several steps in several transactions**, which those
  recipes require.
- The dry run is bounded by its own `statement_timeout`. It applies the plan in
  one transaction, so a scanning step would otherwise hold every earlier step's
  lock — a *planning* call could block writes for minutes. A timeout is
  reported as "unverified beyond this step", not as a failure.
- CI workflows: both compilers × PostgreSQL 15–18, both sanitizers, Valgrind, a
  pooled run and package verification.
- **`assert_invariants` is evaluated**, not merely parsed: named scalar queries
  run before the first batch and after the last, and a difference fails the
  step naming the invariant and both values. It asks what `verify_remaining`
  cannot — a backfill can fill every row and still halve a total.
- **`create_index` on a partitioned table is now a recipe** rather than a
  refusal: concurrent builds per partition, the parent with `ON ONLY`, an
  attach each, then a validity check — because the parent index stays INVALID
  until the last attachment lands. A unique index there is still refused, since
  PostgreSQL requires it to include the partition key and the planner does not
  read it.
- `add_check_constraint`, the same two-step shape as a foreign key.
- **`preserve`** captures the pre-image of a lossy backfill into a side table,
  as a data-modifying CTE of the very statement that updates — so it reads the
  rows as they were and commits with them, with no window between. This is the
  durable answer to the pinned-snapshot idea: a snapshot lets you *look* at old
  values while blocking vacuum throughout; this *keeps* them and doubles as the
  revert path. Verified exact on 3000 rows.
- **`drop_constraint`**, which the one-possible-statement rule initially kept
  out and should not have. That rule exists to stop `psql` wrappers, but a
  migration *repository* has a second requirement it does not cover: a change
  made outside the tool is unsigned, unrecorded and missing from the dependency
  graph, so the repository ends up describing a database that does not exist.
  The decision it makes is not which statement to emit but whether the drop can
  succeed. A unique constraint whose index backs a foreign key elsewhere is
  **refused before anything runs**, naming the dependent — PostgreSQL only
  reports it at execution time, and suggests `CASCADE`. There is no `cascade`
  key: it would drop objects the spec never named. The plan also states what
  goes with the constraint — a foreign key locks the referenced table with
  `AccessExclusiveLock` the statement never names, a unique or exclusion
  constraint takes its index with it, and a NOT NULL constraint leaves the
  column nullable again.
- **Packages that work**, by adopting pg_licht's release method rather than a
  parallel one. Release builds run inside the target distribution's own
  container (`debian:trixie`, `rockylinux:9`) and link **libpqxx statically**
  from a pinned source release; libpq stays dynamic, since its C ABI is stable
  and distributions patch it independently. The previous workflow built all
  three artifacts on bare `ubuntu-latest` against Debian's shared libpqxx —
  which produced an RPM carrying Ubuntu sonames, and a deb pinned to one Debian
  release.
- **Package dependencies are derived from the binary**, by `dpkg-shlibdeps` and
  rpmbuild's soname scan, rather than listed by hand. The hand-written list said
  `libpq5` — the one library the binary does *not* link directly. Installed into
  a clean `debian:13-slim`, the package reported no problem and the binary then
  died with `libpqxx-7.10.so: cannot open shared object file`. It also missed
  `libstdc++6 (>= 14)`. `CPACK_RPM_PACKAGE_REQUIRES` is gone too: it carried a
  *Debian* package name, which makes an RPM uninstallable rather than merely
  under-specified.
- **Seven release targets**, matching pg_licht: deb, rpm and portable tarball
  for Linux x86_64 and arm64, plus a macOS arm64 tarball. CPack names every
  package `-Linux` whatever the architecture, so each artifact is renamed per
  target — otherwise x86_64 and arm64 arrive at the release under one filename
  and one silently overwrites the other. `fail-fast` is off so one platform's
  failure does not cancel the rest, which makes a *partial* release the thing
  to guard against: publish refuses unless all seven artifacts are present, and
  ships a `SHA256SUMS`.
- **The release workflow now installs the package and runs the binary.** Its
  only packaging gate was `Depends | grep -q libpq5`, which passed on the
  broken package — a wrong Depends line still contains the string it is grepped
  for. It also asserts libpqxx is not in the binary's `NEEDED` entries, because
  a silent fall back to the distribution's shared copy is exactly the failure
  that shipped.
- **`getSpecDigest` no longer demands a database it never uses.** The signing
  workflow the man page documents runs on the machine holding the private key —
  deliberately not the machine that can reach production — and refusing to start
  without a conninfo made that impossible. Tools that do need a connection say
  so, through a typed `ConfigError` whose hint names the configuration rather
  than a server log for a statement that never ran.
- **Dependent views are observed, and rebuilt around a change.** PostgreSQL
  refuses `ALTER COLUMN … TYPE` and `DROP COLUMN` whenever a view reads the
  column — and refuses them regardless of cost: `varchar(50)`→`varchar(100)`
  rewrites nothing, is still refused, and succeeds once the view is dropped.
  pg_laswell walks the graph transitively, drops deepest-first, changes,
  recreates shallowest-first, in one transaction. A view on a *different*
  column is left alone.
  The rebuild is the easy half. A hand rebuild silently loses **eight** things,
  all measured: comment, per-column comments, grants, **column-level** grants,
  `security_barrier`, `INSTEAD OF` triggers, a matview's indexes (and with them
  `REFRESH … CONCURRENTLY`), and the owner. All eight are restored — owner
  before grants, so each records the right grantor.
- **`alter_column_type`**, with rewrite classification measured by relfilenode
  rather than assumed: relaxing a type modifier is free, adding or tightening
  one rewrites. `text`→`varchar(200)` rewrites; `varchar(50)`→`text` does not.
  Anything unproven is reported as a rewrite, because a cautious plan costs
  less than an unplanned outage.
- **Plan transcripts**, in the shape of PostgreSQL's own regression suite:
  `cpp/test/plans/NAME.spec.json` in, `expected/NAME.out` committed, a diff on
  change, and `results/` + a unified diff on failure. The property worth
  copying most is that **errors are output** — a refused spec's text lives in an
  expected file, so a refusal is reviewable content rather than an exception
  nobody reads. The suite exists because ninety-odd unit assertions check that
  a warning *contains a phrase*; none shows what the warning says, and a
  rewrite that keeps the phrase passes however much worse it became.
  Observations come from a file, not a database: `plan_migration` is a pure
  function of (spec, observations), so the output is deterministic by
  construction and `render_plan` links no libpq at all.
- **A conformance suite: every intent kind planned and EXECUTED.** 70 cases
  driven from a table, each one planned through the real observation path,
  applied to a database, then checked against the catalog — and
  `Conformance.CoversEveryIntentKind` fails the build if a kind is added
  without a case or an explicit reason for not having one. Seven kinds that
  genuinely need a second cluster (subscriptions, foreign data, default
  privileges) are covered by `cpp/test/replication-tests.sh`, which builds two
  clusters with `initdb` under a temp directory and throws them away.
  **It found four real bugs on its first run**, all invisible to unit tests
  that hand-build observations:
  `attach_partition` refused **every time** in real use — `tools.h` derived
  what to observe from `schema.table`, so the candidate partition was never
  measured; `alter_view` and `create_materialized_view` had the same defect;
  `CREATE RULE … DO INSTEAD (NOTHING)` was a syntax error, since `NOTHING`
  isn't a command and mustn't be parenthesised; and `drop_type` refused **any
  domain with a CHECK constraint**, because the dependant reading counted
  `deptype = 'a'` (auto — dropped *with* the object) as a blocker.
  Observation targets now come from `conflict_keys()`, the same function the
  repository groups by and the executor locks on.
- **Structured prerequisites**, so an agent can act rather than read. Some
  migrations need something this tool cannot do, usually on a machine it is not
  connected to — a replication slot on the publisher, a package on the host,
  free space in a destination tablespace, a credential in `pg_service.conf`.
  Those now come back as `prerequisites`, each with a `kind`, `target`,
  `where`, `requirement`, `verify` and `blocking`. A warning tells a person
  something; a prerequisite tells a skill what must be true, on which machine,
  and how to check whether it already is. An ordinary migration has none.
  **A credential is still never generated**, and the reasons are in the man
  page: the direction is wrong (the password must exist on the *publisher*), it
  would land in the ledger anyway since every statement is stored verbatim, and
  a fresh random value per run would break the byte-identical-plan guarantee
  that makes `planMigration` a promise about `startMigration`.
- **Logical replication, foreign data and the long tail.** `create_publication`
  and friends, `create_subscription` and friends, plus generic
  `create_object`/`drop_object`/`alter_object` covering aggregates, casts,
  collations, conversions, operators and their classes and families, all four
  text-search types, transforms, access methods, languages, and the whole
  foreign-data set. Then `create_table_as`, `import_foreign_schema`,
  `security_label`, `alter_default_privileges`, and procedures folded into the
  function kinds behind `routine_kind`. **80 kinds.**
  **A subscription password is refused, not redacted.** `pg_subscription`
  stores the connection string in the clear and pg_laswell stores every
  statement verbatim in the ledger — so a password in a spec would be in the
  signed spec, in git and in `laswell.step.sql`. Redacting it would make the
  ledger untrue, which is the one property this tool will not trade; the
  refusal names `.pgpass` instead, and does not echo the credential back.
  **`CREATE SUBSCRIPTION` cannot run in a transaction block** (measured), so it
  owns its own — the same shape as CIC. It also warns that the replication slot
  it creates lives on the *publisher* and holds WAL there indefinitely, which
  nothing on this server reports.
  `import_foreign_schema` admits the thing that makes it unusual: its outcome
  depends on the remote schema at the moment it runs, so the same signed spec
  produces different objects on different days.
- **Materialized views, extended statistics and rules**, plus **grants beyond
  tables**. 67 kinds. `create_materialized_view` says whether it runs its query
  now (`WITH DATA`) or leaves the view *unreadable until refreshed* — a query
  against an unpopulated matview fails outright rather than returning nothing.
  `create_statistics` warns that extended statistics do nothing until `ANALYZE`.
  `create_rule` carries the strongest warning after RLS: a rule rewrites
  matching queries for every session with nothing in the query text to say so,
  and `DO INSTEAD` reports success having done nothing.
  `grant`/`revoke` now take an `object_type` — schema, sequence, function, type,
  domain, database — and **check the privilege against it at parse time**,
  because PostgreSQL rejects a wrong one at execution, by which point earlier
  steps have committed. `ALL TABLES IN SCHEMA` warns that it covers what exists
  now and nothing created later.
- **Identity, generated columns and physical layout** — `set_identity`,
  `drop_expression`, `set_column_options`, `set_table_options`, `set_logged`,
  `set_tablespace`, `set_access_method`, `set_replica_identity`, `cluster_on`.
  62 kinds; `ALTER TABLE` goes from 14 of 52 actions to **33**.
  Measured by relfilenode on 300 000 rows, because predicting the rewrite is
  the entire job: **`SET LOGGED`, `SET UNLOGGED` and `SET EXPRESSION` rewrite**;
  identity add/drop, `SET STATISTICS`/`STORAGE`/`COMPRESSION`, storage
  parameters and `REPLICA IDENTITY` do not. `SET LOGGED` additionally writes
  the whole table to WAL — a cost that lands on the replication link, not this
  connection.
  Two prerequisites the spike found by failing: an identity needs the column
  `NOT NULL` first, so the plan runs the `set_not_null` recipe (the same
  composition `add_primary_key` uses); and `SET EXPRESSION` only applies to an
  already-generated column.
  The quiet ones are named too: `REPLICA IDENTITY NOTHING` leaves a logical
  subscriber unable to apply updates with **no error on this server**; storage
  and compression apply only to values written afterwards; `CLUSTER ON`
  reorders nothing; a new identity starts at 1 regardless of what is in the
  column.
- **The ALTER forms for objects that could only be created or dropped** —
  `alter_column_default`, `drop_not_null`, `alter_sequence`, `alter_schema`,
  `alter_extension`, `alter_domain`, `alter_function`, `alter_view`,
  `alter_policy`, plus standalone `set_comment` and `set_owner`. 53 kinds.
  **`alter_domain` is a recipe, not a statement**, and measurement decided it:
  adding a domain constraint took **37.377 ms** on 1.5M values across two
  tables against **0.502 ms** for the same constraint `NOT VALID`. Same shape
  as `add_check_constraint` — and worse, because the scan covers every column
  of that type in *every* table and grows with each new use of the domain.
  Warnings carry what the statements don't: a default reaches no existing row;
  `DROP NOT NULL` widens what the data may contain; an extension update runs
  scripts pg_laswell cannot see and generally cannot reverse; changing a
  function's volatility can make an index over it return wrong answers; an
  owner change decides who bypasses RLS.
- **Conflict keys and advisory locks now come from one function.** They keyed on
  `qualified_table()`, which is empty for object intents — so unrelated types
  serialised against each other while a `drop_type` and an `add_column` of that
  type did **not**, and could run concurrently. Measured on the real binary.
- **Capacity an agent can act on.** `budget.workers` reports worker limits,
  live parallel workers in use, connection headroom, and a verdict naming the
  number behind it. The correction it encodes: migrations run inside
  PostgreSQL, so the client's core count is nearly irrelevant — a stock server
  has **8** parallel worker slots and **2** per maintenance operation, and past
  them an index build silently drops to single-threaded.
- **`maintenance_work_mem` per step**, sized as the configured ceiling divided
  by `max_concurrent_jobs`. Checked in the docs rather than assumed: it is
  *not* multiplied by parallel workers, and it *is* allocated per concurrent
  operation — which is exactly the assumption the docs' advice to raise it
  depends on.
- **Fourteen object kinds, so a repository can describe a database from
  nothing** — `create_schema`/`drop_schema`, `create_extension`/`drop_extension`,
  `create_type`/`drop_type`/`add_enum_value`, `create_function`/`drop_function`,
  `create_trigger`/`drop_trigger`, `create_sequence`/`drop_sequence`,
  `drop_view`. Before these, no schema, extension, enum, function or trigger
  could be expressed at all — the contents of a real first migration.
  Each carries the tool's safeguards rather than being a wrapper.
  **Every drop refuses when something depends on it**, naming the dependant
  PostgreSQL would have named, and none emits `CASCADE`; a non-empty schema is
  refused outright.
  **`add_enum_value` is the sharpest.** Adding a label is *irreversible*
  (`dropping an enum value is not implemented`) and the label cannot be *used*
  until the adding transaction commits (`unsafe use of new value`) — so the step
  takes its own transaction, or a backfill later in the same spec fails on the
  label it just added.
  **`create_function`** uses `CREATE OR REPLACE`, which keeps comment, grants,
  owner *and the OID* so dependent views survive — but cannot change the return
  type, and the drop that can loses all four. That change is **refused with what
  the alternative costs** rather than quietly becoming a drop. `SECURITY
  DEFINER` is flagged.
  **`create_extension`** refuses a name the server does not have available — an
  infrastructure prerequisite, not something a spec can fix — and warns when no
  schema is named, since extension objects land wherever `search_path` points.
- **Twelve more intent kinds**, closing the list of what a migration tool has
  to be able to say. `rename_table` / `rename_column` / `rename_constraint`;
  `create_table` / `drop_table`; `delete_rows`; and `set_row_security`,
  `create_policy`, `drop_policy`, `set_trigger_state`, `grant`, `revoke`.
  **Renames rebuild nothing.** Measured: the catalog repairs itself — a
  dependent view's definition becomes `SELECT id, value AS amount FROM t` on its
  own, and indexes, checks and FKs all follow. But the view keeps its *own*
  output name, so the rename never reaches anything reading through it, and
  nothing errors. The plan names the views and points at `replace_view`.
  **`create_table` is satisfied without comparing shape**, and says so — quietly
  reporting satisfied over a table that differs is how a migration does nothing
  and reports success. **`drop_table`** refuses for dependent views *and*
  inbound foreign keys, both visible beforehand.
  **`delete_rows`** is a retention purge paced exactly like a backfill, reusing
  the executor's generic runner; it warns that a foreign-key violation stops it
  *part-done* because earlier batches have already committed, and that the
  space is not returned to the filesystem.
  **Row security is the loudest warning in the tool**, and it is measured:
  enabling it with no policy took an application role from 1000 rows to **0**,
  silently. And you cannot see it happen — an owner bypasses RLS unless `FORCE`
  is set, and a superuser bypasses it regardless, so verifying from the
  migrating session proves nothing.
- **A build gate against one recurring bug.** Iterating the temporary that
  `.value()` returns has appeared five times in this project;
  `-Wdangling-reference` caught it every time, which is luck rather than a
  guarantee. `cpp/test/no-dangling-items.sh` now fails the build instead, and is
  itself tested against a known-bad snippet.
- **`attach_partition` and `detach_partition`** — partition rotation, which is
  how time-series data is actually retired. `ATTACH` validates the candidate
  unless a CHECK already proves the bounds: **98.393 ms vs 0.914 ms on 2M
  rows**. So the plan adds the CHECK `NOT VALID`, validates it under
  `ShareUpdateExclusiveLock`, attaches, then drops it — the partition bound now
  enforces the same thing, and a leftover CHECK costs time on every insert.
  Two costs it cannot remove, both stated: an unmatched parent index is *built*
  during the attach under `AccessExclusiveLock` (build it first — a matching
  one is attached, not rebuilt), and a `DEFAULT` partition is scanned on every
  attach whatever the candidate proves (**165 ms** with a 2M-row default).
  A plain `DETACH` takes `AccessExclusiveLock` on the **parent**, blocking every
  partition; `CONCURRENTLY` does not and cannot run in a transaction block. An
  interrupted concurrent detach leaves the partition half-detached with only
  `pg_inherits.inhdetachpending` to say so — re-running the intent emits
  `FINALIZE`, the only legal move from there.
- **`add_unique_constraint` and `add_primary_key`**, planned through the index
  that backs them. A plain `ADD CONSTRAINT … UNIQUE` takes `AccessExclusiveLock`
  *and* `ShareLock` and builds the index under them; `CREATE UNIQUE INDEX
  CONCURRENTLY` + `ADD CONSTRAINT … USING INDEX` still takes
  `AccessExclusiveLock` — it removes the *build* under the lock, not the lock.
  The plan says that rather than the usual overclaim.
  Two measured behaviours shape it. `USING INDEX` **renames** the index to the
  constraint name (a NOTICE, nothing more), so the index is named after the
  constraint from the start and the rename is a no-op — and when an existing
  index is adopted under another name, the plan warns. And `ADD PRIMARY KEY`
  sets `NOT NULL` itself, verified by a full scan under the exclusive lock:
  **75 ms on 2M rows nullable vs 0.6 ms already NOT NULL**. So a primary key
  over a nullable column runs the existing `set_not_null` recipe first, which
  does that scan under `ShareUpdateExclusiveLock` instead. The two intents
  compose rather than restating each other.
  There is no `NOT VALID` form for unique, so the build *is* the validation: a
  duplicate leaves an invalid index that `USING INDEX` refuses, and the plan
  says so up front.
- **`replace_view`**, and the measured reason it exists. Adding a column is
  never blocked by a view — but the new column reaches *no* existing view,
  including one written `SELECT *`, because the star is expanded at creation
  and the column list stored. Nothing errors; the column is just not there.
  `add_column` now says so and names the intent that fixes it.
  Exposing it via `CREATE OR REPLACE VIEW` costs **one** loss rather than
  eight: comment, column comments, grants, column grants, `INSTEAD OF`
  triggers, owner and dependent objects all survive. Only the view's options
  are reset — so it silently stops being a `security_barrier`, with no error —
  and the plan re-applies them. `CREATE OR REPLACE` can only *append* columns,
  and a materialized view has no replace form at all, so that one falls back to
  drop-and-recreate.
- **`drop_column`**, which refuses when a view reads the column rather than
  guessing at a rewritten view body, and warns that the values are gone at
  commit.
- `cpp/test/spikes/` — the Phase 0 experiments that settled the design against
  PostgreSQL 18.6. Four of them changed it.
