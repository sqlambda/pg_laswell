# Changes

## 0.1.0 (unreleased)

First skeleton. Nothing is applied to a database yet.

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
- `cpp/test/spikes/` — the Phase 0 experiments that settled the design against
  PostgreSQL 18.6. Four of them changed it.
