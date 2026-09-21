#pragma once
// Citus readings, gathered where every other reading is: in the catalog, and
// handed to the planner as data. The planner never queries.
//
// EVERY query is gated on the extension being installed, and when it is absent
// the whole `citus` key is ABSENT rather than empty. A planner that cannot tell
// "no Citus" from "Citus with no distributed tables" would refuse the wrong
// things, and absent-versus-zero is the only way to say the difference.
// Included inside namespace pglaswell by catalog.h.

// One statement, JSON built in SQL rather than in C++, following catalog.h's
// own style: the shape is visible in the query that produces it.
inline const char* kcitusPresentSql =
    "SELECT EXISTS (SELECT 1 FROM pg_extension WHERE extname = 'citus')";

inline const char* kcitusObservationSql = R"SQL(
SELECT JSONB_BUILD_OBJECT(
  'version', (SELECT extversion FROM pg_extension WHERE extname = 'citus'),
  -- Node inventory. The worker count is what the connection budget has to be
  -- multiplied by, and shouldhaveshards decides where a rebalance may place.
  'nodes', COALESCE((
     SELECT JSONB_AGG(JSONB_BUILD_OBJECT(
              'nodename', nodename, 'nodeport', nodeport,
              'isactive', isactive, 'noderole', noderole,
              'shouldhaveshards', shouldhaveshards)
            ORDER BY nodename, nodeport)
       FROM pg_dist_node), '[]'::jsonb),
  'worker_count', (SELECT count(*) FROM pg_dist_node
                    WHERE noderole = 'primary' AND isactive
                      AND NOT (nodename = COALESCE(
                        (SELECT nodename FROM pg_dist_node WHERE groupid = 0 LIMIT 1), ''))),
  -- Per-table distribution. Keyed schema.table so it merges with the planner's
  -- own idea of a relation without either side having to agree separately.
  'tables', COALESCE((
     SELECT JSONB_OBJECT_AGG(t.nspname || '.' || t.relname, t.entry)
       FROM (
         SELECT n.nspname, c.relname,
                JSONB_BUILD_OBJECT(
                  'partmethod', p.partmethod,
                  -- column_to_column_name is Citus's own, and is the only
                  -- supported way to read this: partkey is TEXT holding a
                  -- serialised node tree, not pg_node_tree, so pg_get_expr
                  -- does not accept it. Found by running against a real
                  -- cluster -- "function pg_get_expr(text, regclass) does not
                  -- exist" -- which is why CITUS.md marks catalog shapes as
                  -- verify-before-encoding.
                  --
                  -- A reference table has an empty partkey, and asking for its
                  -- column name raises rather than returning NULL, so the
                  -- emptiness is checked first.
                  'distribution_column',
                     CASE WHEN p.partkey IS NULL OR p.partkey = '' THEN NULL
                          ELSE column_to_column_name(p.logicalrelid, p.partkey)
                     END,
                  'colocationid', p.colocationid,
                  'repmodel', p.repmodel,
                  'shard_count', (SELECT cc.shardcount FROM pg_dist_colocation cc
                                   WHERE cc.colocationid = p.colocationid),
                  'replication_factor', (SELECT cc.replicationfactor
                                           FROM pg_dist_colocation cc
                                          WHERE cc.colocationid = p.colocationid)
                ) AS entry
           FROM pg_dist_partition p
           JOIN pg_class c ON c.oid = p.logicalrelid
           JOIN pg_namespace n ON n.oid = c.relnamespace) AS t), '{}'::jsonb),
  -- Which functions are already distributed, so a second distribute_function is
  -- recognised as already done rather than attempted again.
  'distributed_functions', COALESCE((
     SELECT JSONB_AGG(DISTINCT n.nspname || '.' || pr.proname)
       FROM pg_dist_object o
       JOIN pg_proc pr ON pr.oid = o.objid
       JOIN pg_namespace n ON n.oid = pr.pronamespace
      WHERE o.classid = 'pg_proc'::regclass), '[]'::jsonb),
  -- WHICH CALLS THIS CITUS ACTUALLY HAS, which is a fact rather than a version
  -- comparison. CITUS.md suggests gating on citus_version() against a
  -- remembered minimum -- believed 11.1 for the concurrent form -- but a
  -- remembered minimum is exactly the kind of number this project refuses to
  -- state: it cannot be derived, only recalled, and it is wrong the moment a
  -- distribution backports or a fork diverges.
  --
  -- pg_proc answers directly. A plan that says "this Citus has no
  -- create_distributed_table_concurrently" is reporting a reading; one that
  -- says "Citus 11.0 is too old" is reporting a belief.
  'calls', COALESCE((
     SELECT JSONB_OBJECT_AGG(p.proname, true)
       FROM pg_proc p
      WHERE p.proname IN ('create_distributed_table_concurrently',
                          'alter_distributed_table',
                          'citus_rebalance_start',
                          'citus_split_shard_by_split_points',
                          'citus_add_local_table_to_metadata',
                          'undistribute_table')), '{}'::jsonb),
  -- Settings the plan must STATE rather than inherit. A plan that inherits a
  -- session setting is not a function of the spec plus observations.
  'settings', JSONB_BUILD_OBJECT(
     'shard_count', current_setting('citus.shard_count', true),
     'shard_replication_factor', current_setting('citus.shard_replication_factor', true),
     'multi_shard_modify_mode', current_setting('citus.multi_shard_modify_mode', true),
     'max_adaptive_executor_pool_size',
        current_setting('citus.max_adaptive_executor_pool_size', true),
     'enable_ddl_propagation', current_setting('citus.enable_ddl_propagation', true))
)
)SQL";

