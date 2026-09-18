#!/usr/bin/env bash
# One repository, three environments.
#
# Each database says what it IS, in a table only the schema owner can write.
# A specification declaring an environment is applied only where the two agree.
# The declaration is inside the signature, so it cannot be edited on the way to
# production; the label is in the database, so a permissive config cannot talk
# production into accepting a development change.
#
# A database NAME would prove less -- a dump restored elsewhere keeps its name,
# and the restored copy would happily accept production's migrations.
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
. "$here/../lib.sh"
need_binaries; wait_for "$ALPHA_PORT"
say "PostgreSQL $(server_version "$ALPHA_PORT")"

for env in dev staging production; do
  recreate_db "$ALPHA_PORT" "env_$env"
  psql -X -q -v ON_ERROR_STOP=1 -c "CREATE TABLE orders(id bigint PRIMARY KEY)" "$(url "$ALPHA_PORT" "env_$env")"
  kid=$(install_ledger "$ALPHA_PORT" "env_$env" "$here/keys" envs)
  # The label is written by the schema OWNER, not by the migrating role.
  psql -X -q -v ON_ERROR_STOP=1 -c \
    "INSERT INTO laswell.environment(name) VALUES ('$env')
     ON CONFLICT (only_one) DO UPDATE SET name = EXCLUDED.name" "$(url "$ALPHA_PORT" "env_$env")"
done
sign_dir "$here/migrations" "$here/keys/envs.key.pem" "$kid"

for env in dev staging production; do
  say "$env"
  DATABASE_URL="$(url "$ALPHA_PORT" "env_$env")" "$PG_LASWELL" --repo "$here/migrations"
done

say "What each database ended up with"
for env in dev staging production; do
  psql -X -t -A -c "SELECT '  $env: ' || coalesce(string_agg(spec_id, ', ' ORDER BY spec_id), 'nothing')
                      FROM laswell.migration" "$(url "$ALPHA_PORT" "env_$env")"
done
echo "  The production-only index exists in exactly one place, and no file"
echo "  had to be different for that to be true."
