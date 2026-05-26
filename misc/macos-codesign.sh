#!/usr/bin/env bash
#
# Sign and notarize a macOS .app bundle for Trinity Engine.
#
# Usage:
#   ./misc/macos-codesign.sh <path-to-Trinity.app>
#
# Required environment variables:
#   MACOS_SIGNING_IDENTITY   "Developer ID Application: Your Name (TEAMID)"
#   AC_API_KEY_PATH          Path to an App Store Connect API key (.p8)
#   AC_API_KEY_ID            10-char Key ID from App Store Connect
#   AC_API_ISSUER_ID         Issuer UUID from App Store Connect
#
# Assumes the signing identity is importable from the default keychain search list.
# In CI, set that up first via `security create-keychain` / `security import`.

set -euo pipefail

if [ $# -ne 1 ]; then
    echo "Usage: $0 <path-to-Trinity.app>" >&2
    exit 1
fi

APP="$1"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ENTITLEMENTS="${SCRIPT_DIR}/xcode/trinity/trinity.entitlements"

if [ ! -d "$APP" ]; then
    echo "error: app bundle not found at $APP" >&2
    exit 1
fi
if [ ! -f "$ENTITLEMENTS" ]; then
    echo "error: entitlements file not found at $ENTITLEMENTS" >&2
    exit 1
fi

: "${MACOS_SIGNING_IDENTITY:?must be set}"
: "${AC_API_KEY_PATH:?must be set}"
: "${AC_API_KEY_ID:?must be set}"
: "${AC_API_ISSUER_ID:?must be set}"

echo ">>> Signing $APP"
echo "    identity: $MACOS_SIGNING_IDENTITY"

# Inside-out signing. Order matters: nested code must be signed before the
# outer bundle, so the bundle's signature covers the (signed) children.

# Bundled dylibs (SDL2, OpenAL): no entitlements — they don't need JIT/library
# validation relaxations, and applying app entitlements to libraries can
# cause notarization to reject the bundle.
find "$APP/Contents/MacOS" -name '*.dylib' -print0 | while IFS= read -r -d '' lib; do
    codesign --force --options runtime --timestamp \
        --sign "$MACOS_SIGNING_IDENTITY" "$lib"
done

# trinity.ded (dedicated server): needs the same JIT entitlement as the
# client because it also runs the game VM. Sign with entitlements.
if [ -f "$APP/Contents/MacOS/trinity.ded" ]; then
    codesign --force --options runtime --timestamp \
        --entitlements "$ENTITLEMENTS" \
        --sign "$MACOS_SIGNING_IDENTITY" \
        "$APP/Contents/MacOS/trinity.ded"
fi

# trinity (main client executable).
codesign --force --options runtime --timestamp \
    --entitlements "$ENTITLEMENTS" \
    --sign "$MACOS_SIGNING_IDENTITY" \
    "$APP/Contents/MacOS/trinity"

# The outer bundle. The bundle's entitlements are what the kernel applies
# to the main executable at launch, so they must match.
codesign --force --options runtime --timestamp \
    --entitlements "$ENTITLEMENTS" \
    --sign "$MACOS_SIGNING_IDENTITY" \
    "$APP"

# Verify the resulting signatures are well-formed and rooted in a trusted CA.
codesign --verify --deep --strict --verbose=2 "$APP"

echo ">>> Submitting to Apple notary service"

NOTARIZE_DIR="$(mktemp -d)"
NOTARIZE_ZIP="$NOTARIZE_DIR/notarize.zip"
trap 'rm -rf "$NOTARIZE_DIR"' EXIT

# notarytool wants a flat archive of the .app (or a .dmg / .pkg).
# ditto with --keepParent preserves the Trinity.app directory inside the zip.
ditto -c -k --sequesterRsrc --keepParent "$APP" "$NOTARIZE_ZIP"

# --wait blocks until Apple finishes (usually 1-5 minutes). On failure,
# notarytool exits non-zero; pull the log with `notarytool log <submission-id>`.
xcrun notarytool submit "$NOTARIZE_ZIP" \
    --key "$AC_API_KEY_PATH" \
    --key-id "$AC_API_KEY_ID" \
    --issuer "$AC_API_ISSUER_ID" \
    --wait

# Staple the notarization ticket onto the .app so Gatekeeper accepts it
# offline (without contacting Apple to verify on first launch).
xcrun stapler staple "$APP"
xcrun stapler validate "$APP"

echo ">>> Signed, notarized, stapled: $APP"
