#pragma once
// TimescaleDB's answer to the question core asks about indexes
// (planner_base.h, IndexTraits): how may one be built and dropped on this
// table? Facts, never SQL. Included inside namespace pglaswell.
//
// For a hypertable, measured on 2.30.2 and 2.28.3:
//   - "hypertables do not support concurrent index creation";
//   - DROP INDEX CONCURRENTLY: "does not support dropping multiple objects";
//   - WITH (timescaledb.transaction_per_chunk) builds one chunk at a time, each
//     in its own transaction -- not inside a transaction block, and not for a
//     UNIQUE index ("cannot use timescaledb.transaction_per_chunk with UNIQUE
//     or PRIMARY KEY"); a failure partway leaves the parent index INVALID with
//     some chunks built. The option is not stored with the index;
//   - the parent holds no rows: relpages 0 and reltuples 0 against 5 MB and
//     20 000 rows in its chunks, so without this core would size a build as a
//     tiny one, and a plain build holds ShareLock on the parent and every chunk
//     until it ends.
inline IndexTraits timescaledb_index_traits(const Observations& obs,
                                            const std::string& qualified, const Intent&) {
  IndexTraits t;
  const auto& ts = obs.extension("timescaledb");
  if (ts.empty()) return t;
  const auto& h = ts_member(ts, "hypertables");
  const auto it = h.find(qualified);
  if (it == h.end()) return t;
  t.answered = true;
  t.concurrent = false;
  t.per_part_option = "timescaledb.transaction_per_chunk";
  t.per_part_unique = false;
  // A hypertable created earlier in the same specification has no reading of
  // its own; its data is the table's, which core already read.
  if (!it->contains("projected_by_step")) {
    t.size_bytes = it->value("size_bytes", -1LL);
    t.rows = it->value("rows", -1LL);
  }
  const auto chunks = it->value("num_chunks", 0LL);
  t.scope = qualified + " and each of its " + std::to_string(chunks) + " chunk" +
            (chunks == 1 ? "" : "s");
  return t;
}
