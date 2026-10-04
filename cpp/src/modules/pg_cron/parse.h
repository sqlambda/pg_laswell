#pragma once
// pg_cron parsers. Included inside namespace pglaswell by spec.h.
//
// What is checked here is only what needs no server: the keys, and that each
// is the right shape. Whether a schedule is one pg_cron understands is its
// parser's business -- measured, "invalid schedule: not a schedule", raised by
// the call itself -- and the dry run executes the call, so a bad schedule is
// refused before anything is applied (plan.h).

inline void pg_cron_require_text(const Intent& in, const std::string& key,
                                 const std::string& at, bool required) {
  if (!in.body.contains(key)) {
    if (required) detail::fail(at + " needs \"" + key + "\"", "");
    return;
  }
  const auto& v = in.body[key];
  if (!v.is_string() || v.get<std::string>().empty()) {
    detail::fail(at + "." + key + " must be a non-empty string", "");
  }
}

inline void parse_pg_cron_schedule(Intent& in) {
  const auto at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(in.body,
                              {"kind", "name", "schedule", "command", "database",
                               "username", "active", "validate_command", "comment"},
                              at);
  // A name is required: pg_cron allows anonymous jobs, but a job with no name
  // cannot be found again by the next run, so scheduling it twice makes two.
  pg_cron_require_text(in, "name", at, true);
  pg_cron_require_text(in, "schedule", at, true);
  pg_cron_require_text(in, "command", at, true);
  // The database the job RUNS in, which is not necessarily the one pg_cron
  // keeps it in. Omitted: the database this specification targets.
  pg_cron_require_text(in, "database", at, false);
  // The role the job runs as. Omitted: the role applying the specification.
  pg_cron_require_text(in, "username", at, false);
  // Whether the dry run checks the command. true unless said otherwise; false
  // is for a command naming what does not exist yet and is not this
  // repository's to create.
  if (in.body.contains("validate_command") && !in.body["validate_command"].is_boolean()) {
    detail::fail(at + ".validate_command must be a boolean",
                 "true (the default) has the dry run check the command's syntax "
                 "and that the tables, functions and procedures it names exist; "
                 "false schedules it unchecked.");
  }
  if (in.body.contains("active") && !in.body["active"].is_boolean()) {
    detail::fail(at + ".active must be a boolean",
                 "false schedules the job without running it until it is "
                 "scheduled again with active true.");
  }
  pg_cron_require_text(in, "comment", at, false);
}

inline void parse_pg_cron_unschedule(Intent& in) {
  const auto at = "intents[" + std::to_string(in.ordinal) + "]";
  detail::reject_unknown_keys(in.body, {"kind", "name", "username", "comment"}, at);
  pg_cron_require_text(in, "name", at, true);
  pg_cron_require_text(in, "username", at, false);
  pg_cron_require_text(in, "comment", at, false);
}
