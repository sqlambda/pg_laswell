#pragma once
// TimescaleDB readings. Included inside namespace pglaswell by catalog.h.
//
// Gated on the extension being installed in THIS database; absent otherwise.
// Every source here was read by a role that is not a superuser on 2.30.2 and
// 2.28.3 (measured): the timescaledb_information views, the size functions and
// _timescaledb_catalog.compression_settings.

inline const char* ktimescaledbPresentSql =
    "SELECT EXISTS (SELECT 1 FROM pg_extension WHERE extname = 'timescaledb')";

// An interval as [months, days, seconds] -- how PostgreSQL keeps one, and how
// parse.h reads a specification's, so the planner compares like with like
// ("7 days" is not "168 hours"; "1 week" is "7 days").
inline std::string timescaledb_interval_sql(const std::string& expr) {
  return "JSONB_BUILD_ARRAY("
         "(EXTRACT(YEAR FROM " + expr + ") * 12 + EXTRACT(MONTH FROM " + expr + "))::bigint, "
         "EXTRACT(DAY FROM " + expr + ")::bigint, "
         "ROUND(EXTRACT(HOUR FROM " + expr + ") * 3600 + EXTRACT(MINUTE FROM " + expr +
         ") * 60 + EXTRACT(SECOND FROM " + expr + "))::bigint)";
}

// A policy's interval lives in its job's config as text -- or as a number, on
// a hypertable whose time column is an integer. Only text is decomposed.
inline std::string timescaledb_config_interval_sql(const std::string& key) {
  return "CASE WHEN JSONB_TYPEOF(j.config->'" + key + "') = 'string' THEN " +
         timescaledb_interval_sql("(j.config->>'" + key + "')::interval") + " END";
}

inline const std::string kTimescaledbObservationText = R"SQL(
SELECT JSONB_BUILD_OBJECT(
  'version', (SELECT extversion FROM pg_extension WHERE extname = 'timescaledb'),
  -- "timescale" or "apache". The Apache build refuses every TSL feature with
  -- "not supported under the current \"apache\" license" (measured on the
  -- -oss image and the PGDG package), so the module refuses them first.
  'license', current_setting('timescaledb.license', true),
  -- What exists, read rather than inferred from a version: a function's
  -- presence is the fact, and a minimum version is a recollection.
  'functions', (SELECT COALESCE(JSONB_OBJECT_AGG(f, EXISTS (
                    SELECT 1 FROM pg_proc p
                     JOIN pg_depend d ON d.objid = p.oid AND d.classid = 'pg_proc'::regclass
                     JOIN pg_extension e ON e.oid = d.refobjid AND e.extname = 'timescaledb'
                    WHERE p.proname = f)), '{}'::jsonb)
                  FROM unnest(ARRAY['by_range', 'add_columnstore_policy',
                                    'remove_columnstore_policy', 'add_compression_policy',
                                    'remove_compression_policy', 'add_retention_policy',
                                    'remove_retention_policy',
                                    'add_continuous_aggregate_policy',
                                    'remove_continuous_aggregate_policy']) AS f),
  'hypertables', COALESCE((SELECT JSONB_OBJECT_AGG(h.hypertable_schema || '.' || h.hypertable_name,
      JSONB_BUILD_OBJECT(
        'num_chunks', h.num_chunks,
        'columnstore', h.compression_enabled,
        'compressed_chunks', (SELECT count(*) FROM timescaledb_information.chunks c
                               WHERE c.hypertable_schema = h.hypertable_schema
                                 AND c.hypertable_name = h.hypertable_name
                                 AND c.is_compressed),
        -- Every partitioning column, in order: a unique index must contain
        -- them all ("cannot create a unique index without the column ...").
        'dimensions', (SELECT JSONB_AGG(JSONB_BUILD_OBJECT(
                           'column', d.column_name,
                           'type', d.column_type::text,
                           'interval', CASE WHEN d.time_interval IS NULL THEN NULL
                                            ELSE )SQL" + timescaledb_interval_sql("d.time_interval") + R"SQL( END)
                         ORDER BY d.dimension_number)
                         FROM timescaledb_information.dimensions d
                        WHERE d.hypertable_schema = h.hypertable_schema
                          AND d.hypertable_name = h.hypertable_name),
        -- The parent holds no rows, so its relpages and reltuples read 0
        -- (measured); these are the hypertable's own answers, chunks included.
        'size_bytes', hypertable_size(format('%I.%I', h.hypertable_schema, h.hypertable_name)::regclass),
        'rows', approximate_row_count(format('%I.%I', h.hypertable_schema, h.hypertable_name)::regclass),
        'segmentby', (SELECT TO_JSONB(cs.segmentby) FROM _timescaledb_catalog.compression_settings cs
                       WHERE cs.relid = format('%I.%I', h.hypertable_schema, h.hypertable_name)::regclass),
        'orderby', (SELECT TO_JSONB(cs.orderby) FROM _timescaledb_catalog.compression_settings cs
                     WHERE cs.relid = format('%I.%I', h.hypertable_schema, h.hypertable_name)::regclass),
        'orderby_desc', (SELECT TO_JSONB(cs.orderby_desc) FROM _timescaledb_catalog.compression_settings cs
                          WHERE cs.relid = format('%I.%I', h.hypertable_schema, h.hypertable_name)::regclass)))
      FROM timescaledb_information.hypertables h), '{}'::jsonb),
  -- Policies are jobs, keyed by what they act on: a hypertable for the
  -- columnstore and retention, a continuous aggregate for refresh.
  'policies', COALESCE((SELECT JSONB_OBJECT_AGG(k, v) FROM (
      SELECT j.hypertable_schema || '.' || j.hypertable_name AS k,
             JSONB_OBJECT_AGG(CASE j.proc_name WHEN 'policy_compression' THEN 'columnstore'
                                               WHEN 'policy_retention' THEN 'retention'
                                               ELSE 'refresh' END,
               JSONB_BUILD_OBJECT(
                 'job_id', j.job_id,
                 'after', )SQL" + timescaledb_config_interval_sql("compress_after") + R"SQL(,
                 'drop_after', )SQL" + timescaledb_config_interval_sql("drop_after") + R"SQL(,
                 'start_offset', )SQL" + timescaledb_config_interval_sql("start_offset") + R"SQL(,
                 'end_offset', )SQL" + timescaledb_config_interval_sql("end_offset") + R"SQL(,
                 'schedule_interval', )SQL" + timescaledb_interval_sql("j.schedule_interval") + R"SQL()) AS v
        FROM timescaledb_information.jobs j
       WHERE j.proc_name IN ('policy_compression', 'policy_retention',
                             'policy_refresh_continuous_aggregate')
       GROUP BY 1) x), '{}'::jsonb),
  'caggs', COALESCE((SELECT JSONB_OBJECT_AGG(c.view_schema || '.' || c.view_name,
      JSONB_BUILD_OBJECT('hypertable', c.hypertable_schema || '.' || c.hypertable_name,
                         'materialized_only', c.materialized_only))
      FROM timescaledb_information.continuous_aggregates c), '{}'::jsonb))
)SQL";

inline const char* ktimescaledbObservationSql = kTimescaledbObservationText.c_str();
inline const char* ktimescaledbAbsentSql = nullptr;
inline constexpr bool ktimescaledbReadsAppliedSteps = false;
