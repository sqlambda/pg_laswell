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
  // What this step declares, recorded in the ledger's copy of the plan: the
  // guard compares cron.job with the newest of these to notice a job edited by
  // hand, on any later plan against this database (guard.h).
  step.detail["schedule"] = schedule;
  step.detail["command"] = command;
  step.detail["active"] = active;

  if (username != me && !super) {
    refuse("job " + name + " would run as " + username + ", and " + me +
           " is not a superuser. pg_cron: \"must be superuser to create a job for "
           "another role\". Omit username to run it as " + me +
           ", or apply this specification as a superuser.");
    return;
  }
  // Another role: pg_cron checks it exists, may log in, and may connect to the
  // job's database -- measured, "role \"nobody\" does not exist", "role
  // \"nologin\" can not log in", "User noconn does not have CONNECT privilege
  // on app". Only a superuser gets here, and only a superuser's reading lists
  // the roles.
  const auto roles = cron.value("roles", json());
  if (username != me && roles.is_object()) {
    if (!roles.contains(username)) {
      refuse("job " + name + " would run as " + username +
             ", and there is no such role. pg_cron: \"role \\\"" + username +
             "\\\" does not exist\".");
      return;
    }
    const auto& r = roles[username];
    if (!r.value("login", false)) {
      refuse("job " + name + " would run as " + username + ", which cannot log in. "
             "pg_cron: \"Jobs may only be run by roles that have the LOGIN "
             "attribute.\"");
      return;
    }
    const auto connect = r.value("connect", json::array());
    if (std::find(connect.begin(), connect.end(), json(database)) == connect.end()) {
      refuse("job " + name + " runs in database " + database + " as " + username +
             ", who may not connect to it. pg_cron: \"User " + username +
             " does not have CONNECT privilege on " + database + "\".");
      return;
    }
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
  // THE ZONE THE SCHEDULE IS READ IN. A cron expression names an hour, and
  // which hour that is belongs to cron.timezone, a server setting (GMT unless
  // set) that has nothing to do with the database's or the session's TimeZone.
  // Part of the step, and so of the plan's digest: on a server with another
  // zone the same specification schedules another moment. An interval
  // schedule ("30 seconds") names no hour and gets no zone.
  const bool interval_schedule = schedule.find("second") != std::string::npos;
  if (!interval_schedule) {
    if (!cron.value("settings_readable", true)) {
      step.why += "; the zone the schedule is read in (cron.timezone) could not be read: " +
                  me + " lacks pg_read_all_settings";
    } else {
      const auto& tz = cron.contains("timezone") ? cron["timezone"] : json();
      const std::string zone = tz.is_string() ? tz.get<std::string>() : "GMT";
      step.detail["timezone"] = zone;
      step.why += "; the schedule is read in " + zone +
                  (tz.is_string() ? " (cron.timezone)"
                                  : " (this pg_cron has no cron.timezone setting)");
    }
  }

  // HOW THE JOB WILL REACH ITS DATABASE. pg_cron refuses, when the job is
  // scheduled, a role that cannot log in, a database that does not exist and a
  // role without CONNECT -- all refused above, first. What it cannot check is
  // authentication: it connects to cron.host as the job's role, and a role
  // pg_hba does not let in is found at the first run, as "connection failed"
  // and nothing more (measured). pg_hba is not read here -- matching its rules
  // against a host name is a guess -- so what is said is what is known: the
  // setting, and whether a job has lately connected as this role to this
  // database. The history is volatile, so it is an advisory; a connection
  // that last FAILED is a warning.
  if (cron.contains("use_background_workers") && cron["use_background_workers"].is_boolean() &&
      cron["use_background_workers"].get<bool>()) {
    step.why += "; it runs in a background worker (cron.use_background_workers), so "
                "no connection is made and pg_hba is not involved";
  } else if (active) {
    const auto host = cron.contains("host") && cron["host"].is_string()
                          ? cron["host"].get<std::string>() : std::string();
    const std::string to = host.empty() ? "cron.host" : host + " (cron.host)";
    const auto& seen = cron.contains("connections") ? cron["connections"] : json();
    const auto pair = database + "/" + username;
    if (seen.is_object() && seen.contains(pair)) {
      const auto& c = seen[pair];
      std::string message = c.value("message", "");
      while (!message.empty() && (message.back() == '\n' || message.back() == ' ')) {
        message.pop_back();
      }
      if (c.value("status", "") == "failed" && message == "connection failed") {
        plan.warnings.push_back(
            "job " + name + ": the last job that ran in " + database + " as " + username +
            " could not connect (\"connection failed\", " + c.value("start_time", "") +
            "). pg_cron connects to " + to + " as that role; pg_hba.conf must let it "
            "in without a password, or the server's .pgpass must hold one. This job "
            "would fail the same way.");
      } else {
        plan.advisories.push_back(
            "job " + name + ": a job has connected to " + database + " as " + username +
            " (last run " + c.value("start_time", "") + "), so pg_cron can reach it "
            "through " + to + ".");
      }
    } else {
      plan.advisories.push_back(
          "job " + name + ": no job has run in " + database + " as " + username +
          " in the recent history, so whether pg_cron can connect is not known here. "
          "It connects to " + to + " as that role; pg_hba.conf must let it in without "
          "a password, or the server's .pgpass must hold one. A refused connection "
          "shows as \"connection failed\" in cron.job_run_details at the first run.");
    }
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
  // THE COMMAND, CHECKED WITHOUT BEING RUN. pg_cron stores it as text and
  // first runs it at its schedule, so a misspelt procedure is otherwise found
  // at 03:00. A SQL-language procedure with the command as its body is parsed
  // and analysed when it is created (check_function_bodies), and never called:
  // measured, that catches a syntax error, a table a DML statement names that
  // does not exist, and a procedure a CALL names that does not exist, with
  // PostgreSQL's own message. PREPARE cannot do it (it rejects CALL and every
  // utility command), and a BEGIN ATOMIC body rejects CALL. A utility command
  // (VACUUM, REFRESH) is checked for syntax only. In pg_temp, in the dry run's
  // transaction, rolled back.
  //
  // Where the job runs in THIS database the check is part of the rehearsal,
  // so it sees what this specification, and in a chain the ones before it,
  // create. Where it runs in another, the dry run connects there as the same
  // role and checks against that database as it is now.
  if (in.body.value("validate_command", true)) {
    std::string tag = "laswell_cmd";
    while (command.find("$" + tag + "$") != std::string::npos) tag += "_";
    const json check = json::array(
        {"SET LOCAL check_function_bodies = on;",
         "CREATE OR REPLACE PROCEDURE pg_temp.laswell_cron_check() LANGUAGE sql AS $" + tag +
             "$ " + command + " $" + tag + "$;"});
    if (database == here) {
      step.detail["rehearse_only"] = check;
    } else {
      step.detail["rehearse_elsewhere"] = json{{"database", database}, {"sql", check}};
    }
    plan.warnings.push_back(
        "job " + name + ": the dry run checks its command in " + database +
        " -- syntax, and that the tables, functions and procedures it names exist"
        " -- as " + me + ", without running it. It first RUNS at its schedule" +
        (username != me ? ", as " + username + ", whose privileges and search_path are "
                              "not what was checked" : std::string()) +
        "; a failure then is recorded in cron.job_run_details.");
  } else {
    plan.warnings.push_back(
        "job " + name + ": validate_command is false, so nothing here checks the "
        "command. pg_cron stores it as text and first runs it at its schedule; a "
        "failure is recorded in cron.job_run_details, not in this plan.");
  }
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
