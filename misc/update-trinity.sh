#!/bin/sh
# Downloads the latest Trinity mod pk3s from GitHub and removes
# any stale checksum-named variants left by auto-download.

REPO="ernie/trinity"
BASE_URL="https://github.com/$REPO/releases/latest/download"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BASEQ3="$SCRIPT_DIR/baseq3"
MISSIONPACK="$SCRIPT_DIR/missionpack"

# Remove checksummed variants (e.g., pak8t.0a1b2c3d.pk3)
# and the current copy, then download fresh.
for dir in "$BASEQ3" "$MISSIONPACK"; do
  if [ -d "$dir" ]; then
    rm -f "$dir"/pak8t.*.pk3 "$dir"/pak8t.pk3
    rm -f "$dir"/pak3t.*.pk3 "$dir"/pak3t.pk3
  fi
done

if [ -d "$BASEQ3" ]; then
  rm -f "$BASEQ3"/zzz-trinity-announcer.*.pk3 "$BASEQ3"/zzz-trinity-announcer.pk3
fi

echo "Downloading latest Trinity pk3s..."
curl -fL -o "$BASEQ3/pak8t.pk3" "$BASE_URL/pak8t.pk3" && echo "  baseq3/pak8t.pk3 OK" || echo "  baseq3/pak8t.pk3 FAILED"
curl -fL -o "$MISSIONPACK/pak3t.pk3" "$BASE_URL/pak3t.pk3" && echo "  missionpack/pak3t.pk3 OK" || echo "  missionpack/pak3t.pk3 FAILED"
curl -fL -o "$BASEQ3/zzz-trinity-announcer.pk3" "$BASE_URL/zzz-trinity-announcer.pk3" && echo "  baseq3/zzz-trinity-announcer.pk3 OK" || echo "  baseq3/zzz-trinity-announcer.pk3 FAILED"

echo "Done."
