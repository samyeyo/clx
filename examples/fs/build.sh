#!/bin/bash
# build.sh — build the fs example:
#   build/demo      demo.lua compiled by clx, with fs linked in as a native module
set -euo pipefail
cd "$(dirname "$0")"

ROOT="$(cd ../.. && pwd)"
INCLUDE="${CLX_INCLUDE:-$ROOT/include}"
CXX_BIN="${CXX:-c++}"
AR_BIN="${AR:-ar}"
BUILD=build

[ -f "$INCLUDE/clx.hpp" ] || { echo "Error: $INCLUDE/clx.hpp not found (set CLX_INCLUDE to clx's include directory)."; exit 1; }

# Rebuild clx when sources/headers changed: generated code is ABI-coupled to the runtime it links.
# build/clx is not relinked for runtime-only edits, so the stamp build.sh writes marks the last build.
STAMP="$ROOT/build/.clx-built"
if [ ! -x "$ROOT/build/clx" ] || [ ! -f "$STAMP" ] \
    || [ -n "$(find "$ROOT/src" "$ROOT/include" "$ROOT/CMakeLists.txt" -newer "$STAMP" -print -quit 2>/dev/null)" ]; then
    echo "Building clx..."
    (cd "$ROOT" && ./build.sh)
    touch "$STAMP"
fi
CLX="$(cd "$ROOT/build" && pwd)/clx"

mkdir -p "$BUILD"

# The Lua side. `clx --modules fs` scans the archive for luaopen_fs, so the module needs
# to be an archive in the working directory.
echo "Compiling fs.a (native module)..."
"$CXX_BIN" -std=c++20 -Os -I "$INCLUDE" -c fs.cpp -o "$BUILD/fs.o"
"$AR_BIN" rcs "$BUILD/fs.a" "$BUILD/fs.o"

echo "Compiling demo.lua..."
cp demo.lua "$BUILD/"
(cd "$BUILD" && "$CLX" demo.lua --minimal --modules fs,table -o demo)

echo "Done."
echo "  ./$BUILD/demo      (demo.lua with fs linked in)"
