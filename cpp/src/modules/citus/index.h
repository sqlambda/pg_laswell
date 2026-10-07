#pragma once
// Citus's answer to the question core asks about indexes (planner_base.h,
// IndexTraits): how may one be built on this table, and how big is the data
// really? Facts, never SQL. Included inside namespace pglaswell.
//
// For a distributed table the coordinator's relation is a shell. Measured on
// Citus 14.0: pg_table_size 8192 bytes, relpages 0 and reltuples 0, against
// 15 MB and 200 000 rows in its shards -- and in the field, 346 MB read as
// "size 0 B < 64 MiB ceiling -> plain build". So core sized every index build
// on a distributed table as tiny and built it plainly, which blocks writes on
// every shard for as long as it runs, and presented that as the safe choice.
//
// CREATE INDEX CONCURRENTLY works on a distributed table (measured: built,
// and valid on the coordinator), so nothing about HOW changes here: only the
// size core decides by. The same reading gives a backfill its row estimate.
//
// No answer when the size was not read -- a node that did not reply, a table
// created earlier in the same specification -- and core then decides as it
// does for any table.
inline IndexTraits citus_index_traits(const Observations& obs, const std::string& qualified,
                                      const Intent&) {
  IndexTraits t;
  const auto& citus = obs.extension("citus");
  if (!citus.is_object() || !citus.contains("sizes")) return t;
  const json& sizes = citus["sizes"];
  if (!sizes.is_object() || !sizes.contains(qualified)) return t;
  const json& mine = sizes[qualified];
  t.answered = true;
  t.concurrent = true;
  t.size_bytes = mine.value("size_bytes", -1LL);
  t.rows = mine.value("rows", -1LL);
  long long shards = 0;
  if (citus.contains("tables") && citus["tables"].is_object() &&
      citus["tables"].contains(qualified)) {
    const json& entry = citus["tables"][qualified];
    if (entry.contains("shard_count") && entry["shard_count"].is_number()) {
      shards = entry["shard_count"].get<long long>();
    }
  }
  t.scope = qualified + (shards > 0 ? " and each of its " + std::to_string(shards) + " shards"
                                    : " and each of its shards");
  return t;
}
