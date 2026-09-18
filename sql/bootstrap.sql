-- pg_laswell bootstrap.
--
-- Run ONCE, BY HAND, by a superuser or an owner role. The binary never runs
-- this and never creates its own schema.
--
-- The reason is a privilege argument, not a convenience one. If pg_laswell
-- created laswell.trusted_key, its runtime role would own it -- and a role that
-- owns a table can INSERT into it, which means the migrating role could grant
-- itself trust and the whole gate would collapse into a comment. The
-- chicken-and-egg is not solvable inside the binary; it is solved by putting
-- the trust root outside it.
--
--   psql -v ON_ERROR_STOP=1 \
--        -v laswell_role=laswell_runner \
--        -v first_key_id=ed25519:9f2c41b7d0e6a85c \
--        -v first_key_b64=MCowBQYDK2VwAyEA... \
--        -v first_key_label=ops-prod-2026 \
--        -f bootstrap.sql
--
-- Re-running at the same schema version is a no-op. Re-running against a
-- different version raises rather than silently upgrading: a ledger whose shape
-- changed under a running job is worse than a refusal.

\set ON_ERROR_STOP on

BEGIN;

CREATE SCHEMA IF NOT EXISTS laswell;
COMMENT ON SCHEMA laswell IS
  'pg_laswell migration ledger and trust root. Owned by a privileged role; the '
  'migrating role has SELECT on trusted_key and nothing more.';

CREATE TABLE IF NOT EXISTS laswell.schema_version (
  version      integer     PRIMARY KEY,
  installed_at timestamptz NOT NULL DEFAULT now(),
  installed_by text        NOT NULL DEFAULT current_user
);
COMMENT ON TABLE  laswell.schema_version              IS 'One row per version installed; the highest is the one this database carries. An upgrade appends rather than replaces, so when the schema changed is on the record.';
COMMENT ON COLUMN laswell.schema_version.version      IS 'Ledger schema version. pg_laswell refuses to run against a version it does not know.';
COMMENT ON COLUMN laswell.schema_version.installed_at IS 'When this version was installed.';
COMMENT ON COLUMN laswell.schema_version.installed_by IS 'Role that ran bootstrap.sql.';

DO $$
DECLARE
  existing integer;
BEGIN
  SELECT version INTO existing FROM laswell.schema_version ORDER BY version DESC LIMIT 1;
  IF existing IS NULL THEN
    INSERT INTO laswell.schema_version(version) VALUES (3);
  ELSIF existing = 1 THEN
    -- 1 -> 2 adds laswell.environment and laswell.release. Both are created
    -- unconditionally below, so the upgrade is the version row: a ledger at 1
    -- has the same job, step and migration history and loses nothing.
    INSERT INTO laswell.schema_version(version) VALUES (2);
    INSERT INTO laswell.schema_version(version) VALUES (3);
  ELSIF existing = 2 THEN
    -- 2 -> 3 adds laswell.epoch and puts an epoch on every migration. Unlike
    -- 1 -> 2 this one has DDL to do, and it is below rather than here: the
    -- epoch table must exist before a foreign key can point at it.
    INSERT INTO laswell.schema_version(version) VALUES (3);
  ELSIF existing <> 3 THEN
    RAISE EXCEPTION
      'laswell schema is at version %, this script installs version 3',
      existing
      USING HINT = 'Use the bootstrap script shipped with the pg_laswell binary '
                   'you are running, or migrate the ledger deliberately.';
  END IF;
END
$$;

-- --------------------------------------------------------------------------
-- Trust. This is the authoritative gate: the client config can be wrong,
-- stale or permissive, and this table still decides.
-- --------------------------------------------------------------------------

