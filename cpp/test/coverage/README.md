# DDL coverage audit

What fraction of PostgreSQL's DDL pg_laswell can express, derived rather than
remembered. Re-run it when a new major version lands.

    python3 cpp/test/coverage/audit.py

`pg18-sql-commands.txt` is the command list from
<https://www.postgresql.org/docs/18/sql-commands.html>, and
`pg18-alter-table-actions.txt` the "where action is one of" list from
<https://www.postgresql.org/docs/18/sql-altertable.html>, both captured
2026-09-05. The script reads the implemented intent kinds out of
`cpp/src/spec.h` rather than from a list kept here, so it cannot drift from the
code -- only from PostgreSQL, which is what the version-stamped files are for.

Three exclusions are deliberate and are counted separately rather than hidden:

* **maintenance** -- REINDEX, VACUUM, ANALYZE, CLUSTER and friends. Excluded by
  the second test this project applies to every intent kind: a migration is a
  change to state, applied exactly once, and these are neither. They change
  nothing a catalog can show afterwards, and the ledger keys on a spec digest,
  so a repeatable operation would run once and never again.
* **cluster- and instance-wide** -- roles, databases, tablespaces, ALTER
  SYSTEM, event triggers. These live outside the database a migration connects
  to, and outside what a per-database ledger can record.
* **not DDL** -- SELECT, transaction control, session settings. INSERT, UPDATE,
  DELETE, MERGE and COPY used to be counted here and are not any more: they are
  covered by the row-level intent kinds, and are scored above like everything
  else.
