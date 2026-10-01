#pragma once
// pg_cron planners. PURE, like every planner: (Intent, Observations,
// ExecutorConfig) in, steps out. Included inside namespace pglaswell.
//
// Both kinds plan the same way whichever layout the server uses, because the
// layout is a reading rather than a choice the specification makes:
//
//   pg_cron in the application database (cron.database_name = app): a spec
//   targeting app schedules its jobs there, in the same transaction as the
//   rest of the spec and its ledger row.
//
//   pg_cron in a maintenance database (cron.database_name = postgres, the
//   default): jobs can only be scheduled FROM postgres. A spec targeting app
//   is refused, naming postgres; the jobs go in a spec whose target.connection
//   is postgres, with "database": "app" saying where each one runs, and
//   depends_on the spec that creates what the command calls. The repository
//   already crosses databases this way (repository.h): a change is
//   single-database, and may require a change on another.
//
// Measured on PostgreSQL 18.6 with pg_cron 1.6.8 (2026-10-01).

// Can a job be scheduled from the database this spec targets? Says why not,
// and returns false when the step is finished.
inline bool pg_cron_usable(const Intent& in, const Observations& obs, Step& step,
                           Plan& plan, const json& cron) {
  const auto refuse = [&](std::string why) {
    step.action = Action::kConflict;
    step.why = std::move(why);
    plan.conflicts.push_back(step.why);
  };
  if (cron.empty()) {
    refuse(in.kind_name + " needs pg_cron, and it is not loaded on this server: "
           "cron.database_name is not a setting here. Add pg_cron to "
           "shared_preload_libraries and restart, then CREATE EXTENSION pg_cron "
           "in the database cron.database_name names (postgres unless it is set).");
    plan.prerequisites.push_back(json{
        {"kind", "extension"},
        {"target", "pg_cron"},
        {"where", "postgresql.conf, then the database cron.database_name names"},
        {"requirement",
         "shared_preload_libraries = 'pg_cron' and a restart; CREATE EXTENSION pg_cron"},
        {"verify", "SHOW cron.database_name"},
        {"blocking", true}});
    return false;
  }
  const auto here = cron.value("current_database", "");
  // Unreadable matters only where pg_cron is NOT installed: where it is,
  // this is its database (the reading says so without the setting).
  if (!cron.value("installed", false) && !cron.value("settings_readable", true)) {
    refuse("pg_cron is not installed in " + here + ", and " +
           cron.value("current_user", "") + " cannot read cron.database_name to "
           "say where it is: the setting needs the privileges of "
           "pg_read_all_settings. GRANT pg_read_all_settings TO " +
           detail::quote_identifier(cron.value("current_user", "")) +
           "; or target the database pg_cron keeps its jobs in.");
    return false;
  }
  const auto home = cron.value("database_name", "");
  if (here != home) {
    refuse("pg_cron keeps its jobs in database " + home + " (cron.database_name), "
           "and this specification targets " + here + ". pg_cron: \"Jobs must be "
           "scheduled from the database configured in cron.database_name\". Put "
           "this intent in a specification whose target.connection is a "
           "connection to " + home + ", with \"database\": \"" + here +
           "\" for a job that runs in " + here + ".");
    return false;
  }
  if (!cron.value("installed", false) &&
      !obs.object("extension:pg_cron").value("exists", false)) {
    refuse("this is the database pg_cron keeps its jobs in, but the extension is "
           "not installed: add a create_extension of pg_cron before this intent, "
           "or run CREATE EXTENSION pg_cron here.");
    return false;
  }
  return true;
}

inline std::string pg_cron_job_key(const std::string& username, const std::string& name) {
  return username + "/" + name;
}

