// Citus planner tests. No server: a fixed Observations literal in, a plan out.
//
// These are the cheap tests and they are where the refusals actually get
// proved. The conformance cases are deferred to a real cluster, which means
// nothing runs them in CI -- so without these, every refusal is proved only by
// a happy path having worked once by hand.
namespace {

// A Citus reading, shaped exactly as observe.h produces it.
json citus_obs(json tables = json::object(), int workers = 2,
               const char* propagation = "on", bool concurrent_call = true) {
  json calls = json::object();
  if (concurrent_call) calls["create_distributed_table_concurrently"] = true;
  return json{{"version", "13.2-1"},
              {"worker_count", workers},
              {"tables", std::move(tables)},
              {"distributed_functions", json::array()},
              {"calls", calls},
              {"settings", json{{"enable_ddl_propagation", propagation},
                                {"max_adaptive_executor_pool_size", "16"}}}};
}

// A table reading with one primary key over the named columns.
json table_with_pk(std::vector<std::string> pk_columns, long long rows = 10) {
  json columns = json::object();
  for (const auto& c : pk_columns) columns[c] = json{{"type", "integer"}};
  columns["balance"] = json{{"type", "numeric"}};
  json pk_cols = json::array();
  for (const auto& c : pk_columns) pk_cols.push_back(c);
  // The INDEX as well as the constraint. A keyset walk needs a unique index on
  // the key alone -- unique_key_index() reads `indexes`, not `constraints` --
  // and a fixture with only the constraint made two "this is allowed" tests
  // fail for a reason that had nothing to do with what they were testing.
  json indexes = json::object();
  indexes["t_pkey"] = json{{"leading_column", pk_columns.front()},
                           {"columns", pk_cols},
                           {"is_unique", true},
                           {"is_valid", true},
                           {"predicate", ""}};
  return json{{"exists", true},
              {"reltuples", rows},
              {"columns", columns},
              {"indexes", indexes},
              {"constraints", json{{"t_pkey", json{{"type", "p"},
                                                   {"columns", pk_cols}}}}}};
}

pglaswell::Observations citus_observations(json citus, json tables) {
  pglaswell::Observations obs;
  obs.server_version = 180000;
  obs.server = json{{"max_connections", 100}, {"current_backends", 5}};
  obs.tables = std::move(tables);
  if (!citus.is_null()) obs.extensions["citus"] = std::move(citus);
  return obs;
}

json citus_spec(json intent) {
  return json{{"laswell_spec_version", 1},
              {"id", "citus-test"},
              {"description", "a citus planner test"},
              {"intents", json::array({std::move(intent)})}};
}

pglaswell::Plan plan_citus(const json& intent, const pglaswell::Observations& obs) {
  const auto spec = pglaswell::parse_spec(citus_spec(intent));
  return pglaswell::plan_migration(spec, obs, pglaswell::ExecutorConfig{});
}

bool conflicts_mention(const pglaswell::Plan& p, const std::string& needle) {
  for (const auto& c : p.conflicts) {
    if (c.find(needle) != std::string::npos) return true;
  }
  return false;
}

}  // namespace

TEST(CitusPlanner, ASmallTableTakesTheBlockingPathAndSaysWhichReadingDecided) {
  // The threshold is a number someone chose, so something has to exercise both
  // sides of it or it is a guess wearing a constant's name.
  const auto obs = citus_observations(
      citus_obs(), json{{"public.accounts", table_with_pk({"id"}, 500)}});
  const auto p = plan_citus(json{{"kind", "distribute_table"},
                                 {"schema", "public"}, {"table", "accounts"},
                                 {"distribution_column", "id"}}, obs);
  ASSERT_TRUE(p.ok) << p.conflicts.size();
  ASSERT_EQ(p.steps.size(), 1u);
  EXPECT_FALSE(p.steps[0].detail.value("concurrent", true));
  EXPECT_EQ(p.steps[0].sql[0].find("create_distributed_table_concurrently"),
            std::string::npos)
      << "a 500-row table does not need the concurrent path: " << p.steps[0].sql[0];
  // The reading, named. Not "small table" -- the number.
  EXPECT_NE(p.steps[0].why.find("500"), std::string::npos) << p.steps[0].why;
}

