// TimescaleDB planner tests. No server: readings shaped exactly as observe.h
// produces them (checked against 2.30.2 and 2.28.3), and a plan out.
// cpp/test/timescaledb-tests.sh runs the same shapes against both editions.
namespace {

json ts_hyper(long long size = 512LL << 20, long long rows = 4000000, bool columnstore = false,
              long long compressed = 0) {
  return json{{"num_chunks", 15},
              {"columnstore", columnstore},
              {"compressed_chunks", compressed},
              {"dimensions", json::array({json{{"column", "ts"},
                                               {"type", "timestamp with time zone"},
                                               {"interval", json::array({0, 1, 0})}}})},
              {"size_bytes", size},
              {"rows", rows},
              {"segmentby", json()},
              {"orderby", json()},
              {"orderby_desc", json()}};
}

json ts_reading(const char* license = "timescale", json hypertables = json::object()) {
  json functions = json::object();
  for (const char* f : {"by_range", "add_columnstore_policy", "remove_columnstore_policy",
                        "add_compression_policy", "remove_compression_policy",
                        "add_retention_policy", "remove_retention_policy",
                        "add_continuous_aggregate_policy", "remove_continuous_aggregate_policy"}) {
    functions[f] = true;
  }
  return json{{"version", "2.30.2"},
              {"license", license},
              {"functions", functions},
              {"hypertables", std::move(hypertables)},
              {"policies", json::object()},
              {"caggs", json::object()}};
}

// Core's reading of the table: a hypertable's parent reads empty (measured).
json ts_table(long long reltuples = 0, bool ts_not_null = true) {
  return json{{"exists", true},
              {"kind", "table"},
              {"reltuples", reltuples},
              {"size_estimate", 0},
              {"columns", json{{"id", {{"type", "bigint"}, {"not_null", true}}},
                               {"ts", {{"type", "timestamp with time zone"},
                                       {"not_null", ts_not_null}}},
                               {"dev", {{"type", "integer"}, {"not_null", true}}},
                               {"v", {{"type", "double precision"}, {"not_null", false}}}}},
              {"indexes", json::object()}};
}

pglaswell::Observations ts_observations(json reading, json table = ts_table(),
                                        const char* name = "public.m") {
  pglaswell::Observations obs;
  obs.server_version = 180000;
  obs.server = json{{"max_connections", 100}, {"current_backends", 5}};
  obs.tables[name] = std::move(table);
  if (!reading.is_null()) obs.extensions["timescaledb"] = std::move(reading);
  return obs;
}

pglaswell::Observations ts_hypertable_obs(json h = ts_hyper(), const char* license = "timescale") {
  return ts_observations(ts_reading(license, json{{"public.m", std::move(h)}}));
}

pglaswell::Plan ts_plan(json intents, const pglaswell::Observations& obs) {
  if (!intents.is_array()) intents = json::array({std::move(intents)});
  const auto spec = pglaswell::parse_spec(json{{"laswell_spec_version", 1},
                                              {"id", "timescaledb-test"},
                                              {"description", "a timescaledb planner test"},
                                              {"intents", std::move(intents)}});
  return pglaswell::plan_migration(spec, obs, pglaswell::ExecutorConfig{});
}

std::string ts_sql(const pglaswell::Plan& p) {
  std::string out;
  for (const auto& s : p.steps) for (const auto& q : s.sql) out += q + "\n";
  return out;
}

bool ts_conflict(const pglaswell::Plan& p, const std::string& needle) {
  for (const auto& c : p.conflicts) if (c.find(needle) != std::string::npos) return true;
  return false;
}

bool ts_warned(const pglaswell::Plan& p, const std::string& needle) {
  for (const auto& w : p.warnings) if (w.find(needle) != std::string::npos) return true;
  return false;
}

json ts_index(json columns, bool unique = false) {
  return json{{"kind", "create_index"}, {"schema", "public"}, {"table", "m"},
              {"name", "m_idx"}, {"columns", std::move(columns)}, {"unique", unique},
              {"comment", "An index."}};
}

}  // namespace

// --- core kinds on a hypertable: the hazard this module exists for ----------

// The parent of a hypertable holds no rows, and the kinds that state a size or
// a row count read it there: "deleting 0 estimated rows", "rewrites the whole
// table: 0 B". They take the module's figure, as create_index and backfill do.
TEST(TimescaledbPlanner, TheKindsThatStateASizeReadTheChunksNotTheEmptyParent) {
  const auto del = ts_plan(json{{"kind", "delete_rows"}, {"schema", "public"}, {"table", "m"},
                                {"key", "id"}, {"where", "v < 0"}},
                           ts_hypertable_obs());
  ASSERT_FALSE(del.steps.empty()) << del.render();
  EXPECT_EQ(del.steps.back().detail.value("rows_estimated", 0LL), 4000000)
      << del.render();
  const auto drop = ts_plan(json{{"kind", "drop_table"}, {"schema", "public"}, {"table", "m"}},
                            ts_hypertable_obs());
  bool said = false;
  for (const auto& w : drop.warnings) {
    if (w.find("512.0 MiB and roughly 4000000 rows") != std::string::npos) said = true;
  }
  EXPECT_TRUE(said) << drop.render();
}

