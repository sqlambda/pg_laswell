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
  --
  -- placements counts shards of DISTRIBUTED tables only. A drained node keeps
  -- its reference-table placements -- a drain moves distributed shards and
  -- nothing else -- so counting those would make a finished drain look
  -- unfinished forever.
  'nodes', COALESCE((
     SELECT JSONB_AGG(JSONB_BUILD_OBJECT(
              'nodename', n.nodename, 'nodeport', n.nodeport,
              'nodeid', n.nodeid, 'groupid', n.groupid,
              'isactive', n.isactive, 'noderole', n.noderole,
              'shouldhaveshards', n.shouldhaveshards,
              'placements', (SELECT count(*) FROM pg_dist_placement pl
                               JOIN pg_dist_shard s USING (shardid)
                               JOIN pg_dist_partition p USING (logicalrelid)
                              WHERE pl.groupid = n.groupid AND p.partmethod <> 'n'),
              'reference_placements', (SELECT count(*) FROM pg_dist_placement pl
                               JOIN pg_dist_shard s USING (shardid)
                               JOIN pg_dist_partition p USING (logicalrelid)
                              WHERE pl.groupid = n.groupid AND p.partmethod = 'n'
                                AND p.repmodel = 't'))
            ORDER BY n.nodename, n.nodeport)
       FROM pg_dist_node n), '[]'::jsonb),
  -- The moves Citus's own rebalancer would make now, from the call it
  -- provides for exactly this question. An empty list is what "balanced"
  -- means; it is Citus's definition rather than a second one made up here.
  --
  -- NULL when it cannot be asked. Measured: with fewer nodes allowed to hold
  -- shards than the replication factor, the call RAISES ("Shard replication
  -- factor (1) cannot be greater than number of nodes with
  -- should_have_shards=true (0)"), and an error here would fail every reading
  -- of the cluster, not only a rebalance. The CASE is what stops that: its
  -- branch is not evaluated when the guard is false (measured on a coordinator
  -- with no workers).
  --
  -- It asks the workers for shard sizes. Measured, 64ms on the three-node
  -- example cluster.
  'rebalance_moves',
     CASE WHEN (SELECT count(*) FROM pg_dist_node
                 WHERE shouldhaveshards AND isactive AND noderole = 'primary')
               >= COALESCE(NULLIF(current_setting('citus.shard_replication_factor',
                                                  true), '')::int, 1)
          THEN COALESCE((
            SELECT JSONB_AGG(JSONB_BUILD_OBJECT(
                     'table', m.table_name::text, 'shardid', m.shardid,
                     'from', m.sourcename || ':' || m.sourceport,
                     'to', m.targetname || ':' || m.targetport)
                   ORDER BY m.shardid)
              FROM get_rebalance_table_shards_plan() m), '[]'::jsonb)
     END,
  -- What only the WORKERS can say, asked of them in one round trip
  -- (run_command_on_workers, measured ~20 ms on the example cluster). A worker
  -- that does not answer is recorded as null and does not fail the reading.
  --
  --   wal_level     moving a shard while writes continue uses logical
  --                 replication. Measured: at wal_level = replica a drain in
  --                 the default transfer mode failed at its first shard, AFTER
  --                 marking the node as taking no shards.
  --   citus         the extension version IN THIS DATABASE on that node, to
  --                 compare with the coordinator's: a worker on another Citus
  --                 version fails partway through distributed work.
  --   prepared /    prepared transactions and the age of the oldest. One left
  --   oldest_s      behind by a killed multi-shard batch holds its locks --
  --                 measured, an INSERT waited on one indefinitely -- and
  --                 holds back the node's xmin horizon, so VACUUM there
  --                 reclaims nothing while it lives.
  'workers', COALESCE((
     SELECT JSONB_OBJECT_AGG(w.nodename || ':' || w.nodeport,
                             CASE WHEN w.success THEN w.result::jsonb END)
       FROM run_command_on_workers($w$
         SELECT json_build_object(
           'wal_level', current_setting('wal_level'),
           'citus', (SELECT extversion FROM pg_extension WHERE extname = 'citus'),
           'prepared', (SELECT count(*) FROM pg_prepared_xacts
                         WHERE database = current_database()),
           'oldest_s', (SELECT COALESCE(EXTRACT(EPOCH FROM now() - min(prepared))::bigint, 0)
                          FROM pg_prepared_xacts WHERE database = current_database()))::text
       $w$) w), '{}'::jsonb),
  -- The same two facts about the coordinator, which is a node too: it runs
  -- the concurrent distribution (measured: create_distributed_table_concurrently
  -- failed on the COORDINATOR's wal_level) and can hold prepared transactions.
  'coordinator', JSONB_BUILD_OBJECT(
     'wal_level', current_setting('wal_level'),
     'prepared', (SELECT count(*) FROM pg_prepared_xacts
                   WHERE database = current_database()),
     'oldest_s', (SELECT COALESCE(EXTRACT(EPOCH FROM now() - min(prepared))::bigint, 0)
                    FROM pg_prepared_xacts WHERE database = current_database())),
  -- Reference tables, and how many of them each node holds. Measured: adding a
  -- node copies NONE of them -- citus_add_node took 20 ms beside 217 MB of
  -- reference data -- and the next create_distributed_table copied them all to
  -- it. So the cost lands on whichever step next needs them everywhere, and a
  -- node holding fewer than all is where it will land.
  -- citus_total_relation_size asks one placement; measured ~1 ms.
  'reference_tables', JSONB_BUILD_OBJECT(
     'count', (SELECT count(*) FROM pg_dist_partition
                WHERE partmethod = 'n' AND repmodel = 't'),
     'bytes', (SELECT COALESCE(sum(citus_total_relation_size(logicalrelid)), 0)
                 FROM pg_dist_partition WHERE partmethod = 'n' AND repmodel = 't')),
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
                  -- The distribution column's TYPE, by oid, because Citus
                  -- refuses to colocate two tables whose distribution columns
                  -- differ in type -- EXACTLY, measured: int against bigint is
                  -- refused as firmly as text against bigint ("Distribution
                  -- column types don't match"). The category is what decides a
                  -- distributed FUNCTION's argument, where Citus coerces
                  -- within a category and fails every call across one.
                  'distribution_type_oid',
                     (SELECT a.atttypid::bigint FROM pg_attribute a
                       WHERE a.attrelid = p.logicalrelid
                         AND p.partkey IS NOT NULL AND p.partkey <> ''
                         AND a.attname = column_to_column_name(p.logicalrelid, p.partkey)),
                  'distribution_type',
                     (SELECT format_type(a.atttypid, NULL) FROM pg_attribute a
                       WHERE a.attrelid = p.logicalrelid
                         AND p.partkey IS NOT NULL AND p.partkey <> ''
                         AND a.attname = column_to_column_name(p.logicalrelid, p.partkey)),
                  'distribution_type_category',
                     (SELECT ty.typcategory::text FROM pg_attribute a
                        JOIN pg_type ty ON ty.oid = a.atttypid
                       WHERE a.attrelid = p.logicalrelid
                         AND p.partkey IS NOT NULL AND p.partkey <> ''
                         AND a.attname = column_to_column_name(p.logicalrelid, p.partkey)),
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
  -- How each function Citus knows about is distributed. NOT a list of names:
  -- that was the first version, and it made citus_distribute_function a silent
  -- no-op. On Citus 11+ every CREATE FUNCTION is propagated to the workers and
  -- recorded in pg_dist_object with NO distribution argument and NO colocation
  -- -- measured, a function created a moment earlier and never distributed was
  -- listed -- so "is it in pg_dist_object" answered yes for every function and
  -- the kind reported "already distributed" without routing anything.
  --
  -- argument_index is 0-based, as Citus stores it; null means the function is
  -- replicated to every node but calls to it are not routed.
  'function_distribution', COALESCE((
     SELECT JSONB_OBJECT_AGG(n.nspname || '.' || pr.proname,
              JSONB_BUILD_OBJECT(
                'argument_index', o.distribution_argument_index,
                'colocationid', o.colocationid))
       FROM pg_dist_object o
       JOIN pg_proc pr ON pr.oid = o.objid
       JOIN pg_namespace n ON n.oid = pr.pronamespace
      WHERE o.classid = 'pg_proc'::regclass), '{}'::jsonb),
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
     -- How often Citus's maintenance daemon resolves prepared transactions a
     -- crashed coordinator session left on the workers. The length of the
     -- window in which a killed multi-shard batch still holds its locks.
     'recover_2pc_interval', current_setting('citus.recover_2pc_interval', true),
     'max_adaptive_executor_pool_size',
        current_setting('citus.max_adaptive_executor_pool_size', true),
     'enable_ddl_propagation', current_setting('citus.enable_ddl_propagation', true))
)
)SQL";

