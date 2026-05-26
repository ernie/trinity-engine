#!/bin/bash

cd `dirname $0`
if [ ! -f Makefile ]; then
	echo "This script must be run from the trinity build directory"
	exit 1
fi

# Build a Universal 2 binary (Apple's term for an x86_64 + aarch64 binary).
#
# Pass 'notarize' as the first argument to also sign and notarize the
# resulting Trinity.app via misc/macos-codesign.sh. That requires the
# following environment variables to be set:
#
#   MACOS_SIGNING_IDENTITY   "Developer ID Application: Your Name (TEAMID)"
#   AC_API_KEY_PATH          path to an App Store Connect API key (.p8)
#   AC_API_KEY_ID            10-char Key ID from App Store Connect
#   AC_API_ISSUER_ID         Issuer UUID from App Store Connect
#
# The signing identity must already be present in a keychain on the
# default search list.

if [ "$1" = "notarize" ]; then
	: "${MACOS_SIGNING_IDENTITY:?must be set to use 'notarize' (e.g. 'Developer ID Application: Your Name (TEAMID)')}"
	: "${AC_API_KEY_PATH:?must be set to a path to your App Store Connect .p8 key}"
	: "${AC_API_KEY_ID:?must be set to your App Store Connect key ID}"
	: "${AC_API_ISSUER_ID:?must be set to your App Store Connect issuer UUID}"
else
	echo "Run with a 'notarize' argument to also sign and notarize the bundle."
fi

echo "Building x86_64 + aarch64 universal2 client/server..."
echo

# For parallel make on multicore boxes.
SYSCTL_PATH=`command -v sysctl 2> /dev/null`
if [ -n "$SYSCTL_PATH" ]; then
	NCPU=`sysctl -n hw.ncpu`
else
	# osxcross on linux
	NCPU=`nproc`
fi

(PLATFORM=darwin ARCH=x86_64 make -j$NCPU) || exit 1
echo; echo
(PLATFORM=darwin ARCH=aarch64 make -j$NCPU) || exit 1
echo

# Build the universal2 app bundle (lipo merges per-arch binaries).
if [ -d build/release-darwin-universal2 ]; then
	rm -r build/release-darwin-universal2
fi
"./make-macosx-app.sh" release || exit 1

if [ "$1" = "notarize" ]; then
	./misc/macos-codesign.sh build/release-darwin-universal2/Trinity.app
fi
