// pg_cron intent kinds. One line each: the enum arm, the name->kind entry, the
// parse dispatch and the planner dispatch are all generated from here.
//
//   PGLASWELL_KIND(spec_name, EnumId, parse_fn, plan_fn)
//
// A job is identified the way pg_cron identifies it -- by its name and the
// role it runs as (cron.job's unique index is jobname, username) -- so a
// specification names jobs, never job ids, which come from a sequence and
// differ on every server.
PGLASWELL_KIND(pg_cron_schedule,   kPgCronSchedule,   parse_pg_cron_schedule,   plan_pg_cron_schedule)
PGLASWELL_KIND(pg_cron_unschedule, kPgCronUnschedule, parse_pg_cron_unschedule, plan_pg_cron_unschedule)
