-- Post-apply assertions for the house database.
--
-- Every check is a question the migrations claim to have answered. Run it
-- against a database pg_laswell has just applied the repository to:
--
--   psql -v ON_ERROR_STOP=1 -d house -f sql/verify.sql
--
-- A failure raises and stops the script; a clean run prints one line per check.
\set ON_ERROR_STOP on
\pset pager off
\timing off
SET client_min_messages = notice;

CREATE OR REPLACE FUNCTION pg_temp.check_that(what text, actual text, expected text)
RETURNS void LANGUAGE plpgsql AS $$
BEGIN
  IF actual IS DISTINCT FROM expected THEN
    RAISE EXCEPTION 'verify FAILED: % -- expected %, got %', what, expected, actual;
  END IF;
  RAISE NOTICE 'ok  %  = %', rpad(what, 46), actual;
END;
$$;

DO $verify$
BEGIN

-- ---------------------------------------------------------------- structure
PERFORM pg_temp.check_that('schema house exists',
  (SELECT count(*)::text FROM pg_namespace WHERE nspname = 'house'), '1');

PERFORM pg_temp.check_that('base tables',
  (SELECT count(*)::text FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace
    WHERE n.nspname = 'house' AND c.relkind = 'r'), '11');

PERFORM pg_temp.check_that('views',
  (SELECT count(*)::text FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace
    WHERE n.nspname = 'house' AND c.relkind = 'v'), '3');

PERFORM pg_temp.check_that('materialized views',
  (SELECT count(*)::text FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace
    WHERE n.nspname = 'house' AND c.relkind = 'm'), '1');

PERFORM pg_temp.check_that('enum types',
  (SELECT count(*)::text FROM pg_type t JOIN pg_namespace n ON n.oid = t.typnamespace
    WHERE n.nspname = 'house' AND t.typtype = 'e'), '8');

-- Documentation is not optional here: every table, column, index, view and
-- type this repository creates carries a COMMENT ON, because an undocumented
-- object is one a stranger cannot read during an incident.
PERFORM pg_temp.check_that('tables and views with no comment',
  (SELECT count(*)::text FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace
    WHERE n.nspname = 'house' AND c.relkind IN ('r','v','m')
      AND obj_description(c.oid, 'pg_class') IS NULL), '0');

PERFORM pg_temp.check_that('columns with no comment',
  (SELECT count(*)::text FROM pg_attribute a
     JOIN pg_class c ON c.oid = a.attrelid
     JOIN pg_namespace n ON n.oid = c.relnamespace
    WHERE n.nspname = 'house' AND c.relkind = 'r'
      AND a.attnum > 0 AND NOT a.attisdropped
      AND col_description(c.oid, a.attnum) IS NULL), '0');

PERFORM pg_temp.check_that('enum types with no comment',
  (SELECT count(*)::text FROM pg_type t JOIN pg_namespace n ON n.oid = t.typnamespace
    WHERE n.nspname = 'house' AND t.typtype = 'e'
      AND obj_description(t.oid, 'pg_type') IS NULL), '0');

-- Every identity column is GENERATED ALWAYS: 'a' in pg_attribute.attidentity.
PERFORM pg_temp.check_that('identity columns, all GENERATED ALWAYS',
  (SELECT count(*)::text FROM pg_attribute a
     JOIN pg_class c ON c.oid = a.attrelid
     JOIN pg_namespace n ON n.oid = c.relnamespace
    WHERE n.nspname = 'house' AND a.attidentity = 'a'), '4');

-- Every foreign key column leads an index. Without one, each parent UPDATE or
-- DELETE scans the child, which pg_laswell warns about when it plans the FK.
PERFORM pg_temp.check_that('FK columns with no leading index',
  (SELECT count(*)::text
     FROM pg_constraint k
     JOIN pg_class c ON c.oid = k.conrelid
     JOIN pg_namespace n ON n.oid = c.relnamespace
    WHERE n.nspname = 'house' AND k.contype = 'f'
      AND NOT EXISTS (
        SELECT 1 FROM pg_index i
         WHERE i.indrelid = k.conrelid
           AND i.indkey[0] = k.conkey[1])), '0');

-- ------------------------------------------------------------------- data
PERFORM pg_temp.check_that('rooms', (SELECT count(*)::text FROM house.room), '11');
PERFORM pg_temp.check_that('brands after the 0094 merge', (SELECT count(*)::text FROM house.brand), '15');
PERFORM pg_temp.check_that('colors after the 0093 copy', (SELECT count(*)::text FROM house.color), '14');