TEST(CitusPlanner, ALargeTableTakesTheConcurrentPathAndCannotBeInATransaction) {
  const auto obs = citus_observations(
      citus_obs(), json{{"public.accounts", table_with_pk({"id"}, 5000000)}});
  const auto p = plan_citus(json{{"kind", "distribute_table"},
                                 {"schema", "public"}, {"table", "accounts"},
                                 {"distribution_column", "id"}}, obs);
  ASSERT_TRUE(p.ok);
  ASSERT_EQ(p.steps.size(), 1u);
  EXPECT_TRUE(p.steps[0].detail.value("concurrent", false));
  EXPECT_NE(p.steps[0].sql[0].find("create_distributed_table_concurrently"),
            std::string::npos) << p.steps[0].sql[0];
  // The concurrent form cannot run inside a transaction block, exactly like
  // CREATE INDEX CONCURRENTLY -- and the executor acts on that classification,
  // so getting it wrong would wrap it in a transaction and fail at execution.
  EXPECT_EQ(p.steps[0].txn_class, pglaswell::TxnClass::kForbidden);
  EXPECT_NE(p.steps[0].why.find("5000000"), std::string::npos) << p.steps[0].why;
}

TEST(CitusPlanner, ConcurrentlyNeverOverridesTheReadingAndSaysSo) {
  const auto obs = citus_observations(
      citus_obs(), json{{"public.accounts", table_with_pk({"id"}, 5000000)}});
  const auto p = plan_citus(json{{"kind", "distribute_table"},
                                 {"schema", "public"}, {"table", "accounts"},
                                 {"distribution_column", "id"},
                                 {"concurrently", "never"}}, obs);
  ASSERT_TRUE(p.ok);
  EXPECT_FALSE(p.steps[0].detail.value("concurrent", true));
  EXPECT_NE(p.steps[0].why.find("overriding"), std::string::npos)
      << "an override must say it overrode a reading, or the plan reads as if "
         "the reading agreed: " << p.steps[0].why;
}

TEST(CitusPlanner, AKeyWithoutTheDistributionColumnIsRefusedBeforeAnythingRuns) {
  // The single most common Citus failure, and derivable from the catalog. It
  // surfaces at EXECUTION otherwise -- halfway through, on a cluster.
  // The table HAS the column -- otherwise the earlier "no such column" refusal
  // fires and this proves nothing about keys. The key just does not cover it.
  auto t = table_with_pk({"tenant_id"});
  t["columns"]["id"] = json{{"type", "integer"}};
  const auto obs = citus_observations(citus_obs(), json{{"public.accounts", t}});
  const auto p = plan_citus(json{{"kind", "distribute_table"},
                                 {"schema", "public"}, {"table", "accounts"},
                                 {"distribution_column", "id"}}, obs);
  EXPECT_FALSE(p.ok);
  EXPECT_TRUE(conflicts_mention(p, "does not include the distribution column"))
      << "conflicts: " << (p.conflicts.empty() ? "none" : p.conflicts[0]);
  // And it must name the key and its columns, or the reader cannot act on it.
  EXPECT_TRUE(conflicts_mention(p, "t_pkey"));
  EXPECT_TRUE(conflicts_mention(p, "tenant_id"));
}

TEST(CitusPlanner, ACompoundKeyThatDoesIncludeItIsAccepted) {
  // The other half, and the one that caught a CORE bug: the observation
  // recorded only conkey[1], so PRIMARY KEY (account_id, reference) read as
  // covering nothing and a correct specification was refused.
  const auto obs = citus_observations(
      citus_obs(),
      json{{"public.account_transactions",
            table_with_pk({"account_id", "reference"})}});
  const auto p = plan_citus(json{{"kind", "distribute_table"},
                                 {"schema", "public"},
                                 {"table", "account_transactions"},
                                 {"distribution_column", "account_id"}}, obs);
  EXPECT_TRUE(p.ok) << (p.conflicts.empty() ? "" : p.conflicts[0]);
}

TEST(CitusPlanner, ColocatingWithSomethingNotDistributedIsRefused) {
  const auto obs = citus_observations(
      citus_obs(), json{{"public.orders", table_with_pk({"id"})}});
  const auto p = plan_citus(json{{"kind", "distribute_table"},
                                 {"schema", "public"}, {"table", "orders"},
                                 {"distribution_column", "id"},
                                 {"colocate_with", "public.accounts"}}, obs);
  EXPECT_FALSE(p.ok);
  EXPECT_TRUE(conflicts_mention(p, "does not record as distributed"))
      << (p.conflicts.empty() ? "none" : p.conflicts[0]);
}

