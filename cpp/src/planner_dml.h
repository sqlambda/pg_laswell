#pragma once

// The row-level DML planners: backfill, insert_rows, update_rows, delete_rows,
// merge_rows and copy_rows.
//
// Split out of planner.h under the plan in docs/ROADMAP.md 6.7. planner.h
// includes this and keeps the single plan_migration() entry point; nothing here
// is called from anywhere else.
//
// WHAT THESE SIX SHARE, and why they are one family rather than six unrelated
// kinds: every one of them has to answer "which rows", and the answer decides
// how the statement is PACED. That is the same shape as the rest of this tool
// -- measure, then say which reading decided the method -- applied to data
// instead of to schema.
//
//   values   the row count is known EXACTLY, at plan time, because the rows are
//            in the spec. Small: one transaction, because a half-applied
//            reference table is worse than one that failed and can be retried.
//            Large: paced, because one transaction over a long list holds its
//            locks for the whole of it.
//   select   the row count is NOT derivable. planner.h is a pure function with
//            no database in it, so the honest answer is to pace unconditionally
//            rather than to state a number this tool cannot measure. Pacing a
//            small result costs one extra round trip.
//   where    a predicate over the target, delete_rows and backfill only. Paced,
//            for the same reason: reltuples is an estimate of the TABLE, not of
//            the predicate.
//
// THE EXECUTOR NEEDS NOTHING NEW. run_backfill() in executor.h is generic over
// the statement -- $1 is the cursor, $2 the batch size, and column 0 of the
// result advances the cursor -- so five of the six are a planner change and
// nothing else. copy_rows is the exception and says why at its own comment.
//
// EVERY SQL SHAPE BELOW WAS MEASURED on PostgreSQL 18.6 before it was written;
// cpp/test/spikes/s21_dml.sh is the script and each finding is quoted at the
// code it decided. Three of them changed the design rather than confirming it:
//
//   * MERGE ... RETURNING returns NOTHING for a source row that matched no WHEN
//     clause, so a paced MERGE that used it would read affected == 0 as "no
//     rows left" and report success having merged nothing. Every paced
//     statement here therefore names its cursor from an outer SELECT over the
//     batch CTE, never from the mutation's own RETURNING.
//   * A (VALUES ...) join column needs an EXPLICIT CAST on the first row.
//     Without one, uuid = text is "operator does not exist" -- while int and
//     text happen to work, which is exactly what makes it easy to ship broken.
//   * Explicit values for an identity column do not advance its sequence, so a
//     seed that succeeds leaves the application's next INSERT failing on a
//     duplicate key. These kinds emit the setval that closes the gap.

#include "planner_base.h"

