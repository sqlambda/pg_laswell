import subprocess, re
# Our kinds, read from the source rather than typed here.
src = open(__import__('os').path.join(__import__('os').path.dirname(__import__('os').path.abspath(__file__)), '..', '..', 'src', 'spec.h')).read()
block = src[src.index('intent_kinds()'):]
block = block[:block.index('return kKinds;')]
kinds = set(re.findall(r'\{"([a-z_]+)"', block))

# Which DDL command each kind covers. "~" = partial, with the gap named.
covers = {
 'CREATE SCHEMA': ('create_schema', ''),
 'DROP SCHEMA': ('drop_schema', ''),
 'ALTER SCHEMA': (None, 'rename / owner'),
 'CREATE EXTENSION': ('create_extension', ''),
 'DROP EXTENSION': ('drop_extension', ''),
 'ALTER EXTENSION': (None, 'UPDATE TO version, SET SCHEMA, ADD/DROP member'),
 'CREATE TYPE': ('create_type', 'enum/domain/composite only; no range or base type'),
 'DROP TYPE': ('drop_type', ''),
 'ALTER TYPE': ('add_enum_value', 'ADD VALUE only; no RENAME, ADD/DROP ATTRIBUTE, SET SCHEMA'),
 'CREATE DOMAIN': ('create_type', 'via type_kind=domain'),
 'DROP DOMAIN': ('drop_type', 'DROP TYPE removes a domain (verified)'),
 'ALTER DOMAIN': (None, 'ADD/DROP CONSTRAINT, SET/DROP NOT NULL, SET/DROP DEFAULT'),
 'CREATE FUNCTION': ('create_function', ''),
 'DROP FUNCTION': ('drop_function', ''),
 'ALTER FUNCTION': (None, 'rename, owner, SET search_path, volatility change'),
 'CREATE PROCEDURE': (None, 'procedures entirely'),
 'DROP PROCEDURE': (None, ''), 'ALTER PROCEDURE': (None, ''),
 'ALTER ROUTINE': (None, ''), 'DROP ROUTINE': (None, ''),
 'CREATE TRIGGER': ('create_trigger', 'no constraint triggers, no REFERENCING'),
 'DROP TRIGGER': ('drop_trigger', ''),
 'ALTER TRIGGER': ('set_trigger_state', 'enable/disable only; no rename'),
 'CREATE SEQUENCE': ('create_sequence', ''),
 'DROP SEQUENCE': ('drop_sequence', ''),
 'ALTER SEQUENCE': (None, 'RESTART, INCREMENT, OWNED BY, rename'),
 'CREATE TABLE': ('create_table', 'no LIKE, INHERITS, EXCLUDE, generated/identity columns'),
 'DROP TABLE': ('drop_table', ''),
 'ALTER TABLE': ('add_column etc', 'see the ALTER TABLE breakdown below'),
 'CREATE TABLE AS': (None, 'CTAS entirely'),
 'CREATE INDEX': ('create_index', ''),
 'DROP INDEX': ('drop_index', ''),
 'ALTER INDEX': ('create_index', 'rename via convergence only; no SET storage params'),
 'CREATE VIEW': ('replace_view', ''),
 'DROP VIEW': ('drop_view', ''),
 'ALTER VIEW': (None, 'rename, owner, SET/RESET options, ALTER COLUMN SET DEFAULT'),
 'CREATE MATERIALIZED VIEW': ('replace_view', 'only replaces an existing one; cannot create a new matview'),
 'DROP MATERIALIZED VIEW': ('drop_view', ''),
 'ALTER MATERIALIZED VIEW': (None, ''),
 'REFRESH MATERIALIZED VIEW': (None, 'not covered; arguably maintenance'),
 'CREATE POLICY': ('create_policy', ''),
 'DROP POLICY': ('drop_policy', ''),
 'ALTER POLICY': (None, 'change USING/CHECK/roles in place'),
 'GRANT': ('grant', 'table-level only; not on schema, function, sequence, database'),
 'REVOKE': ('revoke', 'same limit'),
 'ALTER DEFAULT PRIVILEGES': (None, ''),
 'COMMENT': ('create_*', 'emitted with creation only; cannot change one alone'),
 'TRUNCATE': (None, ''),
}
# Everything else on the page, classified.
maintenance = {'REINDEX','VACUUM','ANALYZE','CLUSTER','CHECKPOINT','DISCARD','LOAD'}
cluster_wide = {'CREATE DATABASE','DROP DATABASE','ALTER DATABASE','CREATE TABLESPACE',
  'DROP TABLESPACE','ALTER TABLESPACE','CREATE ROLE','DROP ROLE','ALTER ROLE',
  'CREATE USER','DROP USER','ALTER USER','CREATE GROUP','DROP GROUP','ALTER GROUP',
  'ALTER SYSTEM','DROP OWNED','REASSIGN OWNED','CREATE EVENT TRIGGER',
  'DROP EVENT TRIGGER','ALTER EVENT TRIGGER'}
not_ddl = {'ABORT','BEGIN','CALL','CLOSE','COMMIT','COMMIT PREPARED','COPY','DEALLOCATE',
  'DECLARE','DELETE','DO','END','EXECUTE','EXPLAIN','FETCH','INSERT','LISTEN','LOCK',
  'MERGE','MOVE','NOTIFY','PREPARE','PREPARE TRANSACTION','RELEASE SAVEPOINT','RESET',
  'ROLLBACK','ROLLBACK PREPARED','ROLLBACK TO SAVEPOINT','SAVEPOINT','SELECT',
  'SELECT INTO','SET','SET CONSTRAINTS','SET ROLE','SET SESSION AUTHORIZATION',
  'SET TRANSACTION','SHOW','START TRANSACTION','UNLISTEN','UPDATE','VALUES'}

page = [l.strip() for l in open(__import__('os').path.join(__import__('os').path.dirname(__import__('os').path.abspath(__file__)), 'pg18-sql-commands.txt')) if l.strip()]
full, partial, missing = [], [], []
for c in page:
    if c in not_ddl or c in maintenance or c in cluster_wide: continue
    if c in covers:
        kind, gap = covers[c]
        (partial if (kind and gap) else full if kind else missing).append((c, kind, gap))
    else:
        missing.append((c, None, ''))
print("FULLY COVERED (%d)" % len(full))
for c,k,_ in full: print("  %-32s %s" % (c,k))
print("\nPARTIAL (%d)" % len(partial))
for c,k,g in partial: print("  %-32s %-18s gap: %s" % (c,k,g))
print("\nNOT COVERED (%d)" % len(missing))
for c,_,_ in missing: print("  %s" % c)
print("\nEXCLUDED BY DESIGN: maintenance %d, cluster/instance-wide %d, not DDL %d"
      % (len(maintenance), len(cluster_wide), len(not_ddl)))
