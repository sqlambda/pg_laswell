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

// --- 2026-10-01: expression keys, and the advisories --------------------------

TEST(PgvectorPlanner, AnExpressionCastIsCheckedAsTheTypeItCastsTo) {
  // The documented way to index a vector wider than 2000: as halfvec.
  const auto ok = pv_plan(pv_index(json::array({json{{"expression", "wide::halfvec(3072)"},
                                                     {"opclass", "halfvec_cosine_ops"}}})),
                          pv_observations());
  EXPECT_TRUE(ok.ok) << ok.render();
  // The same cast with a vector class is the type mismatch pgvector raises.
  const auto bad = pv_plan(pv_index(json::array({json{{"expression", "wide::halfvec(3072)"},
                                                      {"opclass", "vector_cosine_ops"}}})),
                           pv_observations());
  EXPECT_FALSE(pv_refusal(bad, "does not accept data type halfvec").empty()) << bad.render();
  // CAST(... AS ...), and a function over the column, read the same way.
  EXPECT_FALSE(pv_refusal(pv_plan(pv_index(json::array({json{
                                      {"expression", "CAST(embedding AS halfvec(4001))"},
                                      {"opclass", "halfvec_l2_ops"}}})),
                                  pv_observations()),
                          "more than 4000 dimensions")
                   .empty());
  EXPECT_TRUE(pv_plan(pv_index(json::array({json{
                          {"expression", "binary_quantize(embedding)::bit(1536)"},
                          {"opclass", "bit_hamming_ops"}}})),
                      pv_observations())
                  .ok);
  // Anything that is not an outermost cast is the server's to type.
  EXPECT_TRUE(pv_plan(pv_index(json::array({json{{"expression", "l2_normalize(embedding)"},
                                                 {"opclass", "vector_l2_ops"}}})),
                      pv_observations())
                  .ok);
}

namespace {
bool pv_advised(const pglaswell::Plan& p, const std::string& needle) {
  for (const auto& a : p.advisories) {
    if (a.find(needle) != std::string::npos) return true;
  }
  return false;
}
}  // namespace

TEST(PgvectorPlanner, AnIvfflatIndexOnAnEmptyTableIsAnAdvisoryNotARefusal) {
  auto obs = pv_observations();
  obs.tables["public.docs"]["reltuples"] = 0;
  const auto p = pv_plan(pv_index(json::array({"embedding"}), "ivfflat"), obs);
  EXPECT_TRUE(p.ok);
  EXPECT_TRUE(pv_advised(p, "reads as empty")) << p.render();
  // Below one row per list: pgvector's own notice, quoted.
  obs.tables["public.docs"]["reltuples"] = 40;
  EXPECT_TRUE(pv_advised(pv_plan(pv_index(json::array({"embedding"}), "ivfflat"), obs),
                         "about 40 rows for 100 lists"));
}

TEST(PgvectorPlanner, AnHnswGraphLargerThanMaintenanceWorkMemIsSizedForTheReader) {
  // 100 000 rows of vector(1536), m 16: (8 + 6144 + 210 + 512) bytes a row,
  // about 656 MB, against the server's 64 MB.
  auto obs = pv_observations();
  obs.server["maintenance_work_mem_kb"] = 65536;
  const auto p = pv_plan(pv_index(json::array({pv_col("embedding", "vector_cosine_ops")})), obs);
  ASSERT_TRUE(p.ok) << p.render();
  EXPECT_TRUE(pv_advised(p, "graph needs about 656 MB")) << p.render();
  EXPECT_TRUE(pv_advised(p, "maintenance_work_mem 64 MB"));
}

TEST(PgvectorPlanner, AdvisoriesAreShownAndNotHashed) {
  // The row count moves with autovacuum; a plan whose only difference is an
  // advisory must keep its digest, or planMigration and startMigration a
  // minute apart would disagree about a migration that has not changed.
  auto obs = pv_observations();
  obs.server["maintenance_work_mem_kb"] = 65536;
  const auto a = pv_plan(pv_index(json::array({pv_col("embedding", "vector_cosine_ops")})), obs);
  obs.tables["public.docs"]["reltuples"] = 10;
  const auto b = pv_plan(pv_index(json::array({pv_col("embedding", "vector_cosine_ops")})), obs);
  ASSERT_FALSE(a.advisories.empty());
  ASSERT_TRUE(b.advisories.empty()) << b.render();
  EXPECT_EQ(a.digest(), b.digest());
  EXPECT_NE(a.render().find(" note: "), std::string::npos);
  EXPECT_TRUE(a.to_json().contains("advisories"));
  EXPECT_FALSE(b.to_json().contains("advisories"))
      << "a plan without advisories serialises as it always did";
}

