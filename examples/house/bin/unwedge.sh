#!/usr/bin/env bash
# Recover a repository left wedged by an interrupted apply.
#
# An interrupted pg_laswell -- Ctrl-C, or a `| head` that closes the pipe --
# leaves the job it was running with finished_at NULL. The repository scan reads
# any unfinished job as `running`, so the spec stays in_progress and everything
# depending on it reports "not applied and not runnable", exit 2.
#
# The test for a job that died with its connection is the one the manual gives:
# a session advisory lock is released the moment its backend goes away, so a job
# with no finished_at and no lock in pg_locks cannot still be running. There is
# no heartbeat to tune and no timeout to get wrong.
#
# The dead job row is DELETED rather than marked failed, deliberately: pg_laswell
# selects work on `status == "pending"` alone, and a spec whose last job is
# `failed` is printed in the plan, never started, and the run exits 0 anyway.
# Clearing the row puts the spec back to `pending`, which is the only state that
# actually re-runs. Nothing that succeeded is touched.
#
# Run as a superuser or the ledger's owner, NOT as house_runner: bootstrap.sql
# grants the migrating role SELECT, INSERT and UPDATE on the ledger and no
# DELETE anywhere, on the grounds that a migration history you can delete from
# is a migration history nobody can rely on. Repairing one is somebody else's
# privilege, which is the same argument that keeps trusted_key out of its reach.
#
#   bin/unwedge.sh [--port N] [--host H] [--dry-run]
set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)

port=${PGPORT:-5432}
host=${PGHOST:-}
dry=
while [ $# -gt 0 ]; do
  case $1 in
    --port)    port=$2; shift 2 ;;
    --host)    host=$2; shift 2 ;;
    --dry-run) dry=1;   shift ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

pg=(psql -q -p "$port" -d house -v ON_ERROR_STOP=1 -P pager=off)
[ -n "$host" ] && pg+=(-h "$host")

dead="j.finished_at IS NULL
      AND NOT EXISTS (SELECT 1 FROM pg_locks l
                       WHERE l.locktype = 'advisory'
                         AND l.objid = (j.lock_key & 4294967295)::int)"

echo "jobs that died with their connection:"
"${pg[@]}" -c "
  SELECT m.spec_id, j.job_id, j.started_at
    FROM laswell.job j
    JOIN laswell.migration m ON m.migration_id = j.migration_id
   WHERE $dead
   ORDER BY j.started_at"

if [ -n "$dry" ]; then
  echo "(--dry-run: nothing changed)"
  exit 0
fi

# Steps first: laswell.step references the job. The migration row itself stays,
# because laswell.migration is the record that this spec was seen and verified.
"${pg[@]}" \
  -c "DELETE FROM laswell.backfill_cursor c
       USING laswell.job j WHERE j.job_id = c.job_id AND $dead" \
  -c "DELETE FROM laswell.step s
       USING laswell.job j WHERE j.job_id = s.job_id AND $dead" \
  -c "DELETE FROM laswell.job j WHERE $dead"

echo "unwedged. Re-run bin/run.sh; the specs concerned are pending again."
