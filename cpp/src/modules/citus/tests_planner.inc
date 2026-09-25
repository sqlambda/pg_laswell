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
              {"function_distribution", json::object()},
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

// --- §6 refusals and the function no-op, each against a fixed reading ------

namespace {
json dist_entry(const char* column, long long type_oid, const char* type,
                const char* category, long long group) {
  return json{{"partmethod", "h"}, {"repmodel", "s"},
              {"distribution_column", column},
              {"distribution_type_oid", type_oid}, {"distribution_type", type},
              {"distribution_type_category", category}, {"colocationid", group},
              {"shard_count", 32}, {"replication_factor", 1}};
}
json typed_table(const char* column, long long type_oid, const char* type) {
  json t = table_with_pk({column}, 10);
  t["columns"][column] = json{{"type", type}, {"type_oid", type_oid}};
  return t;
}
}  // namespace

TEST(CitusPlanner, ColocationRequiresTheSameDistributionTypeExactly) {
  // Measured: Citus refuses int against a bigint group as firmly as text --
  // "Distribution column types don't match". Checked by oid, the comparison
  // that cannot disagree with Citus's.
  for (const auto& [oid, type, want_ok] :
       std::vector<std::tuple<long long, const char*, bool>>{
           {20, "bigint", true}, {23, "integer", false}, {25, "text", false}}) {
    const auto obs = citus_observations(
        citus_obs(json{{"public.a", dist_entry("id", 20, "bigint", "N", 9)}}),
        json{{"public.b", typed_table("id", oid, type)}});
    const auto p = plan_citus(json{{"kind", "citus_distribute_table"},
                                   {"schema", "public"}, {"table", "b"},
                                   {"distribution_column", "id"},
                                   {"colocate_with", "public.a"},
                                   {"comment", "x"}}, obs);
    EXPECT_EQ(p.ok, want_ok) << type << ": "
                             << (p.conflicts.empty() ? "" : p.conflicts[0]);
    if (!want_ok) {
      EXPECT_TRUE(conflicts_mention(p, "same type exactly")) << type;
    }
  }
}

namespace {
pglaswell::Observations function_obs(json distribution, json arguments,
                                     long long group_type_oid = 20,
                                     const char* group_category = "N") {
  json citus = citus_obs(json{{"public.accounts",
                               dist_entry("id", group_type_oid, "bigint",
                                          group_category, 9)}});
  citus["function_distribution"] = std::move(distribution);
  auto obs = citus_observations(citus, json::object());
  obs.objects["function:public.f"] =
      json{{"exists", true}, {"kind", "function"},
           {"signature", "public.f(bigint)"}, {"arguments", std::move(arguments)}};
  return obs;
}
pglaswell::Plan plan_function(const pglaswell::Observations& obs, const char* arg) {
  return plan_citus(json{{"kind", "citus_distribute_function"}, {"schema", "public"},
                         {"name", "f"}, {"arguments", "bigint"},
                         {"distribution_argument", arg},
                         {"colocate_with", "public.accounts"}, {"comment", "x"}},
                    obs);
}
const pglaswell::Step* function_step(const pglaswell::Plan& p) {
  for (const auto& s : p.steps) {
    if (s.kind == "citus_distribute_function") return &s;
  }
  return nullptr;
}
const json kBigintTid = json::array({json{{"name", "tid"}, {"type_oid", 20},
                                          {"type", "bigint"}, {"category", "N"}}});
}  // namespace

TEST(CitusPlanner, AFunctionCitusMerelyKnowsIsNotAlreadyDistributed) {
  // The no-op this replaces. On Citus 11+ every CREATE FUNCTION is propagated
  // and recorded in pg_dist_object with no distribution argument and no
  // colocation, and the first reading listed names only -- so every function
  // read as "already distributed", and the kind reported satisfied and routed
  // nothing. A function that is only propagated must be distributed.
  const auto obs = function_obs(
      json{{"public.f", json{{"argument_index", nullptr}, {"colocationid", nullptr}}}},
      kBigintTid);
  const auto p = plan_function(obs, "tid");
  ASSERT_TRUE(p.ok) << (p.conflicts.empty() ? "" : p.conflicts[0]);
  const auto* s = function_step(p);
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(s->action, pglaswell::Action::kApply)
      << "a propagated function was reported as already distributed";
}

TEST(CitusPlanner, AFunctionIsSatisfiedOnlyWhenItRoutesAsAsked) {
  // Routed on the same argument in the same group: nothing to do.
  {
    const auto p = plan_function(
        function_obs(json{{"public.f", json{{"argument_index", 0}, {"colocationid", 9}}}},
                     kBigintTid), "tid");
    const auto* s = function_step(p);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->action, pglaswell::Action::kSatisfied);
  }
  // Routed some other way: re-applied, and the plan says what changes. Measured,
  // Citus switches an existing distribution with no error.
  {
    const auto obs = function_obs(
        json{{"public.f", json{{"argument_index", 1}, {"colocationid", 9}}}},
        json::array({kBigintTid[0], json{{"name", "x"}, {"type_oid", 23},
                                         {"type", "integer"}, {"category", "N"}}}));
    const auto p = plan_function(obs, "tid");
    const auto* s = function_step(p);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->action, pglaswell::Action::kApply);
    bool warned = false;
    for (const auto& w : p.warnings) {
      if (w.find("re-distributes") != std::string::npos) warned = true;
    }
    EXPECT_TRUE(warned) << "a changed routing must be named";
  }
}

TEST(CitusPlanner, ADistributionArgumentMustBeOneTheFunctionHas) {
  // Citus refuses both (22023); the plan refuses first and lists the inputs.
  const auto obs = function_obs(json::object(), kBigintTid);
  for (const char* bad : {"nope", "$2", "$0", "$x"}) {
    const auto p = plan_function(obs, bad);
    EXPECT_FALSE(p.ok) << bad;
    EXPECT_TRUE(conflicts_mention(p, "tid $1 bigint")) << bad;
  }
  EXPECT_TRUE(plan_function(obs, "tid").ok);
  EXPECT_TRUE(plan_function(obs, "$1").ok);
}

TEST(CitusPlanner, AnArgumentThatCannotBeCoercedToTheGroupIsRefused) {
  // Measured: Citus ACCEPTS a text argument on a bigint group, and then every
  // call fails, "Cannot coerce text to bigint". Across categories: refused.
  const auto text_arg = json::array({json{{"name", "tid"}, {"type_oid", 25},
                                          {"type", "text"}, {"category", "S"}}});
  const auto p = plan_function(function_obs(json::object(), text_arg), "tid");
  EXPECT_FALSE(p.ok);
  EXPECT_TRUE(conflicts_mention(p, "every call")) << (p.conflicts.empty() ? "" : p.conflicts[0]);

  // Within a category: measured, int on bigint is pushed down. Allowed, with a
  // warning that only that pair was measured.
  const auto int_arg = json::array({json{{"name", "tid"}, {"type_oid", 23},
                                         {"type", "integer"}, {"category", "N"}}});
  const auto q = plan_function(function_obs(json::object(), int_arg), "tid");
  EXPECT_TRUE(q.ok) << (q.conflicts.empty() ? "" : q.conflicts[0]);
  EXPECT_FALSE(q.warnings.empty());
}

TEST(CitusPlanner, AManagedLocalTableIsNotAReferenceTable) {
  // Citus records a managed local table with partmethod 'n' as it does a
  // reference table; repmodel tells them apart ('t' replicated, 's' one
  // placement). Reporting the first as "already a reference table" would
  // claim a table on one node is replicated. Measured: create_reference_table
  // converts it without error, so it is applied.
  const auto plan_ref = [](const char* repmodel) {
    const auto obs = citus_observations(
        citus_obs(json{{"public.r", json{{"partmethod", "n"}, {"repmodel", repmodel},
                                         {"distribution_column", nullptr},
                                         {"colocationid", 0}}}}),
        json{{"public.r", table_with_pk({"id"}, 10)}});
    return plan_citus(json{{"kind", "citus_create_reference_table"},
                           {"schema", "public"}, {"table", "r"}}, obs);
  };
  const auto replicated = plan_ref("t");
  ASSERT_FALSE(replicated.steps.empty());
  EXPECT_EQ(replicated.steps.back().action, pglaswell::Action::kSatisfied);
  const auto managed = plan_ref("s");
  ASSERT_FALSE(managed.steps.empty());
  EXPECT_EQ(managed.steps.back().action, pglaswell::Action::kApply);
}

