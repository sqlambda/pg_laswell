// Conformance: every intent kind, planned and EXECUTED against a real database.
//
// The unit tests assert what the planner emits; this asserts that PostgreSQL
// accepts it and that the catalog afterwards says what the plan claimed. Those
// are different questions, and the gap between them has already cost this
// project real bugs -- a column grant with the clause in the wrong order, an
// object kind whose observation key was never wired up, a whole batch of ALTER
// kinds silently refusing because they were absent from object_key_for().
//
// The list is self-policing: CoversEveryIntentKind fails if a kind is added
// without a case here or an explicit reason for not having one.
namespace conformance {

struct Case {
  const char* kind;                     // the intent kind this proves
  std::vector<const char*> setup;       // SQL making the precondition true
  json body;                            // the intent itself
  const char* verify;                   // one scalar query
  const char* expect;                   // and what it must return

  // Forces the paced path for a case small enough to write out. Without it a
  // two-row `values` form is below dml_single_txn_rows and takes the ONE
  // TRANSACTION branch, so the paced statement -- the one the executor drives,
  // and the one with the cursor in it -- was never executed against a database
  // by anything. Zero leaves the default.
  int single_txn_rows = 0;

  // Skip below this server version, for a case whose SQL genuinely needs a
  // newer PostgreSQL. Named per case rather than assumed, so the skip says
  // which feature and which version rather than disappearing.
  int min_server_version = 0;
};

// Kinds that cannot be proved against a single database, with the reason.
// Each is covered by cpp/test/replication-tests.sh, which builds two clusters.
inline const std::map<std::string, const char*>& deferred_to_two_clusters() {
  static const std::map<std::string, const char*> kDeferred = {
      {"create_subscription", "needs a publisher to connect to"},
      {"alter_subscription", "needs a live subscription"},
      {"drop_subscription", "needs a live subscription"},
      {"import_foreign_schema", "needs a reachable foreign server"},
      {"alter_default_privileges",
       "affects only objects created LATER by a particular role; proving it "
       "needs a second role and a later creation, which the replication script "
       "does"},
      {"security_label", "needs a label provider extension (sepgsql) loaded"},
      {"set_tablespace", "needs a second tablespace on a writable directory"},
  };
  return kDeferred;
}

inline std::vector<Case> cases() {
  return {
    {"create_schema", {},
     json{{"kind","create_schema"},{"schema","cf"},{"comment","Conformance."}},
     "SELECT count(*)::text FROM pg_namespace WHERE nspname='cf'", "1"},

    {"create_extension", {},
     json{{"kind","create_extension"},{"name","pg_trgm"},{"schema","cf"}},
     "SELECT count(*)::text FROM pg_extension WHERE extname='pg_trgm'", "1"},

    {"alter_extension", {},
     json{{"kind","alter_extension"},{"name","pg_trgm"},{"schema","public"}},
     "SELECT n.nspname FROM pg_extension e JOIN pg_namespace n ON n.oid=e.extnamespace"
     " WHERE e.extname='pg_trgm'", "public"},

    {"create_type", {},
     json{{"kind","create_type"},{"schema","cf"},{"name","st"},{"type_kind","enum"},
          {"labels",json::array({"open","closed"})},{"comment","Lifecycle."}},
     "SELECT count(*)::text FROM pg_type WHERE typname='st'", "1"},

    {"add_enum_value", {},
     json{{"kind","add_enum_value"},{"schema","cf"},{"name","st"},{"value","void"},
          {"after","open"}},
     "SELECT string_agg(enumlabel,',' ORDER BY enumsortorder) FROM pg_enum"
     " WHERE enumtypid='cf.st'::regtype", "open,void,closed"},

    {"create_sequence", {},
     json{{"kind","create_sequence"},{"schema","cf"},{"name","sq"},{"comment","Numbers."}},
     "SELECT count(*)::text FROM pg_class WHERE relname='sq' AND relkind='S'", "1"},

    {"alter_sequence", {},
     json{{"kind","alter_sequence"},{"schema","cf"},{"name","sq"},{"increment",5}},
     "SELECT increment_by::text FROM pg_sequences WHERE sequencename='sq'", "5"},

    {"create_table", {},
     json{{"kind","create_table"},{"schema","cf"},{"table","t"},{"comment","Rows."},
          {"primary_key",json::array({"id"})},
          {"columns",json::array({
             json{{"name","id"},{"type","bigint"},{"nullable",false},{"comment","Id."}},
             json{{"name","amount"},{"type","integer"},{"nullable",true},{"comment","N."}},
             json{{"name","code"},{"type","text"},{"nullable",true},{"comment","C."}},
             json{{"name","state"},{"type","cf.st"},{"nullable",true},{"comment","S."}}})}},
     "SELECT count(*)::text FROM pg_class WHERE oid='cf.t'::regclass", "1"},

    {"add_column", {},
     json{{"kind","add_column"},{"schema","cf"},{"table","t"},{"column","note"},
          {"type","text"},{"nullable",true},{"comment","Note."}},
     "SELECT count(*)::text FROM pg_attribute WHERE attrelid='cf.t'::regclass"
     " AND attname='note'", "1"},

    {"alter_column_default", {},
     json{{"kind","alter_column_default"},{"schema","cf"},{"table","t"},
          {"column","note"},{"default","'x'"}},
     "SELECT count(*)::text FROM pg_attrdef WHERE adrelid='cf.t'::regclass", "1"},

    {"create_index", {},
     json{{"kind","create_index"},{"schema","cf"},{"table","t"},{"name","t_amount_idx"},
          {"columns",json::array({"amount"})},{"comment","Lookup."}},
     "SELECT indisvalid::text FROM pg_index WHERE indexrelid='cf.t_amount_idx'::regclass",
     "true"},

    {"add_unique_constraint", {},
     json{{"kind","add_unique_constraint"},{"schema","cf"},{"table","t"},
          {"name","t_code_uq"},{"columns",json::array({"code"})}},
     "SELECT contype::text FROM pg_constraint WHERE conname='t_code_uq'", "u"},

    {"add_check_constraint", {},
     json{{"kind","add_check_constraint"},{"schema","cf"},{"table","t"},
          {"name","t_amount_ck"},{"expression","amount IS NULL OR amount >= 0"}},
     "SELECT convalidated::text FROM pg_constraint WHERE conname='t_amount_ck'", "true"},

    {"rename_constraint", {},
     json{{"kind","rename_constraint"},{"schema","cf"},{"table","t"},
          {"name","t_amount_ck"},{"to","t_amount_positive"}},
     "SELECT count(*)::text FROM pg_constraint WHERE conname='t_amount_positive'", "1"},

    {"drop_constraint", {},
     json{{"kind","drop_constraint"},{"schema","cf"},{"table","t"},{"name","t_amount_positive"}},
     "SELECT count(*)::text FROM pg_constraint WHERE conname='t_amount_positive'", "0"},

    {"create_function",
     {"CREATE TABLE cf.audit(at timestamptz)"},
     json{{"kind","create_function"},{"schema","cf"},{"name","touch"},
          {"arguments",json::array()},{"returns","trigger"},{"language","plpgsql"},
          {"body","BEGIN INSERT INTO cf.audit VALUES (now()); RETURN NEW; END"},
          {"comment","Audits."}},
     "SELECT count(*)::text FROM pg_proc WHERE proname='touch'", "1"},

    {"alter_function", {},
     json{{"kind","alter_function"},{"schema","cf"},{"name","touch"},
          {"arguments",json::array()},{"search_path","cf, pg_catalog"}},
     "SELECT count(*)::text FROM pg_proc WHERE proname='touch' AND proconfig IS NOT NULL", "1"},

    {"create_trigger", {},
     json{{"kind","create_trigger"},{"schema","cf"},{"table","t"},{"name","t_touch"},
          {"timing","AFTER"},{"events",json::array({"INSERT"})},{"function","cf.touch()"}},
     "SELECT count(*)::text FROM pg_trigger WHERE tgname='t_touch'", "1"},

    {"set_trigger_state", {},
     json{{"kind","set_trigger_state"},{"schema","cf"},{"table","t"},
          {"trigger","t_touch"},{"enabled",false}},
     "SELECT tgenabled::text FROM pg_trigger WHERE tgname='t_touch'", "D"},

    {"drop_trigger", {},
     json{{"kind","drop_trigger"},{"schema","cf"},{"table","t"},{"name","t_touch"}},
     "SELECT count(*)::text FROM pg_trigger WHERE tgname='t_touch'", "0"},

    {"backfill",
     {"INSERT INTO cf.t(id, amount, code) SELECT g, g, 'c'||g FROM generate_series(1,50) g"},
     json{{"kind","backfill"},{"schema","cf"},{"table","t"},{"key","id"},
          {"set",json{{"note","'filled'"}}},{"where","note IS NULL OR note = 'x'"}},
     "SELECT count(*)::text FROM cf.t WHERE note='filled'", "50"},

    {"delete_rows", {},
     json{{"kind","delete_rows"},{"schema","cf"},{"table","t"},{"key","id"},
          {"where","id > 40"}},
     "SELECT count(*)::text FROM cf.t", "40"},

    // --- the row-level DML family -------------------------------------------
    //
    // Every one of these supplies `amount` as well as `code`, because
    // set_not_null on amount runs a few cases below and a row left NULL here
    // would fail it -- a nice demonstration that the conformance suite is one
    // database evolving, not a set of independent cases.

    {"insert_rows", {},
     json{{"kind","insert_rows"},{"schema","cf"},{"table","t"},{"key","id"},
          {"columns",json::array({"id","amount","code"})},
          {"values",json::array({json::array({201,7,"i1"}),
                                 json::array({202,8,"i2"})})},
          {"on_conflict","skip"},{"conflict_target",json::array({"id"})}},
     "SELECT string_agg(code,',' ORDER BY id) FROM cf.t WHERE id IN (201,202)",
     "i1,i2"},

    // The same intent again, proving ON CONFLICT DO NOTHING really is
    // idempotent: re-running it must change nothing and must not fail.
    {"insert_rows_is_idempotent", {},
     json{{"kind","insert_rows"},{"schema","cf"},{"table","t"},{"key","id"},
          {"columns",json::array({"id","amount","code"})},
          {"values",json::array({json::array({201,999,"CHANGED"}),
                                 json::array({202,999,"CHANGED"})})},
          {"on_conflict","skip"},{"conflict_target",json::array({"id"})}},
     "SELECT string_agg(code,',' ORDER BY id) FROM cf.t WHERE id IN (201,202)",
     "i1,i2"},

    // Per-row values: 201 and 202 take DIFFERENT codes from one statement,
    // which is the whole reason this kind exists beside backfill.
    {"update_rows", {},
     json{{"kind","update_rows"},{"schema","cf"},{"table","t"},{"key","id"},
          {"columns",json::array({"id","code"})},
          {"values",json::array({json::array({201,"u1"}),
                                 json::array({202,"u2"})})}},
     "SELECT string_agg(code,',' ORDER BY id) FROM cf.t WHERE id IN (201,202)",
     "u1,u2"},

    // The pre-image, executed: the row's value BEFORE this update lands in
    // cf.t_before in the same statement. update_rows above left 201 as "u1";
    // this changes it and the archive must hold what it was.
    {"update_rows_preserved", {},
     json{{"kind","update_rows"},{"schema","cf"},{"table","t"},{"key","id"},
          {"columns",json::array({"id","code"})},
          // Not "p1": code is unique from add_unique_constraint above, and
          // merge_rows_paced seeds a row with that code later.
          {"values",json::array({json::array({201,"kept1"})})},
          {"preserve",json{{"schema","cf"},{"table","t_before"}}}},
     "SELECT code FROM cf.t_before WHERE id = 201", "u1"},

    // MERGE doing both halves at once: 202 exists and is updated, 203 does not
    // and is inserted.
    {"merge_rows", {},
     json{{"kind","merge_rows"},{"schema","cf"},{"table","t"},{"key","id"},
          {"columns",json::array({"id","amount","code"})},
          {"values",json::array({json::array({202,8,"m2"}),
                                 json::array({203,9,"m3"})})}},
     "SELECT string_agg(code,',' ORDER BY id) FROM cf.t WHERE id IN (202,203)",
     "m2,m3"},

    // The paced MERGE, which nothing executed until now. The unpaced case above
    // proves the statement; this proves the one the executor actually drives --
    // a batch CTE, a cursor, and the outer SELECT that advances it. Measured on
    // 18.6, naming the cursor from the MERGE's own RETURNING instead returns
    // nothing for a source row that matched no WHEN clause, and the walk stops
    // early reporting success. That is the bug this case exists to catch.
    {"merge_rows_paced",
     {"INSERT INTO cf.t(id, amount, code) VALUES (301,1,'p1'),(302,2,'p2')"},
     json{{"kind","merge_rows"},{"schema","cf"},{"table","t"},{"key","id"},
          {"columns",json::array({"id","amount","code"})},
          {"values",json::array({json::array({301,9,"pm1"}),
                                 json::array({303,9,"pm3"})})}},
     "SELECT string_agg(code,',' ORDER BY id) FROM cf.t WHERE id IN (301,302,303)",
     "pm1,p2,pm3",
     /*single_txn_rows=*/1,
     /*min_server_version=*/170000},

    // The paced INSERT, UPDATE and DELETE, which -- like the paced MERGE above
    // -- nothing executed until now. The pure planner tests that force pacing
    // never touch a server, so the statements the executor actually drives for
    // these three had never run anywhere; that is how all four came to splice
    // an unquoted schema.table and stay that way. `single_txn_rows = 1` is what
    // forces the paced branch on a case small enough to read.
    {"insert_rows_paced", {},
     json{{"kind","insert_rows"},{"schema","cf"},{"table","t"},{"key","id"},
          {"columns",json::array({"id","amount","code"})},
          {"values",json::array({json::array({401,1,"pi1"}),
                                 json::array({402,2,"pi2"})})}},
     "SELECT string_agg(code,',' ORDER BY id) FROM cf.t WHERE id IN (401,402)",
     "pi1,pi2",
     /*single_txn_rows=*/1},

    // Per-row values through the batch: 401 and 402 take different codes, so a
    // walk that carried one row's value to the other would be caught.
    {"update_rows_paced", {},
     json{{"kind","update_rows"},{"schema","cf"},{"table","t"},{"key","id"},
          {"columns",json::array({"id","code"})},
          {"values",json::array({json::array({401,"pu1"}),
                                 json::array({402,"pu2"})})}},
     "SELECT string_agg(code,',' ORDER BY id) FROM cf.t WHERE id IN (401,402)",
     "pu1,pu2",
     /*single_txn_rows=*/1},

    // And the paced DELETE by key list. 402 survives, so this proves the walk
    // removed what it named rather than everything it saw.
    {"delete_rows_paced", {},
     json{{"kind","delete_rows"},{"schema","cf"},{"table","t"},{"key","id"},
          {"columns",json::array({"id"})},
          {"values",json::array({json::array({401})})}},
     "SELECT string_agg(code,',' ORDER BY id) FROM cf.t WHERE id IN (401,402)",
     "pu2",
     /*single_txn_rows=*/1},

    // The quoting, EXECUTED. The three cases above prove the paced statements
    // run; they cannot prove they are quoted, because cf.t needs no quoting and
    // the unquoted form is valid SQL for it. This one targets a table whose
    // schema-qualified name is a syntax error unquoted in every position, so
    // the paced INSERT the executor drives either carries "cf"."order" or the
    // case fails outright. Before the four mutation CTEs were fixed, it did.
    // The SCHEMA is the reserved word, not the table, and that distinction is
    // the whole case. Measured on 18.6: `INSERT INTO cf.order` parses fine --
    // PostgreSQL accepts a reserved word after a qualifier -- while
    // `INSERT INTO user.t` is a syntax error at "user". So a case targeting a
    // reserved TABLE would have passed against the broken planner and proved
    // nothing; this one targets "user"."order" and fails outright unless the
    // paced INSERT the executor drives carries both halves quoted.
    {"insert_rows_paced_reserved_words",
     {"DROP SCHEMA IF EXISTS \"user\" CASCADE",
      "CREATE SCHEMA \"user\"",
      "CREATE TABLE \"user\".\"order\" (\"end\" bigint PRIMARY KEY, \"desc\" text)"},
     json{{"kind","insert_rows"},{"schema","user"},{"table","order"},{"key","end"},
          {"columns",json::array({"end","desc"})},
          {"values",json::array({json::array({1,"a"}), json::array({2,"b"})})}},
     "SELECT string_agg(\"desc\", ',' ORDER BY \"end\") FROM \"user\".\"order\"",
     "a,b",
     /*single_txn_rows=*/1},

    {"copy_rows", {},
     json{{"kind","copy_rows"},{"schema","cf"},{"table","t"},
          {"columns",json::array({"id","amount","code"})},
          // Codes deliberately not "c4"/"c5": the backfill case above seeds
          // code = 'c'||g for 1..50 and add_unique_constraint made code
          // unique, so those would collide with rows 4 and 5.
          {"values",json::array({json::array({204,10,"z4"}),
                                 json::array({205,11,"z5"})})}},
     "SELECT string_agg(code,',' ORDER BY id) FROM cf.t WHERE id IN (204,205)",
     "z4,z5"},

    // A deleted row's WHOLE pre-image, not just its key: copy_rows above wrote
    // 205 as z5, and after this delete the archive must still say so.
    {"delete_rows_preserved", {},
     json{{"kind","delete_rows"},{"schema","cf"},{"table","t"},{"key","id"},
          {"columns",json::array({"id"})},
          {"values",json::array({json::array({205})})},
          {"preserve",json{{"schema","cf"},{"table","t_deleted"}}}},
     "SELECT code FROM cf.t_deleted WHERE id = 205", "z5"},

    // Clear the rows this family added, so the cases below it -- which count
    // and constrain cf.t -- see the table they were written against. 205 is
    // already gone; a key list naming a missing row is a no-op, by design.
    {"delete_rows_by_key", {},
     json{{"kind","delete_rows"},{"schema","cf"},{"table","t"},{"key","id"},
          {"columns",json::array({"id"})},
          {"values",json::array({json::array({201}), json::array({202}),
                                 json::array({203}), json::array({204}),
                                 json::array({205}),
                                 // ...the three the paced merge case above left
                                 // behind, and the one the paced insert/update
                                 // pair left. The cases below this one COUNT
                                 // cf.t, so anything this family adds has to be
                                 // removed here or it fails a later case with a
                                 // message about the wrong thing entirely.
                                 json::array({301}), json::array({302}),
                                 json::array({303}), json::array({402})})}},
     "SELECT count(*)::text FROM cf.t", "40"},

    {"set_not_null", {},
     json{{"kind","set_not_null"},{"schema","cf"},{"table","t"},{"column","amount"}},
     "SELECT attnotnull::text FROM pg_attribute WHERE attrelid='cf.t'::regclass"
     " AND attname='amount'", "true"},

    {"drop_not_null", {},
     json{{"kind","drop_not_null"},{"schema","cf"},{"table","t"},{"column","amount"}},
     "SELECT attnotnull::text FROM pg_attribute WHERE attrelid='cf.t'::regclass"
     " AND attname='amount'", "false"},

    {"alter_column_type", {},
     json{{"kind","alter_column_type"},{"schema","cf"},{"table","t"},
          {"column","amount"},{"type","bigint"}},
     "SELECT format_type(atttypid,atttypmod) FROM pg_attribute"
     " WHERE attrelid='cf.t'::regclass AND attname='amount'", "bigint"},

    {"set_identity",
     {"ALTER TABLE cf.t ALTER COLUMN amount SET NOT NULL"},
     json{{"kind","set_identity"},{"schema","cf"},{"table","t"},{"column","amount"},
          {"identity","BY DEFAULT"}},
     "SELECT attidentity::text FROM pg_attribute WHERE attrelid='cf.t'::regclass"
     " AND attname='amount'", "d"},

    {"set_column_options", {},
     json{{"kind","set_column_options"},{"schema","cf"},{"table","t"},
          {"column","note"},{"statistics",500}},
     "SELECT attstattarget::text FROM pg_attribute WHERE attrelid='cf.t'::regclass"
     " AND attname='note'", "500"},

    {"set_table_options", {},
     json{{"kind","set_table_options"},{"schema","cf"},{"table","t"},
          {"options",json{{"fillfactor",70}}}},
     "SELECT array_to_string(reloptions,',') FROM pg_class WHERE oid='cf.t'::regclass",
     "fillfactor=70"},

    {"set_replica_identity", {},
     json{{"kind","set_replica_identity"},{"schema","cf"},{"table","t"},{"identity","FULL"}},
     "SELECT relreplident::text FROM pg_class WHERE oid='cf.t'::regclass", "f"},

    {"cluster_on", {},
     json{{"kind","cluster_on"},{"schema","cf"},{"table","t"},{"index","t_amount_idx"}},
     "SELECT indisclustered::text FROM pg_index WHERE indexrelid='cf.t_amount_idx'::regclass",
     "true"},

    // Its own table: an UNLOGGED relation cannot be added to a publication,
    // and cf.t is published further down. Cases share a database on purpose --
    // that is what makes them a sequence rather than 70 isolated fixtures --
    // so one that changes a table's nature gets its own.
    {"set_logged",
     {"CREATE TABLE cf.persist(id int)"},
     json{{"kind","set_logged"},{"schema","cf"},{"table","persist"},{"logged",false}},
     "SELECT relpersistence::text FROM pg_class WHERE oid='cf.persist'::regclass", "u"},

    {"set_access_method", {},
     json{{"kind","set_access_method"},{"schema","cf"},{"table","t"},{"method","heap"}},
     "SELECT a.amname FROM pg_class c JOIN pg_am a ON a.oid=c.relam"
     " WHERE c.oid='cf.t'::regclass", "heap"},

    {"create_statistics", {},
     json{{"kind","create_statistics"},{"schema","cf"},{"name","t_corr"},{"table","t"},
          {"columns",json::array({"amount","code"})}},
     "SELECT count(*)::text FROM pg_statistic_ext WHERE stxname='t_corr'", "1"},

    {"drop_statistics", {},
     json{{"kind","drop_statistics"},{"schema","cf"},{"name","t_corr"}},
     "SELECT count(*)::text FROM pg_statistic_ext WHERE stxname='t_corr'", "0"},

    {"replace_view", {},
     json{{"kind","replace_view"},{"schema","cf"},{"name","v"},
          {"definition","SELECT id, amount FROM cf.t"},{"comment","View."}},
     "SELECT count(*)::text FROM pg_class WHERE oid='cf.v'::regclass", "1"},

    {"alter_view", {},
     json{{"kind","alter_view"},{"schema","cf"},{"name","v"},
          {"options",json::array({"security_barrier=true"})}},
     "SELECT array_to_string(reloptions,',') FROM pg_class WHERE oid='cf.v'::regclass",
     "security_barrier=true"},

    {"drop_view", {},
     json{{"kind","drop_view"},{"schema","cf"},{"name","v"}},
     "SELECT count(*)::text FROM pg_class WHERE relname='v' AND relnamespace='cf'::regnamespace",
     "0"},

    {"create_materialized_view", {},
     json{{"kind","create_materialized_view"},{"schema","cf"},{"name","mv"},
          {"definition","SELECT id FROM cf.t"},{"comment","Snapshot."}},
     "SELECT count(*)::text FROM cf.mv", "40"},

    {"create_table_as", {},
     json{{"kind","create_table_as"},{"schema","cf"},{"table","copyt"},
          {"definition","SELECT id FROM cf.t"},{"comment","Copy."}},
     "SELECT count(*)::text FROM cf.copyt", "40"},

    {"create_rule", {},
     json{{"kind","create_rule"},{"schema","cf"},{"table","copyt"},{"name","no_del"},
          {"event","DELETE"},{"instead",true},{"action","NOTHING"}},
     "SELECT count(*)::text FROM pg_rules WHERE rulename='no_del'", "1"},

    {"drop_rule", {},
     json{{"kind","drop_rule"},{"schema","cf"},{"table","copyt"},{"name","no_del"}},
     "SELECT count(*)::text FROM pg_rules WHERE rulename='no_del'", "0"},

    {"add_primary_key", {},
     json{{"kind","add_primary_key"},{"schema","cf"},{"table","copyt"},
          {"name","copyt_pkey"},{"columns",json::array({"id"})}},
     "SELECT contype::text FROM pg_constraint WHERE conname='copyt_pkey'", "p"},

    {"add_foreign_key",
     {"CREATE TABLE cf.child(pid bigint)"},
     json{{"kind","add_foreign_key"},{"schema","cf"},{"table","child"},
          {"name","child_fk"},{"columns",json::array({"pid"})},
          {"references_schema","cf"},{"references_table","copyt"},
          {"references_columns",json::array({"id"})}},
     "SELECT convalidated::text FROM pg_constraint WHERE conname='child_fk'", "true"},

    {"rename_column", {},
     json{{"kind","rename_column"},{"schema","cf"},{"table","child"},
          {"column","pid"},{"to","parent_id"}},
     "SELECT count(*)::text FROM pg_attribute WHERE attrelid='cf.child'::regclass"
     " AND attname='parent_id'", "1"},

    {"rename_table", {},
     json{{"kind","rename_table"},{"schema","cf"},{"table","child"},{"to","kid"}},
     "SELECT count(*)::text FROM pg_class WHERE relname='kid'", "1"},

    {"drop_column", {},
     json{{"kind","drop_column"},{"schema","cf"},{"table","kid"},{"column","parent_id"}},
     "SELECT count(*)::text FROM pg_attribute WHERE attrelid='cf.kid'::regclass"
     " AND attname='parent_id' AND NOT attisdropped", "0"},

    {"drop_table", {},
     json{{"kind","drop_table"},{"schema","cf"},{"table","kid"}},
     "SELECT count(*)::text FROM pg_class WHERE relname='kid'", "0"},

    {"drop_index", {},
     json{{"kind","drop_index"},{"schema","cf"},{"table","t"},{"name","t_amount_idx"}},
     "SELECT count(*)::text FROM pg_class WHERE relname='t_amount_idx'", "0"},

    {"set_row_security", {},
     json{{"kind","set_row_security"},{"schema","cf"},{"table","t"},{"enabled",true}},
     "SELECT relrowsecurity::text FROM pg_class WHERE oid='cf.t'::regclass", "true"},

    {"create_policy", {},
     json{{"kind","create_policy"},{"schema","cf"},{"table","t"},{"name","p_all"},
          {"using","true"}},
     "SELECT count(*)::text FROM pg_policy WHERE polname='p_all'", "1"},

    {"alter_policy", {},
     json{{"kind","alter_policy"},{"schema","cf"},{"table","t"},{"name","p_all"},
          {"using","id > 0"}},
     "SELECT count(*)::text FROM pg_policy WHERE polname='p_all'", "1"},

    {"drop_policy", {},
     json{{"kind","drop_policy"},{"schema","cf"},{"table","t"},{"name","p_all"}},
     "SELECT count(*)::text FROM pg_policy WHERE polname='p_all'", "0"},

    {"grant",
     {"DO $$BEGIN IF NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname='cf_app')"
      " THEN CREATE ROLE cf_app; END IF; END$$"},
     json{{"kind","grant"},{"schema","cf"},{"table","t"},
          {"privileges",json::array({"SELECT"})},{"to",json::array({"cf_app"})}},
     "SELECT has_table_privilege('cf_app','cf.t','SELECT')::text", "true"},

    {"revoke", {},
     json{{"kind","revoke"},{"schema","cf"},{"table","t"},
          {"privileges",json::array({"SELECT"})},{"from",json::array({"cf_app"})}},
     "SELECT has_table_privilege('cf_app','cf.t','SELECT')::text", "false"},

    {"set_comment", {},
     json{{"kind","set_comment"},{"object_type","TABLE"},{"schema","cf"},{"name","t"},
          {"comment","Re-documented."}},
     "SELECT obj_description('cf.t'::regclass)", "Re-documented."},

    {"set_owner", {},
     json{{"kind","set_owner"},{"object_type","TABLE"},{"schema","cf"},{"name","copyt"},
          {"owner","cf_app"}},
     "SELECT pg_get_userbyid(relowner) FROM pg_class WHERE oid='cf.copyt'::regclass",
     "cf_app"},

    {"create_object", {},
     json{{"kind","create_object"},{"object_type","COLLATION"},{"name","cf.ci"},
          {"definition","(provider = icu, locale = 'en-u-ks-level2', deterministic = false)"}},
     "SELECT count(*)::text FROM pg_collation WHERE collname='ci'", "1"},

    {"alter_object", {},
     json{{"kind","alter_object"},{"object_type","COLLATION"},{"name","cf.ci"},
          {"to","ci2"}},
     "SELECT count(*)::text FROM pg_collation WHERE collname='ci2'", "1"},

    {"drop_object", {},
     json{{"kind","drop_object"},{"object_type","COLLATION"},{"name","cf.ci2"}},
     "SELECT count(*)::text FROM pg_collation WHERE collname='ci2'", "0"},

    {"create_publication", {},
     json{{"kind","create_publication"},{"name","cf_pub"},{"tables",json::array({"cf.t"})}},
     "SELECT count(*)::text FROM pg_publication WHERE pubname='cf_pub'", "1"},

    {"alter_publication",
     {"CREATE TABLE cf.pub2(id int PRIMARY KEY)"},
     json{{"kind","alter_publication"},{"name","cf_pub"},
          {"add_tables",json::array({"cf.pub2"})}},
     "SELECT count(*)::text FROM pg_publication_tables WHERE pubname='cf_pub'", "2"},

    {"drop_publication", {},
     json{{"kind","drop_publication"},{"name","cf_pub"}},
     "SELECT count(*)::text FROM pg_publication WHERE pubname='cf_pub'", "0"},

    {"create_schema", {},   // second use: the partition parent lives elsewhere
     json{{"kind","create_schema"},{"schema","cfp"},{"comment","Partitions."}},
     "SELECT count(*)::text FROM pg_namespace WHERE nspname='cfp'", "1"},

    {"attach_partition",
     {"CREATE TABLE cfp.ev(id bigint, at timestamptz NOT NULL) PARTITION BY RANGE (at)",
      "CREATE TABLE cfp.ev_2025 PARTITION OF cfp.ev FOR VALUES FROM ('2025-01-01') TO ('2026-01-01')",
      "CREATE TABLE cfp.ev_2026(id bigint, at timestamptz NOT NULL)"},
     json{{"kind","attach_partition"},{"schema","cfp"},{"table","ev"},
          {"partition","ev_2026"},{"from","'2026-01-01'"},{"to","'2027-01-01'"}},
     "SELECT count(*)::text FROM pg_inherits WHERE inhparent='cfp.ev'::regclass", "2"},

    {"detach_partition", {},
     json{{"kind","detach_partition"},{"schema","cfp"},{"table","ev"},{"partition","ev_2026"}},
     "SELECT count(*)::text FROM pg_inherits WHERE inhparent='cfp.ev'::regclass", "1"},

    {"create_domain_via_create_type", {},   // alias case; kind is create_type
     json{{"kind","create_type"},{"schema","cf"},{"name","pos"},{"type_kind","domain"},
          {"base","integer"},{"check","VALUE > 0"},{"comment","Positive."}},
     "SELECT typtype::text FROM pg_type WHERE typname='pos'", "d"},

    {"alter_domain", {},
     json{{"kind","alter_domain"},{"schema","cf"},{"name","pos"},
          {"add_check","VALUE < 1000000"},{"constraint_name","pos_lt"}},
     "SELECT convalidated::text FROM pg_constraint WHERE conname='pos_lt'", "true"},

    {"drop_type", {},
     json{{"kind","drop_type"},{"schema","cf"},{"name","pos"}},
     "SELECT count(*)::text FROM pg_type WHERE typname='pos'", "0"},

    {"drop_expression",
     {"CREATE TABLE cf.gen(a int, b int GENERATED ALWAYS AS (a*2) STORED)"},
     json{{"kind","drop_expression"},{"schema","cf"},{"table","gen"},{"column","b"}},
     "SELECT attgenerated::text FROM pg_attribute WHERE attrelid='cf.gen'::regclass"
     " AND attname='b'", ""},

    {"drop_function", {},
     json{{"kind","drop_function"},{"schema","cf"},{"name","touch"},
          {"arguments",json::array()}},
     "SELECT count(*)::text FROM pg_proc WHERE proname='touch'", "0"},

    {"drop_sequence", {},
     json{{"kind","drop_sequence"},{"schema","cf"},{"name","sq"}},
     "SELECT count(*)::text FROM pg_class WHERE relname='sq'", "0"},

    {"alter_schema", {},
     json{{"kind","alter_schema"},{"schema","cfp"},{"to","cfq"}},
     "SELECT count(*)::text FROM pg_namespace WHERE nspname='cfq'", "1"},

    {"drop_extension", {},
     json{{"kind","drop_extension"},{"name","pg_trgm"}},
     "SELECT count(*)::text FROM pg_extension WHERE extname='pg_trgm'", "0"},

    {"drop_schema",
     {"DROP TABLE IF EXISTS cfq.ev CASCADE", "DROP TABLE IF EXISTS cfq.ev_2026 CASCADE"},
     json{{"kind","drop_schema"},{"schema","cfq"}},
     "SELECT count(*)::text FROM pg_namespace WHERE nspname='cfq'", "0"},
  };
}

}  // namespace conformance
