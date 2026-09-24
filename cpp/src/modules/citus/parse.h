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

// --- Phase 2: the lifecycle of a distributed table --------------------------

// Change how a distributed table is distributed: its column, its shard count,
// or the group it is colocated with. At least one, because a call that changes
// nothing is an ERROR in Citus -- measured: "this call doesn't change any
// properties of the table".
inline void parse_citus_alter_distributed_table(Intent& in) {
  const auto at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(
      in.body,
      {"kind", "schema", "table", "distribution_column", "shard_count",
       "colocate_with", "cascade_to_colocated", "comment"},
      at);
  detail::require_identifier(detail::require_string(in.body, "schema", at),
                             "schema", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "table", at),
                             "table", in.ordinal);
  const bool col = in.body.contains("distribution_column");
  const bool count = in.body.contains("shard_count");
  const bool with = in.body.contains("colocate_with");
  if (!col && !count && !with) {
    detail::fail(at + " changes nothing",
                 "Give at least one of distribution_column, shard_count and "
                 "colocate_with. Citus refuses a call that changes no property.");
  }
  if (col) {
    detail::require_identifier(
        detail::require_string(in.body, "distribution_column", at),
        "distribution_column", in.ordinal);
  }
  if (count && (!in.body["shard_count"].is_number_integer() ||
                in.body["shard_count"].get<int>() <= 0)) {
    detail::fail(at + ".shard_count must be a positive integer", "");
  }
  if (with) {
    citus_check_colocate_with(in, at);
    // "default" has no reading to compare against: the default group is
    // Citus's choice at call time, so whether the table is already in it
    // cannot be decided beforehand. Measured, the call rewrites the table
    // regardless -- on a table already alone it moved to a new group.
    if (in.body.value("colocate_with", "") == "default") {
      detail::fail(at + ".colocate_with cannot be \"default\" here",
                   "Name the table to colocate with, or \"none\" for a group of "
                   "its own. Whether a table is already in Citus's default group "
                   "cannot be read beforehand, so the plan could not say whether "
                   "the rewrite is needed.");
    }
    // Measured: "shard_count cannot be different than the shard count of the
    // table in colocate_with". Joining a group adopts its count.
    if (count && !citus_is_colocation_sentinel(in.body.value("colocate_with", ""))) {
      detail::fail(at + " names both colocate_with and shard_count",
                   "Joining a colocation group adopts its shard count, and Citus "
                   "refuses any other. Drop shard_count.");
    }
  }
  // Measured: "distribution_column cannot be cascaded to colocated tables".
  if (col && in.body.value("cascade_to_colocated", false)) {
    detail::fail(at + " cascades a distribution column change",
                 "Citus cascades only shard_count to a colocation group. Change "
                 "each table's column in its own intent.");
  }
  if (in.body.contains("cascade_to_colocated") &&
      !in.body["cascade_to_colocated"].is_boolean()) {
    detail::fail(at + ".cascade_to_colocated must be a boolean", "");
  }
}

// Turn a Citus table back into an ordinary one: its data is gathered back onto
// the coordinator.
inline void parse_citus_undistribute_table(Intent& in) {
  const auto at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(
      in.body, {"kind", "schema", "table", "cascade_via_foreign_keys", "comment"},
      at);
  detail::require_identifier(detail::require_string(in.body, "schema", at),
                             "schema", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "table", at),
                             "table", in.ordinal);
  if (in.body.contains("cascade_via_foreign_keys") &&
      !in.body["cascade_via_foreign_keys"].is_boolean()) {
    detail::fail(at + ".cascade_via_foreign_keys must be a boolean", "");
  }
}

// Add a local table to Citus metadata without distributing it, so it can take
// part in foreign keys with reference tables and be queried alongside them.
inline void parse_citus_add_local_table_to_metadata(Intent& in) {
  const auto at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(
      in.body, {"kind", "schema", "table", "cascade_via_foreign_keys", "comment"},
      at);
  detail::require_identifier(detail::require_string(in.body, "schema", at),
                             "schema", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "table", at),
                             "table", in.ordinal);
  if (in.body.contains("cascade_via_foreign_keys") &&
      !in.body["cascade_via_foreign_keys"].is_boolean()) {
    detail::fail(at + ".cascade_via_foreign_keys must be a boolean", "");
  }
}