namespace {
// Every table the FK tests name, with the columns and keys core's add_foreign_key
// checks for before the guard is ever reached: without these, core refuses a
// table that "does not exist", and a test of the guard tests nothing.
json fk_table(std::vector<std::string> pk) {
  json t = table_with_pk(pk, 10);
  for (const char* c : {"tenant_id", "id", "other", "rid"}) {
    t["columns"][c] = json{{"type", "bigint"}, {"type_oid", 20}};
  }
  return t;
}
pglaswell::Plan plan_fk(json tables, const char* child, json cols, const char* parent,
                        json refs) {
  const json readings = {{"public.d1", fk_table({"tenant_id", "id"})},
                         {"public.d2", fk_table({"tenant_id", "id"})},
                         {"public.d3", fk_table({"tenant_id", "id"})},
                         {"public.r", fk_table({"id"})},
                         {"public.ml", fk_table({"id"})},
                         {"public.plain", fk_table({"id"})}};
  const auto obs = citus_observations(citus_obs(std::move(tables)), readings);
  return plan_citus(json{{"kind", "add_foreign_key"}, {"schema", "public"},
                         {"table", child}, {"name", "fk"}, {"columns", std::move(cols)},
                         {"references_schema", "public"}, {"references_table", parent},
                         {"references_columns", std::move(refs)}},
                    obs);
}
bool fk_refused(const pglaswell::Plan& p, const char* needle) {
  return !p.ok && conflicts_mention(p, needle);
}
}  // namespace

TEST(CitusPlanner, ForeignKeysFollowCitusRulesForEachPairOfTableKinds) {
  // Every combination was measured against Citus 13; each refusal quotes it.
  const json d1 = dist_entry("tenant_id", 20, "bigint", "N", 9);
  const json d2 = dist_entry("tenant_id", 20, "bigint", "N", 9);
  const json d3 = dist_entry("tenant_id", 20, "bigint", "N", 12);
  const json ref = json{{"partmethod", "n"}, {"repmodel", "t"}, {"colocationid", 0}};
  const json managed = json{{"partmethod", "n"}, {"repmodel", "s"}, {"colocationid", 0}};
  const json tables = {{"public.d1", d1}, {"public.d2", d2}, {"public.d3", d3},
                       {"public.r", ref}, {"public.ml", managed}};
  const json tc = json::array({"tenant_id", "other"});
  const json ti = json::array({"tenant_id", "id"});

  {
    const auto ok = plan_fk(tables, "d2", tc, "d1", ti);
    EXPECT_TRUE(ok.ok) << "colocated, same ordinal: "
                       << (ok.conflicts.empty() ? "" : ok.conflicts[0]);
  }
  EXPECT_TRUE(fk_refused(plan_fk(tables, "d2", json::array({"other", "tenant_id"}), "d1", ti),
                         "same position"))
      << "colocated, distribution column at a different position";
  EXPECT_TRUE(fk_refused(plan_fk(tables, "d3", tc, "d1", ti), "not colocated"));
  EXPECT_TRUE(plan_fk(tables, "d2", json::array({"other"}), "r",
                      json::array({"id"})).ok) << "distributed -> reference";
  EXPECT_TRUE(fk_refused(plan_fk(tables, "d2", json::array({"other"}), "plain",
                                 json::array({"id"})), "(local)"))
      << "distributed -> plain local";
  // The case the first guard let through: a MANAGED local table is local, not
  // reference, whatever its partmethod says.
  EXPECT_TRUE(fk_refused(plan_fk(tables, "d2", json::array({"other"}), "ml",
                                 json::array({"id"})), "(local)"))
      << "distributed -> Citus-managed local";
  EXPECT_TRUE(fk_refused(plan_fk(tables, "r", json::array({"id"}), "d1",
                                 json::array({"tenant_id"})), "reference or local"));
  EXPECT_TRUE(fk_refused(plan_fk(tables, "plain", ti, "d1", ti), "reference or local"));
  EXPECT_TRUE(plan_fk(tables, "plain", json::array({"rid"}), "r",
                      json::array({"id"})).ok) << "local -> reference";
}

TEST(CitusPlanner, TheBudgetNamesTheModifyModeItsFanOutDependsOn) {
  // A plan whose connection use depends on a session setting it never names is
  // not a function of the specification and the reading. Measured on Citus 13:
  // sequential holds one connection per worker; parallel opens up to the pool
  // size, and only while shard tasks run long.
  const auto budget_for = [](const char* mode) {
    json citus = citus_obs(json{{"public.ledger",
                                 dist_entry("tenant_id", 20, "bigint", "N", 9)}});
    citus["settings"]["multi_shard_modify_mode"] = mode;
    const auto obs = citus_observations(
        citus, json{{"public.ledger", table_with_pk({"tenant_id", "id"}, 10)}});
    return pglaswell::detail::compute_budget(obs, pglaswell::ExecutorConfig{});
  };
  const auto seq = budget_for("sequential");
  EXPECT_EQ(seq.value("multiShardModifyMode", ""), "sequential");
  EXPECT_EQ(seq.value("workerFanOutPerSession", json()), json(1))
      << "sequential is a guaranteed one connection per worker";
  const auto par = budget_for("parallel");
  EXPECT_EQ(par.value("multiShardModifyMode", ""), "parallel");
  EXPECT_EQ(par.value("workerFanOutPerSession", json()), json("16"));
  EXPECT_NE(par.value("workerFanOutBasis", "").find("UPPER BOUND"), std::string::npos)
      << "parallel's fan-out is a bound, not a count";

  // §7.4: the pacing constants are not silently reused. The budget says where
  // they came from, that a grouped walk commits on one worker, and how long a
  // killed multi-shard batch can hold locks.
  json citus = citus_obs();
  citus["settings"]["recover_2pc_interval"] = "1min";
  const auto b = pglaswell::detail::compute_budget(
      citus_observations(citus, json::object()), pglaswell::ExecutorConfig{});
  const auto basis = b.value("pacingBasis", "");
  EXPECT_NE(basis.find("single-node PostgreSQL"), std::string::npos) << basis;
  EXPECT_NE(basis.find("plain COMMIT"), std::string::npos) << basis;
  EXPECT_NE(basis.find("(1min)"), std::string::npos) << basis;
}

// --- against a live cluster ---------------------------------------------------
//
// Skipped without CITUS_URL, loudly. These prove SQL a fixed reading cannot.

