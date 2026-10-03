#pragma once
// What a TimescaleDB step leaves for the steps after it, in the same plan.
// Receives the module's own slot only (see ../README.md). Included inside
// namespace pglaswell.
//
// The case this exists for is one specification that makes a table a
// hypertable, enables its columnstore, adds a policy and an index: each later
// intent must plan against the hypertable the earlier ones leave, and core's
// own create_index must ask about the hypertable as it will be (index.h).

inline json ts_interval_json(const std::string& text) {
  const auto i = ts_parse_interval(text);
  return i ? json::array({i->months, i->days, i->seconds}) : json();
}

inline void timescaledb_project_create_hypertable(const Intent& in, const std::string& qualified,
                                                  const Step& step, json& mine) {
  if (mine.empty() || step.action != Action::kApply) return;
  auto& h = mine["hypertables"][qualified];
  h = json{{"num_chunks", 0},
           {"columnstore", false},
           {"compressed_chunks", 0},
           {"dimensions", json::array({json{{"column", in.body.value("time_column", "")},
                                            {"interval", in.body.contains("chunk_time_interval")
                                                             ? ts_interval_json(in.body.value("chunk_time_interval", ""))
                                                             : json::array({0, 7, 0})}}})},
           {"projected_by_step", step.ordinal}};
}

inline void timescaledb_project_chunk_interval(const Intent& in, const std::string& qualified,
                                               const Step&, json& mine) {
  if (!mine.contains("hypertables") || !mine["hypertables"].contains(qualified)) return;
  auto& dims = mine["hypertables"][qualified]["dimensions"];
  if (dims.is_array() && !dims.empty()) {
    dims[0]["interval"] = ts_interval_json(in.body.value("chunk_time_interval", ""));
  }
}

inline void timescaledb_project_columnstore(const Intent& in, const std::string& qualified,
                                            const Step&, json& mine) {
  if (!mine.contains("hypertables") || !mine["hypertables"].contains(qualified)) return;
  auto& h = mine["hypertables"][qualified];
  h["columnstore"] = true;
  json seg = json::array(), ord = json::array(), desc = json::array();
  for (const auto& e : in.body.value("segment_by", json::array())) seg.push_back(e);
  for (const auto& e : in.body.value("order_by", json::array())) {
    std::istringstream words(e.get<std::string>());
    std::string c, d;
    words >> c >> d;
    for (auto& ch : d) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    ord.push_back(c);
    desc.push_back(d == "desc");
  }
  h["segmentby"] = seg;
  h["orderby"] = ord;
  h["orderby_desc"] = desc;
}

inline void timescaledb_project_policy(json& mine, const std::string& target, const char* which,
                                       json policy) {
  if (mine.empty()) return;
  if (policy.is_null()) {
    if (mine.contains("policies") && mine["policies"].contains(target)) {
      mine["policies"][target].erase(which);
    }
    return;
  }
  mine["policies"][target][which] = std::move(policy);
}

inline void timescaledb_project_add_columnstore_policy(const Intent& in, const std::string& qualified,
                                                       const Step&, json& mine) {
  timescaledb_project_policy(mine, qualified, "columnstore",
                             json{{"after", ts_interval_json(in.body.value("after", ""))}});
}
inline void timescaledb_project_remove_columnstore_policy(const Intent&, const std::string& qualified,
                                                          const Step&, json& mine) {
  timescaledb_project_policy(mine, qualified, "columnstore", json());
}
inline void timescaledb_project_add_retention_policy(const Intent& in, const std::string& qualified,
                                                     const Step&, json& mine) {
  timescaledb_project_policy(mine, qualified, "retention",
                             json{{"drop_after", ts_interval_json(in.body.value("drop_after", ""))}});
}
inline void timescaledb_project_remove_retention_policy(const Intent&, const std::string& qualified,
                                                        const Step&, json& mine) {
  timescaledb_project_policy(mine, qualified, "retention", json());
}

// The continuous-aggregate kinds name no table: core dispatches them before its
// tables guard, and `qualified` is then empty, so the view is named here.
inline void timescaledb_project_cagg(const Intent& in, const std::string&, const Step& step,
                                     json& mine) {
  if (mine.empty() || step.action != Action::kApply) return;
  const auto view = in.body.value("schema", "") + "." + in.body.value("name", "");
  mine["caggs"][view] = json{{"projected_by_step", step.ordinal}};
}
inline void timescaledb_project_cagg_policy(const Intent& in, const std::string&, const Step&,
                                            json& mine) {
  const auto view = in.body.value("schema", "") + "." + in.body.value("name", "");
  timescaledb_project_policy(
      mine, view, "refresh",
      json{{"start_offset", ts_interval_json(in.body.value("start_offset", ""))},
           {"end_offset", ts_interval_json(in.body.value("end_offset", ""))},
           {"schedule_interval", ts_interval_json(in.body.value("schedule_interval", ""))}});
}
