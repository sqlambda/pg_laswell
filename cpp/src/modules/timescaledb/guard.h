#pragma once
// Plan-level refusals for CORE kinds on a hypertable: what TimescaleDB refuses
// and core would otherwise plan. A refusal only (THE RULE, ../README.md); how
// an index is built or dropped on a hypertable is core's decision, informed by
// index.h. Measured on 2.30.2 (both editions) and 2.28.3. Included inside
// namespace pglaswell.
//
// The guard runs before projection, so it follows the specification in order:
// a table made a hypertable, or given the columnstore, by an earlier intent is
// treated as one by every intent after it.
template <typename Refuse, typename Advise>
inline void timescaledb_plan_refusals(const Spec& spec, const Observations& obs,
                                      const json& /*budget*/, const Refuse& refuse,
                                      const Advise& /*advise*/) {
  const auto& ts = obs.extension("timescaledb");
  if (ts.empty()) return;

  struct Hyper {
    std::vector<std::string> dims;  // every partitioning column, in order
    bool columnstore = false;
    long long compressed = 0;
  };
  std::map<std::string, Hyper> hyper;
  for (const auto& [name, h] : ts_member(ts, "hypertables").items()) {
    Hyper x;
    for (const auto& d : h.value("dimensions", json::array())) x.dims.push_back(d.value("column", ""));
    x.columnstore = h.value("columnstore", false);
    x.compressed = h.value("compressed_chunks", 0LL);
    hyper[name] = x;
  }

  const auto missing_dim = [](const Hyper& h, const std::vector<std::string>& cols) {
    for (const auto& d : h.dims) {
      if (std::find(cols.begin(), cols.end(), d) == cols.end()) return d;
    }
    return std::string();
  };

  for (const auto& in : spec.intents) {
    const auto q = in.qualified_table();
    if (in.kind == IntentKind::kTimescaledbCreateHypertable) {
      hyper[q].dims = {in.body.value("time_column", "")};
      continue;
    }
    if (in.kind == IntentKind::kTimescaledbSetColumnstore) {
      if (hyper.count(q)) hyper[q].columnstore = true;
      continue;
    }
    const auto it = hyper.find(q);
    if (it == hyper.end()) continue;
    const auto& h = it->second;
    const std::string what = in.kind_name + " on " + q;

    // A UNIQUE key must contain every partitioning column. Measured: "cannot
    // create a unique index without the column \"ts\" (used in partitioning)",
    // for an index, a primary key and a unique constraint alike.
    std::vector<std::string> key;
    bool unique = false;
    if (in.kind == IntentKind::kCreateIndex && in.body.value("unique", false)) {
      unique = true;
      for (const auto& c : in.body.value("columns", json::array())) {
        key.push_back(c.is_string() ? c.get<std::string>() : c.value("name", ""));
      }
    } else if (in.kind == IntentKind::kAddPrimaryKey ||
               in.kind == IntentKind::kAddUniqueConstraint) {
      unique = true;
      for (const auto& c : in.body.value("columns", json::array())) key.push_back(c.get<std::string>());
    }
    if (unique) {
      const auto d = missing_dim(h, key);
      if (!d.empty()) {
        refuse(what + ": the key (" + detail::join(key, ", ") + ") does not contain " + d +
               ", and " + q + " is a hypertable partitioned on it. TimescaleDB: \"cannot "
               "create a unique index without the column \\\"" + d + "\\\" (used in "
               "partitioning)\". Add " + d + " to the key.");
      }
    }

    // VALIDATE CONSTRAINT is refused once the columnstore is enabled. That is
    // not a refusal here: constraint.h tells core, which validates in the
    // statement that adds the constraint.

    // ALTER COLUMN TYPE once any chunk is converted: "operation not supported on
    // hypertables with compressed chunks".
    if (h.compressed > 0 && in.kind == IntentKind::kAlterColumnType) {
      refuse(what + ": " + q + " has " + std::to_string(h.compressed) + " chunk(s) in the "
             "columnstore, and TimescaleDB refuses a column type change then (\"operation "
             "not supported on hypertables with compressed chunks\"). Convert them back "
             "first.");
    }
  }
}