TEST(CitusLive, AWorkerSideWaitHasAnAgeTheBreakerCanUse) {
  // citus_lock_waits has no waitstart, so a wait that exists only on a worker
  // used to report no age at all -- and max_waiter_wait_ms was inactive for
  // exactly the contention Citus adds. The observer now reports an UPPER BOUND:
  // the waiting statement's age on the node it waits on. Proved here with the
  // observer's own SQL constant, so this cannot drift from what runs.
  const char* url = std::getenv("CITUS_URL");
  if (url == nullptr || *url == '\0') GTEST_SKIP() << "CITUS_URL is unset";

  pqxx::connection setup(url);
  {
    pqxx::work w(setup);
    w.exec("DROP TABLE IF EXISTS public.laswell_wait_probe");
    w.exec("CREATE TABLE public.laswell_wait_probe (t bigint NOT NULL, id bigint NOT NULL,"
           " v int, PRIMARY KEY (t, id))");
    w.exec("SELECT create_distributed_table('public.laswell_wait_probe', 't')");
    w.exec("INSERT INTO public.laswell_wait_probe VALUES (1, 1, 0)");
    w.commit();
  }

  pqxx::connection blocker(url);
  pqxx::work hold(blocker);
  hold.exec("UPDATE public.laswell_wait_probe SET v = v + 1 WHERE t = 1");
  const int blocker_pid = hold.query_value<int>("SELECT pg_backend_pid()");

  // The waiter, on its own connection and thread: it blocks until `hold` ends.
  std::thread waiter([url] {
    try {
      pqxx::connection c(url);
      pqxx::work w(c);
      w.exec("UPDATE public.laswell_wait_probe SET v = v + 10 WHERE t = 1");
      w.commit();
    } catch (...) {
    }
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));

  // Whatever happens below, the waiter is released and joined: a std::thread
  // destroyed while joinable calls std::terminate, and the first version of this
  // test took the whole binary down when the reading came back without an age.
  struct Release {
    pqxx::work& hold;
    std::thread& waiter;
    ~Release() {
      try { hold.abort(); } catch (...) {}
      if (waiter.joinable()) waiter.join();
    }
  } release{hold, waiter};

  long long age = -1;
  json reading;
  try {
    pqxx::connection obs(url);
    pqxx::work r(obs);
    const auto res = pglaswell::pqxx_exec(
        r, pglaswell::kcitusObserverSql,
        pqxx::params{"{" + std::to_string(blocker_pid) + "}"});
    reading = json::parse(res[0][0].as<std::string>());
  } catch (const std::exception& e) {
    ADD_FAILURE() << "the observer's SQL failed against Citus: " << e.what();
  }
  const auto mine = reading.is_object()
                        ? reading.value(std::to_string(blocker_pid), json::object())
                        : json::object();
  EXPECT_GE(mine.value("direct", 0), 1) << reading.dump();
  const auto a = mine.value("oldestWaitUpperBoundMs", json(nullptr));
  if (a.is_number()) age = a.get<long long>();
  hold.abort();
  waiter.join();
  {
    pqxx::work w(setup);
    w.exec("DROP TABLE public.laswell_wait_probe");
    w.commit();
  }

  // Waited about 1500ms. An upper bound may read higher, never meaningfully
  // lower -- lower would be the unsafe direction for the breaker.
  EXPECT_GE(age, 1300) << "the worker-side wait's age is missing or understated";
  EXPECT_LT(age, 10000) << "the bound is too loose to be useful";
}

// --- Phase 2 ----------------------------------------------------------------

namespace {
json distributed(const char* column, long long group, int shards = 32) {
  return json{{"partmethod", "h"}, {"repmodel", "s"},
              {"distribution_column", column}, {"colocationid", group},
              {"shard_count", shards}};
}
json reference_table() {
  return json{{"partmethod", "n"}, {"repmodel", "t"},
              {"distribution_column", ""}, {"colocationid", 1}};
}
json managed_local() {
  return json{{"partmethod", "n"}, {"repmodel", "s"},
              {"distribution_column", ""}, {"colocationid", 0}};
}
json alter_intent(json extra) {
  json in{{"kind", "citus_alter_distributed_table"},
          {"schema", "public"}, {"table", "a"}};
  in.update(extra);
  return in;
}
std::string first_conflict(const pglaswell::Plan& p) {
  return p.conflicts.empty() ? "none" : p.conflicts[0];
}
}  // namespace

TEST(CitusPlanner, AnAlterThatChangesNothingIsSatisfiedAndNotCalled) {
  // Measured: Citus raises "this call doesn't change any properties of the
  // table" -- so without reading it as done, re-running would FAIL.
  const auto obs = citus_observations(
      citus_obs(json{{"public.a", distributed("id", 5, 32)}}),
      json{{"public.a", table_with_pk({"id"})}});
  const auto p = plan_citus(
      alter_intent(json{{"shard_count", 32}, {"distribution_column", "id"}}), obs);
  ASSERT_TRUE(p.ok) << first_conflict(p);
  ASSERT_EQ(p.steps.size(), 1u);
  EXPECT_EQ(p.steps[0].action, pglaswell::Action::kSatisfied);
  EXPECT_TRUE(p.steps[0].sql.empty());
}

TEST(CitusPlanner, AnAlterEmitsOnlyWhatChanges) {
  const auto obs = citus_observations(
      citus_obs(json{{"public.a", distributed("id", 5, 32)}}),
      json{{"public.a", table_with_pk({"id"}, 1234)}});
  const auto p = plan_citus(
      alter_intent(json{{"shard_count", 64}, {"distribution_column", "id"}}), obs);
  ASSERT_TRUE(p.ok) << first_conflict(p);
  ASSERT_EQ(p.steps.size(), 1u);
  EXPECT_EQ(p.steps[0].sql[0],
            "SELECT alter_distributed_table('public.a', shard_count := 64);");
  EXPECT_EQ(p.steps[0].detail.value("rehearse_by", ""), "execution");
  EXPECT_EQ(p.steps[0].txn_class, pglaswell::TxnClass::kRequired);
  EXPECT_NE(p.steps[0].lock.find("rewritten"), std::string::npos) << p.steps[0].lock;
}

TEST(CitusPlanner, AShardCountChangeInASharedGroupHasToSayWhatHappensToTheOthers) {
  const auto obs = citus_observations(
      citus_obs(json{{"public.a", distributed("id", 5)},
                     {"public.b", distributed("id", 5)}}),
      json{{"public.a", table_with_pk({"id"})}});
  const auto refused = plan_citus(alter_intent(json{{"shard_count", 8}}), obs);
  EXPECT_FALSE(refused.ok);
  EXPECT_TRUE(conflicts_mention(refused, "cascade_to_colocated"))
      << first_conflict(refused);
  EXPECT_TRUE(conflicts_mention(refused, "public.b")) << first_conflict(refused);

  const auto p = plan_citus(
      alter_intent(json{{"shard_count", 8}, {"cascade_to_colocated", true}}), obs);
  ASSERT_TRUE(p.ok) << first_conflict(p);
  EXPECT_NE(p.steps[0].sql[0].find("cascade_to_colocated := true"), std::string::npos)
      << p.steps[0].sql[0];
  EXPECT_EQ(p.steps[0].detail.value("cascades_to", json::array()),
            json::array({"public.b"}));
  // The group stays together, so a later step reads it as unchanged.
  EXPECT_EQ(p.steps[0].detail.value("colocation_group", -1LL), 5);
}

TEST(CitusPlanner, AnAlterThatWouldSilentlyDropAForeignKeyIsRefused) {
  // Measured: moving a table out of its group while a colocated distributed
  // table references it SUCCEEDS with a warning, and the key is gone.
  json a = table_with_pk({"id"});
  a["referenced_by"] = json::array({"b_fk on b"});  // regclass text: public dropped
  const auto obs = citus_observations(
      citus_obs(json{{"public.a", distributed("id", 5)},
                     {"public.b", distributed("id", 5)}}),
      json{{"public.a", a}});
  const auto p = plan_citus(alter_intent(json{{"colocate_with", "none"}}), obs);
  EXPECT_FALSE(p.ok);
  EXPECT_TRUE(conflicts_mention(p, "b_fk")) << first_conflict(p);
  EXPECT_TRUE(conflicts_mention(p, "would be dropped")) << first_conflict(p);

  // Leaving the group on its own, said out loud, is still the same drop.
  const auto alone = plan_citus(
      alter_intent(json{{"shard_count", 8}, {"cascade_to_colocated", false}}), obs);
  EXPECT_FALSE(alone.ok);
  EXPECT_TRUE(conflicts_mention(alone, "would be dropped")) << first_conflict(alone);

  // Cascading keeps the group, and its keys, together: measured, re-created.
  const auto kept = plan_citus(
      alter_intent(json{{"shard_count", 8}, {"cascade_to_colocated", true}}), obs);
  EXPECT_TRUE(kept.ok) << first_conflict(kept);
}

TEST(CitusPlanner, AForeignKeyToAReferenceTableDoesNotStopAnAlter) {
  // Measured: a key to a reference table survives the rewrite.
  json a = table_with_pk({"id"});
  a["constraints"]["a_r_fkey"] = json{{"type", "f"}, {"references", "public.r"}};
  const auto obs = citus_observations(
      citus_obs(json{{"public.a", distributed("id", 5)},
                     {"public.r", reference_table()}}),
      json{{"public.a", a}});
  const auto p = plan_citus(alter_intent(json{{"shard_count", 8}}), obs);
  EXPECT_TRUE(p.ok) << first_conflict(p);
}

