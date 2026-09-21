# Vendor modules

The core binary is PostgreSQL-only, and that is a correctness position rather
than a packaging preference.

`spec.h` opens with the governing invariant: *the set of intent kinds the binary
KNOWS is exactly the set it IMPLEMENTS; there is no recognized-but-unsupported
kind.* A kind that is compiled in but fails on every server without a particular
extension installed is within touching distance of the state that invariant
forbids. The version gate (`merge_rows` refused below PostgreSQL 17) is not the
same thing: that says *your PostgreSQL is too old*, where this would say *your
server is a different product*.

So a vendor's kinds are compiled in only when that vendor's module is enabled,
and a binary built without it **refuses the whole specification** rather than
skipping a step. Loud, and never silent.

## Why build-time and not `dlopen`

A runtime plugin is exactly the failure the invariant names: a specification
applied by a binary missing a step, recorded in the ledger as fully applied.
Signing makes it worse -- identical signed bytes would produce different plans
on two builds, and `planDigest` would stop being a receipt.

A build-time module cannot do that. An unknown kind is a fatal refusal, so two
builds never disagree silently; one simply declines the file.

## Enabling one

    cmake -S cpp -B cpp/build -DPGLASWELL_MODULES=citus

The list is semicolon-separated. With none, nothing below is compiled and the
binary is exactly what it was before modules existed.

## What a module must provide

A directory `modules/<name>/` containing:

| File | Purpose |
|---|---|
| `kinds.inc` | One `PGLASWELL_KIND(...)` line per intent kind. The X-macro that keeps the enum CLOSED. |
| `parse.h` | `parse_<kind>()` per kind, using `reject_unknown_keys` like core does |
| `plan.h` | `plan_<kind>()` per kind. **Must be pure** -- no pqxx, no clock, no connection |
| `observe.h` | catalog readings, gathered into `Observations::extensions["<name>"]` |

### The enum stays closed, deliberately

`planner.h`'s dispatch is 83 cases with **no `default:`**, under `-Werror`, so
`-Wswitch` fails the build on an unhandled kind. A runtime registry would trade
that compiler guarantee for a test. Generating the enum arms from `kinds.inc`
keeps it: the enum is still closed at compile time, and the compiler still
polices every switch.

### Purity is enforced, not requested

`planner_purity_check.cpp` fails the build if `pqxx` reaches `planner.h`. It
covers `modules/*/plan.h` too. Nothing will fail the build if a module sneaks in
a non-determinism, so a module's planner must be a pure function of
`(Intent, Observations, ExecutorConfig)` by discipline.

### Three shapes, which are not interchangeable

- **additive** -- new kinds. The bulk of a module.
- **substitutive** -- how a core READING is taken. Both of the ones a
  distributed vendor needs are already outside `planDigest`
  (`kObservedDetailKeys`, and `budget` erased), so overriding them cannot change
  the receipt.
- **opaque slot** -- `Observations::extensions["<name>"]`, which core never
  inspects. Absent is not zero, and that distinction falls out for free.

## Scope

Extensions to PostgreSQL. Citus, and by the same seam something like Timescale.
**Not** other engines: Greenplum, YugabyteDB and CockroachDB diverge in catalog
shape, DDL semantics, and sometimes lack advisory locks entirely -- which would
remove the net the executor relies on. A seam designed for Citus will not fit
them, and claiming otherwise is a promise this project does not make.