TEST(PgvectorPlanner, AParallelBuildBiggerThanAContainersShmIsMentioned) {
  auto obs = pv_observations();
  obs.tables["public.docs"]["reltuples"] = 10;
  obs.server["maintenance_work_mem_kb"] = 256 * 1024;
  obs.server["max_parallel_maintenance_workers"] = 2;
  obs.extensions["pgvector"]["dynamic_shared_memory_type"] = "posix";
  const auto col = json::array({pv_col("embedding", "vector_cosine_ops")});
  EXPECT_TRUE(pv_advised(pv_plan(pv_index(col), obs), "/dev/shm"));
  obs.server["max_parallel_maintenance_workers"] = 0;
  EXPECT_FALSE(pv_advised(pv_plan(pv_index(col), obs), "/dev/shm"));
}

// --- 2026-10-02: the HNSW build's memory, raised within a deduced limit -------

namespace {
pglaswell::Plan pv_plan_cfg(json intents, const pglaswell::Observations& obs,
                            const pglaswell::ExecutorConfig& cfg) {
  if (!intents.is_array()) intents = json::array({std::move(intents)});
  const auto spec = pglaswell::parse_spec(json{{"laswell_spec_version", 1},
                                              {"id", "pgvector-test"},
                                              {"description", "a pgvector planner test"},
                                              {"intents", std::move(intents)}});
  return pglaswell::plan_migration(spec, obs, cfg);
}
}  // namespace

// Two modules, one table. The first to answer used to win: on a hypertable
// with a vector column, either pgvector's memory request hid TimescaleDB's "no
// concurrent build" (and CREATE INDEX CONCURRENTLY was planned, which a
// hypertable refuses, in the job), or TimescaleDB's answer hid the memory
// request. The answers are merged: each says what it knows.
TEST(PgvectorPlanner, OnAHypertableTheBuildIsTimescaledbsAndTheMemoryIsPgvectors) {
  if (std::string(PGLASWELL_MODULE_SET).find("timescaledb") == std::string::npos) {
    GTEST_SKIP() << "built without the timescaledb module";
  }
  auto obs = pv_observations();
  obs.server["maintenance_work_mem_kb"] = 65536;
  obs.server["shared_buffers_bytes"] = 4LL << 30;
  // The parent of a hypertable holds no rows; its chunks hold 100 000.
  obs.tables["public.docs"]["reltuples"] = 0;
  obs.tables["public.docs"]["stats"] = json{{"n_live_tup", 0}};
  obs.tables["public.docs"]["size_estimate"] = 0;
  obs.extensions["timescaledb"] =
      json{{"version", "2.28.3"},
           {"license", "timescale"},
           {"hypertables",
            json{{"public.docs",
                  json{{"dimensions", json::array({json{{"column", "ts"}, {"kind", "time"}}})},
                       {"num_chunks", 12}, {"compressed_chunks", 0}, {"columnstore", false},
                       {"size_bytes", 2LL << 30}, {"rows", 100000}}}}}};
  pglaswell::ExecutorConfig cfg;
  cfg.max_concurrent_jobs = 2;
  const auto p = pv_plan_cfg(pv_index(json::array({pv_col("embedding", "vector_cosine_ops")})),
                             obs, cfg);
  ASSERT_TRUE(p.ok) << p.render();
  const auto& s = p.steps[0];
  const auto sql = pglaswell::detail::join(s.sql, " ");
  EXPECT_EQ(sql.find("CONCURRENTLY"), std::string::npos)
      << "a hypertable refuses a concurrent build: " << sql;
  EXPECT_EQ(s.detail.value("index_build_by", ""), "timescaledb") << s.detail.dump();
  // 100 000 rows are TimescaleDB's count; what a row costs is pgvector's.
  EXPECT_EQ(s.detail.value("maintenance_work_mem_mb", 0), 656) << s.detail.dump();
  EXPECT_EQ(s.detail.value("memory_wanted_by", ""), "pgvector");
  for (const auto& adv : p.advisories) {
    EXPECT_EQ(adv.find("reads as empty"), std::string::npos) << adv;
  }
}