-- 92 items seeded, three expired food rows deleted by 0097.
PERFORM pg_temp.check_that('items', (SELECT count(*)::text FROM house.item), '89');
PERFORM pg_temp.check_that('  furniture', (SELECT count(*)::text FROM house.furniture), '19');
PERFORM pg_temp.check_that('  kitchenware', (SELECT count(*)::text FROM house.kitchenware), '14');
PERFORM pg_temp.check_that('  electronics', (SELECT count(*)::text FROM house.electronics), '15');
PERFORM pg_temp.check_that('  book', (SELECT count(*)::text FROM house.book), '12');
PERFORM pg_temp.check_that('  food after the 0097 purge', (SELECT count(*)::text FROM house.food), '11');
PERFORM pg_temp.check_that('  cleaning_product', (SELECT count(*)::text FROM house.cleaning_product), '8');
PERFORM pg_temp.check_that('  tool', (SELECT count(*)::text FROM house.tool), '10');

-- The detail tables and the supertype agree in both directions.
PERFORM pg_temp.check_that('detail rows with no item row',
  (SELECT (
     (SELECT count(*) FROM house.furniture        f LEFT JOIN house.item i USING (item_id) WHERE i.item_id IS NULL) +
     (SELECT count(*) FROM house.kitchenware      k LEFT JOIN house.item i USING (item_id) WHERE i.item_id IS NULL) +
     (SELECT count(*) FROM house.electronics      e LEFT JOIN house.item i USING (item_id) WHERE i.item_id IS NULL) +
     (SELECT count(*) FROM house.book             b LEFT JOIN house.item i USING (item_id) WHERE i.item_id IS NULL) +
     (SELECT count(*) FROM house.food             d LEFT JOIN house.item i USING (item_id) WHERE i.item_id IS NULL) +
     (SELECT count(*) FROM house.cleaning_product p LEFT JOIN house.item i USING (item_id) WHERE i.item_id IS NULL) +
     (SELECT count(*) FROM house.tool             t LEFT JOIN house.item i USING (item_id) WHERE i.item_id IS NULL)
   )::text), '0');

PERFORM pg_temp.check_that('item rows with no detail row',
  (SELECT count(*)::text FROM house.item i
    WHERE NOT EXISTS (SELECT 1 FROM house.furniture        x WHERE x.item_id = i.item_id)
      AND NOT EXISTS (SELECT 1 FROM house.kitchenware      x WHERE x.item_id = i.item_id)
      AND NOT EXISTS (SELECT 1 FROM house.electronics      x WHERE x.item_id = i.item_id)
      AND NOT EXISTS (SELECT 1 FROM house.book             x WHERE x.item_id = i.item_id)
      AND NOT EXISTS (SELECT 1 FROM house.food             x WHERE x.item_id = i.item_id)
      AND NOT EXISTS (SELECT 1 FROM house.cleaning_product x WHERE x.item_id = i.item_id)
      AND NOT EXISTS (SELECT 1 FROM house.tool             x WHERE x.item_id = i.item_id)), '0');

-- 0080: six chairs point at the dining table they belong to.
PERFORM pg_temp.check_that('chairs linked to the dining table',
  (SELECT count(*)::text FROM house.furniture WHERE parent_item_id = 1001), '6');

-- 0086: books are shelved on furniture, never on a frying pan.
PERFORM pg_temp.check_that('books on a shelf that is furniture',
  (SELECT count(*)::text FROM house.book b JOIN house.furniture f ON f.item_id = b.shelf_item_id), '12');

-- 0095: the backfill left nothing behind. This is verify_remaining, re-asked.
PERFORM pg_temp.check_that('items still without a current price',
  (SELECT count(*)::text FROM house.item WHERE current_price IS NULL), '0');

-- 0097: nothing expired before September 2026 survives.
PERFORM pg_temp.check_that('food past its date still in stock',
  (SELECT count(*)::text FROM house.food WHERE expiry_date < DATE '2026-09-01'), '0');

-- 0096 then 0099: two books lent, under the renamed column.
PERFORM pg_temp.check_that('books out on loan',
  (SELECT count(*)::text FROM house.book WHERE borrowed_by_friend IS NOT NULL), '2');
PERFORM pg_temp.check_that('borrowed_book view exposes the new name',
  (SELECT count(*)::text FROM information_schema.columns
    WHERE table_schema = 'house' AND table_name = 'borrowed_book'
      AND column_name = 'borrowed_by_friend'), '1');
