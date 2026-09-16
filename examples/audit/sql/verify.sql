-- What the audit database should look like once the repository has been
-- applied. Read-only: it asserts, it does not fix.
--
-- Run against the AUDIT database; it reaches across to the store through the
-- subscription's own view of things where it has to.
\set ON_ERROR_STOP on
\pset pager off

DO $$
DECLARE
  actual text;
BEGIN
  CREATE OR REPLACE FUNCTION pg_temp.check_that(what text, actual text, expected text)
  RETURNS void LANGUAGE plpgsql AS $f$
  BEGIN
    IF actual IS DISTINCT FROM expected THEN
      RAISE EXCEPTION 'verify FAILED: % -- expected %, got %', what, expected, actual;
    END IF;
    RAISE NOTICE 'ok  %  = %', rpad(what, 44), actual;
  END $f$;

  PERFORM pg_temp.check_that('audit schema exists',
    (SELECT count(*)::text FROM pg_namespace WHERE nspname = 'audit'), '1');

  PERFORM pg_temp.check_that('logged_actions is an ORDINARY table',
    (SELECT relkind::text FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace
      WHERE n.nspname = 'audit' AND c.relname = 'logged_actions'), 'r');

  -- The seventeen columns of the wiki's audit table.
  PERFORM pg_temp.check_that('logged_actions columns',
    (SELECT count(*)::text FROM pg_attribute
      WHERE attrelid = 'audit.logged_actions'::regclass
        AND attnum > 0 AND NOT attisdropped), '17');

  -- The key must accept what the publisher considers distinct, or replication
  -- stalls on a duplicate that nothing reports as a migration failure.
  PERFORM pg_temp.check_that('its key matches the publisher''s',
    (SELECT array_to_string(array_agg(a.attname ORDER BY a.attnum), ',')
       FROM pg_index i JOIN pg_attribute a ON a.attrelid = i.indrelid
                                          AND a.attnum = ANY(i.indkey)
      WHERE i.indrelid = 'audit.logged_actions'::regclass AND i.indisprimary),
    'event_id,action_tstamp_tx');

  PERFORM pg_temp.check_that('the subscription exists',
    (SELECT count(*)::text FROM pg_subscription WHERE subname = 'audit_from_store'), '1');

  PERFORM pg_temp.check_that('it subscribes to store_audit',
    (SELECT array_to_string(subpublications, ',') FROM pg_subscription
      WHERE subname = 'audit_from_store'), 'store_audit');

  PERFORM pg_temp.check_that('no password was stored in the clear',
    (SELECT (position('password' in subconninfo) > 0)::text FROM pg_subscription
      WHERE subname = 'audit_from_store'), 'false');

  -- The ledger here records ONLY what ran here. The publication was applied on
  -- store and is recorded in store's ledger, not this one -- which is the
  -- arrangement, not an omission.
  PERFORM pg_temp.check_that('this ledger holds its own two specs',
    (SELECT count(*)::text FROM laswell.migration
      WHERE spec_id IN ('0010-audit-history-table', '0040-subscription')), '2');

  PERFORM pg_temp.check_that('and not the store''s',
    (SELECT count(*)::text FROM laswell.migration
      WHERE spec_id IN ('0020-store-audit-log', '0030-publication')), '0');

  RAISE NOTICE '';
  RAISE NOTICE 'audit database verified.';
END $$;
