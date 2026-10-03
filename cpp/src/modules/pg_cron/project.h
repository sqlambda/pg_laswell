#pragma once
// What a pg_cron step leaves for the steps after it, in the same plan: the
// job list, so scheduling a job and changing it again two intents later --
// or unscheduling it -- is planned against the jobs as the earlier intent
// leaves them. Receives the module's own slot only (see ../README.md).
// Included inside namespace pglaswell.

inline void pg_cron_project_schedule(const Intent& in, const std::string&,
                                     const Step& step, json& mine) {
  if (mine.empty()) return;
  const auto me = mine.value("current_user", "");
  const auto username = in.body.value("username", me);
  const auto name = in.body.value("name", "");
  mine["jobs"][username + "/" + name] = json{
      {"jobid", 0},
      {"schedule", in.body.value("schedule", "")},
      {"command", in.body.value("command", "")},
      {"database", in.body.value("database", mine.value("current_database", ""))},
      {"username", username},
      {"active", in.body.value("active", true)},
      {"projected_by_step", step.ordinal}};
}

inline void pg_cron_project_unschedule(const Intent& in, const std::string&,
                                       const Step&, json& mine) {
  if (mine.empty() || !mine.contains("jobs")) return;
  const auto username = in.body.value("username", mine.value("current_user", ""));
  mine["jobs"].erase(username + "/" + in.body.value("name", ""));
}
