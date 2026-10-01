#pragma once
// Plan-level refusals for pg_cron: the one CORE kind pg_cron constrains is
// create_extension, which succeeds only in the database cron.database_name
// names, and only when the library is loaded. A refusal only (THE RULE,
// ../README.md); core still emits the CREATE EXTENSION. Measured on
// PostgreSQL 18.6 with pg_cron 1.6.8. Included inside namespace pglaswell.
template <typename Refuse>
inline void pg_cron_plan_refusals(const Spec& spec, const Observations& obs,
                                  const Refuse& refuse) {
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
}
