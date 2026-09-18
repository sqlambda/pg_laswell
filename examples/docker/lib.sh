# Shared plumbing for the docker examples. Sourced, not run.
#
# Every example needs the same four things and none of them are the point of any
# example: reach a cluster, install the ledger, make a signing key the database
# trusts, and sign a directory of specifications. Doing them once here keeps each
# example's own script about the arrangement it exists to show.
set -euo pipefail

LASWELL_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
PG_LASWELL=${PG_LASWELL:-$LASWELL_ROOT/cpp/build/pg_laswell}
PG_LASWELL_MCP=${PG_LASWELL_MCP:-$LASWELL_ROOT/cpp/build/pg_laswell_mcp}

ALPHA_PORT=${ALPHA_PORT:-55432}
BETA_PORT=${BETA_PORT:-55433}
GAMMA_PORT=${GAMMA_PORT:-55434}
PGPASSWORD_DEFAULT=laswell

need_binaries() {
  for b in "$PG_LASWELL" "$PG_LASWELL_MCP"; do
    [ -x "$b" ] || {
      echo "no binary at $b" >&2
      echo "build it first:  cmake -S cpp -B cpp/build && cmake --build cpp/build" >&2
      exit 1
    }
  done
  command -v psql >/dev/null || { echo "psql is required" >&2; exit 1; }
  command -v openssl >/dev/null || { echo "openssl is required" >&2; exit 1; }
}

# url <port> [dbname]
url() { printf 'postgresql://postgres:%s@127.0.0.1:%s/%s' "$PGPASSWORD_DEFAULT" "$1" "${2:-app}"; }

wait_for() {  # wait_for <port>
  local i
  for i in $(seq 1 60); do
    psql -X -q -c 'SELECT 1' "$(url "$1")" >/dev/null 2>&1 && return 0
    sleep 1
  done
  echo "no PostgreSQL answering on 127.0.0.1:$1" >&2
  echo "start the clusters:  docker compose -f examples/docker/compose.yml up -d" >&2
  exit 1
}

server_version() { psql -X -t -A -c 'SHOW server_version' "$(url "$1" "${2:-app}")"; }

recreate_db() {  # recreate_db <port> <dbname>
  # stdout dropped, stderr KEPT. An earlier version sent both to /dev/null to
  # hide the "does not exist, skipping" notice, and a real failure -- DROP
  # DATABASE refusing while a replication slot was still held -- became silence
  # plus an empty log. The notice is worth filtering; the error is not.
  psql -X -q -c "DROP DATABASE IF EXISTS \"$2\" WITH (FORCE)" "$(url "$1")" 2>&1 >/dev/null \
    | grep -v 'does not exist, skipping' >&2 || true
  psql -X -q -c "CREATE DATABASE \"$2\"" "$(url "$1")" >/dev/null
}

# make_key <dir> <name> -- writes <name>.key.pem and prints the key id.
make_key() {
  local dir=$1 name=$2
  umask 077; mkdir -p "$dir"
  [ -f "$dir/$name.key.pem" ] || openssl genpkey -algorithm ed25519 -out "$dir/$name.key.pem" 2>/dev/null
  openssl pkey -in "$dir/$name.key.pem" -pubout -outform DER 2>/dev/null | tail -c 32 > "$dir/$name.pub.raw"
  printf 'ed25519:%s' "$(sha256sum "$dir/$name.pub.raw" | cut -c1-16)"
}

# install_ledger <port> <dbname> <keydir> <keyname> -- bootstrap plus the trusted key.
install_ledger() {
  local port=$1 db=$2 dir=$3 name=$4
  local kid; kid=$(make_key "$dir" "$name")
  psql -X -q -c "CREATE ROLE laswell_runner NOLOGIN" "$(url "$port")" >/dev/null 2>&1 || true
  PSQLRC=/dev/null psql -X -q -v ON_ERROR_STOP=1 \
    -v laswell_role=laswell_runner \
    -v first_key_id="$kid" \
    -v first_key_b64="$(base64 -w0 < "$dir/$name.pub.raw")" \
    -v first_key_label="$name" \
    -f "$LASWELL_ROOT/sql/bootstrap.sql" "$(url "$port" "$db")" >/dev/null
  printf '%s' "$kid"
}

# sign_dir <dir> <keyfile> <key_id> -- signs every .json in place, idempotently.
sign_dir() {
  local dir=$1 key=$2 kid=$3 f
  for f in "$dir"/*.json; do
    [ -e "$f" ] || continue
    "$PG_LASWELL_MCP" --call getSpecDigest --args "{\"spec\":$(cat "$f")}" 2>/dev/null \
      | PGL_SPEC="$f" PGL_KEY="$key" PGL_KID="$kid" python3 -c '
import base64, json, os, subprocess, sys, tempfile
d = json.load(sys.stdin)
canon = d.get("canonicalBytes") or d.get("canonical_bytes")
if not canon:
    sys.exit(f"getSpecDigest returned no canonical bytes: {list(d)[:6]}")
with tempfile.NamedTemporaryFile(delete=False) as t:
    t.write(canon.encode()); tmp = t.name
sig = subprocess.run(["openssl", "pkeyutl", "-sign", "-inkey", os.environ["PGL_KEY"],
                      "-rawin", "-in", tmp], capture_output=True, check=True).stdout
os.unlink(tmp)
path = os.environ["PGL_SPEC"]
doc = json.load(open(path))
doc["signatures"] = [{"key_id": os.environ["PGL_KID"], "algorithm": "ed25519",
                      "signature": base64.b64encode(sig).decode()}]
json.dump(doc, open(path, "w"), indent=2)
'
  done
}

# open_epoch <port> <db> <name> <note>
#
# The note is a sentence a human wrote, so it may contain an apostrophe -- and
# splicing one into SQL is how "the application team's lineage" became a syntax
# error the first time this ran. Quoted as a literal, the way the hints the tool
# itself prints are.
sql_literal() { printf "'%s'" "$(printf '%s' "$1" | sed "s/'/''/g")"; }

open_epoch() {
  psql -X -q -v ON_ERROR_STOP=1 -c \
    "INSERT INTO laswell.epoch(name, note) VALUES ($(sql_literal "$3"), $(sql_literal "$4")) ON CONFLICT (name) DO NOTHING" \
    "$(url "$1" "$2")" >/dev/null
}

say() { printf '\n\033[1m%s\033[0m\n' "$*"; }
run() { printf '  $ %s\n' "$*"; "$@"; }
