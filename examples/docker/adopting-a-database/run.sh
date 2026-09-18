#!/usr/bin/env bash
# A database you have, whose history you do not.
#
# Ten years of hand-applied DDL and no scripts. Pointing a migration tool at it
# already "works" in the sense that nothing objects -- but an empty ledger is
# indistinguishable from a database nobody has ever run the tool against. An
# epoch is the difference between those two, recorded and auditable: tracking
# deliberately begins HERE, and what came before is deliberately unknown.
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
. "$here/../lib.sh"

need_binaries
wait_for "$ALPHA_PORT"
DB=legacy
say "PostgreSQL $(server_version "$ALPHA_PORT")"

recreate_db "$ALPHA_PORT" "$DB"

say "1. Ten years of DDL nobody scripted"
psql -X -q -v ON_ERROR_STOP=1 "$(url "$ALPHA_PORT" "$DB")" <<'SQL'
CREATE TABLE customers(id bigint PRIMARY KEY, name text NOT NULL);
CREATE TABLE invoices(id bigint PRIMARY KEY, customer_id bigint REFERENCES customers(id));
CREATE INDEX invoices_customer_idx ON invoices(customer_id);
INSERT INTO customers SELECT g, 'customer ' || g FROM generate_series(1, 500) g;
SQL
psql -X -t -A -c "SELECT '  ' || count(*) || ' tables already here, and no file describes any of them'
                    FROM pg_tables WHERE schemaname='public'" "$(url "$ALPHA_PORT" "$DB")"

kid=$(install_ledger "$ALPHA_PORT" "$DB" "$here/keys" adopt)
export DATABASE_URL="$(url "$ALPHA_PORT" "$DB")"

say "2. The ledger is empty -- and that is ambiguous"
echo "  An empty ledger says nothing about whether this database has ten years"
echo "  of history or none. Opening an epoch is how the difference gets recorded:"
run psql -X -q -c \
  "INSERT INTO laswell.epoch(name, note) VALUES ('adopted-2026-09', 'Adopted on 2026-09-18. Everything before this was applied by hand and is deliberately unknown.')" \
  "$DATABASE_URL"

psql -X -q -c "\pset border 2" -c \
  "SELECT name, opened_by, note FROM laswell.epoch WHERE name <> 'default'" "$DATABASE_URL"

say "3. Track forward from there"
sign_dir "$here/migrations" "$here/keys/adopt.key.pem" "$kid"
run "$PG_LASWELL" --repo "$here/migrations"

say "4. What the tool now claims, and what it does not"
echo "  It answers for the adopted epoch and says nothing about the ten years"
echo "  before it -- which is the honest answer, and the only one available."
run "$PG_LASWELL" --repo "$here/migrations" --status