CREATE TABLE IF NOT EXISTS laswell.trusted_key (
  key_id     text        PRIMARY KEY,
  algorithm  text        NOT NULL DEFAULT 'ed25519' CHECK (algorithm = 'ed25519'),
  public_key bytea       NOT NULL,
  label      text        NOT NULL,
  added_at   timestamptz NOT NULL DEFAULT now(),
  added_by   text        NOT NULL DEFAULT current_user,
  revoked_at timestamptz,
  -- Content addressing, enforced by the database rather than trusted from the
  -- client. Without this a row could file an attacker's key under a trusted
  -- id, and every signature check downstream would pass.
  CONSTRAINT key_id_is_the_content_address_of_the_key
    CHECK (key_id = 'ed25519:' || substr(encode(sha256(public_key), 'hex'), 1, 16))
);
COMMENT ON TABLE  laswell.trusted_key            IS 'Signing keys THIS database accepts. The authoritative trust gate; the client config only fails faster.';
COMMENT ON COLUMN laswell.trusted_key.key_id     IS 'ed25519:<first 16 hex of sha256(public_key)>. Content-addressed, so an id cannot be reassigned to different key bytes.';
COMMENT ON COLUMN laswell.trusted_key.algorithm  IS 'Signature algorithm. Only ed25519 is implemented.';
COMMENT ON COLUMN laswell.trusted_key.public_key IS 'Raw 32-byte Ed25519 public key.';
COMMENT ON COLUMN laswell.trusted_key.label      IS 'Human name, so the key can be talked about during an incident.';
COMMENT ON COLUMN laswell.trusted_key.added_at   IS 'When the key was trusted here.';
COMMENT ON COLUMN laswell.trusted_key.added_by   IS 'Role that added it.';
COMMENT ON COLUMN laswell.trusted_key.revoked_at IS 'When trust was withdrawn. A revoked key verifies nothing; the row is kept so old ledger entries remain attributable.';

-- --------------------------------------------------------------------------
-- What was applied.
-- --------------------------------------------------------------------------

-- --------------------------------------------------------------------------
-- WHICH DATABASE THIS IS, and WHICH RELEASES IT WILL ACCEPT.
--
-- Both live here, in the database, for the same reason laswell.trusted_key
-- does: a permissive client configuration must not be able to talk a
-- production database into accepting something meant for development. An
-- environment read out of a config file beside the specs is an assertion by
-- whoever is running the tool; an environment read out of the database is an
-- assertion by the database itself, and only the second one is worth anything
-- when the question is "am I about to run the dev migration against prod".
--
-- Both are therefore SELECT-only for the migrating role, and writing them is a
-- privileged act performed by whoever owns this schema -- the same person who
-- decides which signing keys are trusted.

CREATE TABLE IF NOT EXISTS laswell.environment (
  -- One row, enforced. Two rows would make "which environment is this" a
  -- question with two answers, and every gate downstream would have to pick.
  only_one    boolean     PRIMARY KEY DEFAULT true CHECK (only_one),
  name        text        NOT NULL CHECK (name <> ''),
  set_at      timestamptz NOT NULL DEFAULT now(),
  set_by      text        NOT NULL DEFAULT current_user
);
COMMENT ON TABLE  laswell.environment      IS 'One row: which environment this database IS. A specification declaring target.environment is applied only where the two agree.';
COMMENT ON COLUMN laswell.environment.name IS 'Environment name, e.g. production, staging, dev. Compared exactly; there is no matching rule to get wrong.';
COMMENT ON COLUMN laswell.environment.set_at IS 'When this database was labelled.';
COMMENT ON COLUMN laswell.environment.set_by IS 'Role that labelled it.';

CREATE TABLE IF NOT EXISTS laswell.release (
  tag             text        PRIMARY KEY CHECK (tag <> ''),
  -- Held is the default and the safe state. A tag nobody has marked is a tag
  -- nobody has approved, so a specification carrying it waits -- including a
  -- tag that does not exist here at all, which is the ordinary case for a
  -- release that has been written but not yet approved.
  ready           boolean     NOT NULL DEFAULT false,
  marked_ready_at timestamptz,
  marked_by       text,
  note            text,
  CONSTRAINT ready_records_who_and_when
    CHECK (NOT ready OR (marked_ready_at IS NOT NULL AND marked_by IS NOT NULL))
);
COMMENT ON TABLE  laswell.release       IS 'Release tags and whether each has been approved. A specification carrying a release tag is applied only once that tag is ready here.';
COMMENT ON COLUMN laswell.release.tag   IS 'The tag, matching a specification''s top-level "release".';
COMMENT ON COLUMN laswell.release.ready IS 'False holds every specification carrying this tag. Absent behaves as false.';
COMMENT ON COLUMN laswell.release.marked_ready_at IS 'When the tag was approved.';
COMMENT ON COLUMN laswell.release.marked_by IS 'Who approved it. Recorded because approval is an authorisation act, not a technical one.';
COMMENT ON COLUMN laswell.release.note IS 'Free text: the change ticket, the approval, whatever the operator wants the next reader to see.';

