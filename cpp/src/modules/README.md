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

A directory `modules/<name>/`. CMake finds each file by name; the first five are
required, and a build naming the module fails at configure time without them.

| File | Required | Purpose |
|---|---|---|
| `kinds.inc` | yes | One `PGLASWELL_KIND(...)` line per intent kind. The X-macro that keeps the enum CLOSED. |
| `module.h` | yes | Includes the parse surface (`parse.h`, `observer.h`); `spec.h` pulls it in. |
| `parse.h` | yes | `parse_<kind>()` per kind, using `reject_unknown_keys` like core does |
| `plan.h` | yes | `plan_<kind>()` per kind. **Must be pure** -- no pqxx, no clock, no connection |
| `observe.h` | yes | `k<name>PresentSql` and `k<name>ObservationSql`: one query, gathered into `Observations::extensions["<name>"]` |
| `../../man/pg_laswell_<name>.7` | yes | The module's own page: its kinds are absent from a stock build, so the core page cannot list them |
| `keys.inc` | no | Object keys and conflict edges for kinds that plan against something other than a table |
| `project.h` + `project.inc` | no | What a step leaves for later steps in the same plan -- written into the module's OWN slot only |
| `guard.h` | no | `<name>_plan_refusals(spec, obs, refuse)`: refusals true of the whole topology |
| `confine.h` | no | `<name>_required_confinement(obs, table)`: the one reading core's paced walks consult |
| `observer.h` | no | Where contention is visible when it is not in `pg_locks` (Citus: on the workers) |
| `tests.inc` | no, in practice yes | Deferred-case reasons and one representative body per kind, for core's coverage tests |
| `tests_planner.inc` | no, in practice yes | The module's planner tests, compiled into the test binary only when it is enabled |

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

### THE RULE: a module may inform core's SQL, never write it

Four shapes are permitted and one is forbidden, and the line between them is what
keeps the design safe rather than merely tidy.

**Permitted:**

- **additive** -- new kinds. The bulk of a module.
- **substitutive** -- how a core READING is taken. Both of the ones a distributed
  vendor needs are already outside `planDigest` (`kObservedDetailKeys`, and
  `budget` erased), so overriding them cannot change the receipt.
- **refusing** -- a plan-level guard may declare a plan impossible. It says no;
  it does not rewrite.
- **informing** -- a module answers a QUESTION core asks, and core decides what
  follows. There is one such question today, asked through `confine.h`: *must
  row-locking batches on this table be confined to single values of a column, and
  which?* Citus answers "its distribution column" for a distributed table, and
  core's `backfill` and `delete_rows` then walk it one value at a time, which is
  what makes their `FOR UPDATE` single-shard. The module contributes a column
  name. Every statement is core's.

**Forbidden: a module may not emit or rewrite SQL for a CORE kind, and may not
shape a plan for a table it has no reading about.**

The second half is the guarantee that survives from the old rule and is tested
as such (`CitusPlanner.AModuleThatHasNothingToSayChangesNothing`, and every
golden plan unchanged): for a table a module says nothing about, a build with the
module and a build without it emit the same statements.

#### Why this changed, 2026-09-22

The rule used to read *"a module may not alter the SQL core emits for a core
kind"*, and it sent a paced walk on a distributed table to a module kind of its
own, `citus_distributed_backfill`, with core's `backfill` refusing and naming it.
Three things overturned that, and the first is decisive:

1. **One configuration, several targets.** A single `laswell.ini` can point one
   repository at plain PostgreSQL in development, Citus in staging to validate an
   upgrade, and some other PostgreSQL-compatible server that needs a module of its
   own. A specification that has to name its vendor's kind can serve only one of
   them. A backfill is a backfill whatever the server runs; the binary adapts per
   target through readings, which is what readings are for.
2. **The builds already disagreed, in the worse direction.** Measured, same
   specification, same distributed table (distributed on its own key): a build
   WITHOUT the module planned the walk, passed the dry run -- `PREPARE` cannot see
   what Citus will refuse to route -- and would have failed partway through
   execution. A build with it refused. "A module must not make builds differ" was
   already false; what the old rule bought was a difference in which the plain
   build was the dangerous one.
3. **The old argument pointed the other way.** It said a table local today and
   distributed tomorrow makes an explicit kind wrong, and treated that as a reason
   to refuse. It is a reason to infer: the specification written while the table
   was local must still run after it is distributed.

#### What keeps it honest without hashing `modules`

