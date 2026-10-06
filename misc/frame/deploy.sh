#!/bin/bash
# Install the WSL build and the mod's dist paks into the Frame's Trinity title; -n is a dry run.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ENGINE=$(dirname "$(dirname "$HERE")")
DIST=$(dirname "$ENGINE")/trinity/dist
B=$HOME/src/trinity-engine/build/release-linux-aarch64
TITLE=devkit-game/${FRAME_TITLE:-Trinity}
DRY=
[ "${1:-}" = -n ] && DRY=1
set -a; . "$ENGINE/.env"; set +a
export SSHPASS="$FRAME_PASS"

"$HERE/ssh.sh" test -d "$TITLE" || { echo "no $TITLE on the Frame; install with the Trinity Installer first"; exit 1; }
if [ -z "$DRY" ] && "$HERE/ssh.sh" pgrep -x trinity > /dev/null; then
	echo "Trinity is running on the Frame; quit it first"
	exit 1
fi

cd "$B"
sshpass -e rsync -aLi --chmod=F755 ${DRY:+-n} -e "ssh -o StrictHostKeyChecking=accept-new" \
	trinity trinity.ded trinity_vulkan_aarch64.so trinity_opengl_aarch64.so trinity_opengl2_aarch64.so \
	libopenxr_loader.so.1 baseq3 missionpack "${FRAME_USER:-steamos}@${FRAME_HOST:-frame}:$TITLE/"

for pak in baseq3/pak8t.pk3 missionpack/pak3t.pk3; do
	src=$DIST/$(basename "$pak")
	want=$(md5sum < "$src" | cut -d' ' -f1)
	have=$("$HERE/ssh.sh" "md5sum < $TITLE/$pak" 2> /dev/null | cut -d' ' -f1 || true)
	if [ "$want" = "$have" ]; then
		echo "$pak is current"
		continue
	fi
	echo "sending $pak"
	[ -n "$DRY" ] || "$HERE/ssh.sh" "cat > $TITLE/$pak.tmp && mv $TITLE/$pak.tmp $TITLE/$pak" < "$src"
done