-- To label this database and approve a release, as the owner of this schema:
--
--   INSERT INTO laswell.environment(name) VALUES ('production')
--     ON CONFLICT (only_one) DO UPDATE SET name = EXCLUDED.name,
--       set_at = now(), set_by = current_user;
--
--   INSERT INTO laswell.release(tag, ready, marked_ready_at, marked_by, note)
--     VALUES ('2026.09', true, now(), current_user, 'CHG-1234 approved')
--     ON CONFLICT (tag) DO UPDATE SET ready = EXCLUDED.ready,
--       marked_ready_at = EXCLUDED.marked_ready_at,
--       marked_by = EXCLUDED.marked_by, note = EXCLUDED.note;

-- --------------------------------------------------------------------------
-- Epochs. Where a lineage of tracked history begins, and what a reset
-- abandons.
--
-- Opened by whoever owns this schema, never by the migrating role, and for the
-- same reason environment and release are: an epoch NARROWS the drift check.
-- The check that notices a migration applied here with no file on disk can
-- only be scoped by saying which lineages this deployment is answering for, so
-- a role that could open its own epoch could excuse its own drift.
--
-- A specification names one in target.epoch, inside the signature. Naming an
-- epoch this database has not opened HOLDS it, exactly as an unapproved
-- release tag does -- the specification is not wrong, it is waiting for a
-- decision that is not the deploying role's to make.
--
-- 'default' always exists. A specification naming no epoch belongs to it, and
-- a ledger upgraded from version 2 has its whole history there, so nothing
-- written before epochs existed has to be reconsidered.
-- --------------------------------------------------------------------------

CREATE TABLE IF NOT EXISTS laswell.epoch (
  name       text        PRIMARY KEY CHECK (name <> ''),
  opened_at  timestamptz NOT NULL DEFAULT now(),
  opened_by  text        NOT NULL DEFAULT current_user,
  -- Retired, never deleted. A reset undoes nothing -- the tables those
  -- migrations created are still there -- so "start over" means stop TRACKING
  -- this lineage, not pretend it never ran. Retiring takes its rows out of the
  -- drift check and leaves "was applied" alone, which is what lets a
  -- specification in another epoch still depend on one of them.
  retired_at timestamptz,
  retired_by text,
  note       text,
  CONSTRAINT retirement_records_who_and_when
    CHECK (retired_at IS NULL OR retired_by IS NOT NULL)
);
COMMENT ON TABLE  laswell.epoch            IS 'Lineages of tracked history. A specification belongs to exactly one; retiring a lineage stops tracking it without unsaying that it ran.';
COMMENT ON COLUMN laswell.epoch.name       IS 'The name a specification carries in target.epoch. ''default'' is where everything unnamed belongs.';
COMMENT ON COLUMN laswell.epoch.opened_at  IS 'When this database began tracking this lineage. For the first epoch, when it began being tracked at all.';
COMMENT ON COLUMN laswell.epoch.opened_by  IS 'Role that opened it. Recorded because opening one narrows a safety check.';
COMMENT ON COLUMN laswell.epoch.retired_at IS 'When this lineage stopped being tracked. NULL means live. Its migrations remain applied.';
COMMENT ON COLUMN laswell.epoch.retired_by IS 'Who retired it.';
COMMENT ON COLUMN laswell.epoch.note       IS 'Free text: why this lineage exists, or why it was retired.';

