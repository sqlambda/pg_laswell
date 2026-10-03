#pragma once
// pg_cron readings. Included inside namespace pglaswell by catalog.h.
//
// pg_cron is loaded per SERVER (shared_preload_libraries) and installed in ONE
// database per server, the one cron.database_name names: measured, CREATE
// EXTENSION pg_cron anywhere else raises "can only create extension in
// database postgres". Jobs live in that database's cron.job and run in
// whichever database each one names.
//
// So there are three readings, not two:
//   absent key           -- the library is not loaded: cron.database_name is
//                           not even a setting (measured: NULL from
//                           current_setting(..., true));
//   installed = false    -- loaded, but this is not where pg_cron keeps jobs,
//                           or it is and CREATE EXTENSION has not run yet;
//   installed = true     -- the full reading below.
// The second comes from kpg_cronAbsentSql, because the full reading names
// cron.job and does not parse where the cron schema does not exist.
//
// cron.database_name is NOT readable by every role. Measured: a role that is
// not a superuser gets "permission denied to examine \"cron.database_name\""
// from current_setting -- even with missing_ok -- unless it has the privileges
// of pg_read_all_settings, and pg_settings simply omits the row. This reading
// runs for EVERY plan on such a server, whatever the specification says, so
// reading the setting unguarded would have failed every plan an ordinary
// migration role makes there. It is read only when pg_has_role says it can
// be, and `settings_readable` says which happened.

inline const char* kpg_cronPresentSql =
    "SELECT EXISTS (SELECT 1 FROM pg_extension WHERE extname = 'pg_cron')";