namespace pglaswell {

namespace detail {

// --- rendering literal rows -------------------------------------------------

// One JSON scalar as a SQL literal. Spec cells are scalars only (spec.h refuses
// objects and arrays), so a jsonb or array value arrives as a STRING in
// PostgreSQL's own input syntax and the column-typed cast below parses it.
inline std::string render_cell(const json& cell) {
  if (cell.is_null()) return "NULL";
  if (cell.is_boolean()) return cell.get<bool>() ? "true" : "false";
  if (cell.is_number()) return cell.dump();
  return quote_literal(cell.get<std::string>());
}

// The declared type of a column, from the observation, for use as a cast.
// Falls back to text only when the column is not in the catalog at all -- which
// the callers refuse before they get here, so it is a belt-and-braces default
// rather than a path anything takes.
inline std::string column_type(const json& columns, const std::string& name) {
  if (columns.contains(name)) return columns[name].value("type", "text");
  return "text";
}

// A literal row set as a joinable relation: (VALUES (..),(..)) AS alias(cols).
//
// The casts on the FIRST row are load-bearing, not decoration. Measured on 18.6
// (S21): with a uuid key,
//
//   UPDATE ty SET .. FROM (VALUES ('1111..','9.50')) AS u(id,amt)
//    WHERE ty.id = u.id
//   ERROR:  operator does not exist: uuid = text
//
// and the same statement with ::uuid and ::numeric on row one succeeds. Rows
// two onward inherit the resolved type -- also measured -- so the cast appears
// once rather than on every row, which keeps a thousand-row seed readable in
// the ledger.
inline std::string values_relation(const json& values, const json& columns,
                                   const std::vector<std::string>& names,
                                   const std::string& alias,
                                   const json& observed_columns) {
  std::string out = "(VALUES ";
  std::size_t r = 0;
  for (const auto& row : values) {
    if (r != 0) out += ",\n        ";
    out += "(";
    for (std::size_t c = 0; c < names.size(); ++c) {
      if (c != 0) out += ", ";
      out += render_cell(row[c]);
      if (r == 0) out += "::" + column_type(observed_columns, names[c]);
    }
    out += ")";
    ++r;
  }
  (void)columns;
  std::vector<std::string> quoted;
  for (const auto& n : names) quoted.push_back(quote_identifier(n));
  out += ") AS " + alias + " (" + join(quoted, ", ") + ")";
  return out;
}

inline std::vector<std::string> column_names(const Intent& in) {
  std::vector<std::string> names;
  for (const auto& c : in.body.value("columns", json::array())) {
    names.push_back(c.get<std::string>());
  }
  return names;
}

// --- the pacing decision ----------------------------------------------------

struct RowSource {
  enum class Form { kValues, kSelect, kWhere } form = Form::kValues;
  long long row_count = -1;   // exact for values, -1 when not derivable
  bool paced = false;
  std::string why;            // the reading that decided it, in full
};

inline RowSource decide_pacing(const Intent& in, const ExecutorConfig& cfg,
                               bool key_available) {
  RowSource s;
  if (in.body.contains("values")) {
    s.form = RowSource::Form::kValues;
    s.row_count = static_cast<long long>(in.body["values"].size());
  } else if (in.body.contains("select")) {
    s.form = RowSource::Form::kSelect;
  } else {
    s.form = RowSource::Form::kWhere;
  }

  const bool forced = in.body.contains("paced");
  const bool want = forced ? in.body["paced"].get<bool>()
                           : (s.form != RowSource::Form::kValues ||
                              s.row_count > cfg.dml_single_txn_rows);

  // Pacing needs a key to walk by. Without one the change still runs, in one
  // transaction, and the plan says plainly that it could not be paced rather
  // than pretending it was.
  s.paced = want && key_available;

  if (s.form == RowSource::Form::kValues) {
    const auto n = std::to_string(s.row_count);
    if (forced) {
      s.why = n + " literal rows; pacing was set explicitly to " +
              (want ? "true" : "false") + " in the spec";
    } else if (s.row_count > cfg.dml_single_txn_rows) {
      s.why = n + " literal rows, above dml_single_txn_rows (" +
              std::to_string(cfg.dml_single_txn_rows) +
              "), so this is paced into short transactions rather than held "
              "open for the whole list";
    } else {
      s.why = n + " literal rows, at or below dml_single_txn_rows (" +
              std::to_string(cfg.dml_single_txn_rows) +
              "), so this runs in ONE transaction and is all-or-nothing -- a "
              "half-applied set of literal rows is worse than one that failed";
    }
  } else if (s.form == RowSource::Form::kSelect) {
    s.why = "the row count of a select cannot be measured without running it, "
            "and this planner has no database in it, so this is paced rather "
            "than sized by a number the tool cannot derive";
  } else {
    s.why = "a predicate matches an unknown number of rows -- reltuples "
            "estimates the TABLE, not the filter -- so this is paced";
  }
  if (want && !key_available) {
    s.why += ". It could NOT be paced: no key column was given, so there is "
             "nothing to walk by, and it runs in one transaction";
  }
  return s;
}

// --- refusals that PostgreSQL would raise at execution ----------------------
//
// All three measured on 18.6 (S21). Raising them in the planner turns an error
// that arrives after some batches have COMMITTED into one that arrives before
// anything runs.
inline bool refuse_unwritable_columns(const Intent& in, const json& columns,
                                      const std::string& qualified,
                                      const std::vector<std::string>& names,
                                      Plan& plan, Step& step) {
  const auto overriding = in.body.value("overriding", "");
  for (const auto& name : names) {
    if (!columns.contains(name)) {
      step.action = Action::kConflict;
      step.why = qualified + "." + name + " does not exist";
      plan.conflicts.push_back(
          step.why +
          "; if an earlier intent in this spec adds it, this intent must come "
          "after it, and the planner does not reorder intents.");
      return true;
    }
    const auto& col = columns[name];
    if (col.value("generated", "") == "s") {
      step.action = Action::kConflict;
      step.why = qualified + "." + name + " is a generated column";
      plan.conflicts.push_back(
          qualified + "." + name +
          " is GENERATED ALWAYS AS ... STORED, and PostgreSQL refuses any "
          "statement that supplies a value for it -- measured on 18.6: "
          "\"cannot insert a non-DEFAULT value into column\". Drop it from "
          "\"columns\"; the database computes it from the columns that remain.");
      return true;
    }
    if (col.value("identity", "") == "a" && overriding != "system") {
      step.action = Action::kConflict;
      step.why = qualified + "." + name + " is GENERATED ALWAYS AS IDENTITY";
      plan.conflicts.push_back(
          qualified + "." + name +
          " is GENERATED ALWAYS AS IDENTITY, so supplying a value is an error "
          "-- measured on 18.6, with PostgreSQL's own hint: \"Use OVERRIDING "
          "SYSTEM VALUE to override.\" Either drop the column from \"columns\" "
          "and let the sequence assign it, or set \"overriding\": \"system\" "
          "and say in the rationale why the generated values are being "
          "overridden.");
      return true;
    }
  }
  return false;
}

// The sequence a set of explicit values leaves behind.
//
// The failure this prevents, measured end to end on 18.6 (S21): five rows
// inserted with explicit ids 1..5 into a GENERATED BY DEFAULT identity column
// left the sequence unset, and the application's very next INSERT died with
// "duplicate key value violates unique constraint". The migration reported
// success. Nothing in the catalog afterwards says the seed was responsible.
//
// The statement derives its number from the table rather than from the spec, so
// it stays correct when the seed was partly applied already, and it writes
// nothing when the table is empty -- setval(seq, NULL) would be an error, and a
// FROM with no rows produces no call at all. Also measured.
inline void emit_sequence_catchup(const Intent& in, const json& columns,
                                  const std::string& qualified,
                                  const std::vector<std::string>& names,
                                  std::vector<Step>& out) {
  const auto sql_rel = quote_qualified(qualified);
  for (const auto& name : names) {
    if (!columns.contains(name)) continue;
    const auto& col = columns[name];
    if (col.value("sequence", json()).is_null()) continue;
    const auto seq = col.value("sequence", "");
    if (seq.empty()) continue;

    Step step;
    step.kind = in.kind_name;
    step.txn_class = TxnClass::kRequired;
    step.lock = "no table lock; a row lock on the sequence for the duration of "
                "the setval";
    step.sql.push_back(
        "SELECT setval(" + quote_literal(seq) + ", s.v)\n"
        "  FROM (SELECT max(" + quote_identifier(name) + ") AS v FROM " +
        sql_rel + ") s\n"
        " WHERE s.v IS NOT NULL;");
    step.why =
        "explicit values were supplied for " + qualified + "." + name +
        ", which draws from " + seq +
        ". Inserting an explicit value does NOT advance a sequence, so without "
        "this the application's next insert collides on a duplicate key -- "
        "after the migration has reported success. Measured on 18.6";
    step.detail["sequence"] = seq;
    step.detail["column"] = name;
    step.detail["derives_value_from"] =
        "max(" + name + ") read from the table at execution time, never from "
        "the spec, so it is correct when the seed was already partly applied";
    out.push_back(std::move(step));
  }
}

// What an author cannot see from the plan and will not see from an error.
inline void warn_about_row_security(const json& t, const std::string& qualified,
                                    bool writes_new_rows, Plan& plan) {
  const auto rs = t.value("row_security", json::object());
  if (!rs.value("enabled", false)) return;
  plan.warnings.push_back(
      qualified +
      " has row-level security enabled, and this step writes rows through it. "
      "The two halves fail differently and only one of them is visible: a WITH "
      "CHECK policy REFUSES a row loudly, but a USING policy silently narrows "
      "which rows are matched at all -- measured on 18.6, a DELETE naming two "
      "ids removed one, reported \"DELETE 1\", and said nothing about the "
      "other. " +
      std::string(writes_new_rows
                      ? "Rows this step inserts must satisfy the WITH CHECK "
                        "clause of every applicable policy."
                      : "Rows this step means to change may not be visible to "
                        "the role running the migration at all.") +
      (rs.value("forced", false)
           ? " force_row_level_security is ON, so the table's owner is subject "
             "to the policies too."
           : " force_row_level_security is OFF, so if the migration runs as the "
             "table's owner it BYPASSES the policies entirely and this will "
             "behave differently from the application -- which is its own kind "
             "of surprise."));
}

}  // namespace detail

// --- backfill --------------------------------------------------------------
//
// One expression applied to many rows. The oldest kind in this family and the
// one whose pacing every other kind here reuses.
inline void plan_backfill(const Intent& in, const Observations& obs,
                          const ExecutorConfig& cfg, Plan& plan,
                          std::vector<Step>& out) {
  Step step;
  step.kind = in.kind_name;
  struct Emit { std::vector<Step>& o; Step& s; ~Emit() { o.push_back(s); } } emit{out, step};
  const auto qualified = in.qualified_table();
  const auto sql_rel = detail::quote_qualified(qualified);
  const auto& t = obs.table(qualified);
  const auto key = in.body.value("key", "");

  step.txn_class = TxnClass::kOwnTxnPerBatch;
  step.lock = "RowExclusiveLock plus row locks, released at every commit";

  if (!t.value("exists", false)) {
    step.action = Action::kConflict;
    step.why = qualified + " does not exist";
    plan.conflicts.push_back(step.why);
    return;
  }

  const json columns = t.value("columns", json::object());
  for (auto it = in.body["set"].begin(); it != in.body["set"].end(); ++it) {
    if (!columns.contains(it.key())) {
      step.action = Action::kConflict;
      step.why = qualified + "." + it.key() + " does not exist";
      plan.conflicts.push_back(
          step.why +
          "; if an earlier intent in this spec adds it, the backfill must come "
          "after that intent, and the planner does not reorder intents.");
      return;
    }
  }

  // A keyset walk needs a unique key, or the cursor can skip or repeat rows.
  // Refused loudly rather than falling back to OFFSET, which degrades to a
  // full scan per batch and is quadratic in the table size.
  bool key_is_unique = false;
  std::string supporting_index;
  // Bound to a local: json::value() returns BY VALUE, so calling it in both
  // begin() and end() yields iterators into two different temporaries. Same
  // defect class as std::ostringstream::str() -- it compiles, and nlohmann
  // catches it at runtime with "cannot compare iterators of different
  // containers", which is a much better outcome than the silent corruption
  // the equivalent std:: idiom would give.
  const json indexes = t.value("indexes", json::object());
  for (auto it = indexes.begin(); it != indexes.end(); ++it) {
    if (it.value().value("leading_column", "") != key) continue;
    supporting_index = it.key();
    if (it.value().value("is_unique", false) && it.value().value("is_valid", false)) {
      key_is_unique = true;
      break;
    }
  }
  if (!key_is_unique) {
    step.action = Action::kConflict;
    step.why = "no unique index leads with " + key;
    plan.conflicts.push_back(
        qualified + " has no valid unique index whose leading column is \"" + key +
        "\", so a keyset walk could skip or repeat rows. Create one first: "
        "CREATE UNIQUE INDEX CONCURRENTLY ... ON " + qualified + " (" + key + ");");
    return;
  }

  const long long rows = t.value("reltuples", 0LL);
  const auto where = in.body.value("where", "");
  const auto from = in.body.value("from", "");

  std::vector<std::string> assignments;
  for (auto it = in.body["set"].begin(); it != in.body["set"].end(); ++it) {
    assignments.push_back(detail::quote_identifier(it.key()) + " = " +
                          it.value().get<std::string>());
  }

  // FOR UPDATE without SKIP LOCKED, deliberately. SKIP LOCKED would silently
  // skip contended rows while the cursor advanced past them, leaving a
  // backfill that reports complete and is not. It looks like the obviously
  // right pacing idiom and is wrong here.
  // The `from` items appear in BOTH the CTE and the outer UPDATE, because the
  // filter may reference them -- a backfill whose predicate joins to another
  // table is the ordinary case, not the exotic one. An earlier version put
  // them only in the outer UPDATE, which produced a CTE referencing an alias
  // that was not in scope: SQL that renders convincingly and does not parse.
  //
  // FOR UPDATE OF t, not a bare FOR UPDATE: with a join, a bare FOR UPDATE
  // locks rows in every table named, so the backfill would take row locks on
  // the lookup table it merely reads. That is contention this tool exists to
  // avoid, inflicted by its own batch statement.
  // The target table is referenced by its OWN NAME, not by an alias.
  //
  // A spec's `where` and `set` expressions have to name the target somehow, and
  // that choice is an interface contract: `orders.warehouse_id = w.id` and
  // `t.warehouse_id = w.id` are both plausible, and a spec written for one
  // fails against the other. Using the table's own name is what someone
  // writing this SQL by hand would do, and it needs no explanation -- an alias
  // would be a convention every author had to learn from a footnote.
  //
  // The `from` items appear in BOTH the CTE and the outer UPDATE, because the
  // filter may reference them: a backfill whose predicate joins to another
  // table is the ordinary case. An earlier version put them only in the outer
  // UPDATE, producing a CTE that referenced an alias not in scope -- SQL that
  // renders convincingly and does not parse.
  //
  // FOR UPDATE OF <target>, not a bare FOR UPDATE: with a join, a bare FOR
  // UPDATE locks rows in every table named, so the backfill would take row
  // locks on the lookup table it merely reads. That is contention this tool
  // exists to avoid, inflicted by its own batch statement.
  // The target referenced by its own bare name, quoted. The contract that a
  // spec's `where` and `set` name the target by its own name is unchanged;
  // what changes is that "order"."end" is emitted rather than order.end, which
  // is the same reference for every name and the only correct one for a
  // reserved word.
  const std::string rel = detail::quote_identifier(in.table());

  // The pre-image, captured in the SAME STATEMENT as the update.
  //
  // Data-modifying CTEs all see one snapshot, so an INSERT ... SELECT reading
  // the target inside this statement sees the rows as they were BEFORE the
  // UPDATE in the same statement. That is what makes the capture atomic with
  // the change: there is no window in which one committed and the other did
  // not, and a crash leaves the backup and the data agreeing.
  //
  // This is what replaced the pinned-snapshot idea. A snapshot lets you LOOK at
  // the old values while holding back the xmin horizon for the whole backfill;
  // this KEEPS them, durably, and doubles as the revert path -- which a
  // snapshot can never be.
  std::string preserve_cte;
  if (in.body.contains("preserve")) {
    const auto pschema = in.body["preserve"].value("schema", "");
    const auto ptable = in.body["preserve"].value("table", "");
    const auto preserved = pschema + "." + ptable;
    // Raw above for the prose and the step detail; quoted here for SQL.
    const auto sql_preserved = detail::quote_qualified(preserved);

    std::vector<std::string> saved_cols{key};
    for (auto it = in.body["set"].begin(); it != in.body["set"].end(); ++it) {
      saved_cols.push_back(it.key());
    }

    // The side table is created by its own step, from the target's real column
    // types -- LIKE would carry constraints and defaults that have no business
    // on a backup.
    std::vector<std::string> quoted_saved_cols;
    for (const auto& c : saved_cols) {
      quoted_saved_cols.push_back(detail::quote_identifier(c));
    }

    std::string cols_ddl;
    for (const auto& c : saved_cols) {
      const auto type = columns.contains(c)
                            ? columns[c].value("type", "text")
                            : std::string("text");
      if (!cols_ddl.empty()) cols_ddl += ", ";
      cols_ddl += detail::quote_identifier(c) + " " + type;
    }
    Step create;
    create.kind = in.kind_name;
    create.txn_class = TxnClass::kRequired;
    create.own_transaction = true;
    create.lock = "AccessExclusiveLock on the new table only";
    create.sql.push_back("CREATE TABLE IF NOT EXISTS " + sql_preserved + " (" +
                         cols_ddl + ", laswell_saved_at timestamptz NOT NULL DEFAULT now());");
    create.sql.push_back("COMMENT ON TABLE " + sql_preserved + " IS " +
                         detail::quote_literal(
                             "Pre-image captured by pg_laswell before backfilling " +
                             sql_rel + ". Each row is what the target looked "
                             "like before the change, written in the same "
                             "transaction as the change itself.") + ";");
    create.why = "preserve: the pre-image needs somewhere to live, and it must "
                 "exist before the first batch writes to it";
    out.push_back(std::move(create));

    std::string select_cols;
    for (const auto& c : saved_cols) {
      if (!select_cols.empty()) select_cols += ", ";
      select_cols += rel + "." + detail::quote_identifier(c);
    }
    preserve_cte = ", preserved AS (\n"
                   "  INSERT INTO " + sql_preserved + " (" +
                   detail::join(quoted_saved_cols, ", ") + ")\n"
                   "  SELECT " + select_cols + "\n"
                   "    FROM " + sql_rel + ", batch AS pb\n"
                   "   WHERE " + rel + "." + detail::quote_identifier(key) + " = pb." + detail::quote_identifier(key) + "\n"
                   ")";
    step.detail["preserve"] = preserved;
  }

  const std::string batch_sql =
      "WITH batch AS (\n"
      "  SELECT " + rel + "." + detail::quote_identifier(key) + "\n"
      "    FROM " + sql_rel + (from.empty() ? "" : ", " + from) + "\n"
      "   WHERE " + rel + "." + detail::quote_identifier(key) + " > $1 AND (" + where + ")\n"
      "   ORDER BY " + rel + "." + detail::quote_identifier(key) + "\n"
      "   LIMIT $2\n"
      "   FOR UPDATE OF " + rel + "\n"
      ")" + preserve_cte + "\n"
      "UPDATE " + sql_rel + "\n"
      "   SET " + detail::join(assignments, ", ") + "\n"
      "  FROM batch AS b" + (from.empty() ? "" : ", " + from) + "\n"
      " WHERE " + rel + "." + detail::quote_identifier(key) + " = b." + detail::quote_identifier(key) + "\n"
      "RETURNING " + rel + "." + detail::quote_identifier(key) + ";";

  step.sql.push_back(batch_sql);
  step.detail["qualified"] = qualified;
  // Carried so the executor can check a resume cursor against the predicate
  // rather than trusting it.
  step.detail["where"] = where;
  step.detail["batch_rows"] = cfg.batch_rows;
  step.detail["commit_interval_ms"] = cfg.commit_interval_ms;
  step.detail["batch_cap_rows"] = cfg.batch_cap_rows;
  step.detail["rows_estimated"] = rows;
  step.detail["key"] = key;
  step.detail["supporting_index"] = supporting_index;
  step.detail["commits_on"] =
      json::array({"lock_waiter", "interval", "batch_cap"});

  if (in.body.contains("verify_remaining")) {
    step.detail["verify_remaining"] = in.body["verify_remaining"];
    // Runs on the WORKER connection, after the last batch. Verification that
    // must see in-flight rows cannot run anywhere else: an imported snapshot
    // does not see the exporter's uncommitted changes (spike S1).
    step.detail["verify_runs_on"] = "worker connection, after the final batch";
  }
  if (in.body.contains("assert_invariants")) {
    step.detail["assert_invariants"] = in.body["assert_invariants"];
    step.detail["invariants_run_on"] =
        "the worker connection, before the first batch and after the last. "
        "They ask whether the work broke something, which verify_remaining "
        "does not: a backfill can complete every row and still halve a total.";
  }

  step.why = std::to_string(rows) + " rows estimated; keyset walk on " + key +
             " via " + supporting_index + ", " + std::to_string(cfg.batch_rows) +
             " rows per batch, committing on a lock waiter or " +
             std::to_string(cfg.commit_interval_ms) + "ms";

  // Without an index that supports (key) under the filter, each batch may
  // rescan from the start -- quadratic in the table size. A warning rather
  // than a refusal, because on a small table it does not matter and the
  // operator may know that.
  bool filtered_index = false;
  for (auto it = indexes.begin(); it != indexes.end(); ++it) {
    const auto def = it.value().value("definition", "");
    if (def.find(" WHERE ") != std::string::npos &&
        it.value().value("leading_column", "") == key) {
      filtered_index = true;
    }
  }
  if (!filtered_index && rows > 1000000) {
    plan.warnings.push_back(
        "no partial index supports (" + key + ") under the backfill's filter on " +
        qualified + "; at " + std::to_string(rows) +
        " estimated rows each batch may rescan already-updated rows. Consider "
        "a partial index before running this against a live system.");
  }
}

// --- the shape every paced row-level statement takes ------------------------
//
// A batch CTE that names the rows, the mutation as a data-modifying CTE beside
// it, and an outer SELECT that hands the executor its cursor.
//
// The outer SELECT is the part that is easy to get wrong, so it is stated once
// here and shared by all four kinds. The obvious shape -- letting the mutation's
// own RETURNING advance the cursor, as backfill does -- is WRONG for anything
// that may not act on every row it considered. Measured on 18.6 (S21):
//
//   WITH batch AS (SELECT * FROM m_s WHERE m_s.id > 5 ORDER BY m_s.id LIMIT 5)
//   MERGE INTO m_t USING batch s ON m_t.id = s.id
//     WHEN MATCHED THEN UPDATE SET v = s.v
//   RETURNING s.id;
//   -- (0 rows)
//
// Five source rows were considered and none matched, so RETURNING produced
// nothing, and executor.h reads a zero-row result as "the walk is finished".
// The step would report success having merged nothing at all. The same trap is
// waiting for an UPDATE whose rows have gone, a DELETE over keys that are
// already deleted, and an INSERT whose every row hits ON CONFLICT DO NOTHING.
//
// Naming the cursor from the BATCH instead is correct for all of them: the
// cursor means "every row up to here has been CONSIDERED", which is the claim
// the walk actually needs. ORDER BY makes it the maximum rather than whichever
// row came back last -- also required, because the executor keeps the last row
// it reads and PostgreSQL guarantees no order without one.
//
// Data-modifying CTEs run whether or not the outer query reads them (measured:
// the unreferenced INSERT ... ON CONFLICT still wrote its rows), so the
// mutation happening in a CTE nobody selects from is deliberate and safe.
inline std::string paced_statement(const std::string& source_relation,
                                   const std::string& key,
                                   const std::string& mutation_cte) {
  const auto k = detail::quote_identifier(key);
  return "WITH src AS (\n" + source_relation + "\n"
         "), batch AS (\n"
         "  SELECT * FROM src WHERE src." + k + " > $1 ORDER BY src." + k + " LIMIT $2\n"
         "), " + mutation_cte + "\n"
         "SELECT batch." + k + " FROM batch ORDER BY batch." + k + ";";
}

namespace detail {

// The rows a row-level intent operates on, as a relation the statement can
// join to: either the literal VALUES table or the spec's own select.
inline std::string source_relation(const Intent& in,
                                  const std::vector<std::string>& names,
                                  const json& observed_columns) {
  if (in.body.contains("select")) {
    return "  " + in.body.value("select", "");
  }
  return "  SELECT * FROM " +
         values_relation(in.body["values"], in.body["columns"], names, "v",
                         observed_columns);
}

// Carried on every paced step so executor.h can run it and jobStatus can
// report it. `where` is what the resume path re-checks a recorded cursor
// against; it is left empty for the forms whose rows do not come from the
// target table, which disables that check rather than running it against the
// wrong relation.
inline void attach_pacing_detail(Step& step, const Intent& in,
                                 const ExecutorConfig& cfg,
                                 const RowSource& src,
                                 const std::string& qualified,
                                 const std::string& key,
                                 const std::string& resume_where) {
  step.txn_class = TxnClass::kOwnTxnPerBatch;
  step.detail["qualified"] = qualified;
  step.detail["key"] = key;
  step.detail["where"] = resume_where;
  step.detail["batch_rows"] = cfg.batch_rows;
  step.detail["commit_interval_ms"] = cfg.commit_interval_ms;
  step.detail["batch_cap_rows"] = cfg.batch_cap_rows;
  step.detail["rows_estimated"] = src.row_count < 0 ? 0LL : src.row_count;
  step.detail["commits_on"] = json::array({"lock_waiter", "interval", "batch_cap"});
  step.detail["cursor_names"] =
      "the batch, not the mutation's RETURNING: a row that matched no target "
      "row still has to advance the cursor, or the walk stops early and "
      "reports success";
  if (in.body.contains("verify_remaining")) {
    step.detail["verify_remaining"] = in.body["verify_remaining"];
    step.detail["verify_runs_on"] = "worker connection, after the final batch";
  }
  if (in.body.contains("assert_invariants")) {
    step.detail["assert_invariants"] = in.body["assert_invariants"];
    step.detail["invariants_run_on"] =
        "the worker connection, before the first batch and after the last";
  }
}

// The common opening every row-level planner needs: the table exists, it is a
// table, and the key is real. Returns false when the step has been refused.
inline bool row_target_ok(const json& t, const std::string& qualified,
                          const std::string& key, Plan& plan, Step& step) {
  if (!t.value("exists", false)) {
    step.action = Action::kConflict;
    step.why = qualified + " does not exist";
    plan.conflicts.push_back(step.why);
    return false;
  }
  const auto kind = t.value("kind", "table");
  if (kind == "view" || kind == "materialized_view") {
    step.action = Action::kConflict;
    step.why = qualified + " is a " + kind;
    plan.conflicts.push_back(
        qualified + " is a " + kind +
        ", not a table. Writing through a view needs an INSTEAD OF trigger or "
        "an auto-updatable definition, and neither is something this planner "
        "can measure -- so it refuses rather than emitting a statement whose "
        "effect it cannot predict.");
    return false;
  }
  if (!key.empty() && !t.value("columns", json::object()).contains(key)) {
    step.action = Action::kConflict;
    step.why = qualified + "." + key + " does not exist";
    plan.conflicts.push_back(step.why + ", so there is nothing to match rows by.");
    return false;
  }
  return true;
}

// Whether a unique index leads with `key`, and which one. This is the
// difference between a keyset walk that is correct and one that skips or
// repeats rows, and -- for merge_rows -- between a MERGE that updates one row
// and one that silently updates several.
inline bool unique_key_index(const json& t, const std::string& key,
                             std::string& index_name) {
  const json indexes = t.value("indexes", json::object());
  bool found = false;
  for (auto it = indexes.begin(); it != indexes.end(); ++it) {
    if (it.value().value("leading_column", "") != key) continue;
    if (index_name.empty()) index_name = it.key();
    if (it.value().value("is_unique", false) && it.value().value("is_valid", false)) {
      index_name = it.key();
      found = true;
      break;
    }
  }
  return found;
}

// Two rows in `values` claiming the same key. Every kind here breaks on it, and
// each breaks differently: an INSERT hits the unique constraint, a MERGE raises
// "MERGE command cannot affect row a second time" (measured), and an UPDATE
// silently applies whichever row PostgreSQL reaches last. Refusing it once,
// here, beats three different failures at three different times.
inline bool refuse_duplicate_keys(const Intent& in,
                                  const std::vector<std::string>& names,
                                  const std::string& key, Plan& plan,
                                  Step& step) {
  if (!in.body.contains("values") || key.empty()) return false;
  std::size_t idx = 0;
  bool found = false;
  for (std::size_t i = 0; i < names.size(); ++i) {
    if (names[i] == key) { idx = i; found = true; break; }
  }
  if (!found) return false;

  std::set<std::string> seen;
  for (const auto& row : in.body["values"]) {
    const auto rendered = render_cell(row[idx]);
    if (!seen.insert(rendered).second) {
      step.action = Action::kConflict;
      step.why = "\"values\" gives key " + rendered + " twice";
      plan.conflicts.push_back(
          "\"values\" contains the key " + rendered +
          " more than once. PostgreSQL breaks on this three different ways "
          "depending on the statement -- an INSERT violates the unique "
          "constraint, a MERGE raises \"MERGE command cannot affect row a "
          "second time\" (measured on 18.6), and an UPDATE quietly applies "
          "whichever row it reaches last -- so it is refused here instead. "
          "Decide which of the two rows is meant.");
      return true;
    }
  }
  return false;
}

}  // namespace detail


// --- insert_rows ------------------------------------------------------------
//
// New rows: seed data, reference data, a backfill of a table from another one.
//
// The decisions it makes, each from a reading rather than from a preference:
// whether one transaction or a paced walk (the row source); whether ON CONFLICT
// can name an arbiter at all (the unique indexes); whether the columns named
// may be written (attidentity / attgenerated); and whether a sequence is left
// behind (the column's owning sequence, plus what the spec supplies).
inline void plan_insert_rows(const Intent& in, const Observations& obs,
                             const ExecutorConfig& cfg, Plan& plan,
                             std::vector<Step>& out) {
  Step step;
  step.kind = in.kind_name;
  const auto qualified = in.qualified_table();
  const auto sql_rel = detail::quote_qualified(qualified);
  const auto& t = obs.table(qualified);
  const auto key = in.body.value("key", "");
  const auto names = detail::column_names(in);

  // Emitted through a guard so every early return still records the step, and
  // so the sequence catch-up lands AFTER the insert that made it necessary.
  bool emitted = false;
  struct Emit {
    std::vector<Step>& o; Step& s; bool& done;
    ~Emit() { if (!done) o.push_back(s); }
  } emit{out, step, emitted};

  if (!detail::row_target_ok(t, qualified, key, plan, step)) return;
  const json columns = t.value("columns", json::object());
  if (detail::refuse_unwritable_columns(in, columns, qualified, names, plan, step)) return;
  if (detail::refuse_duplicate_keys(in, names, key, plan, step)) return;

  // ON CONFLICT's arbiter is not optional and has no default. Measured on 18.6:
  // with no matching unique index the statement is refused outright rather than
  // falling back to the primary key, and against a PARTIAL unique index the
  // arbiter has to repeat the index predicate or it is refused too.
  const auto on_conflict = in.body.value("on_conflict", "refuse");
  std::string conflict_clause;
  if (on_conflict != "refuse") {
    std::vector<std::string> target;
    for (const auto& c : in.body["conflict_target"]) {
      target.push_back(c.get<std::string>());
    }
    const auto conflict_where = in.body.value("conflict_where", "");

    const json indexes = t.value("indexes", json::object());
    std::string arbiter;
    std::string partial_candidate, partial_predicate;
    for (auto it = indexes.begin(); it != indexes.end(); ++it) {
      const auto& ix = it.value();
      if (!ix.value("is_unique", false) || !ix.value("is_valid", false)) continue;
      if (ix.value("has_expressions", false)) continue;
      std::vector<std::string> cols;
      for (const auto& c : ix.value("columns", json::array())) {
        if (c.is_string()) cols.push_back(c.get<std::string>());
      }
      if (cols != target) continue;
      const auto predicate = ix.value("predicate", "");
      if (predicate.empty() && conflict_where.empty()) { arbiter = it.key(); break; }
      if (!predicate.empty()) {
        partial_candidate = it.key();
        partial_predicate = predicate;
        if (!conflict_where.empty()) { arbiter = it.key(); break; }
      }
    }
    if (arbiter.empty()) {
      step.action = Action::kConflict;
      step.why = "no unique index matches the ON CONFLICT target";
      if (!partial_candidate.empty() && conflict_where.empty()) {
        plan.conflicts.push_back(
            qualified + " has a unique index " + partial_candidate +
            " on (" + detail::join(target, ", ") +
            ") but it is PARTIAL, with predicate " + partial_predicate +
            ". Measured on 18.6, ON CONFLICT (" + detail::join(target, ", ") +
            ") is refused against a partial index and ON CONFLICT (" +
            detail::join(target, ", ") + ") WHERE " + partial_predicate +
            " is accepted. Add \"conflict_where\": " +
            detail::quote_literal(partial_predicate) + ".");
      } else {
        plan.conflicts.push_back(
            qualified + " has no valid unique index on (" +
            detail::join(target, ", ") +
            "), so ON CONFLICT has nothing to arbitrate on. Measured on 18.6, "
            "PostgreSQL refuses this at execution -- \"there is no unique or "
            "exclusion constraint matching the ON CONFLICT specification\" -- "
            "rather than falling back to the primary key. Create the index "
            "first, in an earlier intent, or name the columns that a unique "
            "index does cover.");
      }
      return;
    }

    conflict_clause = "ON CONFLICT (" + detail::join(
        [&] {
          std::vector<std::string> q;
          for (const auto& c : target) q.push_back(detail::quote_identifier(c));
          return q;
        }(), ", ") + ")";
    if (!conflict_where.empty()) conflict_clause += " WHERE " + conflict_where;
    if (on_conflict == "skip") {
      conflict_clause += " DO NOTHING";
    } else {
      std::vector<std::string> sets;
      std::vector<std::string> update_cols;
      if (in.body.contains("update_columns")) {
        for (const auto& c : in.body["update_columns"]) {
          update_cols.push_back(c.get<std::string>());
        }
      } else {
        // Everything supplied that is not part of the arbiter. Updating the
        // arbiter columns would rewrite the very values that decided the match.
        for (const auto& n : names) {
          if (std::find(target.begin(), target.end(), n) == target.end()) {
            update_cols.push_back(n);
          }
        }
      }
      if (update_cols.empty()) {
        step.action = Action::kConflict;
        step.why = "on_conflict \"update\" has no column left to update";
        plan.conflicts.push_back(
            "on_conflict \"update\" on " + qualified +
            " would update nothing: every column supplied is part of the "
            "conflict target, so there is no value a matched row could take "
            "that it does not already have. Use \"skip\", or supply a column "
            "outside the arbiter.");
        return;
      }
      for (const auto& c : update_cols) {
        sets.push_back(detail::quote_identifier(c) + " = EXCLUDED." +
                       detail::quote_identifier(c));
      }
      conflict_clause += " DO UPDATE SET " + detail::join(sets, ", ");
    }
    step.detail["conflict_arbiter"] = arbiter;
  }

  std::vector<std::string> quoted;
  for (const auto& n : names) quoted.push_back(detail::quote_identifier(n));
  const auto column_list = "(" + detail::join(quoted, ", ") + ")";

  const auto overriding = in.body.value("overriding", "");
  const std::string overriding_clause =
      overriding == "system" ? "OVERRIDING SYSTEM VALUE\n"
      : overriding == "user" ? "OVERRIDING USER VALUE\n"
                             : "";

  const auto src = detail::decide_pacing(in, cfg, !key.empty());
  step.detail["row_source"] = in.body.contains("values") ? "values" : "select";
  step.detail["paced"] = src.paced;

  if (src.paced) {
    std::vector<std::string> selected;
    for (const auto& n : names) {
      selected.push_back("batch." + detail::quote_identifier(n));
    }
    const auto mutation =
        "ins AS (\n"
        "  INSERT INTO " + qualified + " " + column_list + "\n"
        "  " + (overriding_clause.empty() ? "" : overriding_clause + "  ") +
        "SELECT " + detail::join(selected, ", ") + " FROM batch\n" +
        (conflict_clause.empty() ? "" : "  " + conflict_clause + "\n") +
        "  RETURNING 1\n"
        ")";
    step.sql.push_back(paced_statement(
        detail::source_relation(in, names, columns), key, mutation));
    // No resume predicate: the rows come from the spec or from a query, not
    // from the target, so there is nothing on the target to re-check a
    // recorded cursor against. Left empty deliberately -- executor.h skips the
    // staleness check rather than running it against the wrong relation.
    detail::attach_pacing_detail(step, in, cfg, src, qualified, key, "");
    step.lock = "RowExclusiveLock on " + qualified +
                " plus row locks, released at every commit";
  } else {
    step.txn_class = TxnClass::kRequired;
    step.lock = "RowExclusiveLock on " + qualified +
                " for one transaction; no table-level exclusive lock";
    if (in.body.contains("select")) {
      step.sql.push_back(
          "INSERT INTO " + sql_rel + " " + column_list + "\n" +
          overriding_clause + in.body.value("select", "") +
          (conflict_clause.empty() ? "" : "\n" + conflict_clause) + ";");
    } else {
      std::string rows;
      std::size_t r = 0;
      for (const auto& row : in.body["values"]) {
        if (r != 0) rows += ",\n       ";
        rows += "(";
        for (std::size_t c = 0; c < names.size(); ++c) {
          if (c != 0) rows += ", ";
          rows += detail::render_cell(row[c]);
        }
        rows += ")";
        ++r;
      }
      // No casts here, unlike the joined forms: an INSERT's VALUES list is
      // coerced to the target columns' own types, so the literals stand as
      // written and the ledger records something a person can read back.
      step.sql.push_back(
          "INSERT INTO " + sql_rel + " " + column_list + "\n" +
          overriding_clause + "VALUES " + rows +
          (conflict_clause.empty() ? "" : "\n" + conflict_clause) + ";");
    }
    step.detail["rows"] = src.row_count;
  }

  step.why = src.why;
  if (!conflict_clause.empty()) {
    step.why += "; " +
                std::string(on_conflict == "skip"
                                ? "ON CONFLICT DO NOTHING, so re-running this "
                                  "spec against a database that already has "
                                  "these rows changes nothing"
                                : "ON CONFLICT DO UPDATE, so existing rows are "
                                  "overwritten with the values in the spec");
  }

  // A missing parent is the failure that hurts most under pacing: it arrives
  // per row, mid-walk, after earlier batches have already committed.
  const auto constraints = t.value("constraints", json::object());
  std::vector<std::string> fks;
  for (auto it = constraints.begin(); it != constraints.end(); ++it) {
    if (it.value().value("type", "") == "f") {
      fks.push_back(it.key() + " -> " + it.value().value("references", ""));
    }
  }
  if (!fks.empty() && src.paced) {
    plan.warnings.push_back(
        qualified + " has foreign keys (" + detail::join(fks, ", ") +
        ") and this insert is paced, so a row naming a parent that does not "
        "exist fails its batch AFTER earlier batches have committed -- the "
        "insert stops part-done rather than rolling back. Insert the parents "
        "in an earlier intent, or set \"paced\": false to make it "
        "all-or-nothing if the list is small enough to hold in one "
        "transaction.");
  }
  detail::warn_about_row_security(t, qualified, true, plan);

  // Ordering matters: the insert has to be recorded before the setval that
  // depends on it, and Emit's destructor would otherwise run last.
  out.push_back(step);
  emitted = true;
  detail::emit_sequence_catchup(in, columns, qualified, names, out);
}


// --- update_rows ------------------------------------------------------------
//
// A DIFFERENT VALUE FOR EACH ROW, which is what separates this from backfill.
// backfill applies one expression to many rows -- "region_id = w.region_id
// where region_id is null" -- and that expression is the record of what it did.
// update_rows says row 41 becomes 'NA' and row 42 becomes 'EU' because someone
// decided so, and there is no rule to re-derive it from. That is why `preserve`
// matters more here than anywhere else and why the plan says so.
inline void plan_update_rows(const Intent& in, const Observations& obs,
                             const ExecutorConfig& cfg, Plan& plan,
                             std::vector<Step>& out) {
  Step step;
  step.kind = in.kind_name;
  struct Emit { std::vector<Step>& o; Step& s; ~Emit() { o.push_back(s); } } emit{out, step};

  const auto qualified = in.qualified_table();
  const auto sql_rel = detail::quote_qualified(qualified);
  const auto& t = obs.table(qualified);
  const auto key = in.body.value("key", "");
  const auto names = detail::column_names(in);

  if (!detail::row_target_ok(t, qualified, key, plan, step)) return;
  const json columns = t.value("columns", json::object());
  if (detail::refuse_unwritable_columns(in, columns, qualified, names, plan, step)) return;
  if (detail::refuse_duplicate_keys(in, names, key, plan, step)) return;

  // Without a unique index on the key, one spec row can match several table
  // rows, and every one of them silently takes the value meant for one. Same
  // refusal backfill makes, for a reason that is worse here: backfill's
  // expression would at least be correct for each row it hit.
  std::string supporting_index;
  if (!detail::unique_key_index(t, key, supporting_index)) {
    step.action = Action::kConflict;
    step.why = "no unique index leads with " + key;
    plan.conflicts.push_back(
        qualified + " has no valid unique index whose leading column is \"" +
        key + "\", so one row of \"values\" could match several rows of the "
        "table and every one of them would take a value meant for a single "
        "row -- with no error anywhere. Create one first: CREATE UNIQUE INDEX "
        "CONCURRENTLY ... ON " + qualified + " (" + key + ");");
    return;
  }

  std::vector<std::string> set_columns;
  for (const auto& n : names) {
    if (n != key) set_columns.push_back(n);
  }
  const auto src = detail::decide_pacing(in, cfg, true);
  const auto k = detail::quote_identifier(key);
  step.detail["row_source"] = in.body.contains("values") ? "values" : "select";
  step.detail["paced"] = src.paced;
  step.detail["supporting_index"] = supporting_index;
  step.detail["sets"] = set_columns;

  const auto assignments = [&](const std::string& alias) {
    std::vector<std::string> sets;
    for (const auto& c : set_columns) {
      sets.push_back(detail::quote_identifier(c) + " = " + alias + "." +
                     detail::quote_identifier(c));
    }
    return detail::join(sets, ", ");
  };

  if (src.paced) {
    const auto mutation =
        "upd AS (\n"
        "  UPDATE " + qualified + "\n"
        "     SET " + assignments("batch") + "\n"
        "    FROM batch\n"
        "   WHERE " + sql_rel + "." + k + " = batch." + k + "\n"
        "  RETURNING 1\n"
        ")";
    step.sql.push_back(paced_statement(
        detail::source_relation(in, names, columns), key, mutation));
    detail::attach_pacing_detail(step, in, cfg, src, qualified, key, "");
    step.lock = "RowExclusiveLock on " + qualified +
                " plus row locks, released at every commit";
  } else {
    step.txn_class = TxnClass::kRequired;
    step.lock = "RowExclusiveLock on " + qualified +
                " for one transaction; no table-level exclusive lock";
    const auto relation =
        in.body.contains("select")
            ? "(" + in.body.value("select", "") + ") AS v"
            : detail::values_relation(in.body["values"], in.body["columns"],
                                      names, "v", columns);
    step.sql.push_back(
        "UPDATE " + sql_rel + "\n"
        "   SET " + assignments("v") + "\n"
        "  FROM " + relation + "\n"
        " WHERE " + sql_rel + "." + k + " = v." + k + ";");
    step.detail["rows"] = src.row_count;
  }

  step.why = src.why + "; each row takes its own values, which no expression "
                       "can derive -- so " +
             (in.body.contains("preserve")
                  ? "the previous values are preserved in the same transaction "
                    "as the change"
                  : "there is no record of the previous values unless "
                    "\"preserve\" asks for one");

  // The point backfill's own comment makes, sharpened: here there is not even a
  // predicate to re-run afterwards and see what remains.
  if (!in.body.contains("preserve") && !in.body.contains("assert_invariants")) {
    plan.warnings.push_back(
        "update_rows on " + qualified +
        " overwrites values with no record of what they were. A backfill has "
        "its own WHERE clause as that record -- re-running it says what is "
        "left -- and per-row values have nothing equivalent. \"preserve\" "
        "captures the pre-image in the SAME transaction as the change, which "
        "makes it durable and doubles as the revert path; "
        "\"assert_invariants\" catches a transform that completed and still "
        "broke a total.");
  }
  detail::warn_about_row_security(t, qualified, true, plan);
}


// --- delete_rows ------------------------------------------------------------
//
// A retention purge, paced exactly as a backfill is and for the same reason: a
// single DELETE over a retention window holds locks for as long as it takes and
// is the same outage a single UPDATE would be.
//
// Three forms now, not one. `where` is the retention purge it was built for.
// `values` is "delete exactly these rows", which used to have to be expressed
// as a predicate over a key list -- an invitation to write IN (...) by hand and
// get the pacing wrong. `select` is the same thing at a size nobody would write
// out.
//
// It is a migration by both tests: it changes state, and it is applied once.
inline void plan_delete_rows(const Intent& in, const Observations& obs,
                             const ExecutorConfig& cfg, Plan& plan,
                             std::vector<Step>& out) {
  Step step;
  step.kind = in.kind_name;
  struct Emit { std::vector<Step>& o; Step& s; ~Emit() { o.push_back(s); } } emit{out, step};

  const auto qualified = in.qualified_table();
  const auto sql_rel = detail::quote_qualified(qualified);
  const auto& t = obs.table(qualified);
  const auto key = in.body.value("key", "");
  const auto where = in.body.value("where", "");
  const auto names = detail::column_names(in);

  if (!detail::row_target_ok(t, qualified, key, plan, step)) return;
  if (detail::refuse_duplicate_keys(in, names, key, plan, step)) return;

  // A delete that removes a parent row a foreign key points at fails per row,
  // mid-batch, after some batches have already committed. Saying so first is
  // the difference between a migration that stops understandably and one that
  // stops half-done.
  const auto referenced = t.value("referenced_by", json::array());
  if (!referenced.empty()) {
    std::vector<std::string> refs;
    for (const auto& r : referenced) refs.push_back(r.get<std::string>());
    plan.warnings.push_back(
        qualified + " is referenced by " + detail::join(refs, ", ") +
        ". Any row still referenced will fail its batch, and because this is "
        "paced, earlier batches have already COMMITTED by then -- the purge "
        "stops part-done rather than rolling back. Delete the children first, "
        "in an earlier intent, or confirm the constraint cascades.");
  }

  const auto rows = t.value("reltuples", 0LL);
  const auto src = detail::decide_pacing(in, cfg, true);
  const auto k = detail::quote_identifier(key);
  const bool by_predicate = !in.body.contains("values") && !in.body.contains("select");
  step.detail["row_source"] = by_predicate ? "where"
                            : in.body.contains("values") ? "values" : "select";
  step.detail["paced"] = src.paced;

  if (by_predicate) {
    // The batch is a keyset walk over the TARGET under the predicate, so FOR
    // UPDATE and the resume staleness check both apply -- and here the cursor
    // CAN come from the DELETE's own RETURNING, because every row the batch
    // selected is a row the delete removes.
    //
    step.txn_class = TxnClass::kOwnTxnPerBatch;
    step.sql.push_back(
        "WITH batch AS (\n"
        "  SELECT " + sql_rel + "." + k + "\n"
        "    FROM " + sql_rel + "\n"
        "   WHERE " + sql_rel + "." + k + " > $1 AND (" + where + ")\n"
        "   ORDER BY " + sql_rel + "." + k + "\n"
        "   LIMIT $2\n"
        "   FOR UPDATE\n"
        ")\n"
        "DELETE FROM " + sql_rel + "\n"
        " USING batch AS b\n"
        " WHERE " + sql_rel + "." + k + " = b." + k + "\n"
        "RETURNING " + sql_rel + "." + k + ";");
    detail::attach_pacing_detail(step, in, cfg, src, qualified, key, where);
    step.detail["rows_estimated"] = rows;
    step.lock = "RowExclusiveLock on " + qualified +
                " -- no table-level exclusive lock at any point";
    step.why =
        "deleting " + std::to_string(rows) +
        " estimated rows in batches of " + std::to_string(cfg.batch_rows) +
        ", committing on a lock waiter, on " + std::to_string(cfg.commit_interval_ms) +
        "ms elapsed, or on " + std::to_string(cfg.batch_cap_rows) +
        " rows -- whichever comes first. A single DELETE over the same predicate "
        "would hold its locks for the whole of it";
  } else if (src.paced) {
    const auto mutation =
        "del AS (\n"
        "  DELETE FROM " + qualified + "\n"
        "   USING batch\n"
        "   WHERE " + sql_rel + "." + k + " = batch." + k + "\n"
        "  RETURNING 1\n"
        ")";
    step.sql.push_back(paced_statement(
        detail::source_relation(in, names, t.value("columns", json::object())),
        key, mutation));
    // Empty for the same reason insert_rows leaves it empty: the keys come from
    // the spec, so a recorded cursor cannot be re-checked against the target.
    // A key that is already gone must still advance the cursor -- which is
    // exactly what naming it from the batch achieves.
    detail::attach_pacing_detail(step, in, cfg, src, qualified, key, "");
    step.lock = "RowExclusiveLock on " + qualified +
                " plus row locks, released at every commit";
    step.why = src.why;
  } else {
    step.txn_class = TxnClass::kRequired;
    step.lock = "RowExclusiveLock on " + qualified +
                " for one transaction; no table-level exclusive lock";
    const auto relation =
        in.body.contains("select")
            ? "(" + in.body.value("select", "") + ") AS v"
            : detail::values_relation(in.body["values"], in.body["columns"],
                                      names, "v",
                                      t.value("columns", json::object()));
    step.sql.push_back(
        "DELETE FROM " + sql_rel + "\n"
        " USING " + relation + "\n"
        " WHERE " + sql_rel + "." + k + " = v." + k + ";");
    step.detail["rows"] = src.row_count;
    step.why = src.why;
  }

  if (in.body.contains("verify_remaining")) {
    step.detail["verify_remaining"] = in.body["verify_remaining"];
  }
  detail::warn_about_row_security(t, qualified, false, plan);

  // The space is not returned to the operating system, and an operator who
  // expects it to be will go looking for a bug that is not there.
  plan.warnings.push_back(
      "a delete does not shrink " + qualified +
      " on disk: the rows become dead tuples and the space is reused by future "
      "inserts, not returned to the filesystem. Autovacuum will reclaim it for "
      "reuse; only VACUUM FULL or pg_repack returns it, and neither is a "
      "migration. pg_licht tableBloat shows what is actually there afterwards.");
}


// --- merge_rows -------------------------------------------------------------
//
// MERGE: insert, update and delete decided per row by whether the row is
// already there. It earns its place as a kind rather than a method by the
// refusal at the top of it, which is the sharpest measurement in this file.
//
// Measured on 18.6 (S21), against a table with two rows sharing id 1 and no
// unique index:
//
//   MERGE INTO nokey t USING (SELECT 1::bigint id, 9 v) s ON t.id = s.id
//     WHEN MATCHED THEN UPDATE SET v = s.v ...;
//   -- nokey rows: 2 vals: 9,9
//
// ONE source row updated TWO target rows, with no error and nothing in the
// output to say so. ON is not a key constraint and PostgreSQL does not pretend
// it is. An INSERT would have hit the unique index; a MERGE has none to hit.
// The unique index that would prevent it is visible in the catalog before
// anything runs, so this refuses.
//
// The duplicate-SOURCE case needs no refusal of its own: PostgreSQL raises
// "MERGE command cannot affect row a second time" (also measured), which is a
// clear error at execution. refuse_duplicate_keys() still catches it earlier
// for the values form, where the duplicates are sitting in the spec.
inline void plan_merge_rows(const Intent& in, const Observations& obs,
                            const ExecutorConfig& cfg, Plan& plan,
                            std::vector<Step>& out) {
  Step step;
  step.kind = in.kind_name;
  bool emitted = false;
  struct Emit {
    std::vector<Step>& o; Step& s; bool& done;
    ~Emit() { if (!done) o.push_back(s); }
  } emit{out, step, emitted};

  const auto qualified = in.qualified_table();
  const auto sql_rel = detail::quote_qualified(qualified);
  const auto& t = obs.table(qualified);
  const auto key = in.body.value("key", "");
  const auto names = detail::column_names(in);

  if (!detail::row_target_ok(t, qualified, key, plan, step)) return;
  const json columns = t.value("columns", json::object());
  if (detail::refuse_duplicate_keys(in, names, key, plan, step)) return;

  const auto matched = in.body.value("when_matched", "update");
  const auto not_matched = in.body.value("when_not_matched", "insert");
  const auto by_source = in.body.value("when_not_matched_by_source", "nothing");

  // Only the branches that write new rows are subject to the identity and
  // generated-column rules; a MERGE that only updates or deletes never supplies
  // a value for a generated column.
  if (not_matched == "insert" &&
      detail::refuse_unwritable_columns(in, columns, qualified, names, plan, step)) {
    return;
  }

  std::string supporting_index;
  if (!detail::unique_key_index(t, key, supporting_index)) {
    step.action = Action::kConflict;
    step.why = "no unique index leads with " + key;
    plan.conflicts.push_back(
        qualified + " has no valid unique index whose leading column is \"" +
        key + "\", and MERGE's ON clause is not a key constraint. Measured on "
        "18.6: with duplicate target rows, one source row updated BOTH of them "
        "-- no error, nothing in the row count to notice, and the extra row "
        "silently carrying values meant for another. Create the index first: "
        "CREATE UNIQUE INDEX CONCURRENTLY ... ON " + qualified + " (" + key +
        ");");
    return;
  }

  const auto src = detail::decide_pacing(in, cfg, true);

  // The version floor, measured against real 15, 16 and 17 clusters rather than
  // read off a release note:
  //
  //   paced merge (MERGE ... RETURNING)   syntax error on 15 and 16, ok on 17+
  //   when_not_matched_by_source          syntax error on 15 and 16, ok on 17+
  //   unpaced merge                       ok from 15
  //
  // REFUSED rather than degraded, and the choice is deliberate. Silently
  // dropping to the unpaced form on an older server would turn a bounded change
  // into one long transaction holding its locks for the whole of it -- which is
  // the harm this tool exists to prevent, arrived at by a fallback nobody asked
  // for. A refusal that names the reading is what the rest of the planner does,
  // and it leaves the choice with the author: pace it on 17, or say `paced:
  // false` and accept the transaction.
  //
  // Gated on the OBSERVED server version, the way detach_partition gates
  // CONCURRENTLY on >= 140000. Nothing here is inferred from the spec.
  const bool merge_returning_available = obs.server_version >= 170000;
  if (!merge_returning_available && obs.server_version > 0) {
    const auto server = std::to_string(obs.server_version);
    if (src.paced) {
      step.action = Action::kConflict;
      step.why = "a paced merge needs MERGE ... RETURNING, which is PostgreSQL 17+";
      plan.conflicts.push_back(
          "merge_rows on " + qualified + " would be paced, and a paced merge "
          "emits MERGE ... RETURNING, which arrived in PostgreSQL 17. This "
          "server is " + server +
          ", where it is a syntax error -- measured, not assumed. Either set "
          "\"paced\": false, accepting that the statement holds its locks for "
          "the whole source rather than committing as it goes, or run this "
          "against 17 or later. It is NOT silently unpaced here: that would "
          "turn a bounded change into one long transaction, which is the harm "
          "this tool exists to prevent.");
      return;
    }
    if (by_source == "delete") {
      step.action = Action::kConflict;
      step.why = "when_not_matched_by_source needs PostgreSQL 17+";
      plan.conflicts.push_back(
          "when_not_matched_by_source on " + qualified +
          " emits WHEN NOT MATCHED BY SOURCE, which arrived in PostgreSQL 17. "
          "This server is " + server +
          ", where it is a syntax error -- measured, not assumed. Express the "
          "removal as its own delete_rows intent with a predicate that says "
          "which rows are obsolete; that runs on every version this tool "
          "supports.");
      return;
    }
  }

  // WHEN NOT MATCHED BY SOURCE means "every target row this MERGE did not see".
  // Under pacing the MERGE only ever sees ONE BATCH, so the first batch would
  // delete every row not in that batch -- including all the rows later batches
  // were going to match. There is no correct paced form of it.
  if (by_source == "delete" && src.paced) {
    step.action = Action::kConflict;
    step.why = "when_not_matched_by_source \"delete\" cannot be paced";
    plan.conflicts.push_back(
        "when_not_matched_by_source \"delete\" on " + qualified +
        " means \"delete every target row the source does not mention\", and a "
        "paced MERGE only ever sees one batch of the source at a time. The "
        "first batch would delete everything outside it, including the rows the "
        "later batches were going to match. It is only correct over the WHOLE "
        "source at once, so either set \"paced\": false -- accepting that the "
        "statement holds its locks for the whole table -- or express the "
        "removal as its own delete_rows intent with a predicate that says which "
        "rows are obsolete.");
    return;
  }

  std::vector<std::string> update_cols;
  if (in.body.contains("update_columns")) {
    for (const auto& c : in.body["update_columns"]) update_cols.push_back(c.get<std::string>());
  } else {
    for (const auto& n : names) {
      if (n != key) update_cols.push_back(n);
    }
  }
  if (matched == "update" && update_cols.empty()) {
    step.action = Action::kConflict;
    step.why = "when_matched \"update\" has no column to update";
    plan.conflicts.push_back(
        "when_matched \"update\" on " + qualified +
        " would update nothing: the only column the source carries is the key "
        "it matches on. Supply the columns to write, or set when_matched to "
        "\"nothing\" if the intent is insert-only.");
    return;
  }

  // The WHEN branches, built against whichever alias the source has.
  const auto branches = [&](const std::string& s) {
    std::string out_sql;
    if (matched == "update") {
      std::vector<std::string> sets;
      for (const auto& c : update_cols) {
        sets.push_back(detail::quote_identifier(c) + " = " + s + "." +
                       detail::quote_identifier(c));
      }
      out_sql += "  WHEN MATCHED THEN UPDATE SET " + detail::join(sets, ", ") + "\n";
    } else if (matched == "delete") {
      out_sql += "  WHEN MATCHED THEN DELETE\n";
    }
    if (not_matched == "insert") {
      std::vector<std::string> cols, vals;
      for (const auto& n : names) {
        cols.push_back(detail::quote_identifier(n));
        vals.push_back(s + "." + detail::quote_identifier(n));
      }
      out_sql += "  WHEN NOT MATCHED THEN INSERT (" + detail::join(cols, ", ") +
                 ") VALUES (" + detail::join(vals, ", ") + ")\n";
    }
    if (by_source == "delete") {
      out_sql += "  WHEN NOT MATCHED BY SOURCE THEN DELETE\n";
    }
    return out_sql;
  };

  const auto k = detail::quote_identifier(key);
  step.detail["row_source"] = in.body.contains("values") ? "values" : "select";
  step.detail["paced"] = src.paced;
  step.detail["supporting_index"] = supporting_index;
  step.detail["when_matched"] = matched;
  step.detail["when_not_matched"] = not_matched;
  step.detail["when_not_matched_by_source"] = by_source;

  if (src.paced) {
    const auto mutation =
        "m AS (\n"
        "  MERGE INTO " + qualified + " USING batch\n"
        "     ON " + qualified + "." + k + " = batch." + k + "\n" +
        branches("batch") +
        "  RETURNING 1\n"
        ")";
    step.sql.push_back(paced_statement(
        detail::source_relation(in, names, columns), key, mutation));
    detail::attach_pacing_detail(step, in, cfg, src, qualified, key, "");
    step.lock = "RowExclusiveLock on " + qualified +
                " plus row locks, released at every commit";
  } else {
    step.txn_class = TxnClass::kRequired;
    step.lock = "RowExclusiveLock on " + qualified +
                " for one transaction; no table-level exclusive lock" +
                (by_source == "delete"
                     ? " -- but the statement scans the WHOLE table to find the "
                       "rows the source does not mention"
                     : "");
    const auto relation =
        in.body.contains("select")
            ? "(" + in.body.value("select", "") + ") AS s"
            : detail::values_relation(in.body["values"], in.body["columns"],
                                      names, "s", columns);
    step.sql.push_back(
        "MERGE INTO " + sql_rel + " USING " + relation + "\n"
        "   ON " + sql_rel + "." + k + " = s." + k + "\n" +
        branches("s") + ";");
    step.detail["rows"] = src.row_count;
  }

  step.why = src.why + "; ON " + key + " is backed by " + supporting_index +
             ", which is what makes one source row match at most one target row";

  if (by_source == "delete") {
    plan.warnings.push_back(
        "when_not_matched_by_source \"delete\" on " + qualified +
        " removes every row the source does not mention. This is a full "
        "replacement of the table's contents, not an upsert: any row the "
        "application inserted since the spec was written is deleted, and "
        "nothing in the plan can know how many that is. Consider "
        "\"assert_invariants\" with a count, so the job records what the table "
        "held before and after.");
  }
  if (not_matched == "insert") {
    out.push_back(step);
    emitted = true;
    detail::emit_sequence_catchup(in, columns, qualified, names, out);
  }
  detail::warn_about_row_security(t, qualified, not_matched == "insert", plan);
}


// --- copy_rows --------------------------------------------------------------
//
// COPY is the fifth way PostgreSQL writes rows and the only one in this family
// the paced executor cannot drive. Measured on 18.6 (S21): a COPY whose second
// of three rows violated the primary key rolled back ALL THREE. It is one
// transaction's worth of work whatever its size, so there is no batch boundary
// to place and nothing for a lock waiter to interrupt.
//
// That is stated in the plan rather than worked around, because the honest
// answer for a large load is that COPY is the wrong shape for a paced migration
// and insert_rows with "select" is the right one.
//
// Two more measurements worth carrying into the plan, both of which contradict
// what COPY is usually assumed to do:
//
//   * it is NOT a fast path around constraints. A COPY into a child table whose
//     parent row was missing failed on the foreign key exactly as an INSERT
//     would. Row triggers fire too.
//   * ON_ERROR ignore does not make COPY safe, it makes it QUIET: it skipped
//     the malformed row, wrote the other two, and reported success. A step that
//     can succeed having written less than the spec listed has to say so.
inline void plan_copy_rows(const Intent& in, const Observations& obs,
                           const ExecutorConfig& cfg, Plan& plan,
                           std::vector<Step>& out) {
  Step step;
  step.kind = in.kind_name;
  bool emitted = false;
  struct Emit {
    std::vector<Step>& o; Step& s; bool& done;
    ~Emit() { if (!done) o.push_back(s); }
  } emit{out, step, emitted};

  const auto qualified = in.qualified_table();
  const auto sql_rel = detail::quote_qualified(qualified);
  const auto& t = obs.table(qualified);
  const auto names = detail::column_names(in);

  if (!detail::row_target_ok(t, qualified, "", plan, step)) return;
  const json columns = t.value("columns", json::object());
  if (detail::refuse_unwritable_columns(in, columns, qualified, names, plan, step)) return;

  std::vector<std::string> quoted;
  for (const auto& n : names) quoted.push_back(detail::quote_identifier(n));

  const auto rows = static_cast<long long>(in.body["values"].size());
  step.txn_class = TxnClass::kRequired;
  step.lock = "RowExclusiveLock on " + qualified +
              " for the whole COPY, which is one transaction however long it "
              "takes -- there is no batch boundary to release it at";

  // Byte-for-byte the statement libpqxx will send. Probed against 18.6:
  // pqxx::stream_to::raw_table(tx, "cf_probe", "id, code") logs
  //
  //     COPY cf_probe(id, code) FROM STDIN
  //
  // No space before the paren, no WITH clause. The ledger records statements
  // verbatim, so this has to BE the statement rather than resemble it -- and a
  // test asserts the two agree, because the only thing worse than a missing
  // record is a confident wrong one.
  step.sql.push_back("COPY " + sql_rel + "(" + detail::join(quoted, ", ") +
                     ") FROM STDIN;");

  // The payload travels in the step's detail rather than in its SQL, because
  // the SQL column of the ledger records STATEMENTS -- what was sent to the
  // server as a command -- and COPY's rows are a stream that follows it. Both
  // are recorded; they are recorded as the different things they are.
  json payload = json::array();
  for (const auto& row : in.body["values"]) {
    json out_row = json::array();
    for (std::size_t c = 0; c < names.size(); ++c) out_row.push_back(row[c]);
    payload.push_back(out_row);
  }
  step.detail["copy_columns"] = names;
  step.detail["copy_rows"] = payload;
  step.detail["rows"] = rows;
  step.detail["qualified"] = qualified;
  // The quoted form, separately, because pqxx::stream_to splices the table path
  // into the COPY statement verbatim. `qualified` stays raw: executor.h and
  // jobStatus key on it, and it is the same string the observations map uses.
  step.detail["copy_relation"] = sql_rel;

  step.why =
      std::to_string(rows) +
      " rows sent as COPY ... FROM STDIN. Unlike every other row-level kind "
      "this one is NOT paced and cannot be: measured on 18.6, a COPY whose "
      "second of three rows violated a constraint rolled back all three, so it "
      "is a single transaction whatever its size";

  if (rows > cfg.dml_single_txn_rows) {
    plan.warnings.push_back(
        "copy_rows on " + qualified + " sends " + std::to_string(rows) +
        " rows in ONE transaction, above dml_single_txn_rows (" +
        std::to_string(cfg.dml_single_txn_rows) +
        "), and COPY has no paced form. It holds RowExclusiveLock and its row "
        "locks for the whole load. insert_rows with the same rows IS paced, and "
        "insert_rows with \"select\" is the right shape for a load this size.");
  }
  const auto constraints = t.value("constraints", json::object());
  for (auto it = constraints.begin(); it != constraints.end(); ++it) {
    if (it.value().value("type", "") == "f") {
      plan.warnings.push_back(
          qualified + " has foreign key " + it.key() +
          ", and COPY is not a fast path around it: measured on 18.6, a COPY "
          "naming a parent row that did not exist failed on the constraint "
          "exactly as an INSERT would. Row triggers fire too.");
      break;
    }
  }
  detail::warn_about_row_security(t, qualified, true, plan);

  // The COPY has to be recorded before the setval that depends on it.
  out.push_back(step);
  emitted = true;
  detail::emit_sequence_catchup(in, columns, qualified, names, out);
}

}  // namespace pglaswell
