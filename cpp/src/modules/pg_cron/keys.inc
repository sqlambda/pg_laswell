// pg_cron's contributions to conflict_keys(). Two specifications that schedule
// or unschedule the same job name must not run at once: pg_cron replaces a
// job by name, so the second would silently decide what the first did. Keyed
// on the name alone, because the role a job runs as may be the applying one,
// which no parser can see.
#ifdef PGLASWELL_CONFLICT_KEYS
PGLASWELL_CONFLICT_KEYS(kPgCronSchedule, { add("pg_cron:job:" + in.body.value("name", "")); })
PGLASWELL_CONFLICT_KEYS(kPgCronUnschedule, { add("pg_cron:job:" + in.body.value("name", "")); })
#endif