TEST(TimescaledbPlanner, AnIndexOnALargeHypertableIsBuiltOneChunkAtATimeNeverConcurrently) {
  const auto p = ts_plan(ts_index(json::array({"dev"})), ts_hypertable_obs());
  ASSERT_TRUE(p.ok) << p.render();
  const auto sql = ts_sql(p);
  EXPECT_EQ(sql.find("CONCURRENTLY"), std::string::npos) << sql;
  EXPECT_NE(sql.find("WITH (timescaledb.transaction_per_chunk)"), std::string::npos) << sql;
  EXPECT_EQ(p.steps[0].txn_class, pglaswell::TxnClass::kForbidden)
      << "per-chunk builds cannot run inside a transaction block (measured)";
  EXPECT_EQ(p.steps[0].detail.value("index_build_by", ""), "timescaledb");
  // The size is the hypertable's, not the parent's 0.
  EXPECT_NE(p.steps[0].why.find("512.0 MiB"), std::string::npos) << p.steps[0].why;
  EXPECT_NE(p.steps[0].lock.find("each of its 15 chunks"), std::string::npos) << p.steps[0].lock;
}

TEST(TimescaledbPlanner, StorageParametersAndThePerChunkOptionShareOneWithClause) {
  auto in = ts_index(json::array({"dev"}));
  in["with"] = json{{"fillfactor", 80}};
  const auto sql = ts_sql(ts_plan(in, ts_hypertable_obs()));
  EXPECT_NE(sql.find("WITH (\"fillfactor\" = 80, timescaledb.transaction_per_chunk)"),
            std::string::npos)
      << sql;
}

TEST(TimescaledbPlanner, ASmallQuietHypertableGetsAPlainBuildLikeAnyTable) {
  const auto p = ts_plan(ts_index(json::array({"dev"})), ts_hypertable_obs(ts_hyper(1 << 20, 500)));
  ASSERT_TRUE(p.ok);
  EXPECT_EQ(p.steps[0].txn_class, pglaswell::TxnClass::kOptional);
  EXPECT_EQ(ts_sql(p).find("transaction_per_chunk"), std::string::npos);
}

TEST(TimescaledbPlanner, AUniqueIndexHasOnlyThePlainBuildAndSaysWhatItBlocks) {
  // transaction_per_chunk refuses UNIQUE, and CIC is refused outright: the
  // plain build is the only one there is.
  const auto p = ts_plan(ts_index(json::array({"id", "ts"}), true), ts_hypertable_obs());
  ASSERT_TRUE(p.ok) << p.render();
  const auto sql = ts_sql(p);
  EXPECT_EQ(sql.find("CONCURRENTLY"), std::string::npos) << sql;
  EXPECT_EQ(sql.find("transaction_per_chunk"), std::string::npos) << sql;
  EXPECT_NE(sql.find("CREATE UNIQUE INDEX \"m_idx\""), std::string::npos) << sql;
  EXPECT_TRUE(ts_warned(p, "can only be built plainly")) << p.render();
}

TEST(TimescaledbPlanner, AUniqueKeyWithoutThePartitioningColumnIsRefused) {
  EXPECT_TRUE(ts_conflict(ts_plan(ts_index(json::array({"id"}), true), ts_hypertable_obs()),
                          "cannot create a unique index without the column"));
  EXPECT_TRUE(ts_conflict(ts_plan(json{{"kind", "add_primary_key"}, {"schema", "public"},
                                       {"table", "m"}, {"name", "m_pk"},
                                       {"columns", json::array({"id"})}},
                                  ts_hypertable_obs()),
                          "does not contain ts"));
}

TEST(TimescaledbPlanner, AUniqueConstraintOnALargeHypertableIsOneStatementAndWarned) {
  const auto p = ts_plan(json{{"kind", "add_unique_constraint"}, {"schema", "public"},
                              {"table", "m"}, {"name", "m_uq"},
                              {"columns", json::array({"id", "ts"})}},
                         ts_hypertable_obs());
  ASSERT_TRUE(p.ok) << p.render();
  const auto sql = ts_sql(p);
  EXPECT_EQ(sql.find("CONCURRENTLY"), std::string::npos) << sql;
  EXPECT_NE(sql.find("ADD CONSTRAINT \"m_uq\" UNIQUE"), std::string::npos) << sql;
  EXPECT_TRUE(ts_warned(p, "allows no concurrent index build")) << p.render();
}

