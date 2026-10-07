#!/bin/bash
set -e
cd "$(dirname "$0")"

# Rebuild clx when sources/headers changed; stale libs linked against fresh
# generated code ABI-mismatch and the result crashes at startup
if [ ! -x ../../build/clx ] || [ -n "$(find ../../src ../../include ../../CMakeLists.txt -newer ../../build/clx -print -quit 2>/dev/null)" ]; then
    echo "Building clx..."
    (cd ../.. && ./build.sh)
fi

# Rebuild sokol module when its sources/headers changed; a sokol_clx.a built
# against older clx.h/clx_runtime.h corrupts memory inside clx::open()
if [ ! -f ../sokol/sokol_clx.a ] || [ -n "$(find ../sokol/sokol_clx.cpp ../../include ../sokol/sokol \( -name '*.h' -o -name '*.cpp' \) -newer ../sokol/sokol_clx.a -print -quit 2>/dev/null)" ]; then
    (cd ../sokol && ./build.sh)
fi

# Platform-specific linker flags
case "$(uname -s)" in
    Linux)  PLATFORM_LIBS="-lX11 -lGL -lXcursor -lXi" ;;
    Darwin) PLATFORM_LIBS="-Wl,-framework,Cocoa -Wl,-framework,OpenGL -Wl,-framework,IOKit -Wl,-framework,CoreVideo" ;;
    MINGW*|MSYS*|CYGWIN*) PLATFORM_LIBS="-luser32 -lgdi32 -lopengl32" ;;
    *) echo "Unknown platform: $(uname -s)"; exit 1 ;;
esac

../../build/clx mandelbrot.lua --size --modules sokol_clx -L../sokol --output mandelbrot $PLATFORM_LIBS

echo "Done. Run ./mandelbrot to explore."