INSERT INTO laswell.epoch(name, note)
  VALUES ('default', 'Everything that names no epoch, and everything that predates them.')
  ON CONFLICT (name) DO NOTHING;

CREATE TABLE IF NOT EXISTS laswell.migration (
  migration_id    bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
  spec_id         text        NOT NULL,
  spec_digest     text        NOT NULL,
  epoch           text        NOT NULL DEFAULT 'default'
                                REFERENCES laswell.epoch(name),
  canonical_bytes bytea       NOT NULL,
  signer_key_id   text        NOT NULL REFERENCES laswell.trusted_key(key_id),
  signature       bytea       NOT NULL,
  first_seen_at   timestamptz NOT NULL DEFAULT now(),
  -- Per EPOCH, not globally. The same bytes may legitimately run in two
  -- lineages of one database -- that is exactly what happens when a lineage is
  -- retired and the same specifications are replayed into a new one -- and a
  -- global unique made that violate a constraint instead.
  CONSTRAINT migration_digest_is_unique_within_its_epoch
    UNIQUE (epoch, spec_digest)
);
COMMENT ON TABLE  laswell.migration                 IS 'Every signed spec this database has seen, with the exact bytes that were verified.';
COMMENT ON COLUMN laswell.migration.spec_id         IS 'The author''s identifier for the change, e.g. 0007-add-orders-region.';
COMMENT ON COLUMN laswell.migration.spec_digest     IS 'SHA-256 of the RFC 8785 canonical bytes. What a human quotes.';
COMMENT ON COLUMN laswell.migration.canonical_bytes IS 'Exactly what was verified. What ran is never inferred from a file on someone''s disk.';
COMMENT ON COLUMN laswell.migration.signer_key_id   IS 'The key that signed it. The foreign key is the trust gate: an untrusted signer cannot create this row.';
COMMENT ON COLUMN laswell.migration.signature       IS 'The detached Ed25519 signature over canonical_bytes.';
COMMENT ON COLUMN laswell.migration.first_seen_at   IS 'When this database first saw this spec.';

-- 2 -> 3, for a ledger that already exists. A fresh install got the shape
-- above and this does nothing.
--
-- Lock classification, since this rewrites a constraint on the table that
-- records what ran: ADD COLUMN with a constant default is instant on
-- PostgreSQL 11+ and does not rewrite; DROP CONSTRAINT is a catalog change;
-- ADD CONSTRAINT ... UNIQUE builds an index under AccessExclusiveLock. All
-- three take that lock, and all three are brief HERE because laswell.migration
-- holds one row per specification this database has ever seen -- thousands at
-- the very most, not millions. It is also not a table any application touches,
-- so the lock contends with nothing but another pg_laswell run.
DO $$
BEGIN
  IF NOT EXISTS (SELECT 1 FROM information_schema.columns
                  WHERE table_schema = 'laswell' AND table_name = 'migration'
                    AND column_name = 'epoch') THEN
    ALTER TABLE laswell.migration
      ADD COLUMN epoch text NOT NULL DEFAULT 'default';
    ALTER TABLE laswell.migration
      ADD CONSTRAINT migration_epoch_fkey
      FOREIGN KEY (epoch) REFERENCES laswell.epoch(name);
    -- The global unique goes, and the per-epoch one replaces it. Dropped by
    -- whatever name this database gave it: a constraint created inline as
    -- `spec_digest text NOT NULL UNIQUE` is named by PostgreSQL, and a ledger
    -- restored from a dump taken by an older server may not spell it the same.
    PERFORM 1;
    EXECUTE (
      SELECT string_agg('ALTER TABLE laswell.migration DROP CONSTRAINT '
                        || quote_ident(conname), '; ')
        FROM pg_constraint
       WHERE conrelid = 'laswell.migration'::regclass
         AND contype = 'u'
         AND pg_get_constraintdef(oid) = 'UNIQUE (spec_digest)');
    ALTER TABLE laswell.migration
      ADD CONSTRAINT migration_digest_is_unique_within_its_epoch
      UNIQUE (epoch, spec_digest);
  END IF;
END
$$;

