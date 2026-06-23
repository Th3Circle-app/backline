#!/bin/bash
#
# Build a Release .dmg of Layback Station that the team can install.
#
# This ad-hoc signs the app (NO Apple Developer account needed). Teammates drag it
# to Applications, then RIGHT-CLICK > Open the first time to get past Gatekeeper.
#
# To ship with NO warning at all, enroll in the Apple Developer Program ($99/yr)
# and follow docs/DISTRIBUTION.md to sign with a Developer ID + notarize.
#
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD="$ROOT/build-release"
APPNAME="Layback Station"

echo "==> Configuring + building Release"
cmake -S "$ROOT" -B "$BUILD" -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD" --target LaybackStation --parallel

APP="$BUILD/LaybackStation_artefacts/Release/$APPNAME.app"
[ -d "$APP" ] || { echo "build failed: $APP not found"; exit 1; }

echo "==> Ad-hoc code signing"
codesign --force --deep --sign - "$APP"

echo "==> Building .dmg"
DMG="$ROOT/$APPNAME.dmg"
rm -f "$DMG"
STAGE="$(mktemp -d)"
cp -R "$APP" "$STAGE/"
ln -s /Applications "$STAGE/Applications"
hdiutil create -volname "$APPNAME" -srcfolder "$STAGE" -ov -format UDZO "$DMG"
rm -rf "$STAGE"

echo "==> Done: $DMG"
echo "    Send this .dmg to the team. They drag the app to Applications, then right-click > Open the first time."
