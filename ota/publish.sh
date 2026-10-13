#!/bin/bash
# Usage: publish.sh /path/to/muse-gadget.bin [version-label]
# Copies signed app bin into WG-only OTA tree, writes latest.json, prints URLs.
set -euo pipefail
src=${1:?bin path required}
label=${2:-$(date +%Y%m%d%H%M%S)}
dir=/opt/muse-ota/firmware
dst="${dir}/muse-gadget-185b-${label}.bin"
src_real=$(readlink -f "$src")
dst_real=$(readlink -f "$dst" 2>/dev/null || echo "$dst")

# Refuse publish when esp_app_desc.version inside the image != label.
# This prevents menu (latest.json) saying X while ota_start sees Y and SKIPPED.
embed=$(python3 - "$src" <<'PY'
import struct, sys
path = sys.argv[1]
data = open(path, "rb").read()
magic = struct.pack("<I", 0xABCD5432)
idx = data.find(magic)
if idx < 0:
    print("")
    sys.exit(0)
ver = data[idx + 16 : idx + 48].split(b"\0", 1)[0].decode("ascii", "replace")
print(ver)
PY
)
if [ -z "$embed" ]; then
  echo "ERROR: could not read esp_app_desc.version from $src" >&2
  exit 1
fi
if [ "$embed" != "$label" ]; then
  echo "ERROR: embedded app version '$embed' != publish label '$label'." >&2
  echo "Rebuild with -D PROJECT_VER=$label (and matching version.txt) before publish." >&2
  exit 1
fi

if [ "$src_real" != "$dst_real" ]; then
  install -m 0644 "$src" "$dst"
else
  chmod 0644 "$dst" || true
fi
ln -sfn "$(basename "$dst")" "${dir}/muse-gadget-185b-latest.bin"
sha=$(sha256sum "$dst" | awk '{print $1}')
size=$(stat -c%s "$dst")
url="http://10.0.0.1:8790/firmware/muse-gadget-185b-latest.bin"
notes=${MUSE_OTA_NOTES:-}
python3 - "$dir/latest.json" "$label" "$url" "$sha" "$size" "$notes" <<'PY2'
import json, sys
path, version, url, sha, size, notes = sys.argv[1:7]
doc = {
    "version": version,
    "url": url,
    "sha256": sha,
    "size": int(size),
    "notes": notes,
}
with open(path, "w", encoding="utf-8") as f:
    json.dump(doc, f, indent=2, ensure_ascii=False)
    f.write("\n")
print(json.dumps(doc, indent=2, ensure_ascii=False))
PY2
echo "SHA256=${sha}"
echo "URL=http://10.0.0.1:8790/firmware/$(basename "$dst")"
echo "LATEST=${url}"
echo "MANIFEST=http://10.0.0.1:8790/firmware/latest.json"
echo "Invoke: Settings check-update on device, or Muse device.ota url=<URL>"
