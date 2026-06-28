#!/bin/bash
#
# Build a Release .dmg of Backline that the team can install.
#
# Two modes:
#   1) Ad-hoc (default, no Apple account):   ./package.sh
#        Teammates drag to Applications, then RIGHT-CLICK > Open the first time.
#   2) Developer ID signed + notarized (no Gatekeeper warning):
#        DEV_ID="Developer ID Application: Your Name (TEAMID)" \
#        NOTARY_PROFILE="LaybackNotary" \
#        ./package.sh
#
#      One-time, create the notary profile (needs an app-specific password from
#      appleid.apple.com):
#        xcrun notarytool store-credentials LaybackNotary \
#          --apple-id you@example.com --team-id TEAMID --password xxxx-xxxx-xxxx-xxxx
#      Find your DEV_ID identity with:  security find-identity -v -p codesigning
#
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD="$ROOT/build-release"
APPNAME="Backline"
ENTITLEMENTS="$ROOT/entitlements.plist"

echo "==> Configuring + building Release"
cmake -S "$ROOT" -B "$BUILD" -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD" --target LaybackStation --parallel

APP="$BUILD/LaybackStation_artefacts/Release/$APPNAME.app"
[ -d "$APP" ] || { echo "build failed: $APP not found"; exit 1; }

xattr -cr "$APP"   # strip extended attributes (resource forks) that break codesign

if [ -n "$DEV_ID" ]; then
    echo "==> Developer ID signing (hardened runtime) as: $DEV_ID"
    # Sign nested helper binaries (e.g. the bundled uv) first, then the app bundle.
    if [ -d "$APP/Contents/Resources/bin" ]; then
        find "$APP/Contents/Resources/bin" -type f -perm +111 \
            -exec codesign --force --options runtime --timestamp --sign "$DEV_ID" {} \;
    fi
    codesign --force --options runtime --timestamp \
        --entitlements "$ENTITLEMENTS" --sign "$DEV_ID" "$APP"
    codesign --verify --deep --strict --verbose=2 "$APP" || { echo "codesign verify failed"; exit 1; }
else
    echo "==> Ad-hoc code signing (no Apple Developer account)"
    codesign --force --deep --sign - "$APP"
fi

echo "==> Building .dmg"
DMG="$ROOT/$APPNAME.dmg"
rm -f "$DMG"
STAGE="$(mktemp -d)"
cp -R "$APP" "$STAGE/"
ln -s /Applications "$STAGE/Applications"
hdiutil create -volname "$APPNAME" -srcfolder "$STAGE" -ov -format UDZO "$DMG"
rm -rf "$STAGE"

if [ -n "$DEV_ID" ] && [ -n "$NOTARY_PROFILE" ]; then
    echo "==> Notarizing (this can take a few minutes)"
    xcrun notarytool submit "$DMG" --keychain-profile "$NOTARY_PROFILE" --wait
    echo "==> Stapling the notarization ticket"
    xcrun stapler staple "$DMG"
    echo "==> Notarized + stapled: $DMG  (installs with no Gatekeeper warning)"
else
    echo "==> Done (ad-hoc): $DMG"
    echo "    Team drags the app to Applications, then right-click > Open the first time."
fi
