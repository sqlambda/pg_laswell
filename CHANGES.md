# Changes

## 0.1.0 (unreleased)

The whole path works: repository, signing, planning, dry run, paced execution,
ledger. 287 tests green on GCC 14.2 and Clang 22, under AddressSanitizer/UBSan
and ThreadSanitizer, Valgrind-clean, plus a mandoc lint and a process-level
`--call` contract test.

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
