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
  the second test in ROADMAP.md: a migration is a change to schema state,
  applied exactly once, and these are none of those things.
* **cluster- and instance-wide** -- roles, databases, tablespaces, ALTER
  SYSTEM, event triggers. These live outside the database a migration connects
  to, and outside what a per-database ledger can record.
* **not DDL** -- SELECT, INSERT, transaction control, session settings.