TEST(TimescaledbPlanner, ADropIsPlainEvenWithWaitersAndAnInvalidIndexIsRebuiltWithoutCIC) {
  auto table = ts_table();
  table["lock_waiters"] = 3;
  table["indexes"]["m_idx"] = json{{"is_valid", true}, {"is_unique", false}, {"method", "btree"},
                                   {"columns", json::array({"dev"})}, {"predicate", ""},
                                   {"definition", "x"}};
  auto obs = ts_observations(ts_reading("timescale", json{{"public.m", ts_hyper()}}), table);
  const auto d = ts_plan(json{{"kind", "drop_index"}, {"schema", "public"}, {"table", "m"},
                              {"name", "m_idx"}},
                         obs);
  ASSERT_TRUE(d.ok) << d.render();
  EXPECT_EQ(ts_sql(d).find("CONCURRENTLY"), std::string::npos) << ts_sql(d);
  EXPECT_TRUE(ts_warned(d, "dropped plainly although 3 session(s)"));

  obs.tables["public.m"]["indexes"]["m_idx"]["is_valid"] = false;
  const auto r = ts_plan(ts_index(json::array({"dev"})), obs);
  EXPECT_NE(ts_sql(r).find("DROP INDEX \"public\".\"m_idx\";"), std::string::npos) << ts_sql(r);
  EXPECT_EQ(ts_sql(r).find("CONCURRENTLY"), std::string::npos) << ts_sql(r);
}

TEST(TimescaledbPlanner, AnOrdinaryTableInATimescaleDatabaseIsPlannedExactlyAsBefore) {
  // THE RULE: for a table the module says nothing about, nothing changes.
  auto obs = ts_observations(ts_reading("timescale"), ts_table());
  obs.tables["public.m"]["size_estimate"] = 512LL << 20;
  const auto with = ts_plan(ts_index(json::array({"dev"})), obs);
  obs.extensions.erase("timescaledb");
  const auto without = ts_plan(ts_index(json::array({"dev"})), obs);
  EXPECT_EQ(with.digest(), without.digest());
  EXPECT_NE(ts_sql(with).find("CONCURRENTLY"), std::string::npos);
}

// VALIDATE CONSTRAINT is refused once the columnstore is enabled (measured), so
// core validates in the statement that adds the constraint: one step, no NOT
// VALID, and the lock it holds for the scan said in the step and in a warning.
TEST(TimescaledbPlanner, WithTheColumnstoreAConstraintIsValidatedByTheStatementThatAddsIt) {
  const auto cs = ts_hypertable_obs(ts_hyper(512LL << 20, 4000000, true, 7));
  struct Case { json intent; const char* sql; const char* lock; };
  const std::vector<Case> cases = {
      {json{{"kind", "set_not_null"}, {"schema", "public"}, {"table", "m"}, {"column", "v"}},
       "ALTER TABLE \"public\".\"m\" ALTER COLUMN \"v\" SET NOT NULL;",
       "AccessExclusiveLock on public.m and each of its 15 chunks, held for the whole scan"},
      {json{{"kind", "add_check_constraint"}, {"schema", "public"}, {"table", "m"},
            {"name", "m_v_ck"}, {"expression", "v >= 0"}},
       "ALTER TABLE \"public\".\"m\" ADD CONSTRAINT \"m_v_ck\" CHECK (v >= 0);",
       "AccessExclusiveLock on public.m and each of its 15 chunks, held for the whole scan"},
      {json{{"kind", "add_foreign_key"}, {"schema", "public"}, {"table", "m"},
            {"name", "m_dev_fk"}, {"columns", json::array({"dev"})},
            {"references_schema", "public"}, {"references_table", "d"},
            {"references_columns", json::array({"id"})}},
       "ALTER TABLE \"public\".\"m\" ADD CONSTRAINT \"m_dev_fk\" FOREIGN KEY (dev) "
       "REFERENCES public.d (id);",
       "ShareRowExclusiveLock on public.m and each of its 15 chunks, held for the whole scan; "
       "ShareRowExclusiveLock on public.d for as long"}};
  for (const auto& c : cases) {
    const auto p = ts_plan(c.intent, cs);
    ASSERT_TRUE(p.ok) << c.intent.dump();
    ASSERT_EQ(p.steps.size(), 1u) << ts_sql(p);
    EXPECT_EQ(p.steps[0].sql.at(0), c.sql);
    EXPECT_EQ(p.steps[0].lock, c.lock);
    EXPECT_TRUE(p.steps[0].own_transaction);
    EXPECT_EQ(p.steps[0].detail.value("validated_in_one_step_by", ""), "timescaledb");
    EXPECT_NE(p.steps[0].why.find("operation not supported on hypertables that have "
                                  "columnstore enabled"), std::string::npos);
    EXPECT_NE(p.steps[0].why.find("512"), std::string::npos) << p.steps[0].why;
    bool warned = false;
    for (const auto& w : p.warnings) {
      if (w.find("validated in one statement") != std::string::npos &&
          w.find("quiet window") != std::string::npos) warned = true;
    }
    EXPECT_TRUE(warned) << c.intent.dump();
  }
}

