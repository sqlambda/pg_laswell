#!/usr/bin/env bash
# Build the whole audit topology from nothing: a throwaway cluster, two
# databases, two ledgers, a signing key, and a configuration naming both.
#
#   bin/setup.sh [--port N]        default 5610
#
# TWO throwaway clusters, and the second one is not neatness. MEASURED here:
# a subscription whose publisher is the SAME PostgreSQL instance deadlocks
# against itself. CREATE SUBSCRIPTION takes a transaction id when it writes
# pg_subscription, then asks the publisher for a replication slot; slot
# creation waits for every in-progress transaction to finish, including that
# one -- which cannot finish until the slot exists. pg_stat_activity shows it
# exactly:
#
#   store | walsender | active | wait=Lock:transactionid | CREATE_REPLICATION_SLOT
#
# PostgreSQL cannot see that cycle, because half of it is a walsender reached
# over a socket. Two clusters is also what a real deployment looks like.
#
# Throwaway rather than yours, for two more reasons: logical replication needs
# wal_level=logical, which is not a setting to turn on in somebody's
# development cluster on an example's behalf; and a subscription must
# authenticate to the publisher, which trust auth on loopback makes a
# non-question where a shared cluster would want a password that pg_laswell
# deliberately refuses to put in a specification.
#
# Nothing here touches an existing server. bin/teardown.sh removes all of it.
set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
repo=$(cd "$here/../.." && pwd)
port=5610
while [ $# -gt 0 ]; do
  case "$1" in
    --port) port=$2; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

