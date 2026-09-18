#!/bin/sh
# The 2 -> 3 ledger upgrade, against the REAL previous bootstrap rather than a
# copy of its DDL in a fixture.
#
# A copy would drift from the file it stands for, and the one thing this has to
# prove is that a ledger written by the PREVIOUS release upgrades -- so the
# previous release's own script is what installs it. `git show` is how it is
# fetched; a fixture that has to be kept in step with a file in the same
# repository is a fixture that will quietly stop standing for it.
#
# Needs DATABASE_URL pointing at a cluster it may create databases on.
set -eu

: "${DATABASE_URL:?DATABASE_URL must be set}"
HERE=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

DB=laswell_upgrade_$$
BASE=$(printf '%s' "$DATABASE_URL" | sed 's#/[^/?]*\(?.*\)\{0,1\}$##')
TARGET="$BASE/$DB"

# A key whose id is the content address of its bytes, which the trust table
# checks. Built here so the script needs nothing but psql and coreutils.
KEY_B64=$(printf '%s' "$(head -c 32 /dev/zero | tr '\0' 'A')" | head -c 32 | base64 | tr -d '\n')
KEY_RAW=$(printf 'AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA')
KEY_B64=$(printf '%s' "$KEY_RAW" | base64 | tr -d '\n')
KEY_ID="ed25519:$(printf '%s' "$KEY_RAW" | sha256sum | cut -c1-16)"

psql -X -q -c "DROP DATABASE IF EXISTS $DB" "$DATABASE_URL"
psql -X -q -c "CREATE DATABASE $DB" "$DATABASE_URL"
psql -X -q -c "CREATE ROLE laswell_runner NOLOGIN" "$DATABASE_URL" 2>/dev/null || true

cleanup() {
  psql -X -q -c "DROP DATABASE IF EXISTS $DB" "$DATABASE_URL" >/dev/null 2>&1 || true
  rm -rf "$WORK"
}
trap cleanup EXIT

install_with() {
  PSQLRC=/dev/null psql -X -q -v ON_ERROR_STOP=1 \
    -v laswell_role=laswell_runner \
    -v first_key_id="$KEY_ID" -v first_key_b64="$KEY_B64" \
    -v first_key_label=upgrade-suite \
    -f "$1" "$TARGET"
}

scalar() { psql -X -t -A -c "$1" "$TARGET"; }

# The previous release's script. HEAD~ is not used: this runs on a branch whose
# HEAD may already carry the new schema, and origin/main is what "the version
# before this change" means.
PREV=$(git -C "$HERE" rev-parse --verify --quiet origin/main || git -C "$HERE" rev-parse --verify main)
git -C "$HERE" show "$PREV:sql/bootstrap.sql" > "$WORK/previous.sql"

echo "installing the previous ledger from $PREV"
install_with "$WORK/previous.sql"

BEFORE=$(scalar "SELECT max(version) FROM laswell.schema_version")
[ "$BEFORE" = "2" ] || { echo "expected the previous script to install 2, got $BEFORE"; exit 1; }

psql -X -q -v ON_ERROR_STOP=1 -c \
  "INSERT INTO laswell.migration(spec_id, spec_digest, canonical_bytes, signer_key_id, signature)
   VALUES ('0001-before-epochs','digest-one','\\x00','$KEY_ID','\\x00'),
          ('0002-before-epochs','digest-two','\\x00','$KEY_ID','\\x00')" "$TARGET"

echo "upgrading"
install_with "$HERE/sql/bootstrap.sql"

AFTER=$(scalar "SELECT max(version) FROM laswell.schema_version")
[ "$AFTER" = "3" ] || { echo "expected version 3 after upgrade, got $AFTER"; exit 1; }

ROWS=$(scalar "SELECT count(*) FROM laswell.migration")
[ "$ROWS" = "2" ] || { echo "history was lost: expected 2 rows, got $ROWS"; exit 1; }

EPOCHS=$(scalar "SELECT string_agg(DISTINCT epoch, ',') FROM laswell.migration")
[ "$EPOCHS" = "default" ] || { echo "expected existing history in 'default', got $EPOCHS"; exit 1; }

UNIQ=$(scalar "SELECT string_agg(pg_get_constraintdef(oid), ' | ' ORDER BY conname)
                 FROM pg_constraint
                WHERE conrelid = 'laswell.migration'::regclass AND contype = 'u'")
[ "$UNIQ" = "UNIQUE (epoch, spec_digest)" ] || {
  echo "expected the per-epoch unique to replace the global one, got: $UNIQ"; exit 1; }

# Both halves of what the new constraint is for: the same digest in two epochs
# is allowed, and the same digest twice in ONE epoch is still refused.
psql -X -q -v ON_ERROR_STOP=1 -c \
  "INSERT INTO laswell.epoch(name, note) VALUES ('second','upgrade suite')" "$TARGET"
psql -X -q -v ON_ERROR_STOP=1 -c \
  "INSERT INTO laswell.migration(spec_id, spec_digest, epoch, canonical_bytes, signer_key_id, signature)
   VALUES ('0001-before-epochs','digest-one','second','\\x00','$KEY_ID','\\x00')" "$TARGET"

if psql -X -q -c \
  "INSERT INTO laswell.migration(spec_id, spec_digest, epoch, canonical_bytes, signer_key_id, signature)
   VALUES ('0001-again','digest-one','second','\\x00','$KEY_ID','\\x00')" "$TARGET" 2>/dev/null; then
  echo "a duplicate digest WITHIN one epoch was accepted; the constraint is not doing its job"
  exit 1
fi

echo "re-running the new script must change nothing"
install_with "$HERE/sql/bootstrap.sql"
AGAIN=$(scalar "SELECT max(version) FROM laswell.schema_version")
[ "$AGAIN" = "3" ] || { echo "re-run moved the version to $AGAIN"; exit 1; }
STILL=$(scalar "SELECT count(*) FROM laswell.migration")
[ "$STILL" = "3" ] || { echo "re-run changed the history: $STILL rows"; exit 1; }

echo "ledger upgrade 2 -> 3: ok"