// The columnstore enabled by an earlier intent of the same specification
// counts: a NOT VALID add planned against the reading taken before it would
// commit and then fail to validate.
TEST(TimescaledbPlanner, AColumnstoreEnabledEarlierInTheSpecificationDecidesTheRecipeToo) {
  const json intents = json::array(
      {json{{"kind", "timescaledb_set_columnstore"}, {"schema", "public"}, {"table", "m"},
            {"segment_by", json::array({"dev"})}},
       json{{"kind", "add_check_constraint"}, {"schema", "public"}, {"table", "m"},
            {"name", "m_v_ck"}, {"expression", "v >= 0"}}});
  const auto sql = ts_sql(ts_plan(intents, ts_hypertable_obs()));
  EXPECT_NE(sql.find("CHECK (v >= 0);"), std::string::npos) << sql;
  EXPECT_EQ(sql.find("NOT VALID"), std::string::npos) << sql;
  EXPECT_EQ(sql.find("VALIDATE CONSTRAINT"), std::string::npos) << sql;
}

// THE RULE: a hypertable without the columnstore, and a plain table, keep the
// two-step recipe exactly as it was.
TEST(TimescaledbPlanner, WithoutTheColumnstoreTheTwoStepRecipeIsUnchanged) {
  const json in = {{"kind", "add_check_constraint"}, {"schema", "public"}, {"table", "m"},
                   {"name", "m_v_ck"}, {"expression", "v >= 0"}};
  const auto hyper = ts_plan(in, ts_hypertable_obs());
  auto obs = ts_hypertable_obs();
  obs.extensions.erase("timescaledb");
  const auto plain = ts_plan(in, obs);
  EXPECT_EQ(hyper.digest(), plain.digest());
  ASSERT_EQ(hyper.steps.size(), 2u);
  EXPECT_NE(hyper.steps[0].sql.back().find("NOT VALID;"), std::string::npos);
  EXPECT_NE(hyper.steps[1].sql.back().find("VALIDATE CONSTRAINT"), std::string::npos);
}

// A policy step records what it declared -- applied or already satisfied -- so
// the ledger can later be compared with TimescaleDB's jobs.
TEST(TimescaledbPlanner, APolicyStepRecordsWhatItDeclares) {
  const json in = {{"kind", "timescaledb_add_retention_policy"}, {"schema", "public"},
                   {"table", "m"}, {"drop_after", "90 days"}, {"acknowledge_data_loss", true}};
  const auto p = ts_plan(in, ts_hypertable_obs());
  ASSERT_TRUE(p.ok) << p.render();
  const auto pol = p.steps[0].detail.value("policy", json());
  EXPECT_EQ(pol.value("target", ""), "public.m");
  EXPECT_EQ(pol.value("which", ""), "retention");
  EXPECT_EQ(pol["declared"].value("drop_after", ""), "90 days");
  const json rm = {{"kind", "timescaledb_remove_retention_policy"}, {"schema", "public"},
                   {"table", "m"}};
  const auto q = ts_plan(rm, ts_hypertable_obs());
  ASSERT_TRUE(q.ok) << q.render();
  EXPECT_TRUE(q.steps[0].detail["policy"]["declared"].is_null());
}

