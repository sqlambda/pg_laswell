# Changes

## 0.1.0 (unreleased)

The whole path works: repository, signing, planning, dry run, paced execution,
ledger. 193 tests green on GCC 14.2 and Clang 22, under AddressSanitizer/UBSan
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
- `cpp/test/spikes/` — the Phase 0 experiments that settled the design against
  PostgreSQL 18.6. Four of them changed it.