// Remove the coordinator's local copy of a table's rows once the table has been
// distributed. create_distributed_table leaves them in place.
inline void parse_citus_truncate_local_data(Intent& in) {
  const auto at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(in.body, {"kind", "schema", "table", "comment"}, at);
  detail::require_identifier(detail::require_string(in.body, "schema", at),
                             "schema", in.ordinal);
  detail::require_identifier(detail::require_string(in.body, "table", at),
                             "table", in.ordinal);
}

// --- Phase 3: the cluster itself --------------------------------------------
//
// These name NODES, and a node is a fact of one cluster: worker1:5432 on
// staging is not a host on the developer's single PostgreSQL. A specification
// using them describes a topology, not a schema, and is bound to the clusters
// that topology exists on. citus_rebalance_shards names nothing and is the
// portable one.
//
// No kind moves or splits a shard by id. Shard ids come from a sequence in
// each database -- measured, the same table built by the same specifications
// starts at shard 102140 in one database and 102107 in another on the same
// cluster -- so a signed specification naming one would mean a different
// shard, or none, on every other target. Rebalancing and draining say what is wanted and let
// Citus pick the shards.

inline void citus_parse_node(const Intent& in, const std::string& at) {
  const auto host = detail::require_string(in.body, "host", at);
  if (host.empty() || host.find_first_of(" \t\n'\"") != std::string::npos) {
    detail::fail(at + ".host must be a host name or address", "");
  }
  if (in.body.contains("port") &&
      (!in.body["port"].is_number_integer() || in.body["port"].get<int>() <= 0 ||
       in.body["port"].get<int>() > 65535)) {
    detail::fail(at + ".port must be an integer between 1 and 65535", "");
  }
}

// block_writes, force_logical or auto -- Citus's own words, and the ones the
// man page explains. auto uses logical replication wherever it can, which
// needs wal_level = logical on the workers.
inline void citus_parse_transfer_mode(const Intent& in, const std::string& at) {
  if (!in.body.contains("transfer_mode")) return;
  const auto m = in.body.value("transfer_mode", "");
  if (m != "auto" && m != "force_logical" && m != "block_writes") {
    detail::fail(at + ".transfer_mode must be auto, force_logical or block_writes",
                 "auto and force_logical copy a shard while writes continue, "
                 "through logical replication; block_writes holds writes to "
                 "each shard while it is copied and needs no replication.");
  }
}

inline void parse_citus_add_node(Intent& in) {
  const auto at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(
      in.body, {"kind", "host", "port", "should_have_shards", "comment"}, at);
  citus_parse_node(in, at);
  if (in.body.contains("should_have_shards") &&
      !in.body["should_have_shards"].is_boolean()) {
    detail::fail(at + ".should_have_shards must be a boolean", "");
  }
}

inline void parse_citus_remove_node(Intent& in) {
  const auto at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(in.body, {"kind", "host", "port", "comment"}, at);
  citus_parse_node(in, at);
}

inline void parse_citus_set_coordinator_host(Intent& in) {
  const auto at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(in.body, {"kind", "host", "port", "comment"}, at);
  citus_parse_node(in, at);
}

inline void parse_citus_set_node_property(Intent& in) {
  const auto at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(
      in.body, {"kind", "host", "port", "should_have_shards", "comment"}, at);
  citus_parse_node(in, at);
  // The only property Citus has. Required, so the kind cannot be a no-op.
  if (!in.body.contains("should_have_shards") ||
      !in.body["should_have_shards"].is_boolean()) {
    detail::fail(at + ".should_have_shards is required, and must be a boolean",
                 "It is the only node property Citus lets you set.");
  }
}

inline void parse_citus_drain_node(Intent& in) {
  const auto at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(
      in.body, {"kind", "host", "port", "transfer_mode", "comment"}, at);
  citus_parse_node(in, at);
  citus_parse_transfer_mode(in, at);
}

inline void parse_citus_rebalance_shards(Intent& in) {
  const auto at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(in.body, {"kind", "transfer_mode", "comment"}, at);
  citus_parse_transfer_mode(in, at);
}