inline void plan_pg_cron_schedule(const Intent& in, const Observations& obs,
                                  const ExecutorConfig& cfg, Plan& plan,
                                  std::vector<Step>& out) {
  (void)cfg;
  Step step;
  step.kind = in.kind_name;
  struct Emit { std::vector<Step>& o; Step& s; ~Emit() { o.push_back(s); } } emit{out, step};
  const auto& cron = obs.extension("pg_cron");
  if (!pg_cron_usable(in, obs, step, plan, cron)) return;
  const auto refuse = [&](std::string why) {
    step.action = Action::kConflict;
    step.why = std::move(why);
    plan.conflicts.push_back(step.why);
  };

  const auto name = in.body.value("name", "");
  const auto schedule = in.body.value("schedule", "");
  const auto command = in.body.value("command", "");
  const auto here = cron.value("current_database", "");
  const auto me = cron.value("current_user", "");
  const auto database = in.body.value("database", here);
  const auto username = in.body.value("username", me);
  const bool active = in.body.value("active", true);
  const bool super = cron.value("superuser", false);
  step.detail["job"] = name;
  step.detail["username"] = username;
  step.detail["database"] = database;

  if (username != me && !super) {
    refuse("job " + name + " would run as " + username + ", and " + me +
           " is not a superuser. pg_cron: \"must be superuser to create a job for "
           "another role\". Omit username to run it as " + me +
           ", or apply this specification as a superuser.");
    return;
  }
  // Only on a full reading: a pg_cron created earlier in this same
  // specification has not been read, and its databases are not listed.
  const auto dbs = cron.value("databases", json::object());
  if (!dbs.empty() && !dbs.contains(database)) {
    refuse("job " + name + " runs in database " + database +
           ", which does not exist on this server. pg_cron: \"database \\\"" +
           database + "\\\" does not exist\".");
    return;
  }
  if (username == me && dbs.contains(database) && !dbs[database].get<bool>()) {
    refuse("job " + name + " runs in database " + database + " as " + me +
           ", who may not connect to it. pg_cron: \"User " + me +
           " does not have CONNECT privilege on " + database + "\".");
    return;
  }
  // cron.schedule is granted to PUBLIC and schedules into THIS database, as
  // the caller, active. Anything else is schedule_in_database, which is not
  // (measured: "permission denied for function schedule_in_database").
  const bool plain_call = database == here && username == me && active;
  if (!plain_call && !super && !cron.value("can_schedule_in_database", false)) {
    refuse("job " + name + " needs cron.schedule_in_database" +
           (database != here ? " to run in database " + database
                             : std::string(" to be scheduled inactive")) +
           ", which " + me + " may not call: it is not granted to PUBLIC. "
           "As a superuser: GRANT EXECUTE ON FUNCTION "
           "cron.schedule_in_database(text,text,text,text,text,boolean) TO " +
           detail::quote_identifier(me) + ";");
    return;
  }

  const auto jobs = cron.value("jobs", json::object());
  const auto key = pg_cron_job_key(username, name);
  const bool exists = jobs.contains(key);
  if (exists) {
    const auto& j = jobs[key];
    if (j.value("schedule", "") == schedule && j.value("command", "") == command &&
        j.value("database", "") == database && j.value("active", true) == active) {
      step.action = Action::kSatisfied;
      step.why = "job " + name + " is already scheduled as declared (" + schedule +
                 ", in " + database + " as " + username + ")";
      return;
    }
  }

  step.action = Action::kApply;
  step.txn_class = TxnClass::kRequired;
  // Rehearsed by execution: measured, a cron.schedule rolled back leaves no
  // row in cron.job -- and an invalid schedule is refused by the call itself,
  // so the dry run is what checks the schedule.
  step.detail["rehearse_by"] = "execution";
  step.lock = "no table lock: one row in cron.job, which the pg_cron launcher "
              "reads once the transaction commits";
  if (exists) {
    const auto& j = jobs[key];
    std::vector<std::string> changes;
    if (j.value("schedule", "") != schedule) {
      changes.push_back("schedule " + j.value("schedule", "") + " -> " + schedule);
    }
    if (j.value("command", "") != command) changes.push_back("command");
    if (j.value("database", "") != database) {
      changes.push_back("database " + j.value("database", "") + " -> " + database);
    }
    if (j.value("active", true) != active) {
      changes.push_back(active ? "activated" : "deactivated");
    }
    // Same name, same role: pg_cron replaces the job in place and keeps its id
    // (measured: jobid 1 before and after).
    step.why = "job " + name + " exists and changes: " + detail::join(changes, ", ");
    step.detail["jobid"] = j.value("jobid", 0LL);
  } else {
    step.why = "schedules job " + name + " (" + schedule + ") to run in " + database +
               " as " + username + (active ? "" : ", inactive");
  }
  if (plain_call) {
    step.sql.push_back("SELECT cron.schedule(" + detail::quote_literal(name) + ", " +
                       detail::quote_literal(schedule) + ", " +
                       detail::quote_literal(command) + ");");
  } else {
    step.sql.push_back("SELECT cron.schedule_in_database(" + detail::quote_literal(name) +
                       ", " + detail::quote_literal(schedule) + ", " +
                       detail::quote_literal(command) + ", " +
                       detail::quote_literal(database) + ", " +
                       detail::quote_literal(username) + ", " +
                       (active ? "true" : "false") + ");");
  }
  plan.warnings.push_back(
      "job " + name + ": pg_cron stores the command as text and first runs it at "
      "its schedule, so nothing here checks it. A failure is recorded in "
      "cron.job_run_details, not in this plan.");
}

