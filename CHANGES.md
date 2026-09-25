# Changes

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
