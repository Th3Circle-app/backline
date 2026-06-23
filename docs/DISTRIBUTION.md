# Distribution

How to get Layback Station onto your team's Macs.

## Now (free): ad-hoc .dmg

Run:

```
./package.sh
```

This builds a Release `Layback Station.dmg`. Send it to the team. They:
1. Open the .dmg and drag **Layback Station** to **Applications**.
2. The first launch: **right-click the app → Open** (then "Open" in the dialog). macOS remembers it after that.

The right-click step is needed because the app is only **ad-hoc signed** (no Apple identity). It's safe; it's just Gatekeeper being cautious about an app from an unidentified developer.

## Later (no warnings): Developer ID + notarization

To make the app install with **zero warnings** (required if you sell it, nice for the team), you need an **Apple Developer Program** membership (**$99/yr**). Then:

1. In Xcode/Apple Developer, create a **Developer ID Application** certificate (installs into your Keychain).
2. Sign with it + the hardened runtime:
   ```
   codesign --force --deep --options runtime \
     --sign "Developer ID Application: <Your Name> (TEAMID)" \
     "build-release/LaybackStation_artefacts/Release/Layback Station.app"
   ```
3. Zip and **notarize** with an app-specific password (Apple ID → Sign-In & Security → App-Specific Passwords):
   ```
   xcrun notarytool submit "Layback Station.zip" \
     --apple-id you@example.com --team-id TEAMID --password <app-specific-pw> --wait
   ```
4. **Staple** the ticket so it works offline, then rebuild the .dmg:
   ```
   xcrun stapler staple "build-release/LaybackStation_artefacts/Release/Layback Station.app"
   ```

(We can fold steps 2–4 into `package.sh` once you have the Developer ID cert.)

## Windows

Not built yet (the app is macOS-first via AVFoundation/CoreAudio). A Windows port would swap the video layer to Media Foundation/Direct3D and sign with **Azure Artifact Signing** (~$10/mo, no hardware token).