inline void plan_pg_cron_unschedule(const Intent& in, const Observations& obs,
                                    const ExecutorConfig& cfg, Plan& plan,
                                    std::vector<Step>& out) {
  (void)cfg;
  Step step;
  step.kind = in.kind_name;
  struct Emit { std::vector<Step>& o; Step& s; ~Emit() { o.push_back(s); } } emit{out, step};
  const auto& cron = obs.extension("pg_cron");
  if (!pg_cron_usable(in, obs, step, plan, cron)) return;

  const auto name = in.body.value("name", "");
  const auto me = cron.value("current_user", "");
  const auto username = in.body.value("username", me);
  step.detail["job"] = name;
  step.detail["username"] = username;
  if (username != me && !cron.value("superuser", false)) {
    // Row-level security hides another role's jobs from a non-superuser, so
    // the job cannot even be found (measured: "could not find valid entry for
    // job").
    step.action = Action::kConflict;
    step.why = "job " + name + " belongs to " + username + ", and " + me +
               " is not a superuser: pg_cron shows each role only its own jobs.";
    plan.conflicts.push_back(step.why);
    return;
  }
  const auto jobs = cron.value("jobs", json::object());
  const auto key = pg_cron_job_key(username, name);
  if (!jobs.contains(key)) {
    // Measured: unscheduling a name that does not exist RAISES, so a re-run
    // needs this read as done rather than attempted.
    step.action = Action::kSatisfied;
    step.why = "no job " + name + " for " + username + ": nothing to unschedule";
    return;
  }
  const auto& j = jobs[key];
  step.action = Action::kApply;
  step.txn_class = TxnClass::kRequired;
  step.detail["rehearse_by"] = "execution";
  step.detail["jobid"] = j.value("jobid", 0LL);
  step.lock = "no table lock: one row in cron.job";
  step.why = "unschedules job " + name + " (" + j.value("schedule", "") + ", in " +
             j.value("database", "") + " as " + username + ")";
  if (username == me) {
    step.sql.push_back("SELECT cron.unschedule(" + detail::quote_literal(name) + ");");
  } else {
    // By id, found by name and role in the statement itself: the name form
    // only finds the caller's own jobs.
    step.sql.push_back("SELECT cron.unschedule(jobid) FROM cron.job WHERE jobname = " +
                       detail::quote_literal(name) + " AND username = " +
                       detail::quote_literal(username) + ";");
  }
}
