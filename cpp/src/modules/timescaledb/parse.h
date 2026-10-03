#pragma once
// TimescaleDB parsers. Included inside namespace pglaswell by spec.h.
//
// What is checked here needs no server: keys, shapes, identifiers, and that an
// interval is one this module can compare. The rest -- does the table exist,
// is it a hypertable, which edition is this -- is the planner's, from readings.

// An interval as (months, days, seconds): how PostgreSQL itself keeps one, so
// "7 days" and "168 hours" are NOT the same (days and seconds differ across a
// daylight-saving change) while "1 week" and "7 days" are. The reading
// decomposes the server's intervals the same way (observe.h), so the two can be
// compared without asking the server.
//
// Accepted: one or more "<n> <unit>" pairs, units year(s), month(s)/mon(s),
// week(s), day(s), hour(s), minute(s)/min(s), second(s)/sec(s). A subset of
// PostgreSQL's own input syntax, so the same text is sent to the server as an
// INTERVAL literal and means the same thing there.
// A member of a reading by reference, or an empty object: never a temporary.
// json::value() returns BY VALUE, and a reference into its result dangles --
// the bug shape this project has met five times, which -Wdangling-reference
// stopped here too.
inline const json& ts_member(const json& j, const char* key) {
  static const json kEmpty = json::object();
  if (!j.is_object()) return kEmpty;
  const auto it = j.find(key);
  return it == j.end() ? kEmpty : *it;
}

struct TsInterval {
  long long months = 0;
  long long days = 0;
  long long seconds = 0;
  bool operator==(const TsInterval&) const = default;
};

