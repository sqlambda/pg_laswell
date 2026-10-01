#pragma once
// Plan-level refusals for pg_cron: the one CORE kind pg_cron constrains is
// create_extension, which succeeds only in the database cron.database_name
// names, and only when the library is loaded. A refusal only (THE RULE,
// ../README.md); core still emits the CREATE EXTENSION. Measured on
// PostgreSQL 18.6 with pg_cron 1.6.8. Included inside namespace pglaswell.
template <typename Refuse, typename Advise>
inline void pg_cron_plan_refusals(const Spec& spec, const Observations& obs,
                                  const json& /*budget*/, const Refuse& refuse,
                                  const Advise& advise) {
  for (const auto& in : spec.intents) {
    if (in.kind != IntentKind::kCreateExtension ||
        in.body.value("name", "") != "pg_cron") {
      continue;
    }
    const auto& cron = obs.extension("pg_cron");
    if (cron.empty()) {
      refuse("create_extension pg_cron: pg_cron is not loaded on this server, so "
             "the extension script fails -- PostgreSQL: \"unrecognized "
             "configuration parameter \\\"cron.database_name\\\"\". Add pg_cron to "
             "shared_preload_libraries and restart first.");
      continue;
    }
    // Where pg_cron belongs cannot be read by this role: the server decides.
    if (!cron.value("settings_readable", true)) continue;
    const auto here = cron.value("current_database", "");
    const auto home = cron.value("database_name", "");
    if (here != home) {
      refuse("create_extension pg_cron: pg_cron can only be created in database " +
             home + " (cron.database_name), and this specification targets " + here +
             ". pg_cron: \"can only create extension in database " + home +
             "\". Create it from a specification targeting " + home +
             ", and schedule jobs for " + here + " there with \"database\": \"" +
             here + "\".");
    }
  }

  const auto& cron = obs.extension("pg_cron");
  if (!cron.value("installed", false)) return;
  const auto jobs = cron.value("jobs", json::object());
  const auto me = cron.value("current_user", "");
  const bool super = cron.value("superuser", false);

  // A JOB EDITED BY HAND. The ledger's applied steps (read by core, newest
  // first) say what each job was last declared to be; cron.job says what it
  // is. pg_cron's table is writable by the job's role -- cron.schedule and
  // cron.unschedule by name, a direct UPDATE -- so a job can drift from its
  // specification without any migration being involved. Said on ANY plan
  // against this database, not only when the spec that declared the job is
  // planned again. A role that is not a superuser sees only its own jobs, so
  // only those are compared.
  std::set<std::string> seen;
  for (const auto& st : cron.value("applied_steps", json::array())) {
    const auto action = st.value("action", "");
    if (action != "apply" && action != "satisfied") continue;
    const auto d = st.value("detail", json::object());
    const auto user = d.value("username", "");
    const auto job = d.value("job", "");
    const auto key = user + "/" + job;
    if (job.empty() || !seen.insert(key).second) continue;  // newest only
    if (user != me && !super) continue;
    const bool exists = jobs.contains(key);
    if (st.value("kind", "") == "pg_cron_unschedule") {
      if (exists) {
        advise("job " + job + " (" + user + ") was unscheduled by an applied "
               "specification and is in cron.job again: scheduled by hand since.");
      }
      continue;
    }
    // A step recorded before the declared fields were (0.1.3-alpha1) cannot be
    // compared, and saying nothing is the honest answer.
    if (!d.contains("schedule")) continue;
    if (!exists) {
      advise("job " + job + " (" + user + ") was scheduled by an applied "
             "specification (" + d.value("schedule", "") + ") and is no longer in "
             "cron.job: unscheduled by hand since. Re-applying will not restore "
             "it; a specification that schedules it again will.");
      continue;
    }
    const auto& j = jobs[key];
    std::vector<std::string> changed;
    if (j.value("schedule", "") != d.value("schedule", "")) {
      changed.push_back("schedule " + d.value("schedule", "") + " -> " +
                        j.value("schedule", ""));
    }
    if (j.value("command", "") != d.value("command", "")) changed.push_back("command");
    if (j.value("database", "") != d.value("database", "")) {
      changed.push_back("database " + d.value("database", "") + " -> " +
                        j.value("database", ""));
    }
    if (j.value("active", true) != d.value("active", true)) {
      changed.push_back(j.value("active", true) ? "activated" : "deactivated");
    }
    if (!changed.empty()) {
      advise("job " + job + " (" + user + ") differs from what an applied "
             "specification declared: " + detail::join(changed, ", ") +
             ", changed by hand since.");
    }
  }

  // A JOB WHOSE LAST RUN FAILED, for the jobs this specification touches.
  // From the most recent 1000 runs (observe.h), so a job that has not run in
  // that window says nothing.
  const auto runs = cron.value("last_runs", json::object());
  for (const auto& in : spec.intents) {
    if (in.kind != IntentKind::kPgCronSchedule && in.kind != IntentKind::kPgCronUnschedule) {
      continue;
    }
    const auto user = in.body.value("username", me);
    const auto key = user + "/" + in.body.value("name", "");
    if (!jobs.contains(key)) continue;
    const auto id = std::to_string(jobs[key].value("jobid", 0LL));
    if (!runs.contains(id)) continue;
    const auto& r = runs[id];
    if (r.value("status", "") != "failed") continue;
    std::string message = r.value("message", "");
    while (!message.empty() && (message.back() == '\n' || message.back() == ' ')) {
      message.pop_back();
    }
    advise("job " + in.body.value("name", "") + "'s last run failed: " + message + " (" +
           std::to_string(r.value("failures", 0LL)) + " of its last " +
           std::to_string(r.value("runs", 0LL)) + " runs in the most recent 1000 "
           "failed; cron.job_run_details has them).");
  }
}