A plan a module has shaped emits different statements, so its `planDigest`
differs by construction -- "what ran is what you were shown" holds, because what
you are shown is the grouped walk. And it says so: the step records
`confined_by` and its `why` names the reading that decided. The recorded reason
`modules` is not hashed (`planner_base.h`) is reworded accordingly: the module
set can change statements, but only through readings that already change them,
so hashing the set would add nothing a reader of the statements does not see.

### `citus_distributed_backfill`, removed

`citus_distributed_backfill` existed for one commit and was never pushed. It was
removed rather than kept as "the explicit form", because two ways to do one thing
is the cost the lab's first report named ("the request would have added a second
way to do it"), and because an explicit form is precisely the vendor-naming the
multi-target case cannot use.

### What is ENFORCED, and what is only asked (audited 2026-09-21)

The rule above was written after the seams were built, so the hooks were audited
against it by PROBE rather than by reading, and the holes it found were closed
the same way -- then probed again. Results, because "I believe it complies" is
weaker than "I tried to break it":

| hook | probe | outcome |
|------|-------|---------|
| `OBJECT_KEY` naming a kind core already cases | `error: duplicate case value` | **compiler enforces** |
| `OBJECT_KEY` naming a core kind core leaves to `default:` | `static assertion failed: a module may only answer for kinds IT declared` | **was a hole, now closed** |
| `CONFLICT_KEYS` naming such a kind | same assertion | **was a hole, now closed** |
| `PROJECT` writing to `projected.tables` | `error: 'projected' was not declared in this scope` | **was a hole, now closed** |
| `PROJECT` naming a core kind | `static assertion failed: a module may only project for kinds IT declared` | **was a hole, now closed** |
| `PLAN_GUARD` pushing a warning | `error: 'plan' was not declared in this scope` | **was a hole, now closed** |
| `TOPOLOGY` merge | counts only ever raised | compliant |
| `OBSERVE` | writes only `extensions[<module>]` | compliant |
| `CONFINEMENT` | a function `(const Observations&, table) -> column`; it cannot reach the plan or name another table | compliant by construction |

Every close is the same move, and it is the only one worth making: **pass a hook
what it may touch, and nothing else.** A block that expands inside someone
else's scope inherits every name in it, and then compliance is discipline. A
function inherits its parameters, and then compliance is the signature.

**The plan guard** is a function taking `(spec, obs, refuse)`. `plan` is not
passed, so a guard can say no and say why, and nothing else.

