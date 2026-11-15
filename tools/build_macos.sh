#!/usr/bin/env bash
# Builds the macOS version on a Mac and packages it as an app bundle in a .dmg. Run by .github/workflows/macos.yml on
# GitHub's macOS runners; works the same on any Mac with Xcode (or its command line tools) and Homebrew.
#
#   bash tools/build_macos.sh [--configure] [--no-dmg] [--build DIR] [--out DIR] [--name FILE]
#
# --build is the CMake build folder (default build-macos), --out where the .dmg goes (default tmp/macos), --name its
# file name (default Rush2Recompiled-macOS.dmg). Needs: brew install cmake ninja sdl2 freetype dylibbundler (ccache is
# used when installed), and Xcode's Metal toolchain (Xcode 26: xcodebuild -downloadComponent MetalToolchain).
# RecompiledFuncs/ and rsp/aspMain.cpp come from the ROM (N64Recomp us.toml, RSPRecomp aspMain.us.toml) and must be in
# the tree first: CI downloads them with tools/mac_ci_sources.py. Notes: docs/macos.md.
#
# The app is signed ad hoc (no Apple Developer ID), so a downloaded copy is quarantined by Gatekeeper: the player
# clears that once with Open Anyway in System Settings > Privacy & Security (docs/macos.md).
set -euo pipefail

SRC="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$SRC/build-macos"
OUT="$SRC/tmp/macos"
NAME="Rush2Recompiled-macOS.dmg"
CONFIGURE=0
DMG=1
while [ $# -gt 0 ]; do
    case "$1" in
        --configure) CONFIGURE=1 ;;
        --no-dmg) DMG=0 ;;
        --build) BUILD="$2"; shift ;;
        --out) OUT="$2"; shift ;;
        --name) NAME="$2"; shift ;;
        *) echo "unknown argument $1" >&2; exit 2 ;;
    esac
    shift
done

if [ ! -d "$SRC/RecompiledFuncs" ] || [ ! -f "$SRC/rsp/aspMain.cpp" ]; then
    echo "RecompiledFuncs/ or rsp/aspMain.cpp is missing: generate them from the ROM first (docs/macos.md)" >&2
    exit 1
fi

mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
BREW="$(brew --prefix)"

if [ "$CONFIGURE" = 1 ] || [ ! -f "$BUILD/build.ninja" ]; then
    echo "== Configuring"
    LAUNCHER=()
    if command -v ccache > /dev/null; then
        LAUNCHER=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
                  -DCMAKE_OBJC_COMPILER_LAUNCHER=ccache -DCMAKE_OBJCXX_COMPILER_LAUNCHER=ccache)
    fi
    cmake -S "$SRC" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH="$BREW" -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 "${LAUNCHER[@]}"
fi

echo "== Building"
cmake --build "$BUILD" -- -k 0 # Keep going: one run reports every compile error.

echo "== Packaging"
APP="$OUT/stage/Rush 2 Recompiled.app"
rm -rf "$OUT/stage"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources" "$APP/Contents/Frameworks"
cp "$BUILD/Rush2Recompiled" "$APP/Contents/MacOS/"
# The game runs from Contents/Resources (src/main.cpp), where the UI assets and controller database are.
cp -R "$BUILD/assets" "$APP/Contents/Resources/"
cp "$BUILD/recompcontrollerdb.txt" "$APP/Contents/Resources/"

ICONSET="$OUT/stage/Rush2Recompiled.iconset"
mkdir -p "$ICONSET"
for size in 16 32 128 256 512; do
    sips -z $size $size "$SRC/icons/512.png" --out "$ICONSET/icon_${size}x${size}.png" > /dev/null
    double=$((size * 2))
    if [ $double -le 512 ]; then
        sips -z $double $double "$SRC/icons/512.png" --out "$ICONSET/icon_${size}x${size}@2x.png" > /dev/null
    fi
done
iconutil -c icns "$ICONSET" -o "$APP/Contents/Resources/Rush2Recompiled.icns"
rm -rf "$ICONSET"

VERSION="$(git -C "$SRC" describe --tags --abbrev=0 2>/dev/null | sed 's/^v//' || true)"
VERSION="${VERSION:-0.1.0}"
cat > "$APP/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key><string>Rush 2 Recompiled</string>
    <key>CFBundleDisplayName</key><string>Rush 2: Recompiled</string>
    <key>CFBundleExecutable</key><string>Rush2Recompiled</string>
    <key>CFBundleIdentifier</key><string>com.rush2recompiled.Rush2Recompiled</string>
    <key>CFBundleIconFile</key><string>Rush2Recompiled</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>$VERSION</string>
    <key>CFBundleVersion</key><string>$VERSION</string>
    <key>LSMinimumSystemVersion</key><string>13.0</string>
    <key>LSApplicationCategoryType</key><string>public.app-category.racing-games</string>
    <key>NSHighResolutionCapable</key><true/>
    <key>GCSupportsControllerUserInteraction</key><true/>
</dict>
</plist>
EOF

# Homebrew's SDL2 and FreeType (and what they load) go inside the bundle, so players don't need Homebrew.
dylibbundler -of -b -cd -x "$APP/Contents/MacOS/Rush2Recompiled" -d "$APP/Contents/Frameworks" \
    -p @executable_path/../Frameworks -s "$BREW/lib"
# Homebrew's SDL2 is sdl2-compat, which loads SDL3 at run time (dylibbundler can't see that): first from
# @loader_path/libSDL3.dylib, beside itself in Frameworks.
cp -L "$BREW/lib/libSDL3.dylib" "$APP/Contents/Frameworks/libSDL3.dylib"
chmod u+w "$APP/Contents/Frameworks/libSDL3.dylib"
install_name_tool -id @executable_path/../Frameworks/libSDL3.dylib "$APP/Contents/Frameworks/libSDL3.dylib"
if otool -L "$APP/Contents/Frameworks/libSDL3.dylib" | grep -q "$BREW"; then
    echo "libSDL3.dylib links other Homebrew libraries; bundle them too" >&2
    exit 1
fi

# Ad hoc signature: Apple Silicon won't run unsigned code. Libraries first, then the bundle.
find "$APP/Contents/Frameworks" -name '*.dylib' -exec codesign --force -s - {} \;
codesign --force -s - "$APP"
codesign --verify --verbose "$APP"

[ "$DMG" = 1 ] || { echo "== $APP"; exit 0; }

ln -s /Applications "$OUT/stage/Applications"
rm -f "$OUT/$NAME"
hdiutil create -volname "Rush 2 Recompiled" -srcfolder "$OUT/stage" -ov -format UDZO "$OUT/$NAME"
echo "== $OUT/$NAME"