TEST(TimescaledbPlanner, APolicyChangedByHandIsNoticedOnAnyPlanAgainstTheDatabase) {
  const auto applied = [](const char* kind, json declared) {
    return json{{"kind", kind}, {"action", "apply"},
                {"detail", {{"policy", {{"target", "public.m"}, {"which", "retention"},
                                        {"declared", std::move(declared)}}}}}};
  };
  const auto advised = [](const pglaswell::Plan& p, const std::string& needle) {
    for (const auto& a : p.advisories) if (a.find(needle) != std::string::npos) return true;
    return false;
  };
  // An unrelated specification, so the advisory is not about the plan in hand.
  const json unrelated = {{"kind", "create_schema"}, {"schema", "other"}, {"comment", "Unrelated."}};
  auto obs = ts_hypertable_obs();
  auto& r = obs.extensions["timescaledb"];
  // Newest first, as core reads them: last declared at 90 days.
  r["applied_steps"] = json::array(
      {applied("timescaledb_add_retention_policy", json{{"drop_after", "90 days"}}),
       applied("timescaledb_add_retention_policy", json{{"drop_after", "30 days"}})});

  // Removed by hand.
  EXPECT_TRUE(advised(ts_plan(unrelated, obs), "is no longer there: removed by hand"));
  // Re-added with another interval.
  r["policies"]["public.m"]["retention"] =
      json{{"job_id", 1002}, {"scheduled", true}, {"drop_after", json::array({0, 30, 0})}};
  EXPECT_TRUE(advised(ts_plan(unrelated, obs), "drop_after is no longer 90 days"));
  // Paused.
  r["policies"]["public.m"]["retention"] =
      json{{"job_id", 1002}, {"scheduled", false}, {"drop_after", json::array({0, 90, 0})}};
  EXPECT_TRUE(advised(ts_plan(unrelated, obs), "the job is paused"));
  // As declared -- and "90 days" is not "3 months", but it is 90 days however written.
  r["policies"]["public.m"]["retention"]["scheduled"] = true;
  EXPECT_TRUE(ts_plan(unrelated, obs).advisories.empty());
  // Removed by a specification, and there again.
  r["applied_steps"] = json::array({applied("timescaledb_remove_retention_policy", json())});
  EXPECT_TRUE(advised(ts_plan(unrelated, obs), "exists again: added by hand"));
  // A step from before policies were recorded says nothing.
  r["applied_steps"] = json::array({json{{"kind", "timescaledb_add_retention_policy"},
                                         {"action", "apply"}, {"detail", json::object()}}});
  r["policies"] = json::object();
  EXPECT_TRUE(ts_plan(unrelated, obs).advisories.empty());
}

TEST(TimescaledbPlanner, CompressedChunksBlockATypeChange) {
  const auto cs = ts_hypertable_obs(ts_hyper(512LL << 20, 4000000, true, 7));
  EXPECT_TRUE(ts_conflict(ts_plan(json{{"kind", "alter_column_type"}, {"schema", "public"},
                                       {"table", "m"}, {"column", "v"}, {"type", "numeric"}},
                                  cs),
                          "hypertables with compressed chunks"));
}

// --- the module's own kinds ---------------------------------------------------

TEST(TimescaledbPlanner, CreateHypertableIsSatisfiedRefusedOrApplied) {
  const json in = {{"kind", "timescaledb_create_hypertable"}, {"schema", "public"},
                   {"table", "m"}, {"time_column", "ts"}, {"chunk_time_interval", "1 day"}};
  // Already one, on ts.
  EXPECT_EQ(ts_plan(in, ts_hypertable_obs()).steps[0].action, pglaswell::Action::kSatisfied);
  // A plain empty table: applied.
  const auto p = ts_plan(in, ts_observations(ts_reading()));
  ASSERT_TRUE(p.ok) << p.render();
  EXPECT_EQ(ts_sql(p), "SELECT create_hypertable('\"public\".\"m\"'::regclass, "
                       "by_range('ts', INTERVAL '1 day'));\n");
  EXPECT_EQ(p.steps[0].detail.value("rehearse_by", ""), "execution");
  // With rows and no migrate_data: TimescaleDB's own refusal, quoted.
  EXPECT_TRUE(ts_conflict(ts_plan(in, ts_observations(ts_reading(), ts_table(50000))),
                          "is not empty"));
  auto migrate = in;
  migrate["migrate_data"] = true;
  const auto m = ts_plan(migrate, ts_observations(ts_reading(), ts_table(50000)));
  ASSERT_TRUE(m.ok);
  EXPECT_NE(ts_sql(m).find("migrate_data => true"), std::string::npos);
  EXPECT_TRUE(ts_warned(m, "unreadable and unwritable"));
}

TEST(TimescaledbPlanner, CreateHypertableRefusesWhatTimescaleRefuses) {
  const json in = {{"kind", "timescaledb_create_hypertable"}, {"schema", "public"},
                   {"table", "m"}, {"time_column", "ts"}};
  auto table = ts_table();
  table["indexes"]["m_pkey"] = json{{"is_unique", true}, {"is_valid", true},
                                    {"columns", json::array({"id"})}};
  EXPECT_TRUE(ts_conflict(ts_plan(in, ts_observations(ts_reading(), table)),
                          "does not contain ts"));
  auto on_int = in;
  on_int["time_column"] = "dev";
  EXPECT_TRUE(ts_conflict(ts_plan(on_int, ts_observations(ts_reading())), "partitions on time"));
  auto old = ts_reading();
  old["functions"]["by_range"] = false;
  EXPECT_TRUE(ts_conflict(ts_plan(in, ts_observations(old)), "has no by_range()"));
  // A nullable time column is made NOT NULL by TimescaleDB, silently: said.
  EXPECT_TRUE(ts_warned(ts_plan(in, ts_observations(ts_reading(), ts_table(0, false))),
                        "makes it NOT NULL"));
}