store_cluster=$here/.cluster/store
audit_cluster=$here/.cluster/audit
store_port=$port
audit_port=$((port + 1))
bin=${PGBIN:-}
if [ -z "$bin" ]; then
  for d in $(ls -d /usr/lib/postgresql/*/bin 2>/dev/null | sort -Vr); do
    [ -x "$d/initdb" ] && { bin=$d; break; }
  done
fi
[ -x "${bin:-}/initdb" ] || { echo "no initdb found; set PGBIN" >&2; exit 3; }
echo "using $bin"

# --- the two clusters ------------------------------------------------------
if [ -d "$here/.cluster" ]; then
  echo "clusters already exist at $here/.cluster -- run bin/teardown.sh first" >&2
  exit 3
fi
mkdir -p "$store_cluster" "$audit_cluster"
for pair in "store:$store_cluster:$store_port" "audit:$audit_cluster:$audit_port"; do
  name=${pair%%:*}; rest=${pair#*:}; dir=${rest%%:*}; p=${rest##*:}
  "$bin/initdb" -D "$dir" -U postgres -A trust --no-sync >/dev/null
  # wal_level=logical is what a publication needs; without it CREATE
  # PUBLICATION succeeds and nothing ever replicates, which is the quiet
  # failure this example exists to avoid demonstrating by accident.
  "$bin/pg_ctl" -D "$dir" -l "$dir/server.log" -w start -o \
    "-p $p -c listen_addresses=127.0.0.1 -c unix_socket_directories=$dir \
     -c wal_level=logical -c max_replication_slots=8 -c max_wal_senders=8" >/dev/null
  psql -X -q -h 127.0.0.1 -p "$p" -U postgres -v ON_ERROR_STOP=1 \
       -d postgres -c "CREATE DATABASE $name" >/dev/null
  # hstore is what the wiki's audit trigger stores row_data in, so both sides
  # need it before the table that uses it exists.
  psql -X -q -h 127.0.0.1 -p "$p" -U postgres -v ON_ERROR_STOP=1 \
       -d "$name" -c 'CREATE EXTENSION IF NOT EXISTS hstore' >/dev/null
  echo "$name cluster up on port $p"
done

# --- the signing key -------------------------------------------------------
# Generated here because this is an example. On a real system the private key
# lives on a machine that cannot reach either database, and only the public
# half and the key id ever travel.
mkdir -p "$here/keys"
if [ ! -f "$here/keys/audit-dev.key.pem" ]; then
  openssl genpkey -algorithm ed25519 -out "$here/keys/audit-dev.key.pem"
  chmod 600 "$here/keys/audit-dev.key.pem"
fi
openssl pkey -in "$here/keys/audit-dev.key.pem" -pubout -outform DER \
        -out "$here/keys/audit-dev.pub.der"
python3 - "$here" <<'PY'
import base64, hashlib, pathlib, sys
d = pathlib.Path(sys.argv[1], "keys")
der = (d / "audit-dev.pub.der").read_bytes()
spki = bytes([0x30,0x2a,0x30,0x05,0x06,0x03,0x2b,0x65,0x70,0x03,0x21,0x00])
assert len(der) == 44 and der[:12] == spki, "unexpected public key encoding"
raw = der[12:]
(d / "audit-dev.pub.b64").write_text(base64.b64encode(raw).decode() + "\n")
(d / "audit-dev.key_id").write_text("ed25519:" + hashlib.sha256(raw).hexdigest()[:16] + "\n")
PY
key_id=$(cat "$here/keys/audit-dev.key_id")
key_b64=$(cat "$here/keys/audit-dev.pub.b64")

# --- a ledger in EACH database ---------------------------------------------
# Two databases, two ledgers, and that is the design rather than a limitation:
# each records what was applied to it, so "has that been applied" is answered
# where it ran. pg_laswell never installs its own schema -- if it did, its
# runtime role would own laswell.trusted_key, and a role that owns a table can
# insert into it, so the trust gate would collapse into a comment.
for pair in "store:$store_port" "audit:$audit_port"; do
  db=${pair%%:*}; p=${pair##*:}
  psu=(psql -X -q -h 127.0.0.1 -p "$p" -U postgres -v ON_ERROR_STOP=1)
  "${psu[@]}" -d "$db" \
    -v laswell_role=postgres \
    -v first_key_id="$key_id" \
    -v first_key_b64="$key_b64" \
    -v first_key_label=audit-dev \
    -f "$repo/sql/bootstrap.sql" > /dev/null
  # Labelled by a privileged role, deliberately: every specification here
  # declares target.environment "dev", and a role that could relabel its own
  # database is a gate that exists only as a comment.
  "${psu[@]}" -d "$db" -c \
    "INSERT INTO laswell.environment(name) VALUES ('dev')
       ON CONFLICT (only_one) DO UPDATE SET name = EXCLUDED.name,
       set_at = now(), set_by = current_user" >/dev/null
done
echo "ledgers bootstrapped in both, labelled dev"

# --- the client configuration ----------------------------------------------
# TWO connections, which is the whole point: a specification names one in
# target.connection and is applied there, and depends_on crosses between them.
# Mode 0600 because it declares which signing keys are trusted.
cat > "$here/laswell.ini" <<INI
; pg_laswell configuration for the "audit" example. Written by bin/setup.sh.
; Mode 0600, deliberately.

[trust]
accept = $key_id

[key $key_id]
label      = audit-dev
public_key = $key_b64

[executor]
batch_rows           = 50
commit_interval_ms   = 500
lock_timeout_ms      = 3000
observer_tick_ms     = 250
max_concurrent_jobs  = 4

[store]
host   = 127.0.0.1
port   = $store_port
dbname = store
user   = postgres

[audit]
host   = 127.0.0.1
port   = $audit_port
dbname = audit
user   = postgres
INI
chmod 600 "$here/laswell.ini"

# The subscription dials the publisher by port, and the port is chosen here, so
# the specification carries a placeholder rather than a number that would be
# wrong on anyone else's machine.
sed -i "s/__PORT__/$store_port/" "$here/migrations/0040-subscription.json"

echo
echo "wrote $here/laswell.ini  (store on $store_port, audit on $audit_port)"
echo "next:  bin/sign-specs.sh && bin/run.sh"