**The projection hook** calls a function taking `(in, qualified, step, json& mine)`,
where `mine` is the module's own subtree of `Observations::extensions`. The
projected catalog is not passed, so a module cannot change how a CORE kind plans
two steps later. The slot name is injected by the BUILD
(`PGLASWELL_MODULE_SLOT`, set around each module's include in `CMakeLists.txt`),
not spelled by the module -- so a module cannot name `core` or another module's
slot either, because it never names a slot at all.

**Both key hooks** `static_assert(is_module_kind(...))`. `is_module_kind` is a
constexpr switch generated from the same `kinds.inc` list the enum arms come
from, so it cannot drift from what a module actually declared: answering for
`kAddColumn` is now a compile error rather than a silent change to a core kind's
grouping.

**The naming rule** is a test (`Modules.EveryModuleKindCarriesItsModulesName`)
derived from the enabled module list, so it holds for modules that do not exist
yet.

One thing the compiler cannot reach: a module's PLANNER writes `Step` objects,
and a step's SQL is a string. Nothing stops a module's own kind emitting whatever
SQL it likes -- that is the point of a module. What the rule forbids is a module
changing what core emits for a CORE kind, and every seam that could have done
that now refuses to compile.

### Naming: every module kind carries its module's name

`citus_distribute_table`, not `distribute_table`. Three reasons, and the first
is the one that matters:

1. **A reader of a specification can see which binary can run it.** A kind called
   `distribute_table` looks like something pg_laswell does; `citus_distribute_table`
   says which module implements it, so the refusal on a PostgreSQL-only build is
   predictable rather than surprising.
2. Two modules cannot collide. Another distributed extension wanting
   `distribute_table` would otherwise have to fight for the name.
3. It cannot be mistaken for a core kind in `docs/`, in `--help`, or in a ledger
   row read two years later.

### Three shapes, which are not interchangeable

- **additive** -- new kinds. The bulk of a module.
- **substitutive** -- how a core READING is taken. Both of the ones a
  distributed vendor needs are already outside `planDigest`
  (`kObservedDetailKeys`, and `budget` erased), so overriding them cannot change
  the receipt.
- **opaque slot** -- `Observations::extensions["<name>"]`, which core never
  inspects. Absent is not zero, and that distinction falls out for free.

## Writing one: the Citus module as the worked example

The Citus module is meant to be read as a template. Everything a second module
needs has a working instance in `modules/citus/`, and the steps below point at
it. Say the vendor is called `acme`.

1. **Measure before writing anything.** Every Citus planner cites a behaviour
   observed on a real cluster -- `rebalance_table_shards` keeping a move after
   ROLLBACK, `alter_distributed_table` silently dropping a foreign key -- and
   most refusals exist because a measurement contradicted what the documentation
   implied. Record what you ran and what came back in the comment above the
   code that relies on it.

2. **Declare the kinds** in `acme/kinds.inc`, each prefixed `acme_`. The line
   is the only declaration: enum, name table and both dispatches are generated.

3. **Parse** in `acme/parse.h`. Refuse every key you do not list; refuse at
   parse time anything the vendor refuses unconditionally (Citus: `shard_count`
   with `colocate_with`), so the author learns it without a database.

4. **Read** in `acme/observe.h`. One query building one JSON object, gated on a
   presence probe so that an absent extension is an absent key rather than an
   empty one. A reading must never raise on a healthy but unusual server: the
   Citus rebalance plan is wrapped in a `CASE` because the function raises when
   no node may hold shards, and an error there would fail every plan.

5. **Plan** in `acme/plan.h`, as a pure function. Each kind decides three things
   from the reading: is it already done (`kSatisfied` -- re-running a
   specification must be a no-op, and several vendor calls ERROR when asked to
   do what is already done), is it refused (`kConflict`, with the vendor's own
   error quoted and what to do instead), or what exactly runs (`kApply`).
   Then classify it honestly:
   - `TxnClass::kRequired` plus `detail["rehearse_by"] = "execution"` when the
     call is a `SELECT` that changes the catalog and rolls back cleanly --
     measured, not assumed. The dry run then executes it and rolls it back.
   - `TxnClass::kForbidden` when it does NOT roll back. The dry run skips it and
     lists it as unverified, and the executor runs it outside a transaction.
     Give it `detail["on_failure"]`: what a failure leaves behind.

6. **Project** in `acme/project.h` when a later intent in the same
   specification must see this one's effect. The function receives only the
   module's own slot, so it cannot change how a core kind plans.

7. **Inform core, never write its SQL.** When a core kind must behave
   differently on the vendor's server, the module answers a question core asks
   (`confine.h`) and core emits its own statements. See THE RULE above: for a
   table the module has nothing to say about, both builds must plan identically,
   and a test proves it.

8. **Test** at three levels, and prove each test by breaking the code it
   guards: planner tests against hand-written readings in `tests_planner.inc`;
   live cases against a real server in a script like `cpp/test/citus-tests.sh`,
   which checks that the vendor accepts the emitted SQL; and a CI job that
   starts the vendor's server, like the `citus` job in `tests.yml`.

9. **Document** in `man/pg_laswell_acme.7`. `tools/check-manual.py` fails until
   every kind in `kinds.inc` has a section there.

## Shipping a variant

One source tree, one package per configuration. A build is named after its
modules, so

    cmake -S cpp -B build                          # package: pg-laswell
    cmake -S cpp -B build -DPGLASWELL_MODULES=citus  # package: pg-laswell-citus

produce two packages that install the same two binaries. Each variant declares
that it conflicts with, replaces and provides `pg-laswell`: one binary is
installed at a time, and anything that depends on `pg-laswell` is satisfied by
either. The variant also installs its man7 page, and `--version` prints the
module set, so what is installed is always visible.

This is how one configuration serves several targets. A repository whose
specifications use only core kinds runs unchanged on a developer's PostgreSQL
and on a Citus staging cluster: on Citus, core consults the module (`confine.h`)
and walks each shard separately where a plain walk would be refused.
Specifications that use `citus_*` kinds are refused as a whole by a core-only
binary, so a target that needs them has to run the variant, and nothing is
skipped without anyone noticing. A server needing a different vendor gets its own
module and its own variant: `-DPGLASWELL_MODULES=acme` builds `pg-laswell-acme`.
Modules can be combined (`"citus;acme"` builds `pg-laswell-citus-acme`) as long as
their kind names do not collide, which the naming rule above guarantees.

## Scope

Extensions to PostgreSQL. Citus, and by the same seam something like Timescale.
**Not** other engines: Greenplum, YugabyteDB and CockroachDB diverge in catalog
shape, DDL semantics, and sometimes lack advisory locks entirely -- which would
remove the net the executor relies on. A seam designed for Citus will not fit
them, and claiming otherwise is a promise this project does not make.