TEST(CitusPlanner, AlreadyAloneIsWhatColocateWithNoneAsksFor) {
  // Measured: Citus rewrites a table already alone into a new group rather than
  // refusing -- a whole copy for nothing, which reading it as done avoids.
  const auto obs = citus_observations(
      citus_obs(json{{"public.a", distributed("id", 5)}}),
      json{{"public.a", table_with_pk({"id"})}});
  const auto p = plan_citus(alter_intent(json{{"colocate_with", "none"}}), obs);
  ASSERT_TRUE(p.ok) << first_conflict(p);
  EXPECT_EQ(p.steps[0].action, pglaswell::Action::kSatisfied);
}

TEST(CitusPlanner, AnAlterOntoAColumnTheKeyOmitsIsRefusedBeforeTheRewrite) {
  // Measured: Citus copies the data and only then fails re-adding the key.
  json a = table_with_pk({"id"});
  a["columns"]["t"] = json{{"type", "bigint"}};
  const auto obs = citus_observations(
      citus_obs(json{{"public.a", distributed("id", 5)}}), json{{"public.a", a}});
  const auto p = plan_citus(alter_intent(json{{"distribution_column", "t"}}), obs);
  EXPECT_FALSE(p.ok);
  EXPECT_TRUE(conflicts_mention(p, "does not include the new distribution column"))
      << first_conflict(p);
}

TEST(CitusPlanner, AlteringATableThatIsNotDistributedNamesTheRightKind) {
  const auto obs = citus_observations(
      citus_obs(json{{"public.a", reference_table()}}),
      json{{"public.a", table_with_pk({"id"})}});
  const auto p = plan_citus(alter_intent(json{{"shard_count", 8}}), obs);
  EXPECT_FALSE(p.ok);
  EXPECT_TRUE(conflicts_mention(p, "reference")) << first_conflict(p);
  EXPECT_TRUE(conflicts_mention(p, "citus_distribute_table")) << first_conflict(p);
}

TEST(CitusPlanner, AlterArgumentsCitusRefusesAreRefusedAtParse) {
  // Each is a measured Citus error, or a question the plan could not answer.
  EXPECT_ANY_THROW(pglaswell::parse_spec(citus_spec(alter_intent(json::object()))));
  EXPECT_ANY_THROW(pglaswell::parse_spec(citus_spec(
      alter_intent(json{{"shard_count", 8}, {"colocate_with", "public.c"}}))));
  EXPECT_ANY_THROW(pglaswell::parse_spec(citus_spec(alter_intent(
      json{{"distribution_column", "t"}, {"cascade_to_colocated", true}}))));
  EXPECT_ANY_THROW(pglaswell::parse_spec(citus_spec(
      alter_intent(json{{"colocate_with", "default"}}))));
  EXPECT_NO_THROW(pglaswell::parse_spec(citus_spec(
      alter_intent(json{{"shard_count", 8}, {"colocate_with", "none"}}))));
}

TEST(CitusPlanner, UndistributingWhatCitusDoesNotRecordIsDone) {
  // Measured: "cannot undistribute table because the table is not distributed".
  const auto obs = citus_observations(
      citus_obs(), json{{"public.a", table_with_pk({"id"})}});
  const auto p = plan_citus(json{{"kind", "citus_undistribute_table"},
                                 {"schema", "public"}, {"table", "a"}}, obs);
  ASSERT_TRUE(p.ok) << first_conflict(p);
  EXPECT_EQ(p.steps[0].action, pglaswell::Action::kSatisfied);
}

TEST(CitusPlanner, UndistributingOneEndOfAKeyNeedsTheCascadeWrittenDown) {
  json a = table_with_pk({"id"});
  a["referenced_by"] = json::array({"b_fk on public.b"});
  const auto obs = citus_observations(
      citus_obs(json{{"public.a", distributed("id", 5)},
                     {"public.b", distributed("id", 5)}}),
      json{{"public.a", a}});
  json in{{"kind", "citus_undistribute_table"}, {"schema", "public"}, {"table", "a"}};
  const auto refused = plan_citus(in, obs);
  EXPECT_FALSE(refused.ok);
  EXPECT_TRUE(conflicts_mention(refused, "cascade_via_foreign_keys"))
      << first_conflict(refused);
  in["cascade_via_foreign_keys"] = true;
  const auto p = plan_citus(in, obs);
  ASSERT_TRUE(p.ok) << first_conflict(p);
  EXPECT_EQ(p.steps[0].sql[0],
            "SELECT undistribute_table('public.a', cascade_via_foreign_keys := true);");
  EXPECT_EQ(p.steps[0].detail.value("cascades_to", json::array()),
            json::array({"public.b"}));
}

TEST(CitusPlanner, AnUndistributedTableCanBeDistributedAgainInTheSamePlan) {
  // The projection has to forget it, or the second intent reads "already
  // distributed" and does nothing.
  const auto obs = citus_observations(
      citus_obs(json{{"public.a", distributed("id", 5)}}),
      json{{"public.a", table_with_pk({"id"})}});
  const auto p = plan_citus_many(
      json::array({json{{"kind", "citus_undistribute_table"},
                        {"schema", "public"}, {"table", "a"}},
                   json{{"kind", "citus_distribute_table"}, {"schema", "public"},
                        {"table", "a"}, {"distribution_column", "id"}}}),
      obs);
  ASSERT_TRUE(p.ok) << first_conflict(p);
  ASSERT_EQ(p.steps.size(), 2u);
  EXPECT_EQ(p.steps[1].action, pglaswell::Action::kApply);
}

TEST(CitusPlanner, AddingALocalTableToMetadataReadsWhatCitusAlreadyHolds) {
  json in{{"kind", "citus_add_local_table_to_metadata"},
          {"schema", "public"}, {"table", "l"}};
  const auto tables = json{{"public.l", table_with_pk({"id"})}};

  const auto done = plan_citus(in, citus_observations(
      citus_obs(json{{"public.l", managed_local()}}), tables));
  ASSERT_TRUE(done.ok);
  EXPECT_EQ(done.steps[0].action, pglaswell::Action::kSatisfied);

  // Measured: "table is already distributed" -- for a reference table too.
  const auto ref = plan_citus(in, citus_observations(
      citus_obs(json{{"public.l", reference_table()}}), tables));
  EXPECT_FALSE(ref.ok);
  EXPECT_TRUE(conflicts_mention(ref, "already a reference table"))
      << first_conflict(ref);

  const auto fresh = plan_citus(in, citus_observations(citus_obs(), tables));
  ASSERT_TRUE(fresh.ok) << first_conflict(fresh);
  EXPECT_EQ(fresh.steps[0].sql[0],
            "SELECT citus_add_local_table_to_metadata('public.l');");
}

TEST(CitusPlanner, AddingALocalTableKeyedToAPlainTableNeedsTheCascade) {
  // Measured: "relation is involved in a foreign key relationship with another
  // table". A key to a reference table is the point of the kind, and needs none.
  json l = table_with_pk({"id"});
  l["constraints"]["l_p_fkey"] = json{{"type", "f"}, {"references", "p"}};
  json in{{"kind", "citus_add_local_table_to_metadata"},
          {"schema", "public"}, {"table", "l"}};
  const auto refused = plan_citus(
      in, citus_observations(citus_obs(), json{{"public.l", l}}));
  EXPECT_FALSE(refused.ok);
  EXPECT_TRUE(conflicts_mention(refused, "public.p")) << first_conflict(refused);

  const auto to_ref = plan_citus(
      in, citus_observations(citus_obs(json{{"public.p", reference_table()}}),
                             json{{"public.l", l}}));
  EXPECT_TRUE(to_ref.ok) << first_conflict(to_ref);
}