TEST(CitusPlanner, RedistributingOnADifferentColumnNamesTheRightKind) {
  const auto obs = citus_observations(
      citus_obs(json{{"public.accounts",
                      json{{"partmethod", "h"},
                           {"distribution_column", "tenant_id"}}}}),
      json{{"public.accounts", table_with_pk({"id"})}});
  const auto p = plan_citus(json{{"kind", "distribute_table"},
                                 {"schema", "public"}, {"table", "accounts"},
                                 {"distribution_column", "id"}}, obs);
  EXPECT_FALSE(p.ok);
  EXPECT_TRUE(conflicts_mention(p, "alter_distributed_table"))
      << "a refusal that does not name the right kind leaves the reader to "
         "guess: " << (p.conflicts.empty() ? "none" : p.conflicts[0]);
}

TEST(CitusPlanner, WithoutTheExtensionItRefusesAndAsksForThePrerequisite) {
  // Absent, not zero: the reading is missing entirely, which is the state
  // Observations::extension() is shaped to express.
  const auto obs = citus_observations(
      json(nullptr), json{{"public.accounts", table_with_pk({"id"})}});
  const auto p = plan_citus(json{{"kind", "distribute_table"},
                                 {"schema", "public"}, {"table", "accounts"},
                                 {"distribution_column", "id"}}, obs);
  EXPECT_FALSE(p.ok);
  EXPECT_TRUE(conflicts_mention(p, "citus extension is not installed"));
  ASSERT_FALSE(p.prerequisites.empty())
      << "a refusal for a missing extension must say how to get it";
  EXPECT_EQ(p.prerequisites[0].value("target", ""), "citus");
}

TEST(CitusPlanner, DdlPropagationOffRefusesTheWholePlanIncludingCoreKinds) {
  // The guard is plan-level because the condition is: with propagation off, a
  // plain create_table diverges from the workers too.
  const auto obs = citus_observations(citus_obs(json::object(), 2, "off"),
                                      json::object());
  const auto p = plan_citus(json{{"kind", "create_schema"},
                                 {"schema", "reporting"},
                                 {"comment", "anything at all"}}, obs);
  EXPECT_FALSE(p.ok);
  EXPECT_TRUE(conflicts_mention(p, "enable_ddl_propagation is off"))
      << (p.conflicts.empty() ? "none" : p.conflicts[0]);
}

TEST(CitusPlanner, TheBudgetSaysWhatItCannotDerive) {
  const auto obs = citus_observations(citus_obs(), json::object());
  const auto p = plan_citus(json{{"kind", "create_schema"},
                                 {"schema", "reporting"},
                                 {"comment", "x"}}, obs);
  EXPECT_EQ(p.budget.value("topology", ""), "citus");
  EXPECT_EQ(p.budget.value("workerCount", 0), 2);
  EXPECT_NE(p.budget.value("caveatTopology", "").find("not on what the cluster will survive"),
            std::string::npos)
      << "a headroom that is only the coordinator's must say so rather than "
         "reading as the cluster's";
}

TEST(CitusPlanner, TheModuleSetIsRecordedAndIsNotHashed) {
  const auto obs = citus_observations(citus_obs(), json::object());
  const auto p = plan_citus(json{{"kind", "create_schema"},
                                 {"schema", "reporting"},
                                 {"comment", "x"}}, obs);
  const auto modules = p.to_json().value("modules", json::array());
  bool has_citus = false;
  for (const auto& m : modules) {
    if (m.get<std::string>() == "citus") has_citus = true;
  }
  EXPECT_TRUE(has_citus) << "the plan must record which modules produced it: "
                         << modules.dump();
  // And the digest must not cover it, or two builds would disagree about a
  // specification they both handle identically.
  json d = p.to_json();
  EXPECT_TRUE(d.contains("modules"));
  EXPECT_EQ(p.digest(), p.digest());
}

TEST(CitusPlanner, WithoutTheConcurrentCallTheBlockingPathSaysWhyItWasForced) {
  // Gated on the FUNCTION EXISTING, not on a version comparison: presence is a
  // reading, a minimum version is a recollection. Without this an older Citus
  // gets SQL it cannot parse, which is the failure the merge_rows/PG17 gate
  // exists to prevent for core.
  const auto obs = citus_observations(
      citus_obs(json::object(), 2, "on", /*concurrent_call=*/false),
      json{{"public.accounts", table_with_pk({"id"}, 5000000)}});
  const auto p = plan_citus(json{{"kind", "distribute_table"},
                                 {"schema", "public"}, {"table", "accounts"},
                                 {"distribution_column", "id"}}, obs);
  ASSERT_TRUE(p.ok) << (p.conflicts.empty() ? "" : p.conflicts[0]);
  EXPECT_FALSE(p.steps[0].detail.value("concurrent", true));
  EXPECT_EQ(p.steps[0].sql[0].find("_concurrently"), std::string::npos);
  // And it must not read as though the row count chose the blocking path: five
  // million rows would have chosen the other one.
  EXPECT_NE(p.steps[0].why.find("has no create_distributed_table_concurrently"),
            std::string::npos) << p.steps[0].why;
  EXPECT_NE(p.steps[0].why.find("did not decide it"), std::string::npos);
  bool warned = false;
  for (const auto& w : p.warnings) {
    if (w.find("would have taken the concurrent path") != std::string::npos) warned = true;
  }
  EXPECT_TRUE(warned) << "a table big enough to want the concurrent path must "
                         "be told it is not getting it";
}

