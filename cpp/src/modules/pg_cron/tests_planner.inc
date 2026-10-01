// pg_cron planner tests. No server: readings shaped exactly as observe.h
// produces them, in the two layouts the module supports. Every refusal quotes
// an error measured on PostgreSQL 18.6 with pg_cron 1.6.8;
// cpp/test/extensions-tests.sh runs the kinds against both layouts for real.
namespace {

// pg_cron installed in the database this spec targets.
json cron_home(const char* db = "app", bool super = true, json jobs = json::object(),
               bool grant = false) {
  return json{{"installed", true},
              {"version", "1.6"},
              {"database_name", db},
              {"current_database", db},
              {"current_user", super ? "postgres" : "runner"},
              {"superuser", super},
              {"can_schedule_in_database", grant},
              {"databases", json{{"postgres", true}, {"app", true}, {"locked", false}}},
              {"jobs", std::move(jobs)}};
}

// Loaded, but kept in another database: what kpg_cronAbsentSql reads.
json cron_elsewhere(const char* here = "app", const char* home = "postgres") {
  return json{{"installed", false},
              {"database_name", home},
              {"current_database", here},
              {"current_user", "postgres"},
              {"superuser", true}};
}

pglaswell::Observations cron_observations(json cron) {
  pglaswell::Observations obs;
  obs.server_version = 180000;
  obs.server = json{{"max_connections", 100}, {"current_backends", 5}};
  if (!cron.is_null()) obs.extensions["pg_cron"] = std::move(cron);
  return obs;
}

json cron_job(const char* name = "nightly", const char* schedule = "0 3 * * *") {
  return json{{"kind", "pg_cron_schedule"}, {"name", name}, {"schedule", schedule},
              {"command", "CALL app.purge_expired()"}};
}

pglaswell::Plan cron_plan(json intents, const pglaswell::Observations& obs) {
  if (!intents.is_array()) intents = json::array({std::move(intents)});
  const auto spec = pglaswell::parse_spec(json{{"laswell_spec_version", 1},
                                              {"id", "pg-cron-test"},
                                              {"description", "a pg_cron planner test"},
                                              {"intents", std::move(intents)}});
  return pglaswell::plan_migration(spec, obs, pglaswell::ExecutorConfig{});
}

std::string cron_conflict(const pglaswell::Plan& p, const std::string& needle) {
  for (const auto& c : p.conflicts) {
    if (c.find(needle) != std::string::npos) return c;
  }
  return "";
}

}  // namespace

TEST(PgCronPlanner, InTheApplicationsOwnDatabaseAJobIsAPlainSchedule) {
  const auto p = cron_plan(cron_job(), cron_observations(cron_home()));
  ASSERT_TRUE(p.ok) << p.render();
  ASSERT_EQ(p.steps.size(), 1u);
  const auto& s = p.steps[0];
  EXPECT_EQ(s.action, pglaswell::Action::kApply);
  // Transactional: the job and the ledger row commit together.
  EXPECT_EQ(s.txn_class, pglaswell::TxnClass::kRequired);
  EXPECT_EQ(s.detail.value("rehearse_by", ""), "execution");
  EXPECT_EQ(s.sql.at(0),
            "SELECT cron.schedule('nightly', '0 3 * * *', 'CALL app.purge_expired()');");
}

TEST(PgCronPlanner, FromADatabasePgCronDoesNotLiveInTheRefusalNamesWhereItDoes) {
  const auto p = cron_plan(cron_job(), cron_observations(cron_elsewhere()));
  EXPECT_FALSE(p.ok);
  const auto r = cron_conflict(p, "keeps its jobs in database postgres");
  ASSERT_FALSE(r.empty()) << p.render();
  EXPECT_NE(r.find("target.connection is a connection to postgres"), std::string::npos) << r;
  EXPECT_NE(r.find("\"database\": \"app\""), std::string::npos) << r;
}

TEST(PgCronPlanner, FromTheMaintenanceDatabaseAJobRunsInTheApplicationDatabase) {
  auto job = cron_job();
  job["database"] = "app";
  const auto p = cron_plan(job, cron_observations(cron_home("postgres")));
  ASSERT_TRUE(p.ok) << p.render();
  EXPECT_EQ(p.steps[0].sql.at(0),
            "SELECT cron.schedule_in_database('nightly', '0 3 * * *', "
            "'CALL app.purge_expired()', 'app', 'postgres', true);");
}

