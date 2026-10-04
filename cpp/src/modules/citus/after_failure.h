#pragma once
// What Citus may leave behind when a job fails, read by core's executor after
// the failure (executor.h, Executor::fail) and added to the job's error. A
// reading only: nothing here resolves anything. Included inside namespace
// pglaswell.
//
// A multi-shard write is a two-phase commit. A statement that FAILS is rolled
// back on every node and leaves nothing; one whose coordinator connection dies
// between PREPARE and COMMIT leaves a prepared transaction on a worker, which
// holds its locks and pins that node's xmin horizon (measured; guard.h, 1c).
// The plan guard refuses the NEXT plan once such a transaction is older than
// twice citus.recover_2pc_interval. This says it at once, on the job that
// caused it, with the gid -- while whoever ran the job is still looking.
//
// Measured on Citus 13.2 with a transaction prepared by hand on one worker:
// NULL on a quiet cluster, and otherwise
//   {"workers": {"worker1:5432": [{"gid": "...", "owner": "...", "age_s": 0}]}}
// A worker that cannot be asked is reported as unreadable rather than as clean.

// Whether there is a cluster to ask: Citus installed here, with a worker.
inline const char* kcitusAfterFailureAppliesSql =
    "SELECT EXISTS (SELECT 1 FROM pg_extension WHERE extname = 'citus')"
    "   AND (SELECT count(*) > 0 FROM pg_dist_node"
    "         WHERE isactive AND noderole = 'primary' AND groupid <> 0)";

inline const char* kcitusAfterFailureSql = R"SQL(
SELECT NULLIF(JSONB_STRIP_NULLS(JSONB_BUILD_OBJECT(
  'coordinator', (SELECT JSONB_AGG(JSONB_BUILD_OBJECT('gid', gid, 'owner', owner,
                     'age_s', EXTRACT(EPOCH FROM now() - prepared)::bigint) ORDER BY prepared)
                    FROM pg_prepared_xacts WHERE database = current_database()),
  'workers', (SELECT JSONB_OBJECT_AGG(w.nodename || ':' || w.nodeport,
                       CASE WHEN w.success THEN w.result::jsonb
                            ELSE JSONB_BUILD_OBJECT('unreadable', w.result) END)
                FROM run_command_on_workers($w$
                  SELECT COALESCE(json_agg(json_build_object('gid', gid, 'owner', owner,
                           'age_s', EXTRACT(EPOCH FROM now() - prepared)::bigint)
                           ORDER BY prepared), 'null')::text
                    FROM pg_prepared_xacts WHERE database = current_database() $w$) w
               WHERE NOT w.success OR w.result <> 'null'))), '{}'::jsonb)
)SQL";

// What to do about it, in the job's error beside the reading.
inline const char* kcitusAfterFailureHint =
    "prepared transactions are waiting on the nodes named in left_behind.citus. Each holds "
    "its locks and pins that node's xmin horizon. They may be this job's or another "
    "session's: Citus resolves its own every citus.recover_2pc_interval, and SELECT "
    "recover_prepared_transactions() on the coordinator does it now; any other gid belongs "
    "to whoever prepared it, to COMMIT PREPARED or ROLLBACK PREPARED.";
