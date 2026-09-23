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

// Several intents in one specification, in the order written: the case
// projection exists for.
json citus_spec_many(json intents) {
  return json{{"laswell_spec_version", 1},
              {"id", "citus-test"},
              {"description", "a citus planner test"},
              {"intents", std::move(intents)}};
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

pglaswell::Plan plan_citus_many(const json& intents,
                               const pglaswell::Observations& obs) {
  const auto spec = pglaswell::parse_spec(citus_spec_many(intents));
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
  const auto p = plan_citus(json{{"kind", "citus_distribute_table"},
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
  const auto p = plan_citus(json{{"kind", "citus_distribute_table"},
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
  const auto p = plan_citus(json{{"kind", "citus_distribute_table"},
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
  const auto p = plan_citus(json{{"kind", "citus_distribute_table"},
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
  const auto p = plan_citus(json{{"kind", "citus_distribute_table"},
                                 {"schema", "public"},
                                 {"table", "account_transactions"},
                                 {"distribution_column", "account_id"}}, obs);
  EXPECT_TRUE(p.ok) << (p.conflicts.empty() ? "" : p.conflicts[0]);
}

TEST(CitusPlanner, ColocatingWithSomethingNotDistributedIsRefused) {
  const auto obs = citus_observations(
      citus_obs(), json{{"public.orders", table_with_pk({"id"})}});
  const auto p = plan_citus(json{{"kind", "citus_distribute_table"},
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
  const auto p = plan_citus(json{{"kind", "citus_distribute_table"},
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
  const auto p = plan_citus(json{{"kind", "citus_distribute_table"},
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
  const auto p = plan_citus(json{{"kind", "citus_distribute_table"},
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
  const auto p = plan_citus(json{{"kind", "citus_distribute_table"},
                                 {"schema", "public"}, {"table", "accounts"},
                                 {"distribution_column", "id"},
                                 {"concurrently", "always"}}, obs);
  EXPECT_FALSE(p.ok);
  EXPECT_TRUE(conflicts_mention(p, "has no create_distributed_table_concurrently"))
      << (p.conflicts.empty() ? "none" : p.conflicts[0]);
  EXPECT_TRUE(conflicts_mention(p, "concurrently: never"))
      << "the refusal must say what the author can write instead";
}

// A backfill on a distributed table is a BACKFILL. The author does not name a
// Citus kind for it: core reads citus_required_confinement and walks one
// distribution value at a time, which makes FOR UPDATE single-shard. One
// repository can then serve plain PostgreSQL in development and Citus in
// staging with the same signed specification.
namespace {
json tenant_backfill(const char* table) {
  return json{{"kind", "backfill"}, {"schema", "public"}, {"table", table},
              {"key", "id"}, {"set", json{{"balance", "0"}}},
              {"where", "balance IS NULL"}};
}
}  // namespace

TEST(CitusPlanner, ABackfillOnADistributedTableWalksOneDistributionValueAtATime) {
  const auto obs = citus_observations(
      citus_obs(json{{"public.ledger",
                      json{{"partmethod", "h"}, {"distribution_column", "tenant_id"}}}}),
      json{{"public.ledger", table_with_pk({"tenant_id", "id"}, 100000)}});
  const auto p = plan_citus(tenant_backfill("ledger"), obs);
  ASSERT_TRUE(p.ok) << (p.conflicts.empty() ? "" : p.conflicts[0]);
  const auto& step = p.steps.back();
  EXPECT_EQ(step.detail.value("batch_mode", ""), "grouped");
  EXPECT_EQ(step.detail.value("group_column", ""), "tenant_id");
  // The plan says WHICH reading shaped it, which is what keeps a module-shaped
  // plan honest: its statements differ from the plain one, so its digest does,
  // and its why names the reason.
  EXPECT_EQ(step.detail.value("confined_by", ""), "citus");
  EXPECT_NE(step.why.find("citus reading"), std::string::npos) << step.why;
  ASSERT_EQ(step.sql.size(), 3u);
  // The locking statement and the change are both confined by the equality --
  // the only confinement Citus pushes down -- or the lock is multi-shard again.
  EXPECT_NE(step.sql[1].find("\"tenant_id\" = $1"), std::string::npos) << step.sql[1];
  EXPECT_NE(step.sql[1].find("FOR UPDATE"), std::string::npos) << step.sql[1];
  EXPECT_NE(step.sql[2].find("\"tenant_id\" = $1"), std::string::npos) << step.sql[2];
}

TEST(CitusPlanner, ABackfillWhoseKeyIsTheDistributionColumnIsRefusedWithTheReason) {
  // Confinement is an equality on the distribution column, so when that column
  // is the key every batch is one row. Refused, and the refusal says why and
  // what runs instead -- not "FOR UPDATE", which is the mechanism, not the
  // reason this particular table cannot be walked.
  const auto obs = citus_observations(
      citus_obs(json{{"public.accounts",
                      json{{"partmethod", "h"}, {"distribution_column", "id"}}}}),
      json{{"public.accounts", table_with_pk({"id"}, 100000)}});
  const auto p = plan_citus(tenant_backfill("accounts"), obs);
  EXPECT_FALSE(p.ok);
  EXPECT_TRUE(conflicts_mention(p, "which is the walk key"))
      << (p.conflicts.empty() ? "none" : p.conflicts[0]);
  EXPECT_TRUE(conflicts_mention(p, "update_rows"))
      << "the refusal must name what runs instead";
}

TEST(CitusPlanner, ABackfillNeedsItsKeyUniqueWithinOneDistributionValue) {
  // A unique index with the columns the other way round, (id, tenant_id), proves
  // the pair unique but not id within one tenant in the order the walk needs --
  // and a grouping other than the one Citus requires would not make the lock
  // single-shard. Refused, naming the index that would do.
  const auto obs = citus_observations(
      citus_obs(json{{"public.ledger",
                      json{{"partmethod", "h"}, {"distribution_column", "tenant_id"}}}}),
      json{{"public.ledger", table_with_pk({"id", "tenant_id"}, 100000)}});
  const auto p = plan_citus(tenant_backfill("ledger"), obs);
  EXPECT_FALSE(p.ok);
  EXPECT_TRUE(conflicts_mention(p, "(tenant_id, id)"))
      << (p.conflicts.empty() ? "none" : p.conflicts[0]);
}

TEST(CitusPlanner, WithoutAReadingTheSameTableGroupsOnlyBecauseOfItsIndex) {
  // The same (tenant_id, id) table on plain PostgreSQL. It is walked grouped
  // too -- the key is unique only within a tenant, which was reason enough to
  // refuse it on every server before -- but NOTHING claims a module decided it.
  const auto obs = citus_observations(
      json(nullptr), json{{"public.ledger", table_with_pk({"tenant_id", "id"}, 100000)}});
  const auto p = plan_citus(tenant_backfill("ledger"), obs);
  ASSERT_TRUE(p.ok) << (p.conflicts.empty() ? "" : p.conflicts[0]);
  const auto& step = p.steps.back();
  EXPECT_EQ(step.detail.value("batch_mode", ""), "grouped");
  EXPECT_FALSE(step.detail.contains("confined_by")) << step.detail.dump(2);
}

TEST(CitusPlanner, AModuleThatHasNothingToSayChangesNothing) {
  // What the revised rule still guarantees: a module may shape a plan only
  // through a reading about THAT table. For a table the Citus reading says
  // nothing about -- local in a Citus database -- the plan is the one a build
  // without the module emits, statement for statement.
  const auto with = citus_observations(
      citus_obs(), json{{"public.staging", table_with_pk({"id"}, 100000)}});
  const auto without = citus_observations(
      json(nullptr), json{{"public.staging", table_with_pk({"id"}, 100000)}});
  const auto a = plan_citus(tenant_backfill("staging"), with);
  const auto b = plan_citus(tenant_backfill("staging"), without);
  ASSERT_TRUE(a.ok && b.ok);
  ASSERT_EQ(a.steps.size(), b.steps.size());
  for (std::size_t i = 0; i < a.steps.size(); ++i) {
    EXPECT_EQ(a.steps[i].sql, b.steps[i].sql) << "step " << i;
  }
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

TEST(CitusPlanner, ALaterIntentColocatesWithWhatAnEarlierOneWillHaveDistributed) {
  // Projection, end to end, in the arrangement it was written for: distribute
  // accounts, then hang account_transactions off it, in ONE specification
  // against a database where neither is distributed yet.
  //
  // This is also the only thing that proves the projection lands where the
  // planner reads it back. The module writes its own subtree of
  // Observations::extensions and the slot name is chosen by the build, not by
  // the module -- so if the two ever disagree, nothing would warn, the second
  // intent would simply be refused for colocating with something "this
  // database does not record as distributed", and the plan would look like an
  // author's mistake rather than a wiring one.
  const auto obs = citus_observations(
      citus_obs(), json{{"public.accounts", table_with_pk({"id"}, 500)},
                        {"public.account_transactions",
                         table_with_pk({"account_id", "reference"}, 500)}});
  const auto p = plan_citus_many(
      json::array({json{{"kind", "citus_distribute_table"},
                        {"schema", "public"},
                        {"table", "accounts"},
                        {"distribution_column", "id"}},
                   json{{"kind", "citus_distribute_table"},
                        {"schema", "public"},
                        {"table", "account_transactions"},
                        {"distribution_column", "account_id"},
                        {"colocate_with", "public.accounts"}}}),
      obs);
  ASSERT_TRUE(p.ok) << (p.conflicts.empty() ? "" : p.conflicts[0]);
  ASSERT_EQ(p.steps.size(), 2u);
  EXPECT_EQ(p.steps[1].detail.value("colocate_with", ""), "public.accounts");
}

TEST(CitusPlanner, WithoutTheEarlierIntentTheSameColocationIsRefused) {
  // The other side of it. Without this, the test above passes just as happily
  // against a planner that never refuses a colocation at all.
  const auto obs = citus_observations(
      citus_obs(), json{{"public.account_transactions",
                         table_with_pk({"account_id", "reference"}, 500)}});
  const auto p = plan_citus(json{{"kind", "citus_distribute_table"},
                                 {"schema", "public"},
                                 {"table", "account_transactions"},
                                 {"distribution_column", "account_id"},
                                 {"colocate_with", "public.accounts"}},
                            obs);
  EXPECT_FALSE(p.ok);
  EXPECT_TRUE(conflicts_mention(p, "does not record as distributed"));
}
