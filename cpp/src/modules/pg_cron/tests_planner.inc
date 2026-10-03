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

// --- 2026-10-01: other roles, jobs edited by hand, failed runs ---------------

namespace {
bool cron_advised(const pglaswell::Plan& p, const std::string& needle) {
  for (const auto& a : p.advisories) {
    if (a.find(needle) != std::string::npos) return true;
  }
  return false;
}
json cron_with_roles() {
  auto c = cron_home();
  c["roles"] = json{{"postgres", {{"login", true}, {"connect", {"app", "postgres"}}}},
                    {"reporter", {{"login", true}, {"connect", {"postgres"}}}},
                    {"group_role", {{"login", false}, {"connect", {"app"}}}}};
  return c;
}
json applied_schedule(const char* schedule, const char* action = "apply") {
  return json{{"kind", "pg_cron_schedule"}, {"action", action},
              {"detail", {{"job", "nightly"}, {"username", "postgres"}, {"database", "app"},
                          {"schedule", schedule}, {"command", "CALL app.purge_expired()"},
                          {"active", true}}}};
}
}  // namespace

TEST(PgCronPlanner, AJobForAnotherRoleIsCheckedAsPgCronWouldCheckIt) {
  auto job = cron_job();
  job["username"] = "nobody";
  EXPECT_FALSE(cron_conflict(cron_plan(job, cron_observations(cron_with_roles())),
                             "there is no such role").empty());
  job["username"] = "group_role";
  EXPECT_FALSE(cron_conflict(cron_plan(job, cron_observations(cron_with_roles())),
                             "LOGIN attribute").empty());
  job["username"] = "reporter";
  EXPECT_FALSE(cron_conflict(cron_plan(job, cron_observations(cron_with_roles())),
                             "User reporter does not have CONNECT privilege on app").empty());
  job["database"] = "postgres";
  // reporter may connect to postgres, but pg_cron here is kept in app: the job
  // runs elsewhere, which needs schedule_in_database -- a superuser has it.
  EXPECT_TRUE(cron_plan(job, cron_observations(cron_with_roles())).ok);
}

TEST(PgCronPlanner, AJobEditedByHandIsNoticedOnAnyPlanAgainstTheDatabase) {
  auto c = cron_home("app", true,
                     json{{"postgres/nightly", {{"jobid", 7}, {"schedule", "0 5 * * *"},
                                                {"command", "CALL app.purge_expired()"},
                                                {"database", "app"}, {"username", "postgres"},
                                                {"active", true}}}});
  // Newest first, as core reads them: the job was last declared at 03:00.
  c["applied_steps"] = json::array({applied_schedule("0 3 * * *"),
                                     applied_schedule("0 1 * * *")});
  // An unrelated specification: a plain table. The advisory still appears.
  const auto p = cron_plan(json{{"kind", "create_schema"}, {"schema", "other"},
                                {"comment", "Unrelated."}},
                           cron_observations(c));
  EXPECT_TRUE(cron_advised(p, "schedule 0 3 * * * -> 0 5 * * *")) << p.render();

  // Unscheduled by hand.
  c["jobs"] = json::object();
  EXPECT_TRUE(cron_advised(cron_plan(cron_job(), cron_observations(c)),
                           "no longer in cron.job"));
  // As declared: nothing to say.
  c["jobs"] = json{{"postgres/nightly", {{"jobid", 7}, {"schedule", "0 3 * * *"},
                                         {"command", "CALL app.purge_expired()"},
                                         {"database", "app"}, {"username", "postgres"},
                                         {"active", true}}}};
  EXPECT_TRUE(cron_plan(cron_job(), cron_observations(c)).advisories.empty());
}

TEST(PgCronPlanner, ATouchedJobWhoseLastRunFailedSaysWhy) {
  auto c = cron_home("app", true,
                     json{{"postgres/nightly", {{"jobid", 7}, {"schedule", "0 3 * * *"},
                                                {"command", "CALL app.purge_expired()"},
                                                {"database", "app"}, {"username", "postgres"},
                                                {"active", true}}}});
  c["last_runs"] = json{{"7", {{"status", "failed"}, {"message", "ERROR:  division by zero\n"},
                               {"failures", 3}, {"runs", 4}}}};
  const auto p = cron_plan(cron_job("nightly", "0 4 * * *"), cron_observations(c));
  ASSERT_TRUE(p.ok) << p.render();
  EXPECT_TRUE(cron_advised(p, "last run failed: ERROR:  division by zero (3 of its last 4"))
      << p.render();
}