TEST(CitusPlanner, TruncatingLocalDataRefusesToCascadeIntoRealRows) {
  // Measured: the call truncates with CASCADE, and a local table referencing a
  // reference table was emptied -- its only copy of its rows.
  json r = table_with_pk({"id"});
  r["referenced_by"] = json::array({"lref_fkey on public.lref"});
  json in{{"kind", "citus_truncate_local_data"}, {"schema", "public"}, {"table", "r"}};
  const auto refused = plan_citus(
      in, citus_observations(citus_obs(json{{"public.r", reference_table()}}),
                             json{{"public.r", r}}));
  EXPECT_FALSE(refused.ok);
  EXPECT_TRUE(conflicts_mention(refused, "public.lref")) << first_conflict(refused);

  // Into a distributed table it reaches only the coordinator's stale copy --
  // measured, Citus still served every row.
  const auto ok = plan_citus(
      in, citus_observations(citus_obs(json{{"public.r", reference_table()},
                                            {"public.lref", distributed("id", 5)}}),
                             json{{"public.r", r}}));
  ASSERT_TRUE(ok.ok) << first_conflict(ok);
  EXPECT_EQ(ok.steps[0].sql[0],
            "SELECT truncate_local_data_after_distributing_table('public.r');");
}

TEST(CitusPlanner, TruncatingLocalDataOfATableCitusDoesNotDistributeIsRefused) {
  // Measured: "supplied parameter is not a distributed relation".
  json in{{"kind", "citus_truncate_local_data"}, {"schema", "public"}, {"table", "l"}};
  const auto p = plan_citus(in, citus_observations(
      citus_obs(json{{"public.l", managed_local()}}),
      json{{"public.l", table_with_pk({"id"})}}));
  EXPECT_FALSE(p.ok);
  EXPECT_TRUE(conflicts_mention(p, "only rows")) << first_conflict(p);
}

// --- Phase 3 ----------------------------------------------------------------

namespace {
json node(const char* name, int group, bool shards, long long placements,
          bool active = true) {
  return json{{"nodename", name}, {"nodeport", 5432}, {"groupid", group},
              {"nodeid", group + 1}, {"isactive", active}, {"noderole", "primary"},
              {"shouldhaveshards", shards}, {"placements", placements}};
}
// Coordinator plus two workers, balanced, workers at wal_level logical.
json cluster(json nodes = nullptr, json moves = json::array(),
             const char* wal = "logical") {
  json c = citus_obs();
  c["nodes"] = nodes.is_null()
                   ? json::array({node("coordinator", 0, false, 0),
                                  node("worker1", 1, true, 32),
                                  node("worker2", 2, true, 32)})
                   : nodes;
  c["rebalance_moves"] = std::move(moves);
  c["workers"] = json{{"worker1:5432", json{{"wal_level", wal}, {"citus", "13.2-1"},
                                             {"prepared", 0}, {"oldest_s", 0}}},
                      {"worker2:5432", json{{"wal_level", wal}, {"citus", "13.2-1"},
                                             {"prepared", 0}, {"oldest_s", 0}}}};
  return c;
}
pglaswell::Plan plan_topology(const json& intent, json c) {
  return plan_citus(intent, citus_observations(std::move(c), json::object()));
}
}  // namespace

TEST(CitusPlanner, AddingANodeThatIsThereIsSatisfied) {
  const json in{{"kind", "citus_add_node"}, {"host", "worker2"}};
  const auto p = plan_topology(in, cluster());
  ASSERT_TRUE(p.ok) << first_conflict(p);
  EXPECT_EQ(p.steps[0].action, pglaswell::Action::kSatisfied);

  const auto fresh = plan_topology(
      json{{"kind", "citus_add_node"}, {"host", "worker3"}, {"port", 5433},
           {"should_have_shards", false}},
      cluster());
  ASSERT_TRUE(fresh.ok) << first_conflict(fresh);
  ASSERT_EQ(fresh.steps[0].sql.size(), 2u);
  EXPECT_EQ(fresh.steps[0].sql[0], "SELECT citus_add_node('worker3', 5433);");
  EXPECT_EQ(fresh.steps[0].sql[1],
            "SELECT citus_set_node_property('worker3', 5433, 'shouldhaveshards', false);");
  // Measured: it connects as it runs, so executing it in the dry run finds an
  // unreachable host before the migration does.
  EXPECT_EQ(fresh.steps[0].detail.value("rehearse_by", ""), "execution");
}

TEST(CitusPlanner, RemovingANodeThatHoldsShardsIsRefusedWithWhatToDoFirst) {
  const json in{{"kind", "citus_remove_node"}, {"host", "worker2"}};
  const auto refused = plan_topology(in, cluster());
  EXPECT_FALSE(refused.ok);
  EXPECT_TRUE(conflicts_mention(refused, "citus_drain_node")) << first_conflict(refused);
  EXPECT_TRUE(conflicts_mention(refused, "32")) << first_conflict(refused);

  const auto drained = plan_topology(
      in, cluster(json::array({node("coordinator", 0, false, 0),
                               node("worker1", 1, true, 64),
                               node("worker2", 2, false, 0)})));
  ASSERT_TRUE(drained.ok) << first_conflict(drained);
  EXPECT_EQ(drained.steps[0].sql[0], "SELECT citus_remove_node('worker2', 5432);");

  const auto gone = plan_topology(
      json{{"kind", "citus_remove_node"}, {"host", "worker9"}}, cluster());
  ASSERT_TRUE(gone.ok);
  EXPECT_EQ(gone.steps[0].action, pglaswell::Action::kSatisfied);

  const auto coordinator = plan_topology(
      json{{"kind", "citus_remove_node"}, {"host", "coordinator"}}, cluster());
  EXPECT_FALSE(coordinator.ok);
  EXPECT_TRUE(conflicts_mention(coordinator, "citus_set_coordinator_host"));
}

TEST(CitusPlanner, TheCoordinatorHostIsReadFromGroupZero) {
  const auto same = plan_topology(
      json{{"kind", "citus_set_coordinator_host"}, {"host", "coordinator"}}, cluster());
  ASSERT_TRUE(same.ok);
  EXPECT_EQ(same.steps[0].action, pglaswell::Action::kSatisfied);
  // worker1 is a node, but not group 0: not the coordinator.
  const auto other = plan_topology(
      json{{"kind", "citus_set_coordinator_host"}, {"host", "worker1"}}, cluster());
  ASSERT_TRUE(other.ok);
  EXPECT_EQ(other.steps[0].action, pglaswell::Action::kApply);
}

TEST(CitusPlanner, ANodePropertyAlreadyAsAskedIsSatisfied) {
  const json off{{"kind", "citus_set_node_property"}, {"host", "worker2"},
                 {"should_have_shards", false}};
  const auto p = plan_topology(off, cluster());
  ASSERT_TRUE(p.ok);
  EXPECT_EQ(p.steps[0].action, pglaswell::Action::kApply);
  const auto done = plan_topology(
      off, cluster(json::array({node("worker2", 2, false, 0)})));
  EXPECT_EQ(done.steps[0].action, pglaswell::Action::kSatisfied);
  const auto absent = plan_topology(
      json{{"kind", "citus_set_node_property"}, {"host", "worker9"},
           {"should_have_shards", true}}, cluster());
  EXPECT_FALSE(absent.ok);
}

TEST(CitusPlanner, ADrainRunsOutsideATransactionAndIsNeverRehearsed) {
  // Measured: drain marks the node on a connection of its own, and the mark
  // survived a ROLLBACK even when the drain failed. Executing it in a dry run
  // would change the cluster.
  const json in{{"kind", "citus_drain_node"}, {"host", "worker2"}};
  const auto p = plan_topology(in, cluster());
  ASSERT_TRUE(p.ok) << first_conflict(p);
  EXPECT_EQ(p.steps[0].txn_class, pglaswell::TxnClass::kForbidden);
  EXPECT_FALSE(p.steps[0].detail.contains("rehearse_by"));
  EXPECT_EQ(p.steps[0].sql[0],
            "SELECT citus_drain_node('worker2', 5432, shard_transfer_mode := 'auto');");
  EXPECT_FALSE(p.steps[0].detail.value("on_failure", "").empty());

  const auto done = plan_topology(
      in, cluster(json::array({node("worker1", 1, true, 64),
                               node("worker2", 2, false, 0)})));
  EXPECT_EQ(done.steps[0].action, pglaswell::Action::kSatisfied);
  // Empty but still taking shards is not drained: a rebalance would fill it.
  const auto empty_open = plan_topology(
      in, cluster(json::array({node("worker1", 1, true, 64),
                               node("worker2", 2, true, 0)})));
  EXPECT_EQ(empty_open.steps[0].action, pglaswell::Action::kApply);

  const auto nowhere = plan_topology(
      in, cluster(json::array({node("coordinator", 0, false, 0),
                               node("worker2", 2, true, 32)})));
  EXPECT_FALSE(nowhere.ok);
  EXPECT_TRUE(conflicts_mention(nowhere, "nowhere to go")) << first_conflict(nowhere);
}

