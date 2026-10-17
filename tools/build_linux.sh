#!/usr/bin/env bash
# Builds the Linux version and packages it as an AppImage, from WSL (or any Linux box).
#
#   wsl -d Ubuntu -- bash tools/build_linux.sh [--configure] [--no-sync] [--no-appimage] [--out DIR] [--name FILE]
#                                              [--work DIR]
#
# --no-sync builds the copy as it is (after editing it by hand, say to leave out another change in progress).
# --name is the AppImage's file name (default Rush2Recompiled-x86_64.AppImage). --work is the build folder, under
# $HOME unless absolute (tools/release.py builds releases in its own, so they don't resync over the everyday one).
#
# The tree is copied to WORK (default ~/rush2-linux, on the Linux disk: building over /mnt/c is several times slower,
# and the CMake patch step must not touch the Windows checkout's submodules) and built there with clang and Ninja.
# The AppImage lands in --out (default tmp/linux in this tree). Needs: clang lld cmake ninja-build pkg-config
# libsdl2-dev libfreetype-dev libgtk-3-dev libvulkan-dev libpulse-dev libasound2-dev desktop-file-utils file wget libfuse2t64 (Ubuntu 24.04 names).
# RecompiledFuncs/ comes from the Windows side (N64Recomp us.toml) and is copied as is. Notes: docs/linux.md.
set -euo pipefail

SRC="$(cd "$(dirname "$0")/.." && pwd)"
WORK="${WORK:-$HOME/rush2-linux}"
OUT="$SRC/tmp/linux"
NAME="Rush2Recompiled-x86_64.AppImage"
CONFIGURE=0
SYNC=1
APPIMAGE=1
while [ $# -gt 0 ]; do
    case "$1" in
        --configure) CONFIGURE=1 ;;
        --no-sync) SYNC=0 ;;
        --no-appimage) APPIMAGE=0 ;;
        --out) OUT="$2"; shift ;;
        --name) NAME="$2"; shift ;;
        --work) case "$2" in /*) WORK="$2" ;; *) WORK="$HOME/$2" ;; esac; shift ;;
        *) echo "unknown argument $1" >&2; exit 2 ;;
    esac
    shift
done

mkdir -p "$WORK" "$OUT"
OUT="$(cd "$OUT" && pwd)"
if [ "$SYNC" = 1 ]; then
    echo "== Copying $SRC to $WORK/src"
    # A Windows checkout has CRLF files and lib/patches/*.patch are LF: git converts on Windows, plain `git apply` here
    # doesn't. So every file a patch touches is copied LF, and only when its LF text differs from the copy's (which
    # then gets a new time, so Ninja rebuilds it); rsync leaves those files alone, or it would put the CRLF text back
    # each run. Keeping the source's time instead left a stale object when the copy had been edited by hand (--no-sync).
    patched="$WORK/patched_files.txt"
    : > "$patched"
    for patch in "$SRC"/lib/patches/*.patch; do
        lib="$(basename "$patch" .patch)"
        grep -E '^\+\+\+ b/' "$patch" | sed 's|^+++ b/||' | while read -r file; do
            echo "/lib/$lib/$file" >> "$patched"
        done
    done
    # Every file rsync transfers gets the current time: -a keeps the source's, which can be older than an object built
    # from a hand-edited copy, and Ninja would keep that object. Links out of the tree are copied as what they point
    # to: tools/release.py's export links lib to the checkout's, and a link in the copy would have CMake's patch step
    # run git apply in the checkout's submodules.
    rsync -a -i --copy-unsafe-links --delete --exclude=.git --exclude='/build*' --exclude=/tmp --exclude='*.z64' --exclude='*.log' \
        --exclude='/.tools' --exclude-from="$patched" "$SRC/" "$WORK/src/" |
        sed -n 's/^>f[^ ]* //p' | while read -r file; do touch "$WORK/src/$file"; done
    while read -r file; do
        if [ -f "$SRC$file" ]; then
            mkdir -p "$(dirname "$WORK/src$file")"
            sed 's/\r$//' "$SRC$file" > "$WORK/src$file.lf"
            if cmp -s "$WORK/src$file.lf" "$WORK/src$file"; then
                rm "$WORK/src$file.lf"
            else
                mv "$WORK/src$file.lf" "$WORK/src$file"
            fi
        fi
    done < "$patched"
fi

if [ "$CONFIGURE" = 1 ] || [ ! -f "$WORK/build/build.ninja" ]; then
    echo "== Configuring"
    cmake -S "$WORK/src" -B "$WORK/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
        -DRUSH2_GIT_SOURCE="$SRC" # The copy has no .git: version and online build ID come from the real checkout.
fi

echo "== Building"
cmake --build "$WORK/build"

[ "$APPIMAGE" = 1 ] || exit 0

echo "== Packaging"
TOOLS="$WORK/appimage-tools"
mkdir -p "$TOOLS"
for tool in linuxdeploy-x86_64.AppImage appimagetool-x86_64.AppImage; do
    if [ ! -x "$TOOLS/$tool" ]; then
        case "$tool" in
            linuxdeploy*) url="https://github.com/linuxdeploy/linuxdeploy/releases/download/1-alpha-20250213-2/$tool" ;;
            appimagetool*) url="https://github.com/AppImage/appimagetool/releases/download/1.9.0/$tool" ;;
        esac
        wget -q -O "$TOOLS/$tool" "$url"
        chmod +x "$TOOLS/$tool"
    fi
done

APPDIR="$WORK/AppDir"
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin"
cp "$WORK/build/Rush2Recompiled" "$APPDIR/usr/bin/"
cp -r "$WORK/build/assets" "$APPDIR/usr/bin/"
cp "$WORK/build/recompcontrollerdb.txt" "$APPDIR/usr/bin/"
cp "$SRC/linux/Rush2Recompiled.desktop" "$APPDIR/"
cp "$SRC/icons/512.png" "$APPDIR/Rush2Recompiled.png"

# linuxdeploy gathers the shared libraries the game links (SDL2, freetype, GTK for the file dialog...) into the AppDir,
# leaving out the ones every system has (glibc, libGL, Vulkan's loader and the GPU drivers).
export APPIMAGE_EXTRACT_AND_RUN=1
"$TOOLS/linuxdeploy-x86_64.AppImage" --appdir "$APPDIR" \
    --executable "$APPDIR/usr/bin/Rush2Recompiled" \
    --desktop-file "$APPDIR/Rush2Recompiled.desktop" --icon-file "$APPDIR/Rush2Recompiled.png"
ARCH=x86_64 "$TOOLS/appimagetool-x86_64.AppImage" "$APPDIR" "$OUT/$NAME"
echo "== $OUT/$NAME"
