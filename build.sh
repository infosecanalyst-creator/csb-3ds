#!/usr/bin/env bash
# csb-3ds build script. Runs inside the devkitpro/devkitarm Docker image
# (or any machine with devkitARM + libctru; SDL2 for 3DS is built from source automatically).
#
#   From Windows (Docker Desktop), in this folder:   .\build-windows.ps1
#   From Linux/macOS with Docker:                    docker run --rm -v "$PWD":/work -w /work devkitpro/devkitarm bash build.sh
#
# Output: out/csb3ds.3dsx  -> copy to SD:/3ds/csb3ds/
set -euo pipefail

CSB_REPO=https://github.com/zelurker/CSB.git
CSB_COMMIT=ee93fec75e5c2712139fa34a3192f0835da99262   # tested anchor set; bump deliberately
HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="$HERE/build"
export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"

echo "== csb-3ds: checking toolchain"
[ -x "$DEVKITPRO/devkitARM/bin/arm-none-eabi-gcc" ] || { echo "devkitARM not found under $DEVKITPRO"; exit 1; }
need=""
for tool in python3 git cmake; do command -v $tool >/dev/null || need="$need $tool"; done
if [ -n "$need" ]; then
  echo "== installing:$need"
  apt-get update -qq && apt-get install -y -qq $need >/dev/null
fi

# devkitPro's "3ds-sdl" package is SDL 1.2; CSBwin needs SDL2, which supports the 3DS
# upstream but has to be built from source. It is cached in build/sdl2 after the first run.
SDL2_TAG=release-2.30.9
SDL2_PREFIX="$WORK/sdl2"
if [ ! -f "$SDL2_PREFIX/lib/libSDL2.a" ]; then
  echo "== building SDL2 for 3DS ($SDL2_TAG, one-time, a few minutes)"
  mkdir -p "$WORK"
  rm -rf "$WORK/SDL2-src"
  git clone -q --depth 1 --branch "$SDL2_TAG" https://github.com/libsdl-org/SDL.git "$WORK/SDL2-src"
  cmake -S "$WORK/SDL2-src" -B "$WORK/SDL2-src/build" \
        -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/3DS.cmake" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$SDL2_PREFIX" \
        -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF >/dev/null
  cmake --build "$WORK/SDL2-src/build" -j"$(nproc)"
  cmake --install "$WORK/SDL2-src/build" >/dev/null
fi
SDL_CFLAGS="-I$SDL2_PREFIX/include/SDL2 -I$SDL2_PREFIX/include -D_GNU_SOURCE=1"
SDL_LIBS="-L$SDL2_PREFIX/lib -lSDL2"

echo "== fetching CSBwin source ($CSB_COMMIT)"
if [ ! -d "$WORK/CSB/.git" ]; then
  mkdir -p "$WORK"
  git clone -q "$CSB_REPO" "$WORK/CSB"
fi
git -C "$WORK/CSB" fetch -q origin "$CSB_COMMIT" 2>/dev/null || true
git -C "$WORK/CSB" checkout -q -f "$CSB_COMMIT"
git -C "$WORK/CSB" clean -qfdx src

echo "== applying 3DS patch"
python3 "$HERE/patch_csb_3ds.py" "$WORK/CSB/src"
cp "$HERE"/n3ds/* "$WORK/CSB/src/"
# The in-game manual: n3ds/manual.txt compiled in as a C string.
python3 "$HERE/tools/embed_manual.py" "$HERE/n3ds/manual.txt" "$WORK/CSB/src/n3ds_manual.h"
cp "$HERE/Makefile.3ds" "$WORK/CSB/src/"
[ -f "$HERE/icon.png" ] && cp "$HERE/icon.png" "$WORK/CSB/src/"

echo "== building"
make -C "$WORK/CSB/src" -f Makefile.3ds -j"$(nproc)" SDL_CFLAGS="$SDL_CFLAGS" SDL_LIBS="$SDL_LIBS"

mkdir -p "$HERE/out"
cp "$WORK/CSB/src/csb3ds.3dsx" "$HERE/out/"
# csb.cfg: CSBwin's own config with the TrueType message overlay off (the 3DS build draws
# the original message area) and the window at the native 320x200. Keep its [keys] section:
# the 3DS buttons are mapped onto those scancodes.
sed -e 's/^width = .*/width = 320/' -e 's/^height = .*/height = 200/' \
    -e 's/^winx = .*/winx = 320/' -e 's/^winy = .*/winy = 200/' \
    -e 's/^experimental_overlay = .*/experimental_overlay = 0/' \
    "$WORK/CSB/csb.cfg" > "$HERE/out/csb.cfg"
echo
echo "Done: out/csb3ds.3dsx and out/csb.cfg"
echo "Copy both (plus GRAPHICS.DAT and DUNGEON.DAT) to SD:/3ds/csb3ds/"