TEST(CitusPlanner, AMoveThatNeedsLogicalReplicationIsRefusedWithoutIt) {
  // Measured: on workers at wal_level = replica the default mode failed at the
  // first shard -- after the node had been marked.
  const auto rep = cluster(nullptr, json::array({json{{"shardid", 1}}}), "replica");
  const auto drain = plan_topology(
      json{{"kind", "citus_drain_node"}, {"host", "worker2"}}, rep);
  EXPECT_FALSE(drain.ok);
  EXPECT_TRUE(conflicts_mention(drain, "wal_level")) << first_conflict(drain);
  const auto rebalance = plan_topology(json{{"kind", "citus_rebalance_shards"}}, rep);
  EXPECT_FALSE(rebalance.ok);
  EXPECT_TRUE(conflicts_mention(rebalance, "block_writes")) << first_conflict(rebalance);

  const auto blocking = plan_topology(
      json{{"kind", "citus_rebalance_shards"}, {"transfer_mode", "block_writes"}}, rep);
  EXPECT_TRUE(blocking.ok) << first_conflict(blocking);
}

TEST(CitusPlanner, ARebalanceIsSatisfiedWhenCitusHasNoMovesToMake) {
  const json in{{"kind", "citus_rebalance_shards"}};
  const auto balanced = plan_topology(in, cluster());
  ASSERT_TRUE(balanced.ok);
  EXPECT_EQ(balanced.steps[0].action, pglaswell::Action::kSatisfied);

  const auto uneven = plan_topology(
      in, cluster(nullptr, json::array({json{{"table", "public.a"}, {"shardid", 7},
                                             {"from", "worker1:5432"},
                                             {"to", "worker2:5432"}}})));
  ASSERT_TRUE(uneven.ok) << first_conflict(uneven);
  EXPECT_EQ(uneven.steps[0].txn_class, pglaswell::TxnClass::kForbidden);
  EXPECT_EQ(uneven.steps[0].detail.value("moves", 0), 1);
  EXPECT_EQ(uneven.steps[0].sql[0],
            "SELECT rebalance_table_shards(shard_transfer_mode := 'auto');");

  // Citus could not be asked: fewer shard-holding nodes than the replication
  // factor, where the call raises.
  const auto unaskable = plan_topology(in, cluster(nullptr, json(nullptr)));
  EXPECT_FALSE(unaskable.ok);
  EXPECT_TRUE(conflicts_mention(unaskable, "replication")) << first_conflict(unaskable);
}

TEST(CitusPlanner, TopologyArgumentsAreCheckedAtParse) {
  EXPECT_ANY_THROW(pglaswell::parse_spec(citus_spec(
      json{{"kind", "citus_set_node_property"}, {"host", "w"}})));
  EXPECT_ANY_THROW(pglaswell::parse_spec(citus_spec(
      json{{"kind", "citus_add_node"}, {"host", "w"}, {"port", 70000}})));
  EXPECT_ANY_THROW(pglaswell::parse_spec(citus_spec(
      json{{"kind", "citus_drain_node"}, {"host", "w"}, {"transfer_mode", "fast"}})));
  EXPECT_ANY_THROW(pglaswell::parse_spec(citus_spec(
      json{{"kind", "citus_add_node"}, {"host", "w'; DROP"}})));
  // No kind names a shard by id: they differ per cluster.
  EXPECT_ANY_THROW(pglaswell::parse_spec(citus_spec(
      json{{"kind", "citus_move_shard_placement"}, {"shard_id", 102141}})));
}

// --- Workers from the configuration -----------------------------------------

namespace {
pglaswell::Plan plan_workers(const json& intent, json c, const std::string& workers) {
  const auto spec = pglaswell::parse_spec(citus_spec(intent));
  pglaswell::ExecutorConfig cfg;
  if (!workers.empty()) cfg.module_settings["citus.workers"] = workers;
  return pglaswell::plan_migration(spec, citus_observations(std::move(c), json::object()),
                                   cfg);
}
const json kEnsure{{"kind", "citus_ensure_workers"}};
}  // namespace

TEST(CitusPlanner, TheSameSpecificationAddsWhateverThisConnectionConfigures) {
  // The point of the kind: identical bytes, and the configuration of the
  // connection decides how many workers that is.
  const auto staging = plan_workers(kEnsure, cluster(), "worker1, worker2, worker3");
  ASSERT_TRUE(staging.ok) << first_conflict(staging);
  ASSERT_EQ(staging.steps[0].sql.size(), 1u);
  EXPECT_EQ(staging.steps[0].sql[0], "SELECT citus_add_node('worker3', 5432);");
  EXPECT_EQ(staging.steps[0].detail.value("rehearse_by", ""), "execution");

  const auto prod = plan_workers(
      kEnsure, cluster(), "worker1, worker2, w3:5433, w4:5433, w5:5433");
  ASSERT_TRUE(prod.ok) << first_conflict(prod);
  EXPECT_EQ(prod.steps[0].sql.size(), 3u);

  const auto done = plan_workers(kEnsure, cluster(), "worker1:5432, worker2");
  ASSERT_TRUE(done.ok);
  EXPECT_EQ(done.steps[0].action, pglaswell::Action::kSatisfied);
}

TEST(CitusPlanner, WithoutConfiguredWorkersTheKindSaysWhereToPutThem) {
  const auto p = plan_workers(kEnsure, cluster(), "");
  EXPECT_FALSE(p.ok);
  EXPECT_TRUE(conflicts_mention(p, "citus.workers")) << first_conflict(p);
}

TEST(CitusPlanner, TheSignedBoundsLimitWhatTheConfigurationMaySupply) {
  // The configuration is unsigned; a worker receives shards of the data.
  const json bounded{{"kind", "citus_ensure_workers"},
                     {"hosts_matching", "worker*"}, {"max_workers", 3}};
  EXPECT_TRUE(plan_workers(bounded, cluster(), "worker1, worker2, worker3").ok);
  const auto rogue = plan_workers(bounded, cluster(), "worker1, worker2, evil.example");
  EXPECT_FALSE(rogue.ok);
  EXPECT_TRUE(conflicts_mention(rogue, "evil.example")) << first_conflict(rogue);
  const auto many = plan_workers(bounded, cluster(), "worker1, worker2, worker3, worker4");
  EXPECT_FALSE(many.ok);
  EXPECT_TRUE(conflicts_mention(many, "max_workers")) << first_conflict(many);
  const auto few = plan_workers(
      json{{"kind", "citus_ensure_workers"}, {"min_workers", 3}}, cluster(),
      "worker1, worker2");
  EXPECT_FALSE(few.ok);
  EXPECT_TRUE(conflicts_mention(few, "min_workers")) << first_conflict(few);
}

TEST(CitusPlanner, AWorkerTheConfigurationDoesNotListIsKeptRefusedOrRemoved) {
  // worker2 is registered and unlisted; with shards, and without.
  const auto kept = plan_workers(kEnsure, cluster(), "worker1");
  ASSERT_TRUE(kept.ok);
  EXPECT_EQ(kept.steps[0].action, pglaswell::Action::kSatisfied);
  EXPECT_FALSE(kept.warnings.empty());

  const auto strict = plan_workers(
      json{{"kind", "citus_ensure_workers"}, {"unlisted", "refuse"}}, cluster(), "worker1");
  EXPECT_FALSE(strict.ok);
  EXPECT_TRUE(conflicts_mention(strict, "worker2:5432")) << first_conflict(strict);

  const json remove{{"kind", "citus_ensure_workers"}, {"unlisted", "remove"}};
  const auto holding = plan_workers(remove, cluster(), "worker1");
  EXPECT_FALSE(holding.ok);
  EXPECT_TRUE(conflicts_mention(holding, "citus_drain_node")) << first_conflict(holding);
  const auto drained = plan_workers(
      remove, cluster(json::array({node("coordinator", 0, false, 0),
                                   node("worker1", 1, true, 64),
                                   node("worker2", 2, false, 0)})),
      "worker1");
  ASSERT_TRUE(drained.ok) << first_conflict(drained);
  EXPECT_EQ(drained.steps[0].sql[0], "SELECT citus_remove_node('worker2', 5432);");
}

