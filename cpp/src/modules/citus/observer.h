#pragma once
// Cluster-wide waiter observation. The correctness half of Phase 0.
//
// THE BUG THIS FIXES. pg_laswell paces on contention: it commits when someone
// starts waiting on a lock it holds. The reading is pg_locks plus
// pg_blocking_pids() on its own backend -- which on Citus is COORDINATOR-LOCAL.
// A paced write against a distributed table does its real work on workers, so
// the coordinator backend can show no waiters at all while every worker piles
// up behind it. Unfixed, pacing on Citus measures nothing and the ledger records
// that it paced correctly, which is worse than not pacing: a false reassurance
// is harder to catch than a missing one.
//
// citus_lock_waits is the cluster-wide graph, verified on 13.2:
//   waiting_gpid, blocking_gpid, blocked_statement,
//   current_statement_in_blocking_process, waiting_nodeid, blocking_nodeid
//
// It deals in GLOBAL pids. The pids pg_laswell holds are coordinator-local
// backend pids, so they are mapped through citus_stat_activity, which carries
// both.
//
// WHAT IT CANNOT ANSWER, said rather than defaulted: citus_lock_waits has no
// waitstart, so a worker-side wait has no measurable age. Returning 0 would be
// read as "nobody has been waiting long" and would silently disable the
// max_waiter_wait_ms breaker. The counts come from the cluster; the timings stay
// with the local reading, and a wait that only exists on a worker reports its
// age as absent.

// Is this database part of a Citus cluster with workers? Not just "is the
// extension installed": a single-node Citus has no worker to be blind to, and
// paying for the distributed graph there would buy nothing.
inline const char* kcitusObserverAppliesSql =
    "SELECT EXISTS (SELECT 1 FROM pg_extension WHERE extname = 'citus')"
    "   AND (SELECT count(*) > 0 FROM pg_dist_node"
    "         WHERE isactive AND noderole = 'primary' AND groupid <> 0)";

// Tier 1, cluster-wide: is anyone anywhere waiting on a lock? One row from an
// already-materialised view, so a quiet cluster costs a round trip and nothing
// more -- the same bargain the local tier 1 makes.
inline const char* kcitusAnyWaitersSql =
    "SELECT count(*) FROM citus_lock_waits";

// Tiers 2 and 3, cluster-wide, for every backend pid at once.
//
// Direction matters here exactly as it does locally: the question is "who is
// blocked BY me", so the walk starts from my gpids and follows blocking_gpid
// forwards. Starting from waiting_gpid gives the exactly-backwards answer.
inline const char* kcitusObserverSql = R"SQL(
WITH RECURSIVE
  mine AS (
    SELECT DISTINCT a.pid AS worker, a.global_pid AS gpid
      FROM citus_stat_activity a
     WHERE a.pid = ANY($1::int[])
       AND a.global_pid IS NOT NULL
  ),
  chain(worker, gpid, depth, path) AS (
      SELECT m.worker, m.gpid, 0, ARRAY[m.gpid] FROM mine m
    UNION ALL
      SELECT c.worker, w.waiting_gpid, c.depth + 1, c.path || w.waiting_gpid
        FROM chain c
        JOIN citus_lock_waits w ON w.blocking_gpid = c.gpid
       WHERE NOT w.waiting_gpid = ANY(c.path) AND c.depth < 16
  )
SELECT COALESCE(JSONB_OBJECT_AGG(x.worker::text, JSONB_BUILD_OBJECT(
         'direct', x.direct,
         'transitive', x.transitive,
         -- The node the nearest waiter is on. Recorded because "a waiter
         -- appeared" and "a waiter appeared ON A WORKER" are different facts,
         -- and the second is the one a coordinator-only reading could not see.
         'blockingWaiterNode', x.waiter_node)), '{}'::jsonb)
  FROM (
    SELECT m.worker,
           (SELECT count(DISTINCT w.waiting_gpid) FROM citus_lock_waits w
             WHERE w.blocking_gpid = m.gpid) AS direct,
           (SELECT count(DISTINCT c.gpid) FROM chain c
             WHERE c.worker = m.worker AND c.depth > 0) AS transitive,
           (SELECT w.waiting_nodeid FROM citus_lock_waits w
             WHERE w.blocking_gpid = m.gpid LIMIT 1) AS waiter_node
      FROM mine m) AS x
)SQL";