inline std::optional<TsInterval> ts_parse_interval(const std::string& text) {
  TsInterval out;
  std::istringstream in(text);
  std::string number, unit;
  bool any = false;
  while (in >> number) {
    if (!(in >> unit)) return std::nullopt;
    if (number.empty() || number.size() > 12 ||
        !std::all_of(number.begin(), number.end(),
                     [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) {
      return std::nullopt;
    }
    const long long n = std::stoll(number);
    for (auto& c : unit) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (unit == "year" || unit == "years") out.months += 12 * n;
    else if (unit == "month" || unit == "months" || unit == "mon" || unit == "mons") out.months += n;
    else if (unit == "week" || unit == "weeks") out.days += 7 * n;
    else if (unit == "day" || unit == "days") out.days += n;
    else if (unit == "hour" || unit == "hours") out.seconds += 3600 * n;
    else if (unit == "minute" || unit == "minutes" || unit == "min" || unit == "mins") out.seconds += 60 * n;
    else if (unit == "second" || unit == "seconds" || unit == "sec" || unit == "secs") out.seconds += n;
    else return std::nullopt;
    any = true;
  }
  if (!any) return std::nullopt;
  return out;
}

inline void ts_require_interval(const Intent& in, const std::string& key,
                                const std::string& at, bool required) {
  if (!in.body.contains(key)) {
    if (required) detail::fail(at + " needs \"" + key + "\"", "");
    return;
  }
  const auto& v = in.body[key];
  if (!v.is_string() || !ts_parse_interval(v.get<std::string>())) {
    detail::fail(at + "." + key + " must be an interval such as \"7 days\" or "
                 "\"1 day 12 hours\"",
                 "One or more <number> <unit> pairs; units year, month, week, day, "
                 "hour, minute, second. The same text is sent to PostgreSQL.");
  }
}

inline void ts_require_table(Intent& in, const std::string& at) {
  detail::require_identifier(detail::require_string(in.body, "schema", at), "schema",
                             in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "table", at), "table",
                             in.ordinal);
}

inline void ts_optional_text(const Intent& in, const std::string& key, const std::string& at) {
  if (!in.body.contains(key)) return;
  if (!in.body[key].is_string() || in.body[key].get<std::string>().empty()) {
    detail::fail(at + "." + key + " must be a non-empty string", "");
  }
}

inline void parse_timescaledb_create_hypertable(Intent& in) {
  const auto at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(in.body,
                              {"kind", "schema", "table", "time_column",
                               "chunk_time_interval", "migrate_data", "comment"},
                              at);
  ts_require_table(in, at);
  detail::require_identifier(detail::require_string(in.body, "time_column", at),
                             "time_column", in.ordinal);
  ts_require_interval(in, "chunk_time_interval", at, false);
  if (in.body.contains("migrate_data") && !in.body["migrate_data"].is_boolean()) {
    detail::fail(at + ".migrate_data must be a boolean",
                 "true moves rows already in the table into chunks, holding "
                 "AccessExclusiveLock on it while they move.");
  }
  ts_optional_text(in, "comment", at);
}

inline void parse_timescaledb_set_chunk_time_interval(Intent& in) {
  const auto at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(in.body,
                              {"kind", "schema", "table", "chunk_time_interval", "comment"},
                              at);
  ts_require_table(in, at);
  ts_require_interval(in, "chunk_time_interval", at, true);
  ts_optional_text(in, "comment", at);
}

// order_by entries: "column" or "column DESC" / "column ASC" -- the vocabulary
// TimescaleDB's own timescaledb.orderby setting uses.
inline void parse_timescaledb_set_columnstore(Intent& in) {
  const auto at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(in.body,
                              {"kind", "schema", "table", "segment_by", "order_by", "comment"},
                              at);
  ts_require_table(in, at);
  for (const char* key : {"segment_by", "order_by"}) {
    if (!in.body.contains(key)) continue;
    if (!in.body[key].is_array()) detail::fail(at + "." + key + " must be an array", "");
    for (const auto& e : in.body[key]) {
      if (!e.is_string()) detail::fail(at + "." + key + " entries must be strings", "");
      std::istringstream words(e.get<std::string>());
      std::string column, direction, extra;
      words >> column >> direction >> extra;
      detail::require_identifier(column, key, in.ordinal);
      for (auto& c : direction) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      const bool ordered = std::string(key) == "order_by";
      if (!extra.empty() || (!direction.empty() && (!ordered || (direction != "asc" &&
                                                                 direction != "desc")))) {
        detail::fail(at + "." + key + " entry \"" + e.get<std::string>() + "\" is not " +
                         (ordered ? "a column, optionally followed by ASC or DESC"
                                  : "a column name"),
                     "");
      }
    }
  }
  ts_optional_text(in, "comment", at);
}

inline void parse_timescaledb_add_columnstore_policy(Intent& in) {
  const auto at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(in.body, {"kind", "schema", "table", "after", "comment"}, at);
  ts_require_table(in, at);
  ts_require_interval(in, "after", at, true);
  ts_optional_text(in, "comment", at);
}

inline void parse_timescaledb_remove_columnstore_policy(Intent& in) {
  const auto at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(in.body, {"kind", "schema", "table", "comment"}, at);
  ts_require_table(in, at);
  ts_optional_text(in, "comment", at);
}

// A retention policy DELETES DATA, on a schedule, for as long as it exists --
// so it is never implied: the specification says so in a key of its own,
// which is signed with the rest.
inline void parse_timescaledb_add_retention_policy(Intent& in) {
  const auto at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(
      in.body, {"kind", "schema", "table", "drop_after", "acknowledge_data_loss", "comment"},
      at);
  ts_require_table(in, at);
  ts_require_interval(in, "drop_after", at, true);
  if (!in.body.contains("acknowledge_data_loss") ||
      in.body["acknowledge_data_loss"] != true) {
    detail::fail(at + " needs \"acknowledge_data_loss\": true",
                 "A retention policy drops every chunk older than drop_after, on a "
                 "schedule, from now on. That is data deleted by design, so the "
                 "specification has to say it means it.");
  }
  ts_optional_text(in, "comment", at);
}

inline void parse_timescaledb_remove_retention_policy(Intent& in) {
  const auto at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(in.body, {"kind", "schema", "table", "comment"}, at);
  ts_require_table(in, at);
  ts_optional_text(in, "comment", at);
}

// The query is SQL, as `where` is elsewhere: there is nothing to validate it
// against without a server, and it is signed with the rest of the
// specification, which is what makes it reviewable. The dry run creates the
// view and rolls it back (WITH NO DATA runs in a transaction, measured).
inline void parse_timescaledb_create_continuous_aggregate(Intent& in) {
  const auto at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(in.body,
                              {"kind", "schema", "name", "query", "materialized_only",
                               "refresh", "comment"},
                              at);
  detail::require_identifier(detail::require_string(in.body, "schema", at), "schema",
                             in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "name", at), "name",
                             in.ordinal);
  (void)detail::require_string(in.body, "query", at);
  for (const char* key : {"materialized_only", "refresh"}) {
    if (in.body.contains(key) && !in.body[key].is_boolean()) {
      detail::fail(at + "." + key + " must be a boolean", "");
    }
  }
  ts_optional_text(in, "comment", at);
}

inline void parse_timescaledb_add_continuous_aggregate_policy(Intent& in) {
  const auto at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(in.body,
                              {"kind", "schema", "name", "start_offset", "end_offset",
                               "schedule_interval", "comment"},
                              at);
  detail::require_identifier(detail::require_string(in.body, "schema", at), "schema",
                             in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "name", at), "name",
                             in.ordinal);
  ts_require_interval(in, "start_offset", at, true);
  ts_require_interval(in, "end_offset", at, true);
  ts_require_interval(in, "schedule_interval", at, true);
  ts_optional_text(in, "comment", at);
}