TEST(CitusPlanner, AConfiguredWorkerListThatCannotBeRightIsRefused) {
  EXPECT_FALSE(plan_workers(kEnsure, cluster(), "worker1, worker1:5432").ok);
  EXPECT_FALSE(plan_workers(kEnsure, cluster(), "worker1,, worker3").ok);
  EXPECT_FALSE(plan_workers(kEnsure, cluster(), "worker1:notaport").ok);
  const auto coord = plan_workers(kEnsure, cluster(), "coordinator, worker1");
  EXPECT_FALSE(coord.ok);
  EXPECT_TRUE(conflicts_mention(coord, "coordinator")) << first_conflict(coord);
}

TEST(CitusPlanner, EnsureWorkersArgumentsAreCheckedAtParse) {
  EXPECT_ANY_THROW(pglaswell::parse_spec(citus_spec(
      json{{"kind", "citus_ensure_workers"}, {"workers", "w1"}})));
  EXPECT_ANY_THROW(pglaswell::parse_spec(citus_spec(
      json{{"kind", "citus_ensure_workers"}, {"min_workers", 4}, {"max_workers", 2}})));
  EXPECT_ANY_THROW(pglaswell::parse_spec(citus_spec(
      json{{"kind", "citus_ensure_workers"}, {"unlisted", "drop"}})));
}

TEST(CitusPlanner, TheGlobIsStarAndQuestionMarkOnly) {
  EXPECT_TRUE(pglaswell::citus_glob("*.citus.internal", "w1.citus.internal"));
  EXPECT_FALSE(pglaswell::citus_glob("*.citus.internal", "w1.citus.internal.evil"));
  EXPECT_TRUE(pglaswell::citus_glob("10.20.30.?", "10.20.30.7"));
  EXPECT_FALSE(pglaswell::citus_glob("10.20.30.?", "10.20.30.77"));
  EXPECT_TRUE(pglaswell::citus_glob("worker*", "worker12"));
  EXPECT_FALSE(pglaswell::citus_glob("worker*", "aworker1"));
}

// --- Node membership projects, so one specification can build a cluster -----

namespace {
pglaswell::Plan plan_topology_many(const json& intents, json c,
                                   const std::string& workers = "") {
  const auto spec = pglaswell::parse_spec(citus_spec_many(intents));
  pglaswell::ExecutorConfig cfg;
  if (!workers.empty()) cfg.module_settings["citus.workers"] = workers;
  return pglaswell::plan_migration(spec, citus_observations(std::move(c), json::object()),
                                   cfg);
}
}  // namespace

TEST(CitusPlanner, OneSpecificationCanRegisterACluster) {
  // Reported from pgshard, exactly this specification: the second intent was
  // refused -- "not a node of this cluster" -- for a node the first adds.
  const auto p = plan_topology_many(
      json::array({json{{"kind", "citus_set_coordinator_host"}, {"host", "10.0.0.50"}},
                   json{{"kind", "citus_set_node_property"}, {"host", "10.0.0.50"},
                        {"should_have_shards", false}},
                   json{{"kind", "citus_ensure_workers"}, {"hosts_matching", "10.0.0.*"}}}),
      cluster(json::array(), json::array()), "10.0.0.51, 10.0.0.52");
  ASSERT_TRUE(p.ok) << first_conflict(p);
  ASSERT_EQ(p.steps.size(), 3u);
  EXPECT_EQ(p.steps[0].action, pglaswell::Action::kApply);
  // Citus registers the coordinator taking no shards, which is what was asked.
  EXPECT_EQ(p.steps[1].action, pglaswell::Action::kSatisfied) << p.steps[1].why;
  EXPECT_EQ(p.steps[2].sql.size(), 2u);
}

TEST(CitusPlanner, ALaterIntentSeesANodeAnEarlierOneAdds) {
  const auto p = plan_topology_many(
      json::array({json{{"kind", "citus_add_node"}, {"host", "worker3"}},
                   json{{"kind", "citus_set_node_property"}, {"host", "worker3"},
                        {"should_have_shards", false}}}),
      cluster());
  ASSERT_TRUE(p.ok) << first_conflict(p);
  EXPECT_EQ(p.steps[1].action, pglaswell::Action::kApply);
  EXPECT_EQ(p.steps[1].sql[0],
            "SELECT citus_set_node_property('worker3', 5432, 'shouldhaveshards', false);");
}

TEST(CitusPlanner, ARebalanceAfterAMembershipChangeRunsRatherThanReadingAStalePlan) {
  // The rebalance plan was read for the cluster before the new workers joined:
  // "no moves" then is not "balanced" afterwards.
  const auto p = plan_topology_many(
      json::array({json{{"kind", "citus_ensure_workers"}},
                   json{{"kind", "citus_rebalance_shards"}}}),
      cluster(), "worker1, worker2, worker3");
  ASSERT_TRUE(p.ok) << first_conflict(p);
  ASSERT_EQ(p.steps.size(), 2u);
  EXPECT_EQ(p.steps[1].action, pglaswell::Action::kApply) << p.steps[1].why;
  EXPECT_EQ(p.steps[1].txn_class, pglaswell::TxnClass::kForbidden);

  // Nothing changed membership: the reading still decides.
  const auto alone = plan_topology_many(
      json::array({json{{"kind", "citus_rebalance_shards"}}}), cluster());
  EXPECT_EQ(alone.steps[0].action, pglaswell::Action::kSatisfied);
}

TEST(CitusPlanner, ANodeDrainedEarlierInTheSpecificationCanBeRemoved) {
  const auto p = plan_topology_many(
      json::array({json{{"kind", "citus_drain_node"}, {"host", "worker2"}},
                   json{{"kind", "citus_remove_node"}, {"host", "worker2"}}}),
      cluster());
  ASSERT_TRUE(p.ok) << first_conflict(p);
  EXPECT_EQ(p.steps[1].sql[0], "SELECT citus_remove_node('worker2', 5432);");

  // And a removed node is gone for what follows.
  const auto gone = plan_topology_many(
      json::array({json{{"kind", "citus_drain_node"}, {"host", "worker2"}},
                   json{{"kind", "citus_remove_node"}, {"host", "worker2"}},
                   json{{"kind", "citus_set_node_property"}, {"host", "worker2"},
                        {"should_have_shards", true}}}),
      cluster());
  EXPECT_FALSE(gone.ok);
}

TEST(CitusPlanner, AWorkerEnsureWorkersAddsIsSeenByTheNextIntent) {
  const auto p = plan_topology_many(
      json::array({json{{"kind", "citus_ensure_workers"}},
                   json{{"kind", "citus_set_node_property"}, {"host", "worker3"},
                        {"should_have_shards", false}}}),
      cluster(), "worker1, worker2, worker3");
  ASSERT_TRUE(p.ok) << first_conflict(p);
  EXPECT_EQ(p.steps[1].action, pglaswell::Action::kApply) << p.steps[1].why;
}

// --- The pieces CITUS.md asked for, each from a measurement -----------------

namespace {
json distribute_intent(const char* table = "a") {
  return json{{"kind", "citus_distribute_table"}, {"schema", "public"},
              {"table", table}, {"distribution_column", "id"}};
}
}  // namespace