-- After the upgrade, not before it: on a version 2 ledger the column does
-- not exist until the block above adds it, and a COMMENT on a missing column
-- aborts the whole transaction.
COMMENT ON COLUMN laswell.migration.epoch          IS 'Which lineage this specification belongs to. Scopes the drift check: a deployment answers only for the epochs it brought.';

CREATE TABLE IF NOT EXISTS laswell.job (
  job_id         uuid        PRIMARY KEY,
  migration_id   bigint      NOT NULL REFERENCES laswell.migration(migration_id),
  state          text        NOT NULL CHECK (state IN
                   ('planned','running','throttled','paused_contention','stalled',
                    'succeeded','failed','cancelled','interrupted','aborted_contention')),
  plan           jsonb       NOT NULL,
  plan_digest    text        NOT NULL,
  observations   jsonb       NOT NULL,
  server_version integer     NOT NULL,
  backend_pid    integer     NOT NULL,
  lock_key       bigint      NOT NULL,
  started_at     timestamptz NOT NULL DEFAULT now(),
  finished_at    timestamptz,
  error          jsonb
);
COMMENT ON TABLE  laswell.job                IS 'One attempt to apply one migration.';
COMMENT ON COLUMN laswell.job.state          IS 'Lifecycle. interrupted is inferred, not written: a job with no finished_at whose lock_key is absent from pg_locks died with its connection.';
COMMENT ON COLUMN laswell.job.plan           IS 'The full plan, as shown before execution. Not regenerated afterwards.';
COMMENT ON COLUMN laswell.job.plan_digest    IS 'Digest of the plan. planMigration and startMigration must agree, which is how "what ran is what you were shown" becomes checkable.';
COMMENT ON COLUMN laswell.job.observations   IS 'What the planner measured. A decision can be re-read later and argued with.';
COMMENT ON COLUMN laswell.job.backend_pid    IS 'Coordination connection''s backend pid.';
COMMENT ON COLUMN laswell.job.lock_key       IS 'The session advisory lock this job holds. Its absence from pg_locks is how a crashed job is detected, with no heartbeat and no timeout tuning.';
COMMENT ON COLUMN laswell.job.error          IS 'Structured failure, with the hint that names what to change.';

CREATE INDEX IF NOT EXISTS job_unfinished_idx
  ON laswell.job (lock_key) WHERE finished_at IS NULL;
COMMENT ON INDEX laswell.job_unfinished_idx IS
  'Supports the crashed-job scan: unfinished jobs whose advisory lock no longer exists.';

CREATE TABLE IF NOT EXISTS laswell.step (
  job_id        uuid        NOT NULL REFERENCES laswell.job(job_id),
  ordinal       integer     NOT NULL,
  txn_group     integer     NOT NULL,
  kind          text        NOT NULL,
  txn_class     text        NOT NULL CHECK (txn_class IN
                  ('txn_required','txn_optional','txn_forbidden','own_txn_per_batch')),
  sql           text        NOT NULL,
  why           text        NOT NULL,
  state         text        NOT NULL,
  started_at    timestamptz,
  finished_at   timestamptz,
  rows_affected bigint,
  detail        jsonb,
  error         jsonb,
  PRIMARY KEY (job_id, ordinal)
);
COMMENT ON TABLE  laswell.step               IS 'What each step did, in the order it was attempted.';
COMMENT ON COLUMN laswell.step.txn_group     IS 'Steps sharing a group ran in one transaction. A change of group is where atomicity ends.';
COMMENT ON COLUMN laswell.step.txn_class     IS 'txn_forbidden means the statement cannot run inside a transaction block at all -- CREATE INDEX CONCURRENTLY.';
COMMENT ON COLUMN laswell.step.sql           IS 'The statement verbatim, as executed. Never reconstructed from the spec.';
COMMENT ON COLUMN laswell.step.why           IS 'The planner rule that chose this method, and the measurement behind it.';
COMMENT ON COLUMN laswell.step.state         IS 'succeeded | failed | skipped_satisfied | cancelled. skipped_satisfied is a success recorded with its justification, so "we did not need to" is distinguishable from "we forgot to".';
COMMENT ON COLUMN laswell.step.detail        IS 'Per-kind facts: the commit-reason histogram for a backfill, locker counts for a concurrent index build.';