TEST(TimescaledbPlanner, IntervalsCompareAsPostgresKeepsThem) {
  const json in = {{"kind", "timescaledb_set_chunk_time_interval"}, {"schema", "public"},
                   {"table", "m"}, {"chunk_time_interval", "24 hours"}};
  // [0, 1, 0] is one day; 24 hours is [0, 0, 86400] -- different in PostgreSQL.
  EXPECT_EQ(ts_plan(in, ts_hypertable_obs()).steps[0].action, pglaswell::Action::kApply);
  auto day = in;
  day["chunk_time_interval"] = "1 day";
  EXPECT_EQ(ts_plan(day, ts_hypertable_obs()).steps[0].action, pglaswell::Action::kSatisfied);
  ASSERT_TRUE(pglaswell::ts_parse_interval("1 week"));
  EXPECT_EQ(*pglaswell::ts_parse_interval("1 week"), *pglaswell::ts_parse_interval("7 days"));
  EXPECT_FALSE(pglaswell::ts_parse_interval("7 days; DROP TABLE x"));
  EXPECT_FALSE(pglaswell::ts_parse_interval("forever"));
}

TEST(TimescaledbPlanner, TimescaleLicenseKindsAreRefusedOnTheApacheBuild) {
  const auto apache = ts_hypertable_obs(ts_hyper(), "apache");
  for (const json& in : {json{{"kind", "timescaledb_set_columnstore"}, {"schema", "public"},
                              {"table", "m"}, {"segment_by", json::array({"dev"})}},
                         json{{"kind", "timescaledb_add_retention_policy"}, {"schema", "public"},
                              {"table", "m"}, {"drop_after", "90 days"},
                              {"acknowledge_data_loss", true}},
                         json{{"kind", "timescaledb_create_continuous_aggregate"},
                              {"schema", "public"}, {"name", "h"}, {"query", "SELECT 1"}}}) {
    EXPECT_TRUE(ts_conflict(ts_plan(in, apache), "license")) << in.dump();
  }
  // Hypertables themselves are Apache.
  EXPECT_TRUE(ts_plan(json{{"kind", "timescaledb_set_chunk_time_interval"}, {"schema", "public"},
                           {"table", "m"}, {"chunk_time_interval", "2 days"}},
                      apache)
                  .ok);
}

TEST(TimescaledbPlanner, OneSpecificationBuildsAHypertableWithItsColumnstoreAndPolicy) {
  const auto p = ts_plan(
      json::array({json{{"kind", "timescaledb_create_hypertable"}, {"schema", "public"},
                        {"table", "m"}, {"time_column", "ts"}},
                   json{{"kind", "timescaledb_set_columnstore"}, {"schema", "public"},
                        {"table", "m"}, {"segment_by", json::array({"dev"})},
                        {"order_by", json::array({"ts DESC"})}},
                   json{{"kind", "timescaledb_add_columnstore_policy"}, {"schema", "public"},
                        {"table", "m"}, {"after", "7 days"}},
                   ts_index(json::array({"dev"}))}),
      ts_observations(ts_reading()));
  ASSERT_TRUE(p.ok) << p.render();
  const auto sql = ts_sql(p);
  EXPECT_NE(sql.find("timescaledb.segmentby = 'dev', timescaledb.orderby = 'ts DESC'"),
            std::string::npos) << sql;
  EXPECT_NE(sql.find("CALL add_columnstore_policy('\"public\".\"m\"'::regclass, after => INTERVAL '7 days');"),
            std::string::npos) << sql;
  // The index is planned against the hypertable the first intent leaves: no
  // CONCURRENTLY, decided by timescaledb.
  EXPECT_EQ(p.steps.back().detail.value("index_build_by", ""), "timescaledb");
  EXPECT_EQ(sql.find("CONCURRENTLY"), std::string::npos) << sql;
}

TEST(TimescaledbPlanner, APolicyNeedsTheColumnstoreAndAChangedOneIsReplaced) {
  const json in = {{"kind", "timescaledb_add_columnstore_policy"}, {"schema", "public"},
                   {"table", "m"}, {"after", "7 days"}};
  EXPECT_TRUE(ts_conflict(ts_plan(in, ts_hypertable_obs()), "columnstore not enabled"));
  auto r = ts_reading("timescale", json{{"public.m", ts_hyper(512LL << 20, 1, true)}});
  r["policies"]["public.m"]["columnstore"] = json{{"job_id", 1001}, {"after", json::array({0, 3, 0})}};
  const auto p = ts_plan(in, ts_observations(r));
  ASSERT_TRUE(p.ok) << p.render();
  ASSERT_EQ(p.steps[0].sql.size(), 2u);
  EXPECT_NE(p.steps[0].sql[0].find("remove_columnstore_policy"), std::string::npos);
  r["policies"]["public.m"]["columnstore"]["after"] = json::array({0, 7, 0});
  EXPECT_EQ(ts_plan(in, ts_observations(r)).steps[0].action, pglaswell::Action::kSatisfied);
}