TEST(CitusPlanner, DistributingIntoAnEmptyClusterIsRefused) {
  // Measured: with nothing registered, create_distributed_table SUCCEEDS,
  // registers the coordinator as localhost with every shard on it, and every
  // citus_add_node afterwards is refused.
  const auto tables = json{{"public.a", table_with_pk({"id"})}};
  for (const json& in : {distribute_intent(),
                         json{{"kind", "citus_create_reference_table"},
                              {"schema", "public"}, {"table", "a"}},
                         json{{"kind", "citus_add_local_table_to_metadata"},
                              {"schema", "public"}, {"table", "a"}}}) {
    const auto p = plan_citus(in, citus_observations(cluster(json::array()), tables));
    EXPECT_FALSE(p.ok) << in.dump();
    EXPECT_TRUE(conflicts_mention(p, "localhost")) << first_conflict(p);
  }
  // Registering first, earlier in the same specification, is enough.
  const auto spec = pglaswell::parse_spec(citus_spec_many(json::array(
      {json{{"kind", "citus_set_coordinator_host"}, {"host", "coordinator"}},
       json{{"kind", "citus_ensure_workers"}}, distribute_intent()})));
  pglaswell::ExecutorConfig cfg;
  cfg.module_settings["citus.workers"] = "worker1, worker2";
  const auto ok = pglaswell::plan_migration(
      spec, citus_observations(cluster(json::array()), tables), cfg);
  EXPECT_TRUE(ok.ok) << first_conflict(ok);
}

TEST(CitusPlanner, DistributingWhereNoNodeMayHoldShardsIsRefused) {
  // Measured: "replication_factor (1) exceeds number of worker nodes (0)".
  const auto p = plan_citus(distribute_intent(), citus_observations(
      cluster(json::array({node("coordinator", 0, false, 0)})),
      json{{"public.a", table_with_pk({"id"})}}));
  EXPECT_FALSE(p.ok);
  EXPECT_TRUE(conflicts_mention(p, "nowhere to be distributed")) << first_conflict(p);
}

TEST(CitusPlanner, AddingAWorkerToALocalhostCoordinatorIsRefusedUntilItIsRegistered) {
  const auto nodes = json::array({node("localhost", 0, true, 32)});
  const auto p = plan_citus(json{{"kind", "citus_add_node"}, {"host", "worker1"}},
                            citus_observations(cluster(nodes), json::object()));
  EXPECT_FALSE(p.ok);
  EXPECT_TRUE(conflicts_mention(p, "citus_set_coordinator_host")) << first_conflict(p);
  const auto fixed = plan_topology_many(
      json::array({json{{"kind", "citus_set_coordinator_host"}, {"host", "coordinator"}},
                   json{{"kind", "citus_add_node"}, {"host", "worker1"}}}),
      cluster(nodes));
  EXPECT_TRUE(fixed.ok) << first_conflict(fixed);
}

TEST(CitusPlanner, TheStepThatCopiesReferenceTablesSaysSo) {
  // Measured: adding a node copies none; the next distribute copied them all.
  json c = cluster(json::array({node("coordinator", 0, false, 0),
                                node("worker1", 1, true, 32),
                                node("worker2", 2, true, 0)}));
  c["reference_tables"] = json{{"count", 2}, {"bytes", 227540992}};
  for (auto& n : c["nodes"]) {
    n["reference_placements"] = n["nodename"] == "worker2" ? 0 : 2;
  }
  const auto p = plan_citus(distribute_intent(), citus_observations(
      c, json{{"public.a", table_with_pk({"id"})}}));
  ASSERT_TRUE(p.ok) << first_conflict(p);
  EXPECT_EQ(p.steps[0].detail.value("reference_copy_to", json::array()),
            json::array({"worker2:5432"}));
  EXPECT_EQ(p.steps[0].detail.value("reference_bytes", 0LL), 227540992);

  const auto add = plan_citus(json{{"kind", "citus_add_node"}, {"host", "worker3"}},
                              citus_observations(c, json::object()));
  ASSERT_TRUE(add.ok);
  bool warned = false;
  for (const auto& w : add.warnings) warned = warned || w.find("reference") != std::string::npos;
  EXPECT_TRUE(warned);
}

TEST(CitusPlanner, TheConcurrentFormIsChosenOnlyWhenNothingRulesItOut) {
  // It is never rehearsed and does not roll back: measured, a failed one left
  // the table half converted. So what fails it is read first.
  json big = table_with_pk({"id"}, 5000000);
  big["columns"]["id"]["not_null"] = true;
  const auto tables = json{{"public.a", big}};
  json logical = cluster(nullptr, json::array(), "logical");
  logical["coordinator"] = json{{"wal_level", "logical"}, {"prepared", 0}, {"oldest_s", 0}};
  json replica = logical;
  replica["coordinator"]["wal_level"] = "replica";

  const auto ok = plan_citus(distribute_intent(), citus_observations(logical, tables));
  ASSERT_TRUE(ok.ok) << first_conflict(ok);
  EXPECT_TRUE(ok.steps[0].detail.value("concurrent", false));

  // The COORDINATOR's wal_level decides too (measured: it raised there).
  const auto fell_back = plan_citus(distribute_intent(), citus_observations(replica, tables));
  ASSERT_TRUE(fell_back.ok);
  EXPECT_FALSE(fell_back.steps[0].detail.value("concurrent", true));
  EXPECT_NE(fell_back.steps[0].why.find("coordinator is at wal_level replica"),
            std::string::npos) << fell_back.steps[0].why;

  json always = distribute_intent();
  always["concurrently"] = "always";
  const auto refused = plan_citus(always, citus_observations(replica, tables));
  EXPECT_FALSE(refused.ok);
  EXPECT_TRUE(conflicts_mention(refused, "half converted")) << first_conflict(refused);

  // A nullable distribution column rules it out: a NULL fails the copy halfway.
  json nullable = big;
  nullable["columns"]["id"]["not_null"] = false;
  const auto null_fell_back = plan_citus(
      distribute_intent(), citus_observations(logical, json{{"public.a", nullable}}));
  ASSERT_TRUE(null_fell_back.ok);
  EXPECT_FALSE(null_fell_back.steps[0].detail.value("concurrent", true));
  bool warned = false;
  for (const auto& w : null_fell_back.warnings) {
    warned = warned || w.find("refuses every write that leaves it NULL") != std::string::npos;
  }
  EXPECT_TRUE(warned);
}

TEST(CitusPlanner, AWorkerOnAnotherCitusVersionRefusesThePlan) {
  json c = cluster();
  c["workers"]["worker2:5432"]["citus"] = "12.1-1";
  const auto p = plan_citus(distribute_intent(), citus_observations(
      c, json{{"public.a", table_with_pk({"id"})}}));
  EXPECT_FALSE(p.ok);
  EXPECT_TRUE(conflicts_mention(p, "worker2:5432 (12.1-1)")) << first_conflict(p);

  json missing = cluster();
  missing["workers"]["worker1:5432"]["citus"] = nullptr;
  EXPECT_FALSE(plan_citus(distribute_intent(), citus_observations(
      missing, json{{"public.a", table_with_pk({"id"})}})).ok);

  // A worker that did not answer is unknown, not wrong.
  json silent = cluster();
  silent["workers"]["worker1:5432"] = nullptr;
  EXPECT_TRUE(plan_citus(distribute_intent(), citus_observations(
      silent, json{{"public.a", table_with_pk({"id"})}})).ok);
}

TEST(CitusPlanner, PreparedTransactionsNobodyResolvesRefuseThePlan) {
  // Measured: one held its locks (an INSERT waited on it indefinitely); Citus
  // resolves its own every recover_2pc_interval.
  const auto tables = json{{"public.a", table_with_pk({"id"})}};
  json c = cluster();
  c["settings"]["recover_2pc_interval"] = "1min";
  c["workers"]["worker1:5432"]["prepared"] = 2;
  c["workers"]["worker1:5432"]["oldest_s"] = 30;
  EXPECT_TRUE(plan_citus(distribute_intent(), citus_observations(c, tables)).ok)
      << "a young one is Citus's business, still inside its recovery window";
  c["workers"]["worker1:5432"]["oldest_s"] = 900;
  const auto p = plan_citus(distribute_intent(), citus_observations(c, tables));
  EXPECT_FALSE(p.ok);
  EXPECT_TRUE(conflicts_mention(p, "worker1:5432 (2, the oldest 900 s)")) << first_conflict(p);
  // The window follows the setting: at 10min, 900 s is still inside it.
  c["settings"]["recover_2pc_interval"] = "10min";
  EXPECT_TRUE(plan_citus(distribute_intent(), citus_observations(c, tables)).ok);
}