TEST(PgCronPlanner, ScheduleInDatabaseNeedsAGrantANonSuperuserRarelyHas) {
  auto job = cron_job();
  job["database"] = "app";
  const auto refused = cron_plan(job, cron_observations(cron_home("postgres", false)));
  const auto r = cron_conflict(refused, "not granted to PUBLIC");
  ASSERT_FALSE(r.empty()) << refused.render();
  EXPECT_NE(r.find("GRANT EXECUTE ON FUNCTION cron.schedule_in_database"), std::string::npos);
  // With the grant, it plans.
  EXPECT_TRUE(cron_plan(job, cron_observations(
                                 cron_home("postgres", false, json::object(), true))).ok);
  // And a job scheduled inactive needs the same call, even in this database.
  auto inactive = cron_job();
  inactive["active"] = false;
  EXPECT_FALSE(cron_conflict(cron_plan(inactive, cron_observations(cron_home("app", false))),
                             "to be scheduled inactive")
                   .empty());
}

TEST(PgCronPlanner, ANonSuperuserCannotScheduleForAnotherRole) {
  auto job = cron_job();
  job["username"] = "postgres";
  EXPECT_FALSE(cron_conflict(cron_plan(job, cron_observations(cron_home("app", false))),
                             "must be superuser to create a job for another role")
                   .empty());
}

TEST(PgCronPlanner, ADatabaseTheJobCannotReachIsRefused) {
  auto job = cron_job();
  job["database"] = "nope";
  EXPECT_FALSE(cron_conflict(cron_plan(job, cron_observations(cron_home("postgres"))),
                             "does not exist")
                   .empty());
  auto locked = cron_job();
  locked["database"] = "locked";
  EXPECT_FALSE(cron_conflict(cron_plan(locked, cron_observations(
                                                   cron_home("postgres", true))),
                             "does not have CONNECT privilege on locked")
                   .empty());
}

TEST(PgCronPlanner, AJobScheduledAsDeclaredIsSatisfiedAndAChangeSaysWhat) {
  const json existing = {{"postgres/nightly",
                          {{"jobid", 7}, {"schedule", "0 3 * * *"},
                           {"command", "CALL app.purge_expired()"}, {"database", "app"},
                           {"username", "postgres"}, {"active", true}}}};
  const auto same = cron_plan(cron_job(), cron_observations(cron_home("app", true, existing)));
  ASSERT_TRUE(same.ok);
  EXPECT_EQ(same.steps[0].action, pglaswell::Action::kSatisfied) << same.render();

  const auto changed = cron_plan(cron_job("nightly", "0 4 * * *"),
                                 cron_observations(cron_home("app", true, existing)));
  ASSERT_TRUE(changed.ok);
  EXPECT_EQ(changed.steps[0].action, pglaswell::Action::kApply);
  EXPECT_NE(changed.steps[0].why.find("schedule 0 3 * * * -> 0 4 * * *"), std::string::npos)
      << changed.steps[0].why;
}

TEST(PgCronPlanner, WithoutPgCronLoadedNothingIsScheduledAndThePrerequisiteIsNamed) {
  const auto p = cron_plan(cron_job(), cron_observations(json()));
  EXPECT_FALSE(cron_conflict(p, "not loaded on this server").empty()) << p.render();
  ASSERT_FALSE(p.prerequisites.empty());
  EXPECT_EQ(p.prerequisites[0].value("target", ""), "pg_cron");
}

TEST(PgCronPlanner, TheExtensionAndItsFirstJobInOneSpecification) {
  // Loaded, this is its database, not created yet: the spec creates it first.
  auto loaded = cron_elsewhere("app", "app");
  const auto p = cron_plan(
      json::array({json{{"kind", "create_extension"}, {"name", "pg_cron"}}, cron_job()}),
      cron_observations(loaded));
  ASSERT_TRUE(p.ok) << p.render();
  // Without the create_extension, refused.
  EXPECT_FALSE(cron_conflict(cron_plan(cron_job(), cron_observations(loaded)),
                             "extension is not installed")
                   .empty());
}