CREATE TABLE IF NOT EXISTS laswell.backfill_cursor (
  job_id     uuid        NOT NULL REFERENCES laswell.job(job_id),
  ordinal    integer     NOT NULL,
  last_key   text        NOT NULL,
  rows_done  bigint      NOT NULL DEFAULT 0,
  commits    integer     NOT NULL DEFAULT 0,
  updated_at timestamptz NOT NULL DEFAULT now(),
  PRIMARY KEY (job_id, ordinal)
);
COMMENT ON TABLE  laswell.backfill_cursor IS
  'Resume position for a paced backfill. Written by the WORKER connection inside '
  'the same transaction as the data it describes -- cursor and data must be '
  'atomically consistent, or a crash produces re-applied or skipped rows. Job '
  'and step rows are written by the coordination connection instead, because '
  'they must survive a worker rollback.';
COMMENT ON COLUMN laswell.backfill_cursor.last_key IS
  'Highest key committed so far, as text. Always text: JSON numbers are doubles in most clients.';
COMMENT ON COLUMN laswell.backfill_cursor.commits  IS
  'How many transactions the backfill has committed. With the reason histogram in step.detail, this is the observable proof that pacing works.';

-- --------------------------------------------------------------------------
-- The first trusted key.
-- --------------------------------------------------------------------------

\if :{?first_key_id}
INSERT INTO laswell.trusted_key (key_id, public_key, label)
VALUES (:'first_key_id', decode(:'first_key_b64', 'base64'), :'first_key_label')
ON CONFLICT (key_id) DO NOTHING;
\endif

-- --------------------------------------------------------------------------
-- Privileges.
--
-- The runtime role can read the trust table and write the ledger. It must NOT
-- be able to write laswell.trusted_key: that is the whole point of running this
-- script as someone else.
-- --------------------------------------------------------------------------

REVOKE ALL ON laswell.trusted_key FROM PUBLIC;
-- The same for the two tables that answer where a migration may be applied.
-- PUBLIC holds nothing on a freshly created table, so this grants no new
-- protection today; it states the intent, and it means a later GRANT ... TO
-- PUBLIC on the schema cannot quietly hand these away.
REVOKE ALL ON laswell.environment FROM PUBLIC;
REVOKE ALL ON laswell.release     FROM PUBLIC;
REVOKE ALL ON laswell.epoch       FROM PUBLIC;

\if :{?laswell_role}
GRANT USAGE ON SCHEMA laswell TO :"laswell_role";
GRANT SELECT ON laswell.trusted_key    TO :"laswell_role";
GRANT SELECT ON laswell.schema_version TO :"laswell_role";
-- SELECT and no more, deliberately, and for the trusted_key reason: a role that
-- could label its own database, or approve its own release, is a gate that
-- exists only as a comment.
GRANT SELECT ON laswell.environment    TO :"laswell_role";
GRANT SELECT ON laswell.release        TO :"laswell_role";
-- SELECT only, for the sharpest version of that reason yet: an epoch narrows
-- the check that catches drift, so a role that could open one could excuse its
-- own.
GRANT SELECT ON laswell.epoch          TO :"laswell_role";
GRANT SELECT, INSERT, UPDATE ON laswell.migration       TO :"laswell_role";
GRANT SELECT, INSERT, UPDATE ON laswell.job             TO :"laswell_role";
GRANT SELECT, INSERT, UPDATE ON laswell.step            TO :"laswell_role";
GRANT SELECT, INSERT, UPDATE ON laswell.backfill_cursor TO :"laswell_role";
GRANT USAGE, SELECT ON ALL SEQUENCES IN SCHEMA laswell TO :"laswell_role";
-- Deliberately no DELETE anywhere: the ledger is append-and-amend. A migration
-- history you can delete from is a migration history nobody can rely on.
\endif

COMMIT;

\echo 'pg_laswell: schema laswell installed at version 3.'
