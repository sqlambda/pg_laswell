// pgvector planner tests. No server: a reading shaped exactly as observe.h
// produces it, and a plan out. Every refusal quotes an error measured on
// PostgreSQL 18.6 with pgvector 0.8.6; cpp/test/extensions-tests.sh checks the
// same shapes against a real server.
namespace {

json pv_reading() {
  return json{{"version", "0.8.6"},
              {"opclasses", pglaswell::pgvector_known_opclasses()},
              {"methods", json{{"hnsw", {{"can_unique", false}, {"can_multi_col", false}}},
                               {"ivfflat", {{"can_unique", false}, {"can_multi_col", false}}}}}};
}

json pv_docs_table() {
  return json{{"exists", true},
              {"reltuples", 100000},
              {"size_estimate", 512LL * 1024 * 1024},
              {"columns", json{{"id", {{"type", "bigint"}}},
                               {"embedding", {{"type", "vector(1536)"}}},
                               {"wide", {{"type", "vector(3072)"}}},
                               {"half", {{"type", "halfvec(3072)"}}},
                               {"half_wide", {{"type", "halfvec(4001)"}}},
                               {"sparse", {{"type", "sparsevec(30000)"}}},
                               {"bits", {{"type", "bit(1024)"}}},
                               {"undeclared", {{"type", "vector"}}},
                               {"title", {{"type", "text"}}}}},
              {"indexes", json::object()}};
}

pglaswell::Observations pv_observations(bool installed = true) {
  pglaswell::Observations obs;
  obs.server_version = 180000;
  obs.server = json{{"max_connections", 100}, {"current_backends", 5}};
  obs.tables["public.docs"] = pv_docs_table();
  if (installed) obs.extensions["pgvector"] = pv_reading();
  return obs;
}

json pv_index(json columns, const char* method = "hnsw", json with = json()) {
  json in = {{"kind", "create_index"}, {"schema", "public"}, {"table", "docs"},
             {"name", "docs_vec_idx"}, {"method", method},
             {"columns", std::move(columns)}, {"comment", "Nearest neighbours."}};
  if (!with.is_null()) in["with"] = std::move(with);
  return in;
}

json pv_col(const char* name, const char* opclass) {
  return json{{"name", name}, {"opclass", opclass}};
}

pglaswell::Plan pv_plan(json intents, const pglaswell::Observations& obs) {
  if (!intents.is_array()) intents = json::array({std::move(intents)});
  const auto spec = pglaswell::parse_spec(json{{"laswell_spec_version", 1},
                                              {"id", "pgvector-test"},
                                              {"description", "a pgvector planner test"},
                                              {"intents", std::move(intents)}});
  return pglaswell::plan_migration(spec, obs, pglaswell::ExecutorConfig{});
}

std::string pv_refusal(const pglaswell::Plan& p, const std::string& needle) {
  for (const auto& c : p.conflicts) {
    if (c.find(needle) != std::string::npos) return c;
  }
  return "";
}

}  // namespace

TEST(PgvectorPlanner, AnHnswIndexWithinTheLimitsPlansWithItsStorageParameters) {
  const auto p = pv_plan(pv_index(json::array({pv_col("embedding", "vector_cosine_ops")}),
                                  "hnsw", json{{"m", 16}, {"ef_construction", 64}}),
                         pv_observations());
  ASSERT_TRUE(p.ok) << p.render();
  std::string sql;
  for (const auto& s : p.steps) for (const auto& q : s.sql) sql += q + "\n";
  EXPECT_NE(sql.find("USING hnsw (\"embedding\" \"vector_cosine_ops\") WITH "
                     "(\"ef_construction\" = 64, \"m\" = 16)"),
            std::string::npos)
      << sql;
}

TEST(PgvectorPlanner, MoreDimensionsThanTheMethodIndexesIsRefusedWithTheWayOut) {
  const auto p = pv_plan(pv_index(json::array({pv_col("wide", "vector_l2_ops")})),
                         pv_observations());
  EXPECT_FALSE(p.ok);
  const auto r = pv_refusal(p, "more than 2000 dimensions for hnsw index");
  ASSERT_FALSE(r.empty()) << p.render();
  EXPECT_NE(r.find("halfvec"), std::string::npos) << r;

  // halfvec takes up to 4000, so the same width as halfvec is fine ...
  EXPECT_TRUE(pv_plan(pv_index(json::array({pv_col("half", "halfvec_l2_ops")})),
                      pv_observations()).ok);
  // ... and one more than that is not.
  EXPECT_FALSE(pv_refusal(pv_plan(pv_index(json::array({pv_col("half_wide", "halfvec_l2_ops")})),
                                  pv_observations()),
                          "more than 4000 dimensions")
                   .empty());
}

TEST(PgvectorPlanner, AnHnswIndexMustNameItsOperatorClass) {
  // hnsw has no default class for any type: the class IS the distance.
  const auto p = pv_plan(pv_index(json::array({"embedding"})), pv_observations());
  const auto r = pv_refusal(p, "names no operator class");
  ASSERT_FALSE(r.empty()) << p.render();
  EXPECT_NE(r.find("vector_cosine_ops"), std::string::npos) << r;
}

TEST(PgvectorPlanner, IvfflatHasADefaultClassForVectorOnly) {
  EXPECT_TRUE(pv_plan(pv_index(json::array({"embedding"}), "ivfflat"), pv_observations()).ok);
  EXPECT_FALSE(pv_plan(pv_index(json::array({"half"}), "ivfflat"), pv_observations()).ok);
}

