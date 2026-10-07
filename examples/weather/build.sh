#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"

LUASOCKET_VERSION="${LUASOCKET_VERSION:-3.1.0}"
LUASOCKET_URL="https://github.com/lunarmodules/luasocket/archive/refs/tags/v${LUASOCKET_VERSION}.tar.gz"
BUILD=build
SRC="$BUILD/luasocket-${LUASOCKET_VERSION}/src"
CLX_INCLUDE="${CLX_INCLUDE:-../../include}"
CC="${CC:-cc}"
AR="${AR:-ar}"

[ -f "$CLX_INCLUDE/lua.h" ] || { echo "Error: $CLX_INCLUDE/lua.h not found (set CLX_INCLUDE to clx's include directory)."; exit 1; }

# Rebuild clx when sources/headers changed; stale libs linked against fresh
# generated code ABI-mismatch and the result crashes at startup
if [ ! -x ../../build/clx ] || [ -n "$(find ../../src ../../include ../../CMakeLists.txt -newer ../../build/clx -print -quit 2>/dev/null)" ]; then
    echo "Building clx..."
    (cd ../.. && ./build.sh)
fi
CLX="$(cd ../../build && pwd)/clx"

# Smallest-binary flags, probed so the example also works on toolchains without -Oz/-flto=auto.
# Note: -fno-rtti would save ~50 KB but breaks the runtime (indirect call through null); keep RTTI.
CXX_BIN="${CLX_CXX:-${CXX:-c++}}"
SIZE_FLAGS="-Oz -finline-functions -flto=auto"
if ! "$CXX_BIN" $SIZE_FLAGS -x c++ -E -o /dev/null - </dev/null >/dev/null 2>&1; then
    SIZE_FLAGS="-Os -finline-functions"
fi
case "$(uname -s)" in
    Darwin) SIZE_FLAGS="$SIZE_FLAGS -Wl,-exported_symbols_list,/dev/null" ;;
esac

# --- 1. fetch luasocket (pinned release, skipped when already extracted) ---
if [ ! -d "$SRC" ]; then
    mkdir -p "$BUILD"
    TARBALL="$BUILD/luasocket-${LUASOCKET_VERSION}.tar.gz"
    if [ ! -f "$TARBALL" ]; then
        echo "Fetching luasocket ${LUASOCKET_VERSION}..."
        if command -v curl >/dev/null 2>&1; then
            curl -sL -o "$TARBALL" "$LUASOCKET_URL"
        elif command -v wget >/dev/null 2>&1; then
            wget -q -O "$TARBALL" "$LUASOCKET_URL"
        else
            echo "Error: need curl or wget to download luasocket."
            exit 1
        fi
    fi
    tar xzf "$TARBALL" -C "$BUILD"
fi

# --- 2. compile the C cores against clx's Lua C API headers ---
echo "Compiling luasocket C modules..."
mkdir -p "$BUILD/obj"
SOCKET_OBJS=""
for f in luasocket auxiliar buffer compat except inet io options select tcp timeout udp usocket; do
    "$CC" -c -Os -I"$CLX_INCLUDE" -I"$SRC" "$SRC/$f.c" -o "$BUILD/obj/$f.o"
    SOCKET_OBJS="$SOCKET_OBJS $BUILD/obj/$f.o"
done
"$CC" -c -Os -I"$CLX_INCLUDE" -I"$SRC" "$SRC/mime.c" -o "$BUILD/obj/mime.o"

# Archive names match the require names: socket.core -> socket.core.a
"$AR" rcs "$BUILD/socket.core.a" $SOCKET_OBJS
"$AR" rcs "$BUILD/mime.core.a" "$BUILD/obj/mime.o"

# --- 3. stage the Lua side; the directory layout gives the dotted require names ---
mkdir -p "$BUILD/socket"
cp "$SRC/socket.lua" "$SRC/ltn12.lua" "$SRC/mime.lua" "$BUILD/"
cp "$SRC/url.lua" "$SRC/headers.lua" "$SRC/http.lua" "$SRC/tp.lua" "$BUILD/socket/"
cp ../../benchmarks/dkjson.lua "$BUILD/"

# --- 4. compile and link the example ---
echo "Compiling weather..."
cd "$BUILD"
"$CLX" ../weather.lua \
    socket.lua socket/url.lua socket/headers.lua socket/http.lua socket/tp.lua \
    ltn12.lua mime.lua dkjson.lua \
    --modules socket.core,mime.core \
    $SIZE_FLAGS \
    -o weather

echo "Done. Run ./build/weather (or ./build/weather 48.85 2.35 Paris)"
