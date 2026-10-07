#!/bin/bash
# Copy the engine and mod working trees into WSL and cross-compile the Steam Frame client; extra arguments go to make.
set -euo pipefail
ENGINE=$(cd "$(dirname "$0")/../.." && pwd)
PROJECTS=$(dirname "$ENGINE")
DST=$HOME/src
LOG=$DST/frame-build.log
mkdir -p "$DST"

rsync -a --delete --exclude /build/ --exclude /Makefile.local "$ENGINE/" "$DST/trinity-engine/"
rsync -a --delete --exclude /dist/ --exclude /.git/ "$PROJECTS/trinity/" "$DST/trinity/"

rev=$(sed -n 's/.*"openxr_revision": "\([a-f0-9]*\)".*/\1/p' "$DST/trinity-engine/misc/release-dependencies.json")
if [ ! -d "$DST/OpenXR-SDK/.git" ]; then
	src=$PROJECTS/OpenXR-SDK
	[ -d "$src/.git" ] || src=https://github.com/KhronosGroup/OpenXR-SDK.git
	git clone -q "$src" "$DST/OpenXR-SDK"
fi
git -C "$DST/OpenXR-SDK" fetch -q origin
git -C "$DST/OpenXR-SDK" checkout -q "$rev"

cat > "$DST/trinity-engine/Makefile.local" <<EOF
BUILD_TRINITY_NATIVE_FALLBACK=1
TRINITY_FRAME=1
TRINITY_SOURCE_DIR=../trinity
BUILD_OPENXR_LOADER=1
TRINITY_OPENXR_SOURCE_DIR=$DST/OpenXR-SDK
TRINITY_NATIVE_TOOLCHAIN_FILE=$DST/trinity-engine/misc/trinity-native/linux-arm64.cmake
TRINITY_OPENXR_TOOLCHAIN_FILE=$DST/trinity-engine/misc/trinity-native/linux-arm64.cmake
EOF

cd "$DST/trinity-engine"
# the Windows checkout's CRLF files would all read as modified to git in WSL
version=$(git -c core.autocrlf=true -c core.fileMode=false describe --tags --always --dirty 2>/dev/null || echo unknown)
status=0
PKG_CONFIG_LIBDIR=/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig \
	make -j"$(nproc)" release ARCH=aarch64 CC=aarch64-linux-gnu-gcc HOST_CC=cc \
	STRIP=aarch64-linux-gnu-strip TRINITY_ENGINE_VERSION="$version" "$@" > "$LOG" 2>&1 || status=$?
# make can exit 0 after a compile error in this build
if [ "$status" -ne 0 ] || grep -qE 'error:|\*\*\*' "$LOG"; then
	grep -E 'error:|\*\*\*' "$LOG" || tail -20 "$LOG"
	echo "build failed; full log in $LOG"
	exit 1
fi
echo "built $version in $DST/trinity-engine/build/release-linux-aarch64"