// Jobs are read as the applying role sees them. cron.job has row-level
// security (measured: policy cron_job_policy, username = CURRENT_USER), so a
// role that is not a superuser sees its own jobs and no others -- which is
// also exactly the set it can change. Keyed "username/jobname", pg_cron's own
// identity (unique index jobname_username_uniq).
inline const char* kpg_cronObservationSql = R"SQL(
SELECT JSONB_BUILD_OBJECT(
  'installed', true,
  'version', (SELECT extversion FROM pg_extension WHERE extname = 'pg_cron'),
  'settings_readable', pg_has_role('pg_read_all_settings', 'USAGE'),
  -- Unreadable: the database pg_cron is installed in IS its database, because
  -- CREATE EXTENSION pg_cron succeeds nowhere else.
  'database_name', CASE WHEN pg_has_role('pg_read_all_settings', 'USAGE')
                        THEN current_setting('cron.database_name', true)
                        ELSE current_database() END,
  'current_database', current_database(),
  'current_user', current_user,
  'superuser', (SELECT rolsuper FROM pg_roles WHERE rolname = current_user),
  -- The zone a cron expression is read in, and how a job reaches its
  -- database. All three are refused to a role without pg_read_all_settings
  -- (measured: "permission denied to examine \"cron.timezone\""), so they are
  -- absent then, and settings_readable says why. With background workers no
  -- connection is made at all; otherwise pg_cron connects to cron.host as the
  -- job's role, and pg_hba decides whether that works.
  'timezone', CASE WHEN pg_has_role('pg_read_all_settings', 'USAGE')
                   THEN current_setting('cron.timezone', true) END,
  'host', CASE WHEN pg_has_role('pg_read_all_settings', 'USAGE')
               THEN current_setting('cron.host', true) END,
  'use_background_workers',
     CASE WHEN pg_has_role('pg_read_all_settings', 'USAGE')
          THEN current_setting('cron.use_background_workers', true) = 'on' END,
  -- Not granted to PUBLIC, unlike cron.schedule (measured: "permission denied
  -- for function schedule_in_database" until GRANT EXECUTE). Guarded by
  -- to_regprocedure because releases before 1.4 do not have it.
  'can_schedule_in_database',
     CASE WHEN to_regprocedure('cron.schedule_in_database(text,text,text,text,text,boolean)') IS NULL
          THEN false
          ELSE has_function_privilege(
                 to_regprocedure('cron.schedule_in_database(text,text,text,text,text,boolean)')::oid,
                 'EXECUTE') END,
  -- Which databases exist, and whether the applying role may connect to each:
  -- pg_cron checks the second when the job is scheduled (measured: "User
  -- runner does not have CONNECT privilege on app").
  'databases', COALESCE((SELECT JSONB_OBJECT_AGG(datname,
                                  has_database_privilege(datname, 'CONNECT'))
                           FROM pg_database WHERE datallowconn), '{}'::jsonb),
  -- Every role a job could run as, for a superuser only: only a superuser may
  -- schedule a job for a role other than its own, so only then is it read.
  -- Whether it may log in and which databases it may connect to are what
  -- pg_cron checks when the job is scheduled.
  'roles', CASE WHEN (SELECT rolsuper FROM pg_roles WHERE rolname = current_user)
           THEN COALESCE((SELECT JSONB_OBJECT_AGG(r.rolname, JSONB_BUILD_OBJECT(
                  'login', r.rolcanlogin,
                  'connect', COALESCE((SELECT JSONB_AGG(d.datname ORDER BY d.datname)
                                         FROM pg_database d
                                        WHERE d.datallowconn
                                          AND has_database_privilege(r.oid, d.oid, 'CONNECT')),
                                      '[]'::jsonb)))
                  FROM pg_roles r WHERE r.rolname !~ '^pg_'), '{}'::jsonb) END,
  -- The last run of each job, from the most recent 1000 runs only:
  -- job_run_details grows until something purges it, is indexed on runid and
  -- nothing else (measured), and keeps runs of jobs since unscheduled. Shown as
  -- an advisory when a specification touches a job whose last run failed.
  -- Row-level security limits it to the applying role's runs, as for cron.job.
  'last_runs', COALESCE((SELECT JSONB_OBJECT_AGG(x.jobid::text, JSONB_BUILD_OBJECT(
                   'status', x.status, 'message', x.return_message,
                   'start_time', x.start_time, 'failures', x.failures, 'runs', x.runs))
                 FROM (SELECT DISTINCT ON (jobid) jobid, status, return_message, start_time,
                              count(*) FILTER (WHERE status = 'failed')
                                OVER (PARTITION BY jobid) AS failures,
                              count(*) OVER (PARTITION BY jobid) AS runs
                         FROM (SELECT jobid, runid, status, return_message, start_time
                                 FROM cron.job_run_details
                                ORDER BY runid DESC LIMIT 1000) recent
                        ORDER BY jobid, runid DESC) x), '{}'::jsonb),
  -- Whether a job has lately connected as a role to a database, from the same
  -- most recent 1000 runs: the newest run for each pair. A role pg_hba does
  -- not let in shows only here, and only once a job has run (measured:
  -- status failed, return_message "connection failed", nothing more).
  'connections', COALESCE((SELECT JSONB_OBJECT_AGG(x.database || '/' || x.username,
                     JSONB_BUILD_OBJECT('status', x.status, 'message', x.return_message,
                                        'start_time', x.start_time))
                   FROM (SELECT DISTINCT ON (database, username)
                                database, username, status, return_message, start_time
                           FROM (SELECT database, username, runid, status, return_message,
                                        start_time
                                   FROM cron.job_run_details
                                  WHERE status IN ('succeeded', 'failed')
                                  ORDER BY runid DESC LIMIT 1000) recent
                          ORDER BY database, username, runid DESC) x), '{}'::jsonb),
  'jobs', COALESCE((SELECT JSONB_OBJECT_AGG(username || '/' || jobname,
                             JSONB_BUILD_OBJECT(
                               'jobid', jobid, 'schedule', schedule,
                               'command', command, 'database', database,
                               'username', username, 'active', active))
                      FROM cron.job WHERE jobname IS NOT NULL), '{}'::jsonb))
)SQL";

// Not installed here. Readable: absent when the library is not loaded, else
// where pg_cron keeps its jobs. Unreadable: whether it is loaded at all cannot
// be told, so the reading says that, and a pg_cron kind is refused naming the
// grant rather than guessing.
inline const char* kpg_cronAbsentSql = R"SQL(
SELECT CASE
       WHEN NOT pg_has_role('pg_read_all_settings', 'USAGE') THEN JSONB_BUILD_OBJECT(
         'installed', false,
         'settings_readable', false,
         'current_database', current_database(),
         'current_user', current_user,
         'superuser', false)
       WHEN current_setting('cron.database_name', true) IS NULL THEN NULL
       ELSE JSONB_BUILD_OBJECT(
         'installed', false,
         'settings_readable', true,
         'database_name', current_setting('cron.database_name', true),
         'current_database', current_database(),
         'current_user', current_user,
         'superuser', (SELECT rolsuper FROM pg_roles WHERE rolname = current_user))
       END
)SQL";

// The steps this database's ledger records as applied, so a job edited by hand
// is noticed on ANY plan against this database, not only when the spec that
// scheduled it is planned again (guard.h). Read by core; see catalog.h.
inline constexpr bool kpg_cronReadsAppliedSteps = true;