TEST(PgvectorPlanner, AnOperatorClassForAnotherTypeIsRefused) {
  const auto p = pv_plan(pv_index(json::array({pv_col("embedding", "halfvec_l2_ops")})),
                         pv_observations());
  EXPECT_FALSE(pv_refusal(p, "does not accept data type vector").empty()) << p.render();
  const auto q = pv_plan(pv_index(json::array({pv_col("embedding", "vector_euclid_ops")})),
                         pv_observations());
  EXPECT_FALSE(pv_refusal(q, "does not exist for access method hnsw").empty()) << q.render();
}

TEST(PgvectorPlanner, IvfflatCannotIndexSparsevecAndNeitherIndexesText) {
  EXPECT_FALSE(pv_refusal(pv_plan(pv_index(json::array({pv_col("sparse", "sparsevec_l2_ops")}),
                                           "ivfflat"),
                                  pv_observations()),
                          "which ivfflat cannot index")
                   .empty());
  EXPECT_FALSE(pv_refusal(pv_plan(pv_index(json::array({"title"})), pv_observations()),
                          "has no default operator class for access method")
                   .empty());
  // sparsevec on hnsw is fine: its limit is non-zero elements, not dimensions.
  EXPECT_TRUE(pv_plan(pv_index(json::array({pv_col("sparse", "sparsevec_l2_ops")})),
                      pv_observations()).ok);
}

TEST(PgvectorPlanner, AColumnWithoutDimensionsIsRefused) {
  const auto p = pv_plan(pv_index(json::array({pv_col("undeclared", "vector_l2_ops")})),
                         pv_observations());
  EXPECT_FALSE(pv_refusal(p, "column does not have dimensions").empty()) << p.render();
}

TEST(PgvectorPlanner, UniqueAndMulticolumnAreWhatTheMethodSaysTheyAre) {
  auto unique = pv_index(json::array({pv_col("embedding", "vector_l2_ops")}));
  unique["unique"] = true;
  EXPECT_FALSE(pv_refusal(pv_plan(unique, pv_observations()), "does not support unique indexes")
                   .empty());
  const auto multi = pv_index(json::array({pv_col("embedding", "vector_l2_ops"),
                                           pv_col("half", "halfvec_l2_ops")}));
  EXPECT_FALSE(pv_refusal(pv_plan(multi, pv_observations()), "does not support multicolumn")
                   .empty());
}

TEST(PgvectorPlanner, StorageParametersAreTheMethodsOwnAndInItsBounds) {
  const auto col = json::array({pv_col("embedding", "vector_l2_ops")});
  EXPECT_FALSE(pv_refusal(pv_plan(pv_index(col, "hnsw", json{{"fillfactor", 70}}),
                                  pv_observations()),
                          "unrecognized parameter \\\"fillfactor\\\"")
                   .empty());
  EXPECT_FALSE(pv_refusal(pv_plan(pv_index(col, "hnsw", json{{"m", 1}}), pv_observations()),
                          "between 2 and 100")
                   .empty());
  EXPECT_FALSE(pv_refusal(pv_plan(pv_index(json::array({"embedding"}), "ivfflat",
                                           json{{"lists", 40000}}),
                                  pv_observations()),
                          "between 1 and 32768")
                   .empty());
  // m = 40 alone is refused, because ef_construction keeps its default of 64.
  const auto r = pv_refusal(pv_plan(pv_index(col, "hnsw", json{{"m", 40}}), pv_observations()),
                            "ef_construction 64 (the default) is less than 2 * m (80)");
  EXPECT_FALSE(r.empty());
  EXPECT_TRUE(pv_plan(pv_index(col, "hnsw", json{{"m", 40}, {"ef_construction", 80}}),
                      pv_observations()).ok);
}

TEST(PgvectorPlanner, ATableCreatedInTheSameSpecificationIsChecked) {
  // The guard runs before projection, so it reads the types the earlier
  // intents declare -- and with no reading at all, because this spec creates
  // the extension too, it falls back to the classes pgvector 0.8.6 declares.
  auto obs = pv_observations(false);
  obs.tables.erase("public.docs");
  const auto p = pv_plan(
      json::array({json{{"kind", "create_extension"}, {"name", "vector"}},
                   json{{"kind", "create_table"}, {"schema", "public"}, {"table", "docs"},
                        {"comment", "Documents."},
                        {"columns", json::array({json{{"name", "embedding"},
                                                      {"type", "vector(3072)"},
                                                      {"nullable", false},
                                                      {"comment", "The embedding."}}})}},
                   pv_index(json::array({pv_col("embedding", "vector_l2_ops")}))}),
      obs);
  EXPECT_FALSE(pv_refusal(p, "more than 2000 dimensions").empty()) << p.render();
}

TEST(PgvectorPlanner, WithoutPgvectorTheModuleSaysNothing) {
  // Not installed, not created here: nothing to refuse for. What happens to an
  // hnsw index then is core's business (the access method does not exist).
  const auto p = pv_plan(pv_index(json::array({pv_col("wide", "vector_l2_ops")})),
                         pv_observations(false));
  EXPECT_TRUE(pv_refusal(p, "dimensions").empty()) << p.render();
  // And an ordinary btree on a vector database is untouched.
  EXPECT_TRUE(pv_plan(pv_index(json::array({"id"}), "btree"), pv_observations()).ok);
}
