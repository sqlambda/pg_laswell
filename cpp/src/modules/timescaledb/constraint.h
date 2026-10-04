#pragma once
// TimescaleDB's answer to the question core asks about constraints
// (planner_base.h, ConstraintTraits): can one be validated in a step of its
// own on this table? Facts, never SQL. Included inside namespace pglaswell.
//
// For a hypertable with the columnstore enabled, measured on 2.30.2 and 2.28.3
// with 137 of 140 chunks converted:
//   - VALIDATE CONSTRAINT: "operation not supported on hypertables that have
//     columnstore enabled", for a check and a foreign key alike, after the NOT
//     VALID add succeeded;
//   - a validating ADD CONSTRAINT ... CHECK, ADD CONSTRAINT ... FOREIGN KEY and
//     ALTER COLUMN ... SET NOT NULL all work, and all read the converted
//     chunks: each failed on a violating row that existed only in one;
//   - the check and SET NOT NULL hold AccessExclusiveLock on the hypertable
//     and every chunk; the foreign key holds ShareRowExclusiveLock on them and
//     on the referenced table.
// A hypertable without the columnstore validates in a second step as any table
// does, so it gets no answer here.
inline ConstraintTraits timescaledb_constraint_traits(const Observations& obs,
                                                      const std::string& qualified) {
  ConstraintTraits t;
  const auto& ts = obs.extension("timescaledb");
  if (ts.empty()) return t;
  const auto& h = ts_member(ts, "hypertables");
  const auto it = h.find(qualified);
  if (it == h.end() || !it->value("columnstore", false)) return t;
  t.answered = true;
  t.separate_validation = false;
  t.reason = "operation not supported on hypertables that have columnstore enabled";
  if (!it->contains("projected_by_step")) {
    t.size_bytes = it->value("size_bytes", -1LL);
    t.rows = it->value("rows", -1LL);
  }
  const auto chunks = it->value("num_chunks", 0LL);
  t.scope = qualified + " and each of its " + std::to_string(chunks) + " chunk" +
            (chunks == 1 ? "" : "s");
  return t;
}
