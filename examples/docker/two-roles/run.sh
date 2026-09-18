#!/usr/bin/env bash
# Two connections, one database -- and why that is not two databases.
#
# A role that owns the DDL beside the one the application uses is an ordinary
# configuration. It is also the case a scheduler gets wrong if it decides
# independence by comparing connection NAMES: different names, so surely
# different databases, so surely safe to run at once. They are the same
# database and the two migrations touch the same table.
#
# Identity is the cluster's system_identifier with the database's oid, so the
# two names are recognised as one database and the overlap is found.
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
. "$here/../lib.sh"
need_binaries; wait_for "$ALPHA_PORT"
DB=tworoles
say "PostgreSQL $(server_version "$ALPHA_PORT")"
recreate_db "$ALPHA_PORT" "$DB"
psql -X -q -v ON_ERROR_STOP=1 -c "CREATE TABLE orders(id bigint PRIMARY KEY)" "$(url "$ALPHA_PORT" "$DB")"
kid=$(install_ledger "$ALPHA_PORT" "$DB" "$here/keys" roles)
sign_dir "$here/migrations" "$here/keys/roles.key.pem" "$kid"

cat > "$here/roles.ini" <<INI
[ddl]
host = 127.0.0.1
port = $ALPHA_PORT
dbname = $DB
user = postgres
password = laswell

[alias]
host = 127.0.0.1
port = $ALPHA_PORT
dbname = $DB
user = postgres
password = laswell
INI
chmod 600 "$here/roles.ini"

say "Two connection names, one database"
psql -X -t -A -c "SELECT '  both resolve to system_identifier ' ||
    (SELECT system_identifier FROM pg_control_system()) || ', database oid ' ||
    (SELECT oid FROM pg_database WHERE datname = current_database())::text" "$(url "$ALPHA_PORT" "$DB")"

say "They are NOT proven concurrent, because they are not different databases"
run "$PG_LASWELL" --config "$here/roles.ini" --repo "$here/migrations" --status
echo
echo "  Both touch public.orders. Had the two names been taken for two"
echo "  databases, they would have been run at once on a claim of proof."

say "Applied one after the other, which is correct"
run "$PG_LASWELL" --config "$here/roles.ini" --repo "$here/migrations"
