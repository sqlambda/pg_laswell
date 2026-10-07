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
  // BUILT ONE CHUNK AT A TIME, CONCURRENTLY. The per-chunk option above holds a
  // write-blocking lock on one chunk after another, and a write that does not
  // name the time column has to lock them all: found in the field, updates by
  // id stopped for the whole build. Measured on 2.30.2 (Apache edition), a
  // 1.2 GB hypertable of 19 chunks:
  //   - CREATE INDEX ... ON ONLY <hypertable> creates the index on the parent
  //     alone -- ShareLock on it and its chunks, for a catalog change -- and a
  //     chunk created afterwards gets the index by itself;
  //   - CREATE INDEX CONCURRENTLY on a chunk, directly, works;
  //   - TimescaleDB counts those chunk indexes as the hypertable index's: the
  //     planner uses them, and DROP INDEX on the hypertable's removed them all.
  //   With the per-chunk option: 4.6 s, an update by id waited up to 3 491 ms.
  //   This way: 2.5 s, worst update by id 43 ms, worst insert 42 ms.
  //
  // Said only where it was measured: 2.30 and later, and a hypertable WITHOUT
  // the columnstore -- what a concurrent build does on a converted chunk has
  // not been measured, nor has 2.28. Elsewhere there is no answer here and the
  // per-chunk option above stands.
  const auto version = ts.value("version", "");
  int major = 0, minor = 0;
  try {
    std::size_t dot = 0;
    major = std::stoi(version, &dot);
    if (dot < version.size()) minor = std::stoi(version.substr(dot + 1));
  } catch (const std::exception&) {
    major = 0;
  }
  const bool measured_here = (major > 2 || (major == 2 && minor >= 30)) &&
                             !it->value("columnstore", false) &&
                             !it->contains("projected_by_step");
  if (measured_here && it->contains("chunks") && (*it)["chunks"].is_array()) {
    for (const auto& c : (*it)["chunks"]) {
      IndexPart part;
      part.relation = c.value("relation", "");
      if (c.contains("indexes") && c["indexes"].is_object()) {
        for (const auto& [iname, valid] : c["indexes"].items()) {
          part.indexes[iname] = valid.is_boolean() && valid.get<bool>();
        }
      }
      if (!part.relation.empty()) t.parts.push_back(std::move(part));
    }
  }
  const auto chunks = it->value("num_chunks", 0LL);
  t.scope = qualified + " and each of its " + std::to_string(chunks) + " chunk" +
            (chunks == 1 ? "" : "s");
  return t;
}