TEST(PgCronPlanner, AScheduleStepRecordsWhatItDeclares) {
  const auto p = cron_plan(cron_job(), cron_observations(cron_home()));
  ASSERT_TRUE(p.ok);
  EXPECT_EQ(p.steps[0].detail.value("schedule", ""), "0 3 * * *");
  EXPECT_EQ(p.steps[0].detail.value("command", ""), "CALL app.purge_expired()");
  EXPECT_TRUE(p.steps[0].detail.value("active", false));
}

// --- 2026-10-03: the command is checked without being run --------------------

TEST(PgCronPlanner, TheCommandIsCheckedInTheRehearsalWhereTheJobRunsHere) {
  const auto p = cron_plan(cron_job(), cron_observations(cron_home()));
  ASSERT_TRUE(p.ok);
  const auto& s = p.steps[0];
  // What runs is unchanged: one statement. The check is rehearsal-only.
  ASSERT_EQ(s.sql.size(), 1u);
  ASSERT_TRUE(s.detail.contains("rehearse_only")) << s.detail.dump();
  EXPECT_FALSE(s.detail.contains("rehearse_elsewhere"));
  const auto check = s.detail["rehearse_only"];
  ASSERT_EQ(check.size(), 2u);
  EXPECT_EQ(check[0], "SET LOCAL check_function_bodies = on;");
  EXPECT_EQ(check[1],
            "CREATE OR REPLACE PROCEDURE pg_temp.laswell_cron_check() LANGUAGE sql AS "
            "$laswell_cmd$ CALL app.purge_expired() $laswell_cmd$;");

  // The rehearsal runs the step's statement and then the check; the real run
  // has only the first.
  const auto inputs = pglaswell::detail::rehearsal_inputs(p);
  ASSERT_EQ(inputs.steps.size(), 1u);
  ASSERT_EQ(inputs.steps[0].second.size(), 3u);
  EXPECT_NE(inputs.steps[0].second[2].find("pg_temp.laswell_cron_check"), std::string::npos);
  EXPECT_TRUE(inputs.elsewhere.empty());
}

TEST(PgCronPlanner, AJobForAnotherDatabaseIsCheckedThere) {
  auto job = cron_job();
  job["database"] = "app";
  const auto p = cron_plan(job, cron_observations(cron_home("postgres")));
  ASSERT_TRUE(p.ok);
  const auto& s = p.steps[0];
  EXPECT_FALSE(s.detail.contains("rehearse_only"));
  ASSERT_TRUE(s.detail.contains("rehearse_elsewhere"));
  EXPECT_EQ(s.detail["rehearse_elsewhere"].value("database", ""), "app");
  const auto inputs = pglaswell::detail::rehearsal_inputs(p);
  ASSERT_EQ(inputs.elsewhere.size(), 1u);
  EXPECT_EQ(inputs.elsewhere[0].value("step", -1), s.ordinal);
  EXPECT_EQ(inputs.steps[0].second.size(), 1u) << "nothing extra runs in the spec's own database";
}

TEST(PgCronPlanner, ValidateCommandFalseSchedulesItUncheckedAndSaysSo) {
  auto job = cron_job();
  job["validate_command"] = false;
  const auto p = cron_plan(job, cron_observations(cron_home()));
  ASSERT_TRUE(p.ok);
  EXPECT_FALSE(p.steps[0].detail.contains("rehearse_only"));
  bool said = false;
  for (const auto& w : p.warnings) {
    if (w.find("validate_command is false") != std::string::npos) said = true;
  }
  EXPECT_TRUE(said);
}

TEST(PgCronPlanner, ACommandCannotCloseTheQuotingItIsCheckedIn) {
  // The command is the body of a dollar-quoted string. One that contains the
  // tag would end the string early and run whatever followed it.
  auto job = cron_job();
  job["command"] = "SELECT 1 $laswell_cmd$; DROP TABLE x; --";
  const auto p = cron_plan(job, cron_observations(cron_home()));
  ASSERT_TRUE(p.ok);
  const auto check = p.steps[0].detail["rehearse_only"][1].get<std::string>();
  EXPECT_NE(check.find("AS $laswell_cmd_$ SELECT 1 $laswell_cmd$; DROP TABLE x; -- $laswell_cmd_$;"),
            std::string::npos)
      << check;
}
