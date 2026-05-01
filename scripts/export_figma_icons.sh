#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
  echo "Usage: $0 '<figma-url-with-node-id>' ['output-dir']"
  exit 1
fi

: "${FIGMA_TOKEN:?Set FIGMA_TOKEN first (Figma personal access token)}"

FIGMA_URL="$1"
OUT_DIR="${2:-/home/avenblake/BDR_CP/App resource/icons}"
mkdir -p "$OUT_DIR"

FILE_KEY="$(python3 - "$FIGMA_URL" <<'PY'
import re,sys,urllib.parse as u
url=sys.argv[1]
m=re.search(r'^/(design|file)/([^/]+)/', u.urlparse(url).path)
if not m:
    raise SystemExit("Could not parse file key from URL")
print(m.group(2))
PY
)"

NODE_ID_HYPHEN="$(python3 - "$FIGMA_URL" <<'PY'
import sys,urllib.parse as u
q=u.parse_qs(u.urlparse(sys.argv[1]).query)
nid=q.get("node-id",[None])[0]
if not nid:
    raise SystemExit("URL must include node-id=...")
print(nid)
PY
)"

NODE_ID_COLON="${NODE_ID_HYPHEN//-/:}"
NODE_ID_ENC="${NODE_ID_COLON//:/%3A}"

NODES_JSON="$(curl -sS -H "X-Figma-Token: $FIGMA_TOKEN" \
  "https://api.figma.com/v1/files/$FILE_KEY/nodes?ids=$NODE_ID_ENC")"

mapfile -t ITEMS < <(jq -r --arg nid "$NODE_ID_COLON" '
  def walk_children:
    .children[]? as $c
    | $c, ($c | walk_children);

  .nodes[$nid].document
  | walk_children
  | select((.visible // true) == true)
  | select(.type == "COMPONENT" or .type == "INSTANCE" or .type == "FRAME" or .type == "GROUP" or .type == "VECTOR")
  | "\(.id)\t\(.name)"
' <<< "$NODES_JSON")

if [[ ${#ITEMS[@]} -eq 0 ]]; then
  echo "No exportable child nodes found under node $NODE_ID_COLON"
  exit 1
fi

IDS_CSV="$(printf '%s\n' "${ITEMS[@]}" | cut -f1 | paste -sd, -)"
IDS_ENC="${IDS_CSV//:/%3A}"

IMAGES_JSON="$(curl -sS -H "X-Figma-Token: $FIGMA_TOKEN" \
  "https://api.figma.com/v1/images/$FILE_KEY?ids=$IDS_ENC&format=svg")"

count=0
while IFS=$'\t' read -r id name; do
  url="$(jq -r --arg id "$id" '.images[$id] // empty' <<< "$IMAGES_JSON")"
  [[ -z "$url" ]] && continue

  safe="$(echo "$name" | tr '[:upper:]' '[:lower:]' | sed -E 's/[^a-z0-9._-]+/_/g; s/^_+|_+$//g')"
  [[ -z "$safe" ]] && safe="icon_$count"

  target="$OUT_DIR/$safe.svg"
  n=2
  while [[ -e "$target" ]]; do
    target="$OUT_DIR/${safe}_$n.svg"
    n=$((n+1))
  done

  curl -sSL "$url" -o "$target"
  echo "saved: $target"
  count=$((count+1))
done < <(printf '%s\n' "${ITEMS[@]}")

echo "Exported $count SVG(s) to: $OUT_DIR"