TEST(TimescaledbPlanner, RetentionMustAcknowledgeTheDataLossItCauses) {
  EXPECT_THROW(ts_plan(json{{"kind", "timescaledb_add_retention_policy"}, {"schema", "public"},
                            {"table", "m"}, {"drop_after", "90 days"}},
                       ts_hypertable_obs()),
               std::exception);
  const auto p = ts_plan(json{{"kind", "timescaledb_add_retention_policy"}, {"schema", "public"},
                              {"table", "m"}, {"drop_after", "90 days"},
                              {"acknowledge_data_loss", true}},
                         ts_hypertable_obs());
  ASSERT_TRUE(p.ok);
  EXPECT_TRUE(ts_warned(p, "DATA LOSS by design"));
}

TEST(TimescaledbPlanner, AContinuousAggregateIsCreatedEmptyAndFilledOutsideATransaction) {
  const auto p = ts_plan(
      json::array({json{{"kind", "timescaledb_create_continuous_aggregate"}, {"schema", "public"},
                        {"name", "m_hourly"},
                        {"query", "SELECT time_bucket('1 hour', ts) AS b, avg(v) FROM public.m GROUP BY 1"},
                        {"refresh", true}, {"comment", "Hourly."}},
                   json{{"kind", "timescaledb_add_continuous_aggregate_policy"},
                        {"schema", "public"}, {"name", "m_hourly"}, {"start_offset", "3 days"},
                        {"end_offset", "1 hour"}, {"schedule_interval", "1 hour"}}}),
      ts_hypertable_obs());
  ASSERT_TRUE(p.ok) << p.render();
  ASSERT_EQ(p.steps.size(), 3u);
  EXPECT_NE(p.steps[0].sql[0].find("WITH (timescaledb.continuous) AS SELECT"), std::string::npos);
  EXPECT_NE(p.steps[0].sql[0].find("WITH NO DATA;"), std::string::npos);
  EXPECT_EQ(p.steps[1].txn_class, pglaswell::TxnClass::kForbidden)
      << "refresh_continuous_aggregate cannot run inside a transaction block (measured)";
  EXPECT_EQ(p.steps[2].action, pglaswell::Action::kApply)
      << "the policy plans against the aggregate the first intent creates";
}

TEST(TimescaledbPlanner, ATableCreatedInTheSameSpecificationIsMadeAHypertable) {
  // Found by the example: the projected column carries the type as written,
  // "timestamptz", not the catalog's "timestamp with time zone".
  // As core reads a table a specification creates: present, and absent.
  auto obs = ts_observations(ts_reading(), json{{"exists", false}});
  const auto p = ts_plan(
      json::array({json{{"kind", "create_table"}, {"schema", "public"}, {"table", "m"},
                        {"comment", "Readings."},
                        {"columns", json::array({json{{"name", "ts"}, {"type", "timestamptz(3)"},
                                                      {"nullable", false}, {"comment", "When."}}})}},
                   json{{"kind", "timescaledb_create_hypertable"}, {"schema", "public"},
                        {"table", "m"}, {"time_column", "ts"}}}),
      obs);
  ASSERT_TRUE(p.ok) << p.render();
  EXPECT_EQ(p.steps.back().action, pglaswell::Action::kApply);
  EXPECT_TRUE(pglaswell::ts_is_time_type("Timestamp  With Time Zone"));
  EXPECT_FALSE(pglaswell::ts_is_time_type("timestamptzz"));
  EXPECT_FALSE(pglaswell::ts_is_time_type("bigint"));
}

