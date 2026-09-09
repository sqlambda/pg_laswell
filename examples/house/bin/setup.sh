#!/usr/bin/env bash
# Build the "house" example database from nothing: roles, database, ledger, config.
#
# Run as a superuser on the target cluster. pg_laswell never runs bootstrap.sql
# itself and never creates its own schema -- if it did, its runtime role would
# own laswell.trusted_key, and a role that owns a table can insert into it, so
# the trust gate would collapse into a comment.
#
#   bin/setup.sh [--port N] [--host H] [--superuser-db postgres]
set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
repo=$(cd "$here/../.." && pwd)

port=${PGPORT:-5432}
host=${PGHOST:-}
sudb=postgres
while [ $# -gt 0 ]; do
  case $1 in
    --port)         port=$2; shift 2 ;;
    --host)         host=$2; shift 2 ;;
    --superuser-db) sudb=$2; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

psu=(psql -p "$port" -v ON_ERROR_STOP=1 -q)
[ -n "$host" ] && psu+=(-h "$host")
conn_host=${host:-127.0.0.1}

umask 077
mkdir -p "$here/keys"

# --- the signing key -------------------------------------------------------
# Ed25519, generated here because this is an example. On a real system the
# private key lives on a machine that cannot reach production at all, and only
# the public half and the key id ever travel.
if [ ! -f "$here/keys/house-dev.key.pem" ]; then
  openssl genpkey -algorithm ed25519 -out "$here/keys/house-dev.key.pem"
  chmod 600 "$here/keys/house-dev.key.pem"
fi
openssl pkey -in "$here/keys/house-dev.key.pem" -pubout -outform DER \
        -out "$here/keys/house-dev.pub.der"
python3 - "$here" <<'PY'
import base64, hashlib, pathlib, sys
d = pathlib.Path(sys.argv[1], "keys")
der = (d / "house-dev.pub.der").read_bytes()
spki = bytes([0x30,0x2a,0x30,0x05,0x06,0x03,0x2b,0x65,0x70,0x03,0x21,0x00])
assert len(der) == 44 and der[:12] == spki, "unexpected public key encoding"
raw = der[12:]
# The key id is the content address of the key: "ed25519:" plus the first
# sixteen hex characters of the SHA-256 of the raw 32-byte public key. It
# therefore cannot be reassigned to different bytes.
(d / "house-dev.pub.b64").write_text(base64.b64encode(raw).decode() + "\n")
(d / "house-dev.key_id").write_text("ed25519:" + hashlib.sha256(raw).hexdigest()[:16] + "\n")
PY
key_id=$(cat "$here/keys/house-dev.key_id")

# --- the runtime password --------------------------------------------------
if [ ! -f "$here/keys/house_runner.password" ]; then
  openssl rand -hex 16 > "$here/keys/house_runner.password"
  chmod 600 "$here/keys/house_runner.password"
fi
pw=$(cat "$here/keys/house_runner.password")

# --- roles and database ----------------------------------------------------
"${psu[@]}" -d "$sudb" <<SQL
DROP DATABASE IF EXISTS house;
DROP ROLE IF EXISTS house_runner;
DROP ROLE IF EXISTS house_app;

-- The migrating role. It owns the house schema and everything the repository
-- creates, and it has SELECT and nothing more on the trust tables.
CREATE ROLE house_runner LOGIN PASSWORD '$pw';
COMMENT ON ROLE house_runner IS 'pg_laswell migrating role for the house database. Owns the application schema; cannot write laswell.trusted_key.';

-- The application role. Granted by 0100, and never granted any DDL right.
CREATE ROLE house_app NOLOGIN;
COMMENT ON ROLE house_app IS 'Read/write application role for the house inventory. Holds no DDL rights.';

CREATE DATABASE house OWNER house_runner ENCODING 'UTF8';
COMMENT ON DATABASE house IS 'Toy household-inventory database, used to exercise pg_laswell migrations.';
SQL

# --- the ledger ------------------------------------------------------------
"${psu[@]}" -d house \
  -v laswell_role=house_runner \
  -v first_key_id="$key_id" \
  -v first_key_b64="$(cat "$here/keys/house-dev.pub.b64")" \
  -v first_key_label=house-dev \
  -f "$repo/sql/bootstrap.sql" > /dev/null

# The environment label is set by a privileged role, deliberately: every spec
# in this repository declares target.environment "dev", and a role that could
# relabel its own database is a gate that exists only as a comment.
"${psu[@]}" -d house \
  -c "INSERT INTO laswell.environment(name) VALUES ('dev')
        ON CONFLICT (only_one) DO UPDATE SET name = EXCLUDED.name,
             set_at = now(), set_by = current_user;" \
  -c "GRANT CREATE, CONNECT ON DATABASE house TO house_runner;" \
  -c "GRANT USAGE ON SCHEMA laswell TO house_app;" \
  -c "GRANT pg_read_all_stats TO house_runner;" > /dev/null

# --- the client configuration ----------------------------------------------
# Mode 0600: it holds a password and it declares which signing keys are
# trusted. Same rule as ~/.pgpass, for the same two reasons.
cat > "$here/laswell.ini" <<INI
; pg_laswell configuration for the "house" example database.
; Written by bin/setup.sh. Mode 0600, deliberately.

[trust]
accept = $key_id

[key $key_id]
label      = house-dev
public_key = $(cat "$here/keys/house-dev.pub.b64")

[executor]
; Small numbers on purpose: this database is a toy, and the point is to see
; pacing happen at all rather than to see it go fast.
batch_rows           = 50
commit_interval_ms   = 500
batch_cap_rows       = 500
dml_single_txn_rows  = 100
lock_timeout_ms      = 3000
observer_tick_ms     = 250
; 12, not 8: the largest group pg_laswell derives from this repository is
; eleven, and a lower ceiling would hide that behind the executor's own limit.
max_concurrent_jobs  = 12
throttle_waiters     = 4
pause_waiters        = 8
resume_waiters       = 1
app_pool_size        = 10

[house]
host     = $conn_host
port     = $port
dbname   = house
user     = house_runner
password = $pw
INI
chmod 600 "$here/laswell.ini"

echo "house: database, ledger and $here/laswell.ini are ready."
echo "next:  bin/sign-specs.sh && bin/run.sh"