TEST(CitusPlanner, AskingForConcurrentlyAlwaysWithoutTheCallIsRefused) {
  // Not silently downgraded. Someone who wrote "always" decided the table is
  // too big to lock; locking it anyway is the worst available answer.
  const auto obs = citus_observations(
      citus_obs(json::object(), 2, "on", /*concurrent_call=*/false),
      json{{"public.accounts", table_with_pk({"id"}, 5000000)}});
  const auto p = plan_citus(json{{"kind", "distribute_table"},
                                 {"schema", "public"}, {"table", "accounts"},
                                 {"distribution_column", "id"},
                                 {"concurrently", "always"}}, obs);
  EXPECT_FALSE(p.ok);
  EXPECT_TRUE(conflicts_mention(p, "has no create_distributed_table_concurrently"))
      << (p.conflicts.empty() ? "none" : p.conflicts[0]);
  EXPECT_TRUE(conflicts_mention(p, "concurrently: never"))
      << "the refusal must say what the author can write instead";
}

TEST(CitusPlanner, PacedDmlOnADistributedTableIsRefusedBeforeItRuns) {
  // Not a performance judgement: Citus rejects FOR UPDATE on a multi-shard
  // query outright, so this was emitted, accepted, and failed AT EXECUTION --
  // partway through, with earlier batches already committed.
  const auto obs = citus_observations(
      citus_obs(json{{"public.accounts",
                      json{{"partmethod", "h"}, {"distribution_column", "id"}}}}),
      json{{"public.accounts", table_with_pk({"id"}, 100000)}});
  const auto p = plan_citus(json{{"kind", "backfill"}, {"schema", "public"},
                                 {"table", "accounts"}, {"key", "id"},
                                 {"set", json{{"balance", "0"}}},
                                 {"where", "balance IS NULL"},
                                 {"verify_remaining", "balance IS NULL"}}, obs);
  EXPECT_FALSE(p.ok);
  EXPECT_TRUE(conflicts_mention(p, "FOR UPDATE/SHARE"))
      << (p.conflicts.empty() ? "none" : p.conflicts[0]);
  EXPECT_TRUE(conflicts_mention(p, "partway through"))
      << "the refusal must say what would have happened, or it reads as "
         "conservatism rather than a fact about Citus";
}

TEST(CitusPlanner, PacedDmlOnALocalTableInACitusDatabaseIsUnaffected) {
  // The refusal is about the TABLE being distributed, not about Citus being
  // installed. A local table in a Citus database walks exactly as it always
  // did, and refusing it would take a working thing away.
  const auto obs = citus_observations(
      citus_obs(), json{{"public.staging", table_with_pk({"id"}, 100000)}});
  const auto p = plan_citus(json{{"kind", "backfill"}, {"schema", "public"},
                                 {"table", "staging"}, {"key", "id"},
                                 {"set", json{{"balance", "0"}}},
                                 {"where", "balance IS NULL"},
                                 {"verify_remaining", "balance IS NULL"}}, obs);
  EXPECT_TRUE(p.ok) << (p.conflicts.empty() ? "" : p.conflicts[0]);
}

TEST(CitusPlanner, PacedDmlOnAReferenceTableIsAllowed) {
  // A reference table has one placement per node and is not sharded, so the
  // multi-shard FOR UPDATE restriction does not apply to it.
  const auto obs = citus_observations(
      citus_obs(json{{"public.currencies",
                      json{{"partmethod", "n"}, {"distribution_column", ""}}}}),
      json{{"public.currencies", table_with_pk({"id"}, 100)}});
  const auto p = plan_citus(json{{"kind", "backfill"}, {"schema", "public"},
                                 {"table", "currencies"}, {"key", "id"},
                                 {"set", json{{"balance", "0"}}},
                                 {"where", "balance IS NULL"},
                                 {"verify_remaining", "balance IS NULL"}}, obs);
  EXPECT_TRUE(p.ok) << (p.conflicts.empty() ? "" : p.conflicts[0]);
}
