#pragma once
// pgvector's answer to the question core asks about indexes
// (planner_base.h, IndexTraits): for an HNSW build, how much
// maintenance_work_mem the graph would like. Nothing else changes -- a
// concurrent build is fine, and the table's own size is the right one -- so
// core decides as it always does, and only the memory may be raised, by
// index_build_mwm_bytes: never past a configured ceiling, and without one, up
// to a limit deduced from shared_buffers. Included inside namespace pglaswell.
#include "modules/pgvector/guard.h"

inline IndexTraits pgvector_index_traits(const Observations& obs, const std::string& qualified,
                                         const Intent& in) {
  IndexTraits t;
  if (obs.extension("pgvector").empty()) return t;
  if (in.kind != IntentKind::kCreateIndex || in.body.value("method", "btree") != "hnsw") return t;
  const auto& columns = in.body.value("columns", json::array());
  if (columns.size() != 1) return t;  // refused by the guard anyway
  const auto& c = columns[0];
  std::string type;
  if (c.is_string() || !c.value("name", "").empty()) {
    const auto name = c.is_string() ? c.get<std::string>() : c.value("name", "");
    const auto& cols = obs.table(qualified).value("columns", json::object());
    if (cols.contains(name)) type = cols[name].value("type", "");
  } else {
    type = pgvector_cast_type(c.value("expression", ""));
  }
  if (type.empty()) return t;
  const auto pt = pgvector_parse_type(type);
  const auto with = in.body.value("with", json::object());
  const long long m = with.contains("m") ? pgvector_int(with["m"]).value_or(16) : 16;
  const long long per_row = pgvector_graph_bytes_per_row(pt, m);
  const auto& tab = obs.table(qualified);
  const long long rows = std::max(tab.value("reltuples", 0LL),
                                  tab.value("stats", json::object()).value("n_live_tup", 0LL));
  if (per_row <= 0 || pt.dims <= 0 || rows <= 0) return t;
  t.answered = true;
  t.memory_wanted = rows * per_row;
  return t;
}
