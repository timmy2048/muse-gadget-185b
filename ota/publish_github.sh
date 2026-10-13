#!/usr/bin/env bash
# Publish a built muse-gadget.bin as a public GitHub Release.
# Usage: publish_github.sh /path/to/muse-gadget.bin [version] [notes]
# Repo must be PUBLIC (no download token in firmware).
set -euo pipefail

REPO="${MUSE_OTA_GITHUB_REPO:-timmy2048/muse-gadget-185b}"
src=${1:?bin path required}
label=${2:-}
notes_arg=${3:-${MUSE_OTA_NOTES:-}}

embed=$(python3 - "$src" <<'PY'
import struct, sys
data = open(sys.argv[1], "rb").read()
magic = struct.pack("<I", 0xABCD5432)
idx = data.find(magic)
if idx < 0:
    print("")
    raise SystemExit(0)
ver = data[idx + 16 : idx + 48].split(b"\0", 1)[0].decode("ascii", "replace")
print(ver)
PY
)
if [ -z "$embed" ]; then
  echo "ERROR: could not read esp_app_desc.version from $src" >&2
  exit 1
fi
if [ -z "$label" ]; then
  label="$embed"
fi
if [ "$embed" != "$label" ]; then
  echo "ERROR: embedded app version '$embed' != publish label '$label'." >&2
  exit 1
fi

workdir=$(mktemp -d)
trap 'rm -rf "$workdir"' EXIT
bin_ver="$workdir/muse-gadget-185b-${label}.bin"
bin_latest="$workdir/muse-gadget-185b-latest.bin"
manifest="$workdir/latest.json"
cp -f "$src" "$bin_ver"
cp -f "$src" "$bin_latest"
sha=$(sha256sum "$bin_latest" | awk '{print $1}')
size=$(wc -c < "$bin_latest" | tr -d ' ')
url="https://github.com/${REPO}/releases/latest/download/muse-gadget-185b-latest.bin"
python3 - "$manifest" "$label" "$url" "$sha" "$size" "$notes_arg" <<'PY'
import json, sys
path, version, url, sha, size, notes = sys.argv[1:7]
doc = {"version": version, "url": url, "sha256": sha, "size": int(size), "notes": notes}
with open(path, "w", encoding="utf-8") as f:
    json.dump(doc, f, indent=2, ensure_ascii=False)
    f.write("\n")
print(json.dumps(doc, indent=2, ensure_ascii=False))
PY

tag="v${label}"
title="muse-gadget-185b ${label}"
body=${notes_arg:-"Firmware ${label}. OTA via SETTINGS check-update."}

if gh release view "$tag" -R "$REPO" >/dev/null 2>&1; then
  echo "Deleting existing release $tag ..."
  gh release delete "$tag" -R "$REPO" --yes --cleanup-tag
fi

gh release create "$tag" -R "$REPO" --title "$title" --notes "$body" \
  "$bin_ver" "$bin_latest" "$manifest"

echo "SHA256=${sha}"
echo "LATEST=https://github.com/${REPO}/releases/latest/download/muse-gadget-185b-latest.bin"
echo "MANIFEST=https://github.com/${REPO}/releases/latest/download/latest.json"
