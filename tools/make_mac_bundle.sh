#!/usr/bin/env bash
# Wrap a Mach-O muhammad.clap dylib into a macOS .clap bundle.
# usage: tools/make_mac_bundle.sh build/muhammad.clap dist/muhammad.clap
set -euo pipefail
SRC=${1:?source dylib}; DST=${2:?target bundle}
VER=$(sed -n 's/set(P5_VERSION "\(.*\)")/\1/p' "$(dirname "$0")/../CMakeLists.txt" | head -1)
rm -rf "$DST"
mkdir -p "$DST/Contents/MacOS"
cp "$SRC" "$DST/Contents/MacOS/muhammad"
cat > "$DST/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleExecutable</key><string>muhammad</string>
  <key>CFBundleIdentifier</key><string>muhammad.unresolvable</string>
  <key>CFBundleName</key><string>Muhammad</string>
  <key>CFBundlePackageType</key><string>BNDL</string>   <!-- plugin bundle, not an app (CI hygiene) -->
  <key>CFBundleShortVersionString</key><string>${VER}</string>
  <key>CFBundleVersion</key><string>${VER}</string>
  <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
  <key>LSMinimumSystemVersion</key><string>11.0</string>
  <key>NSHighResolutionCapable</key><true/>
</dict></plist>
PLIST
echo "built $DST (${VER})"
