# Plan transcripts

Golden-file tests in the shape of PostgreSQL's own regression suite
(`src/test/regress`), and for the same reason.

    NAME.spec.json        the input specification
    NAME.obs.json         the observations to plan against
                          (falls back to shared.obs.json)
    expected/NAME.out     the transcript, committed and reviewed
    results/NAME.out      what actually came out, written on failure
    results/NAME.diff     and the difference

Run through ctest, or directly:

    cpp/test/plan-tests.sh cpp/build/render_plan cpp/test/plans

After an intended change, regenerate and **read the diff before committing**:

    PLAN_TESTS_ACCEPT=1 cpp/test/plan-tests.sh cpp/build/render_plan cpp/test/plans

## Why, when there are already 300-odd unit tests

Those tests assert that a warning *contains a phrase* — there are ninety-odd
`EXPECT_NE(w.find("..."), npos)` in `test_main.cpp`. None of them shows a reader
what the warning actually says, and a rewrite that keeps the phrase passes
silently however much worse it has become.

A transcript shows the whole thing. A behaviour change arrives as a diff
somebody reads. That is what PostgreSQL gets from `expected/*.out`, and the
property worth copying most is that **errors are output**: `ERROR:` lines live
in its expected files, so a refusal is reviewable content rather than an
exception nobody sees. `spec_refused.out` is that here — the refusal text, and
the evidence that it does not echo the credential it refused.

## Why observations come from a file

`plan_migration` is a pure function of (spec, observations). Feeding it a fixed
observation file makes the output deterministic by construction — no table
sizes drifting with autovacuum, no database to set up, nothing to skip when one
is absent — and it is the only test that exercises that purity directly.
`render_plan` links the pure headers alone, with no libpq.