TEST(PgCronPlanner, CreatingTheExtensionOutsideItsDatabaseIsRefused) {
  const auto p = cron_plan(json{{"kind", "create_extension"}, {"name", "pg_cron"}},
                           cron_observations(cron_elsewhere()));
  EXPECT_FALSE(cron_conflict(p, "can only create extension in database postgres").empty())
      << p.render();
  const auto unloaded = cron_plan(json{{"kind", "create_extension"}, {"name", "pg_cron"}},
                                  cron_observations(json()));
  EXPECT_FALSE(cron_conflict(unloaded, "unrecognized configuration parameter").empty())
      << unloaded.render();
}

TEST(PgCronPlanner, UnscheduleIsDoneWhenTheJobIsGoneAndByIdForAnotherRole) {
  EXPECT_EQ(cron_plan(json{{"kind", "pg_cron_unschedule"}, {"name", "nightly"}},
                      cron_observations(cron_home()))
                .steps[0]
                .action,
            pglaswell::Action::kSatisfied);
  const json theirs = {{"runner/nightly",
                        {{"jobid", 9}, {"schedule", "0 3 * * *"}, {"command", "SELECT 1"},
                         {"database", "app"}, {"username", "runner"}, {"active", true}}}};
  const auto p = cron_plan(
      json{{"kind", "pg_cron_unschedule"}, {"name", "nightly"}, {"username", "runner"}},
      cron_observations(cron_home("app", true, theirs)));
  ASSERT_TRUE(p.ok) << p.render();
  EXPECT_EQ(p.steps[0].sql.at(0),
            "SELECT cron.unschedule(jobid) FROM cron.job WHERE jobname = 'nightly' "
            "AND username = 'runner';");
}

TEST(PgCronPlanner, AJobScheduledEarlierInTheSameSpecificationCanBeUnscheduled) {
  const auto p = cron_plan(
      json::array({cron_job(), json{{"kind", "pg_cron_unschedule"}, {"name", "nightly"}}}),
      cron_observations(cron_home()));
  ASSERT_TRUE(p.ok) << p.render();
  ASSERT_EQ(p.steps.size(), 2u);
  EXPECT_EQ(p.steps[1].action, pglaswell::Action::kApply)
      << "the second intent must see the job the first schedules";
}

TEST(PgCronPlanner, ARoleThatCannotReadTheSettingIsToldWhichGrantItNeeds) {
  // Measured: cron.database_name is readable only with pg_read_all_settings,
  // and this reading runs for every plan on the server -- so it must not fail,
  // and a pg_cron kind must say what is missing.
  const json unreadable = {{"installed", false}, {"settings_readable", false},
                           {"current_database", "app"}, {"current_user", "runner"},
                           {"superuser", false}};
  const auto p = cron_plan(cron_job(), cron_observations(unreadable));
  EXPECT_FALSE(cron_conflict(p, "GRANT pg_read_all_settings TO \"runner\"").empty())
      << p.render();
  // Where pg_cron IS installed, the setting is not needed: this is its
  // database. An unprivileged role schedules its own job there.
  auto installed = cron_home("app", false);
  installed["settings_readable"] = false;
  EXPECT_TRUE(cron_plan(cron_job(), cron_observations(installed)).ok);
  // And a create_extension is left to the server, not refused on a guess.
  const auto e = cron_plan(json{{"kind", "create_extension"}, {"name", "pg_cron"}},
                           cron_observations(unreadable));
  EXPECT_TRUE(cron_conflict(e, "pg_cron").empty()) << e.render();
}

TEST(PgCronSpec, AJobNeedsANameAScheduleAndACommand) {
  for (const char* missing : {"name", "schedule", "command"}) {
    auto job = cron_job();
    job.erase(missing);
    EXPECT_THROW(pglaswell::parse_spec(json{{"laswell_spec_version", 1},
                                            {"id", "x"},
                                            {"description", "x"},
                                            {"intents", json::array({job})}}),
                 std::exception)
        << missing;
  }
}
