#pragma once
// Plan-level refusals for a Citus cluster: conditions of the TOPOLOGY, which no
// per-kind planner can own because they are true of every kind at once.
//
// A FUNCTION, not a block, and it receives (spec, obs, refuse) -- deliberately
// NOT the plan. A guard can say no and say why; it cannot reach what core
// emits. An audit found the earlier block form compiled happily while pushing
// a warning, which is the forbidden shape available by accident.
//
// Independent passes: DDL propagation, one Citus version across the nodes,
// prepared transactions nobody is resolving, what a distributed table cannot
// do, and foreign keys by the kinds of table at their two ends. Every
// refusal quotes the Citus error it pre-empts, so the author reads the rule
// they would otherwise have hit at execution.
template <typename Refuse, typename Advise>
inline void citus_plan_refusals(const Spec& spec, const Observations& obs,
                                const json& /*budget*/, const Refuse& refuse,
                                const Advise& /*advise*/) {
  const auto& citus = obs.extension("citus");
  if (citus.empty()) return;
  const auto tables = citus.value("tables", json::object());
  // "distributed", "reference" or "local". A table Citus does not record is a
  // plain local table in a Citus database, and walks and writes as it always did.
  //
  // partmethod 'n' is NOT enough to call a table a reference table. Citus also
  // records a CITUS-MANAGED LOCAL table with partmethod 'n' -- measured: adding a
  // foreign key from a local table to a reference table silently added the
  // local one to Citus metadata -- and the two differ in repmodel: 't' for a
  // reference table, 's' for a managed local one. Treating the second as the
  // first let a distributed-to-local foreign key past this guard.
  const auto kind_of = [&tables](const std::string& qualified) -> std::string {
    const auto it = tables.find(qualified);
    if (it == tables.end()) return "local";
    if (it->value("partmethod", "") != "n") return "distributed";
    return it->value("repmodel", "") == "t" ? "reference" : "local";
  };

  // 0. THE TRIGGER RECIPE. add_column with "fill" and no default adds the
  // column with a trigger that fills it. Measured on Citus 14.0:
  //   - the recipe's first transaction alters the table and creates the
  //     function, and Citus refuses the second after the first ("cannot run
  //     function command because there was a parallel operation on a
  //     distributed table in the transaction") -- which is what a field probe
  //     met, as a dry-run failure carrying a hint about a setting;
  //   - with the function created in a transaction of its own, CREATE TRIGGER
  //     is refused: "triggers are not supported on distributed tables", and
  //     "... on reference tables";
  //   - it is accepted only with citus.enable_unsafe_triggers on, which is the
  //     vendor's own word for it. pg_laswell does not turn that on.
  // So the recipe is not available there, and this says so before anything is
  // built, naming the form that is: with a "default" the column is NOT NULL at
  // once and only a backfill follows, which needs no trigger (measured in the
  // same probe: it works, one walk a tenant at a time).
  for (const auto& in : spec.intents) {
    if (in.kind != IntentKind::kAddColumn || !in.body.contains("fill")) continue;
    if (in.body.contains("default") && !in.body["default"].is_null()) continue;
    const auto q = in.qualified_table();
    const auto kind = kind_of(q);
    if (kind == "local") continue;
    refuse("add_column on " + q + ": \"fill\" without a \"default\" fills new rows by a "
           "trigger, and " + q + " is a " + kind + " table. Citus: \"triggers are not "
           "supported on " + kind + " tables\" unless citus.enable_unsafe_triggers is on, "
           "which pg_laswell will not turn on for you. Give " + in.body.value("column", "") +
           " a \"default\" as well: the column is then NOT NULL from the start, no "
           "trigger is created, and the backfill writes the rows whose value differs.");
  }

  // 0b. WHAT CITUS REFUSES OF CORE'S OWN DDL on a distributed or reference
  // table. Each measured on Citus 14.0, the statement run by hand:
  //
  //   a unique index, unique constraint or primary key without the
  //   distribution column: "creating unique indexes on non-partition columns
  //   is currently unsupported"; "Distributed relations cannot have UNIQUE,
  //   EXCLUDE, or PRIMARY KEY constraints that do not include the partition
  //   column". With it, and ADD CONSTRAINT ... USING INDEX with it, accepted.
  //
  //   CREATE TRIGGER, and ALTER TABLE ... DISABLE TRIGGER: "triggers are not
  //   supported on distributed tables" / "... on reference tables".
  //
  //   DROP COLUMN and ALTER COLUMN TYPE of the distribution column: "cannot
  //   execute ALTER TABLE command involving partition column". Of another
  //   column, accepted.
  //
  // The unique kinds matter most: a unique index is built CONCURRENTLY, which
  // the dry run cannot rehearse, so Citus's refusal came in the job, after
  // whatever the specification did before it had committed.
  //
  // This pass follows the specification in order: a table distributed by an
  // earlier intent is distributed for the ones after it, though no reading
  // says so yet.
  {
    struct Declared { std::string kind, column; };
    std::map<std::string, Declared> declared;
    const auto now = [&](const std::string& q) -> Declared {
      const auto d = declared.find(q);
      if (d != declared.end()) return d->second;
      const auto it = tables.find(q);
      // A reference table's distribution column is recorded as null.
      const json column = it == tables.end() ? json() : it->value("distribution_column", json());
      return {kind_of(q), column.is_string() ? column.get<std::string>() : std::string()};
    };
    for (const auto& in : spec.intents) {
      const auto q = in.qualified_table();
      if (in.kind == IntentKind::kCitusDistributeTable) {
        declared[q] = {"distributed", in.body.value("distribution_column", "")};
        continue;
      }
      if (in.kind == IntentKind::kCitusCreateReferenceTable) {
        declared[q] = {"reference", ""};
        continue;
      }
      if (in.kind == IntentKind::kCitusUndistributeTable) {
        declared[q] = {"local", ""};
        continue;
      }
      const auto is = now(q);
      if (is.kind == "local") continue;
      const std::string what = in.kind_name + " on " + q;

      if (in.kind == IntentKind::kCreateTrigger || in.kind == IntentKind::kSetTriggerState) {
        refuse(what + ": " + q + " is a " + is.kind + " table. Citus: \"triggers are not "
               "supported on " + is.kind + " tables\" -- for CREATE TRIGGER and for "
               "ENABLE or DISABLE TRIGGER alike -- unless citus.enable_unsafe_triggers is "
               "on, which pg_laswell will not turn on for you.");
        continue;
      }
      if (is.kind != "distributed" || is.column.empty()) continue;

      std::vector<std::string> key;
      bool unique = false;
      if (in.kind == IntentKind::kCreateIndex && in.body.value("unique", false)) {
        unique = true;
        for (const auto& c : in.body.value("columns", json::array())) {
          key.push_back(c.is_string() ? c.get<std::string>() : c.value("name", ""));
        }
      } else if (in.kind == IntentKind::kAddPrimaryKey ||
                 in.kind == IntentKind::kAddUniqueConstraint) {
        unique = true;
        for (const auto& c : in.body.value("columns", json::array())) {
          key.push_back(c.get<std::string>());
        }
        // The period is compared for overlap, not equality, so it is not
        // where the distribution column can be.
        if (in.body.value("without_overlaps", false) && !key.empty()) key.pop_back();
      } else if (in.kind == IntentKind::kAddExclusionConstraint) {
        // Measured on Citus 14: "cannot create constraint on" the table for an
        // exclusion constraint, and for a key with a period, that does not
        // compare the distribution column for equality.
        unique = true;
        for (const auto& e : in.body.value("elements", json::array())) {
          if (e.contains("column") && e.value("with", "") == "=") key.push_back(e.value("column", ""));
        }
      }
      if (unique && std::find(key.begin(), key.end(), is.column) == key.end()) {
        const bool exclusion = in.kind == IntentKind::kAddExclusionConstraint;
        refuse(what + ": " +
               (exclusion ? "the columns it compares with \"=\" (" : "the key (") +
               detail::join(key, ", ") + ") " + (exclusion ? "do" : "does") + " not contain " +
               is.column + ", and " + q + " is distributed on it. Citus: \"Distributed "
               "relations cannot have UNIQUE, EXCLUDE, or PRIMARY KEY constraints that do "
               "not include the partition column\". Add " + is.column +
               (exclusion ? ", compared with \"=\"." : " to the key."));
        continue;
      }
      if ((in.kind == IntentKind::kDropColumn || in.kind == IntentKind::kAlterColumnType) &&
          in.body.value("column", "") == is.column) {
        refuse(what + ": " + is.column + " is the column " + q + " is distributed on. "
               "Citus: \"cannot execute ALTER TABLE command involving partition column\". "
               "Changing it means undistributing the table and distributing it again, "
               "which moves every row and is its own decision.");
      }
    }
  }

  // 1. DDL PROPAGATION. DECIDED: a hard refusal, not a warning. Whether it
  // should refuse or warn was an open question; this is the decision, with
  // the reason.
  //
  // With citus.enable_ddl_propagation off, DDL runs on the coordinator and NOT
  // on the workers. The coordinator's catalog and the workers' then diverge --
  // and they diverge SILENTLY, because every reading pg_laswell takes,
  // including the drift check itself, is taken on the coordinator. The ledger
  // would record a migration as fully applied, the coordinator would agree, and
  // the shards would not have it.
  //
  // That is the precise failure the whole repository model exists to catch,
  // occurring one level below where the repository can see. A warning would be
  // a false reassurance. Refusing is recoverable in one statement; silent
  // divergence is not recoverable at all without comparing every worker by
  // hand. It applies to EVERY kind: a plain create_table on a Citus cluster is
  // propagated DDL too.
  const auto settings = citus.value("settings", json::object());
  const auto propagation = settings.value("enable_ddl_propagation", "");
  if (propagation == "off" || propagation == "false") {
    refuse(
        "citus.enable_ddl_propagation is off, so DDL would run on the "
        "coordinator and not on the workers. The coordinator's catalog and the "
        "workers' would diverge, and every reading pg_laswell takes -- including "
        "the check that catches drift -- is taken on the coordinator, so nothing "
        "here would ever report it. Turn it on for the migration: SET "
        "citus.enable_ddl_propagation TO on; (or ALTER SYSTEM, if it is off by "
        "default here).");
  }

  // 1b. ONE CITUS VERSION ACROSS THE CLUSTER. Each worker is asked which
  // version of the extension is installed IN THIS DATABASE; a worker on another
  // version -- mid-upgrade, or never upgraded -- fails distributed work
  // partway, after the coordinator's half has run. A worker that did not
  // answer is not judged: its version is unknown, not wrong.
  {
    const auto mine = citus.value("version", "");
    const auto workers = citus.value("workers", json::object());
    std::vector<std::string> differ;
    for (auto it = workers.begin(); it != workers.end(); ++it) {
      if (!it.value().is_object()) continue;
      const auto v = it.value().value("citus", json(nullptr));
      if (v.is_null()) {
        differ.push_back(it.key() + " (citus is not installed in this database there)");
      } else if (v.is_string() && v.get<std::string>() != mine) {
        differ.push_back(it.key() + " (" + v.get<std::string>() + ")");
      }
    }
    if (!differ.empty()) {
      refuse("the workers do not all run the coordinator's Citus " + mine + ": " +
             detail::join(differ, ", ") +
             ". Distributed work runs on every node, and one on another version "
             "fails partway, after the coordinator's half has run. Finish the "
             "upgrade -- ALTER EXTENSION citus UPDATE in this database on each "
             "node -- before migrating.");
    }
  }

  // 1c. PREPARED TRANSACTIONS NOBODY IS RESOLVING. A multi-shard write is a
  // two-phase commit, and one killed between PREPARE and COMMIT leaves a
  // prepared transaction on a worker. Measured, it holds its locks -- an INSERT
  // waited on one indefinitely -- and it pins that node's xmin horizon, so
  // VACUUM there reclaims nothing while it lives. Citus's maintenance daemon
  // resolves its OWN (measured: recover_prepared_transactions() cleared a
  // citus_-named one) every citus.recover_2pc_interval; anything older than
  // twice that is not being resolved, by Citus or by whoever else prepared it.
  {
    const auto interval_s = [&]() -> long long {
      const auto raw = settings.value("recover_2pc_interval", "");
      try {
        std::size_t used = 0;
        const long long n = std::stoll(raw, &used);
        if (n < 0) return -1;  // disabled: Citus resolves nothing
        const auto unit = raw.substr(used);
        if (unit == "ms" || unit.empty()) return n / 1000;
        if (unit == "s") return n;
        if (unit == "min") return n * 60;
        if (unit == "h") return n * 3600;
        if (unit == "d") return n * 86400;
      } catch (const std::exception&) {
      }
      return 60;  // Citus's default, when the setting cannot be read
    }();
    const long long limit = interval_s < 0 ? 120 : std::max<long long>(2 * interval_s, 120);
    std::vector<std::string> stuck;
    const auto check = [&](const std::string& node, const json& r) {
      if (!r.is_object()) return;
      const auto n = r.value("prepared", 0LL);
      const auto age = r.value("oldest_s", 0LL);
      if (n > 0 && age > limit) {
        stuck.push_back(node + " (" + std::to_string(n) + ", the oldest " +
                        std::to_string(age) + " s)");
      }
    };
    check("the coordinator", citus.value("coordinator", json::object()));
    const auto workers = citus.value("workers", json::object());
    for (auto it = workers.begin(); it != workers.end(); ++it) check(it.key(), it.value());
    if (!stuck.empty()) {
      refuse("prepared transactions older than " + std::to_string(limit) +
             " s are waiting on " + detail::join(stuck, ", ") + ". Each holds its locks and pins that "
             "node's xmin horizon, and at this age nothing is resolving them. "
             "Look at pg_prepared_xacts there: SELECT "
             "recover_prepared_transactions() on the coordinator resolves Citus's "
             "own, and any other gid belongs to whoever prepared it, to COMMIT "
             "PREPARED or ROLLBACK PREPARED.");
    }
  }

  // 2. WHAT A DISTRIBUTED TABLE CANNOT DO, per form rather than per kind.
  // Measured on Citus 13, preparing and EXECUTING each emitted statement:
  //
  //   insert_rows, copy_rows, update_rows, delete_rows by values   run
  //   backfill, delete_rows by predicate      run, as a grouped walk
  //   merge_rows                              refused, for MERGE
  //
  // The two walks are not refused here: they take FOR UPDATE, which Citus
  // refuses across shards, and core reads citus_required_confinement
  // (confine.h) and walks one distribution value at a time, which makes the lock
  // single-shard. The refusal that remains for them -- a distribution column
  // that IS the walk key -- is core's, because it is core's walk that cannot be
  // shaped.
  for (const auto& in : spec.intents) {
    if (in.kind != IntentKind::kMergeRows) continue;
    const auto qualified = in.qualified_table();
    if (kind_of(qualified) != "distributed") continue;
    // Naming the right restriction matters: told it was about FOR UPDATE, an
    // author would look for a way to avoid a row lock this statement never
    // takes.
    refuse(
        "\"merge_rows\" on " + qualified + " cannot run: " + qualified +
        " is distributed, and Citus restricts MERGE against a distributed target "
        "-- measured: \"non-IMMUTABLE functions are not yet supported in MERGE "
        "sql with distributed tables\". This is a limit of MERGE on Citus, not of "
        "the pacing. Express the change as update_rows and insert_rows, which "
        "both run here.");
  }

  // 3. FOREIGN KEYS, by the kinds of table at the two ends. Measured on Citus 13,
  // every combination:
  //
  //   distributed -> distributed, colocated, distribution columns at the
  //                  same position in both key lists                  runs
  //   distributed -> distributed, not colocated                       refused
  //   distributed -> distributed, colocated, distribution column
  //                  at a different position                          refused
  //   distributed -> reference                                        runs
  //   distributed -> local                                            refused
  //   reference   -> distributed, local -> distributed                refused
  //   reference   -> local, local -> reference                        run
  //
  // Only readings are checked: a table distributed earlier in the same
  // specification has no reading yet. The dry run executes the DDL inside the
  // transaction it rolls back, so that case is still caught before anything
  // commits -- by PostgreSQL rather than here.
  for (const auto& in : spec.intents) {
    if (in.kind != IntentKind::kAddForeignKey) continue;
    const auto child = in.qualified_table();
    const auto parent =
        in.body.value("references_schema", in.body.value("schema", "")) + "." +
        in.body.value("references_table", "");
    const auto ck = kind_of(child);
    const auto pk = kind_of(parent);

    if (ck != "distributed" && pk == "distributed") {
      refuse("add_foreign_key from " + child + " (a " + ck + " table) to " + parent +
             " (distributed) cannot run: Citus does not support foreign keys "
             "from reference or local tables to distributed ones -- \"Reference "
             "tables and local tables can only have foreign keys to reference "
             "tables and local tables\".");
      continue;
    }
    if (ck != "distributed" || pk == "reference") continue;  // both supported
    if (pk == "local") {
      refuse("add_foreign_key from " + child + " (distributed) to " + parent +
             " (local) cannot run: a distributed table can reference only a "
             "colocated distributed table or a reference table. Make " + parent +
             " a reference table first, or distribute it colocated with " + child +
             ".");
      continue;
    }

    // distributed -> distributed.
    const auto& c = tables[child];
    const auto& p = tables[parent];
    if (c.value("colocationid", -1) != p.value("colocationid", -2)) {
      refuse("add_foreign_key from " + child + " to " + parent + " cannot run: "
             "both are distributed and they are not colocated (groups " +
             std::to_string(c.value("colocationid", 0)) + " and " +
             std::to_string(p.value("colocationid", 0)) + "). Citus supports a "
             "foreign key between distributed tables only inside one colocation "
             "group, where the referencing and referenced rows are on the same "
             "node.");
      continue;
    }
    // Same position in both key lists, not merely present. Measured: a key that
    // maps some other column onto the parent's distribution column is refused --
    // "including partition column in the same ordinal in the both tables".
    const auto cols = in.body.value("columns", json::array());
    const auto refs = in.body.value("references_columns", json::array());
    const auto cd = c.value("distribution_column", "");
    const auto pd = p.value("distribution_column", "");
    int ci = -1, pi = -1;
    for (std::size_t i = 0; i < cols.size(); ++i) {
      if (cols[i] == cd) ci = static_cast<int>(i);
    }
    for (std::size_t i = 0; i < refs.size(); ++i) {
      if (refs[i] == pd) pi = static_cast<int>(i);
    }
    if (ci >= 0 && ci == pi) continue;
    const std::string what =
        ci < 0 ? "does not include " + cd
               : (pi < 0 ? "does not reference " + pd
                         : std::string("pairs them at different positions"));
    refuse("add_foreign_key from " + child + " to " + parent + " cannot run: the "
           "key must pair " + child + "." + cd + " with " + parent + "." + pd +
           " at the same position in both column lists, and it " + what +
           ". Citus: \"foreign keys are supported ... between two colocated "
           "tables including partition column in the same ordinal in the both "
           "tables\".");
  }
}