PERFORM pg_temp.check_that('borrowed_book no longer exposes the old one',
  (SELECT count(*)::text FROM information_schema.columns
    WHERE table_schema = 'house' AND table_name = 'borrowed_book'
      AND column_name = 'borrowed_by'), '0');

-- 0090 and 0091: the enum labels landed in the position they asked for.
PERFORM pg_temp.check_that('air_fryer follows oven in electronics_kind',
  (SELECT string_agg(e.enumlabel, ',' ORDER BY e.enumsortorder)
     FROM pg_enum e JOIN pg_type t ON t.oid = e.enumtypid
    WHERE t.typname = 'electronics_kind'
      AND e.enumlabel IN ('oven', 'air_fryer')), 'oven,air_fryer');
PERFORM pg_temp.check_that('cheese follows milk in food_kind',
  (SELECT string_agg(e.enumlabel, ',' ORDER BY e.enumsortorder)
     FROM pg_enum e JOIN pg_type t ON t.oid = e.enumtypid
    WHERE t.typname = 'food_kind'
      AND e.enumlabel IN ('milk', 'cheese')), 'milk,cheese');

-- 0098: the column is wider and the view that blocked it is back.
PERFORM pg_temp.check_that('electronics.model width',
  (SELECT format_type(a.atttypid, a.atttypmod)
     FROM pg_attribute a WHERE a.attrelid = 'house.electronics'::regclass
       AND a.attname = 'model'), 'character varying(120)');
PERFORM pg_temp.check_that('electronics_catalogue rows',
  (SELECT count(*)::text FROM house.electronics_catalogue), '15');

-- 0092: the column was added and no view picked it up, which is the point.
PERFORM pg_temp.check_that('item.disposed_on exists',
  (SELECT count(*)::text FROM pg_attribute
    WHERE attrelid = 'house.item'::regclass AND attname = 'disposed_on'
      AND NOT attisdropped), '1');
PERFORM pg_temp.check_that('item_overview does NOT expose disposed_on',
  (SELECT count(*)::text FROM information_schema.columns
    WHERE table_schema = 'house' AND table_name = 'item_overview'
      AND column_name = 'disposed_on'), '0');

-- 0051-0054: updated_at is maintained rather than merely defaulted.
PERFORM pg_temp.check_that('touch_updated_at triggers',
  (SELECT count(*)::text FROM pg_trigger
    WHERE NOT tgisinternal AND tgname LIKE '%\_touch\_updated\_at'), '4');

-- 0064: REFRESH ... CONCURRENTLY is legal, which needs a unique index.
PERFORM pg_temp.check_that('room_value has a unique index',
  (SELECT count(*)::text FROM pg_index
    WHERE indrelid = 'house.room_value'::regclass AND indisunique), '1');

-- 0100: the application role can read and write, and cannot change the schema.
PERFORM pg_temp.check_that('house_app may SELECT house.item',
  has_table_privilege('house_app', 'house.item', 'SELECT')::text, 'true');
PERFORM pg_temp.check_that('house_app may INSERT house.item',
  has_table_privilege('house_app', 'house.item', 'INSERT')::text, 'true');
PERFORM pg_temp.check_that('house_app may NOT TRUNCATE house.item',
  has_table_privilege('house_app', 'house.item', 'TRUNCATE')::text, 'false');
PERFORM pg_temp.check_that('house_app may NOT CREATE in house',
  has_schema_privilege('house_app', 'house', 'CREATE')::text, 'false');

-- --------------------------------------------------------------- the ledger
PERFORM pg_temp.check_that('migrations recorded',
  (SELECT count(*)::text FROM laswell.migration), '51');
-- Not "no job ever failed": the ledger is append-and-amend and keeps failures
-- on purpose, so an interrupted run that was resumed leaves a failed job row
-- behind for good. The invariant that matters is that every migration ended up
-- with a job that succeeded.
PERFORM pg_temp.check_that('migrations with no successful job',
  (SELECT count(*)::text FROM laswell.migration m
    WHERE NOT EXISTS (SELECT 1 FROM laswell.job j
                       WHERE j.migration_id = m.migration_id
                         AND j.state = 'succeeded')), '0');
PERFORM pg_temp.check_that('jobs still unfinished',
  (SELECT count(*)::text FROM laswell.job WHERE finished_at IS NULL), '0');
PERFORM pg_temp.check_that('steps recorded',
  (SELECT (count(*) > 100)::text FROM laswell.step), 'true');

  RAISE NOTICE 'verify: ok';
END;
$verify$;