// A hypertable's index, built one chunk at a time CONCURRENTLY. The per-chunk
// option holds a write-blocking lock on chunk after chunk, and a write that
// does not name the time column waits behind all of them -- found in the field,
// updates by id stopped for the whole build. Measured on 2.30.2: the index ON
// ONLY the parent, then CREATE INDEX CONCURRENTLY on each chunk, which
// TimescaleDB counts as the hypertable index's (worst update 43 ms against
// 3 491 ms). Offered only where that was measured.
TEST(TimescaledbPlanner, AnIndexIsBuiltOnTheParentOnlyThenOnEachChunkConcurrently) {
  const auto with_chunks = [](json h, json indexes_on_first = json::object()) {
    h["chunks"] = json::array(
        {json{{"relation", "_timescaledb_internal._hyper_1_1_chunk"},
              {"indexes", std::move(indexes_on_first)}},
         json{{"relation", "_timescaledb_internal._hyper_1_2_chunk"}, {"indexes", json::object()}},
         json{{"relation", "_timescaledb_internal._hyper_1_3_chunk"}, {"indexes", json::object()}}});
    return h;
  };
  const json index = ts_index(json::array({"dev"}));
  const auto name = index.value("name", "");

  auto p = ts_plan(index, ts_hypertable_obs(with_chunks(ts_hyper())));
  ASSERT_TRUE(p.ok) << p.render();
  std::vector<const pglaswell::Step*> builds;
  for (const auto& s : p.steps) {
    if (s.kind == "create_index" && s.txn_class == pglaswell::TxnClass::kForbidden) builds.push_back(&s);
  }
  ASSERT_EQ(builds.size(), 3u) << p.render();
  // The parent first, in a transaction, behind the weaker lock.
  EXPECT_EQ(p.steps[0].sql.at(0), "LOCK TABLE \"public\".\"m\" IN SHARE UPDATE EXCLUSIVE MODE;");
  EXPECT_NE(p.steps[0].sql.at(1).find("CREATE INDEX \"" + name + "\" ON ONLY \"public\".\"m\" USING btree (\"dev\")"),
            std::string::npos) << p.steps[0].sql.at(1);
  EXPECT_EQ(ts_sql(p).find("transaction_per_chunk"), std::string::npos) << ts_sql(p);
  // Each chunk by its own name, named as TimescaleDB names its own.
  EXPECT_EQ(builds[0]->sql.at(0),
            "CREATE INDEX CONCURRENTLY \"_hyper_1_1_chunk_" + name +
                "\" ON \"_timescaledb_internal\".\"_hyper_1_1_chunk\" USING btree (\"dev\");");
  EXPECT_EQ(builds[0]->detail.value("schema", ""), "_timescaledb_internal");
  // And each is checked: a concurrent build can return and leave it invalid.
  int checks = 0;
  for (const auto& s : p.steps) if (s.kind == "verify_index_valid") ++checks;
  EXPECT_EQ(checks, 3) << p.render();

  // RESUMED. The parent's index is there; chunk 1 has its own, valid; chunk 2's
  // concurrent build failed and left it invalid.
  auto h = with_chunks(ts_hyper(), json{{"_hyper_1_1_chunk_" + name, true}});
  h["chunks"][1]["indexes"] = json{{"_hyper_1_2_chunk_" + name, false}};
  auto obs = ts_hypertable_obs(h);
  obs.tables["public.m"]["indexes"][name] =
      json{{"is_valid", true}, {"is_unique", false}, {"leading_column", "dev"},
           {"columns", json::array({"dev"})}, {"predicate", ""}};
  p = ts_plan(index, obs);
  ASSERT_TRUE(p.ok) << p.render();
  const auto sql = ts_sql(p);
  EXPECT_EQ(sql.find("ON ONLY"), std::string::npos) << "the parent is not created twice: " << sql;
  EXPECT_EQ(sql.find("_hyper_1_1_chunk"), std::string::npos) << "a finished chunk is left alone: " << sql;
  EXPECT_NE(sql.find("DROP INDEX CONCURRENTLY \"_timescaledb_internal\".\"_hyper_1_2_chunk_" + name + "\";"),
            std::string::npos) << sql;
  EXPECT_NE(sql.find("CREATE INDEX CONCURRENTLY \"_hyper_1_3_chunk_" + name + "\""), std::string::npos);
  // Every chunk done: satisfied.
  for (auto& c : h["chunks"]) {
    const auto rel = c.value("relation", "");
    c["indexes"] = json{{rel.substr(rel.find('.') + 1) + "_" + name, true}};
  }
  obs = ts_hypertable_obs(h);
  obs.tables["public.m"]["indexes"][name] =
      json{{"is_valid", true}, {"is_unique", false}, {"leading_column", "dev"},
           {"columns", json::array({"dev"})}, {"predicate", ""}};
  p = ts_plan(index, obs);
  ASSERT_EQ(p.steps.size(), 1u) << p.render();
  EXPECT_EQ(p.steps[0].action, pglaswell::Action::kSatisfied);

  // NOT offered where it was not measured: the columnstore, and 2.28. There
  // the per-chunk option stands.
  auto columnstore = ts_hypertable_obs(with_chunks(ts_hyper(512LL << 20, 4000000, true, 2)));
  EXPECT_NE(ts_sql(ts_plan(index, columnstore)).find("transaction_per_chunk"), std::string::npos);
  auto old = ts_hypertable_obs(with_chunks(ts_hyper()));
  old.extensions["timescaledb"]["version"] = "2.28.3";
  EXPECT_NE(ts_sql(ts_plan(index, old)).find("transaction_per_chunk"), std::string::npos);
  // A UNIQUE index keeps the plain build: per-part uniqueness was not measured.
  const auto uniq = ts_plan(ts_index(json::array({"id", "ts"}), true),
                            ts_hypertable_obs(with_chunks(ts_hyper())));
  EXPECT_EQ(ts_sql(uniq).find("ON ONLY"), std::string::npos) << ts_sql(uniq);
}
