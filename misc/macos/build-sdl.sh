#!/bin/bash
# Stages the universal SDL2 dylib the darwin Makefile links, at the pinned sdl_revision. FORCE=1 rebuilds it.

set -euo pipefail

cd "$(dirname "$0")/../.."

DEST_DIR="code/libsdl/macosx"
DEST_FILE="${DEST_DIR}/libSDL2-2.0.0.dylib"
REVISION=$(sed -n 's/.*"sdl_revision": "\([0-9a-f]*\)".*/\1/p' misc/release-dependencies.json)
# SDL 2.32 supports macOS 10.11 and later
DEPLOYMENT_TARGET="${SDL_MACOSX_DEPLOYMENT_TARGET:-10.11}"

if [ -f "${DEST_FILE}" ] && [ "${FORCE:-0}" != "1" ]; then
	echo "SDL already present at ${DEST_FILE} (FORCE=1 to rebuild)"
	exit 0
fi
if [ -z "${REVISION}" ]; then
	echo "No sdl_revision in misc/release-dependencies.json" >&2
	exit 1
fi

TMPDIR=$(mktemp -d)
trap 'rm -rf "${TMPDIR}"' EXIT

SOURCE=../SDL
if [ ! -d "${SOURCE}/.git" ] || [ "$(git -C "${SOURCE}" rev-parse HEAD)" != "${REVISION}" ]; then
	SOURCE="${TMPDIR}/SDL"
	echo "Fetching SDL ${REVISION}"
	git init -q "${SOURCE}"
	git -C "${SOURCE}" fetch -q --depth 1 https://github.com/libsdl-org/SDL.git "${REVISION}"
	git -C "${SOURCE}" checkout -q FETCH_HEAD
fi

echo "Building SDL ${REVISION} for arm64 and x86_64, macOS ${DEPLOYMENT_TARGET}"
cmake -S "${SOURCE}" -B "${TMPDIR}/build" -DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET="${DEPLOYMENT_TARGET}" \
	-DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF -DSDL2_DISABLE_SDL2MAIN=ON
cmake --build "${TMPDIR}/build" --config Release --parallel "$(sysctl -n hw.logicalcpu)"

mkdir -p "${DEST_DIR}"
cp "${TMPDIR}/build/libSDL2-2.0.0.dylib" "${DEST_FILE}"
# the app bundle carries the dylib beside the executable
install_name_tool -id "@executable_path/libSDL2-2.0.0.dylib" "${DEST_FILE}"

echo "Staged ${DEST_FILE}:"
ls -lh "${DEST_FILE}"
lipo -info "${DEST_FILE}"
otool -D "${DEST_FILE}"
