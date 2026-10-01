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