TEST(PgvectorPlanner, AnHnswBuildIsGivenTheMemoryItNeedsUpToAShareOfSharedBuffers) {
  // 100 000 rows of vector(1536), m 16: about 656 MB of graph. shared_buffers
  // 4 GB, two concurrent jobs: the limit is 2048 MB, so it gets all 656.
  auto obs = pv_observations();
  obs.server["maintenance_work_mem_kb"] = 65536;
  obs.server["shared_buffers_bytes"] = 4LL << 30;
  pglaswell::ExecutorConfig cfg;
  cfg.max_concurrent_jobs = 2;
  const auto p = pv_plan_cfg(pv_index(json::array({pv_col("embedding", "vector_cosine_ops")})),
                             obs, cfg);
  ASSERT_TRUE(p.ok) << p.render();
  const auto& s = p.steps[0];
  EXPECT_EQ(s.detail.value("maintenance_work_mem_mb", 0), 656) << s.detail.dump();
  EXPECT_EQ(s.detail.value("memory_wanted_by", ""), "pgvector");
  EXPECT_FALSE(s.detail.contains("index_build_by"))
      << "pgvector asked for memory; it did not change how the index is built";
  EXPECT_NE(s.why.find("maintenance_work_mem raised to 656MB"), std::string::npos) << s.why;
  for (const auto& a : p.advisories) {
    EXPECT_EQ(a.find("graph needs"), std::string::npos) << "it got what it needs: " << a;
  }
}

TEST(PgvectorPlanner, AboveTheLimitItIsRaisedAsFarAsAllowedAndTheRestIsSaid) {
  // shared_buffers 512 MB over two jobs: 256 MB, short of 656.
  auto obs = pv_observations();
  obs.server["maintenance_work_mem_kb"] = 65536;
  obs.server["shared_buffers_bytes"] = 512LL << 20;
  pglaswell::ExecutorConfig cfg;
  cfg.max_concurrent_jobs = 2;
  const auto p = pv_plan_cfg(pv_index(json::array({pv_col("embedding", "vector_cosine_ops")})),
                             obs, cfg);
  ASSERT_TRUE(p.ok);
  EXPECT_EQ(p.steps[0].detail.value("maintenance_work_mem_mb", 0), 256);
  EXPECT_NE(p.steps[0].why.find("limited to shared_buffers 512MB divided by max_concurrent_jobs (2)"),
            std::string::npos) << p.steps[0].why;
  bool said = false;
  for (const auto& a : p.advisories) {
    if (a.find("already raised from the server's 64 MB") != std::string::npos) said = true;
  }
  EXPECT_TRUE(said) << p.render();
}

TEST(PgvectorPlanner, AConfiguredCeilingWinsAndAnUntunedServerIsNotRaised) {
  auto obs = pv_observations();
  obs.server["maintenance_work_mem_kb"] = 65536;
  obs.server["shared_buffers_bytes"] = 4LL << 30;
  pglaswell::ExecutorConfig cfg;
  cfg.max_concurrent_jobs = 2;
  cfg.maintenance_work_mem_mb = 200;  // 100 per step, the operator's decision
  const auto c = pv_plan_cfg(pv_index(json::array({pv_col("embedding", "vector_cosine_ops")})),
                             obs, cfg);
  EXPECT_EQ(c.steps[0].detail.value("maintenance_work_mem_mb", 0), 100) << c.steps[0].detail.dump();
  EXPECT_FALSE(c.steps[0].detail.contains("memory_wanted_by"));

  // shared_buffers left at 128 MB: 64 MB a job, no more than the server's own.
  obs.server["shared_buffers_bytes"] = 128LL << 20;
  cfg.maintenance_work_mem_mb = 0;
  const auto u = pv_plan_cfg(pv_index(json::array({pv_col("embedding", "vector_cosine_ops")})),
                             obs, cfg);
  EXPECT_FALSE(u.steps[0].detail.contains("maintenance_work_mem_mb")) << u.steps[0].detail.dump();
}

TEST(PgvectorPlanner, OnlyAnHnswBuildAsksForMemory) {
  auto obs = pv_observations();
  obs.server["maintenance_work_mem_kb"] = 65536;
  obs.server["shared_buffers_bytes"] = 4LL << 30;
  const auto ivf = pv_plan(pv_index(json::array({"embedding"}), "ivfflat"), obs);
  EXPECT_FALSE(ivf.steps[0].detail.contains("maintenance_work_mem_mb"));
  const auto bt = pv_plan(pv_index(json::array({"id"}), "btree"), obs);
  EXPECT_FALSE(bt.steps[0].detail.contains("maintenance_work_mem_mb"));
}
