#!/usr/bin/env bash
# Sign every specification in migrations/ with the house-dev key.
#
# pg_laswell verifies signatures and never creates them, so the canonical bytes
# come from getSpecDigest and the signing happens here, with openssl. The
# signature covers the spec MINUS its own "signatures" key, so re-signing an
# already-signed file is idempotent.
set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
mcp=${PGLASWELL_MCP:-$here/../../cpp/build/pg_laswell_mcp}
key=$here/keys/house-dev.key.pem
key_id=$(cat "$here/keys/house-dev.key_id")

[ -x "$mcp" ] || { echo "no pg_laswell_mcp at $mcp -- build it, or set PGLASWELL_MCP" >&2; exit 1; }

tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
rc=0

for spec in "$here"/migrations/*.json; do
  name=$(basename "$spec")

  # getSpecDigest needs no database, which is the point: the machine holding the
  # private key is deliberately not the machine that can reach the database.
  python3 -c '
import json, sys
doc = json.load(open(sys.argv[1]))
doc.pop("signatures", None)
json.dump({"spec": doc}, sys.stdout)
' "$spec" > "$tmp/args.json"

  if ! "$mcp" --call getSpecDigest --args "@$tmp/args.json" > "$tmp/digest.json"; then
    echo "REFUSED  $name"
    python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); print("         " + str(d.get("error") or d)); print("         " + str(d.get("hint","")))' "$tmp/digest.json" || cat "$tmp/digest.json"
    rc=1
    continue
  fi

  python3 -c '
import json, sys
print(json.load(open(sys.argv[1]))["canonicalBytes"], end="")
' "$tmp/digest.json" > "$tmp/canonical.bin"

  sig=$(openssl pkeyutl -sign -inkey "$key" -rawin -in "$tmp/canonical.bin" | base64 -w0)

  python3 -c '
import json, sys
path, key_id, sig = sys.argv[1], sys.argv[2], sys.argv[3]
doc = json.load(open(path))
doc["signatures"] = [{"key_id": key_id, "algorithm": "ed25519", "signature": sig}]
with open(path, "w") as fh:
    json.dump(doc, fh, indent=2, ensure_ascii=False)
    fh.write("\n")
' "$spec" "$key_id" "$sig"

  echo "signed   $name"
done

exit $rc
