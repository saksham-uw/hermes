#!/bin/bash
# Run on macOS with Apple's command line tools.
set -euo pipefail
cd "$(dirname "$0")/.."
version=${1:?usage: scripts/build-release.sh X.Y.Z}
[[ "$version" =~ ^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$ ]] || { echo 'Expected X.Y.Z' >&2; exit 1; }
sparkle=$(scripts/sparkle.sh)
out="$PWD/build/releases/$version"
mkdir -p "$out"
work=$(mktemp -d "$out/work.XXXXXX")
trap 'rm -rf "$work"' EXIT
bundle="$work/HERMES.app"
contents="$bundle/Contents"
mkdir -p "$contents"/{MacOS,Resources,Frameworks}
ditto "$sparkle/Sparkle.framework" "$contents/Frameworks/Sparkle.framework"
cp "$sparkle/LICENSE" "$contents/Resources/Sparkle-LICENSE"
cp -R app/Fonts "$contents/Resources/"
cp ascii-font.txt "$contents/Resources/"
cp app/HERMES.icns "$contents/Resources/"
public_key=$(cat app/update-public-key.txt)
cat > "$contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0"><dict>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleIdentifier</key><string>com.sakshamuw.hermes</string>
<key>CFBundleExecutable</key><string>HERMES</string>
<key>CFBundleIconFile</key><string>HERMES</string>
<key>CFBundleName</key><string>HERMES</string>
<key>CFBundleShortVersionString</key><string>$version</string>
<key>CFBundleVersion</key><string>$version</string>
<key>LSMinimumSystemVersion</key><string>13.0</string>
<key>NSHighResolutionCapable</key><true/>
<key>SUFeedURL</key><string>https://hermes-mac-saksham.vercel.app/downloads/appcast.xml</string>
<key>SUPublicEDKey</key><string>$public_key</string>
<key>SUEnableAutomaticChecks</key><true/>
<key>SUScheduledCheckInterval</key><integer>3600</integer>
<key>SUVerifyUpdateBeforeExtraction</key><true/>
<key>SURequireSignedFeed</key><true/>
</dict></plist>
PLIST
for arch in arm64 x86_64; do
  xcrun swiftc -O -target "$arch-apple-macos13.0" -F "$sparkle" -framework Sparkle \
    -Xlinker -rpath -Xlinker @executable_path/../Frameworks app/*.swift -o "$work/HERMES-$arch"
done
lipo -create "$work/HERMES-arm64" "$work/HERMES-x86_64" -output "$contents/MacOS/HERMES"
codesign --force --sign - "$bundle"
codesign --verify --deep --strict "$bundle"
ditto -c -k --sequesterRsrc --keepParent "$bundle" "$work/HERMES.zip"
sign() {
  if [[ -n "${SPARKLE_PRIVATE_KEY:-}" ]]; then
    printf '%s' "$SPARKLE_PRIVATE_KEY" | "$sparkle/bin/sign_update" --ed-key-file - "$@"
  else
    "$sparkle/bin/sign_update" --account hermes-saksham-uw "$@"
  fi
}
signature=$(sign -p "$work/HERMES.zip")
bytes=$(stat -f %z "$work/HERMES.zip")
# Verify against the public key shipped in the app, including on CI.
swift scripts/verify.swift "$work/HERMES.zip" "$public_key" "$signature"
cat > "$work/appcast.xml" <<XML
<?xml version="1.0" encoding="utf-8"?>
<rss version="2.0" xmlns:sparkle="http://www.andymatuschak.org/xml-namespaces/sparkle"><channel>
<title>HERMES</title>
<item><title>HERMES $version</title>
<sparkle:version>$version</sparkle:version>
<sparkle:shortVersionString>$version</sparkle:shortVersionString>
<sparkle:minimumSystemVersion>13.0.0</sparkle:minimumSystemVersion>
<enclosure url="https://github.com/saksham-uw/hermes/releases/download/v$version/HERMES.zip" length="$bytes" type="application/octet-stream" sparkle:edSignature="$signature"/>
</item></channel></rss>
XML
sign "$work/appcast.xml"
sign --verify "$work/appcast.xml"
mv "$work/HERMES.zip" "$work/appcast.xml" "$out/"
echo "Release ready in $out"
