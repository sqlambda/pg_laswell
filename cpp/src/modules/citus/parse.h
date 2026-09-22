#pragma once
// Citus intent parsing.
//
// Written against core's helpers, and following core's rule: every key not
// listed is REFUSED, never ignored. A key this binary skips and a newer one
// honours is a silent difference between what was reviewed and what ran.
// NO namespace of its own: this header is included INSIDE namespace pglaswell,
// next to the helpers it is written against. Opening the namespace again would
// nest it -- pglaswell::pglaswell -- and the generated dispatch would not find
// anything.

// Citus's own sentinels for colocate_with. Every real value is schema.table, so
// a bare word is unambiguous -- but the parse error says so rather than leaving
// a reader to work out why a table called "none" cannot be named.
inline bool citus_is_colocation_sentinel(const std::string& v) {
  return v == "none" || v == "default";
}

inline void citus_check_colocate_with(const Intent& in, const std::string& at) {
  const auto v = in.body.value("colocate_with", "");
  if (v.empty()) return;
  if (citus_is_colocation_sentinel(v)) return;
  if (v.find('.') == std::string::npos) {
    detail::fail(at + ".colocate_with must be \"schema.table\", or one of "
                      "Citus's own words \"none\" and \"default\"",
                 "An unqualified name is ambiguous with those two sentinels, so "
                 "a table genuinely called \"none\" has to be written "
                 "\"public.none\".");
  }
}

inline void parse_citus_distribute_table(Intent& in) {
  const auto at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(
      in.body,
      {"kind", "schema", "table", "distribution_column", "shard_method",
       "colocate_with", "shard_count", "concurrently", "comment"},
      at);
  detail::require_identifier(detail::require_string(in.body, "schema", at),
                             "schema", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "table", at),
                             "table", in.ordinal);
  detail::require_identifier(
      detail::require_string(in.body, "distribution_column", at),
      "distribution_column", in.ordinal);

  const auto method = in.body.value("shard_method", "hash");
  if (method != "hash" && method != "append" && method != "range") {
    detail::fail(at + ".shard_method must be hash, append or range",
                 "hash is the default and the only one Citus recommends for "
                 "new work; append and range are legacy.");
  }

  const auto when = in.body.value("concurrently", "auto");
  if (when != "auto" && when != "always" && when != "never") {
    detail::fail(at + ".concurrently must be auto, always or never",
                 "auto lets the planner decide from the table's size and say "
                 "which reading decided. always and never are the overrides "
                 "for someone who knows better than the reading.");
  }

  citus_check_colocate_with(in, at);

  // Mutually exclusive in practice: colocating ADOPTS the group's shard count,
  // so naming both asks for two different numbers and Citus honours one.
  // Refused here rather than at execution, where the answer would already be
  // half applied.
  if (in.body.contains("colocate_with") && in.body.contains("shard_count") &&
      !citus_is_colocation_sentinel(in.body.value("colocate_with", ""))) {
    detail::fail(at + " names both colocate_with and shard_count",
                 "Colocating adopts the shard count of the group it joins, so "
                 "the explicit one would be ignored. Drop shard_count to "
                 "colocate, or drop colocate_with to choose a count.");
  }
  if (in.body.contains("shard_count")) {
    if (!in.body["shard_count"].is_number_integer() ||
        in.body["shard_count"].get<int>() <= 0) {
      detail::fail(at + ".shard_count must be a positive integer", "");
    }
  }
}

inline void parse_citus_create_reference_table(Intent& in) {
  const auto at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(in.body, {"kind", "schema", "table", "comment"}, at);
  detail::require_identifier(detail::require_string(in.body, "schema", at),
                             "schema", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "table", at),
                             "table", in.ordinal);
}

inline void parse_citus_distribute_function(Intent& in) {
  const auto at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(
      in.body,
      {"kind", "schema", "name", "arguments", "distribution_argument",
       "colocate_with", "force_delegation", "comment"},
      at);
  detail::require_identifier(detail::require_string(in.body, "schema", at),
                             "schema", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "name", at),
                             "name", in.ordinal);
  detail::require_string(in.body, "arguments", at);

  // Colocation and a distribution argument travel together: colocating a
  // function with a table is what makes the argument mean anything, and Citus
  // refuses the pair broken in either direction.
  const bool has_arg = in.body.contains("distribution_argument");
  const bool has_col = in.body.contains("colocate_with");
  if (has_arg != has_col) {
    detail::fail(at + " names " + std::string(has_arg ? "distribution_argument"
                                                      : "colocate_with") +
                     " without the other",
                 "A distribution argument says which argument routes the call; "
                 "colocate_with says which table's shards it routes to. One "
                 "without the other cannot be acted on. Omit both to replicate "
                 "the function to every node instead.");
  }
  if (has_col) citus_check_colocate_with(in, at);
  if (in.body.contains("force_delegation") &&
      !in.body["force_delegation"].is_boolean()) {
    detail::fail(at + ".force_delegation must be a boolean", "");
  }
}


// A paced backfill confined to one shard at a time.
//
// The same vocabulary core's backfill uses -- key, set, where -- because it is
// the same operation. What it does NOT take is the distribution column: that is
// a catalog fact read from pg_dist_partition, not an author's choice, and a spec
// naming it could disagree with the cluster.
inline void parse_citus_distributed_backfill(Intent& in) {
  const auto at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(
      in.body,
      {"kind", "schema", "table", "key", "set", "where", "verify_remaining",
       "assert_invariants"},
      at);
  detail::require_identifier(detail::require_string(in.body, "schema", at),
                             "schema", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "table", at),
                             "table", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "key", at), "key",
                             in.ordinal);
  if (!in.body.contains("set") || !in.body["set"].is_object() ||
      in.body["set"].empty()) {
    detail::fail(at + " needs a non-empty \"set\" of column to expression",
                 "The same shape backfill takes.");
  }
  for (auto it = in.body["set"].begin(); it != in.body["set"].end(); ++it) {
    detail::require_identifier(it.key(), "set", in.ordinal);
    if (!it.value().is_string()) {
      detail::fail(at + ".set." + it.key() + " must be a SQL expression string",
                   "");
    }
  }
  // Mandatory, exactly as core's backfill requires it: a walk with no predicate
  // rewrites every row and can never report itself finished.
  if (detail::require_string(in.body, "where", at).empty()) {
    detail::fail(at + " needs a \"where\"",
                 "A backfill without a predicate has no way to say it is done, "
                 "and would rewrite every row on every run.");
  }
}
