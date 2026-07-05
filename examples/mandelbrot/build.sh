#!/bin/bash
set -e
cd "$(dirname "$0")"

[ -f "../sokol/sokol_clx.a" ] || (cd ../sokol && ./build.sh)

[ -f "../../build/clx" ] || { echo "Building clx..."; (cd ../.. && ./build.sh); }

# Platform-specific linker flags
case "$(uname -s)" in
    Linux)  PLATFORM_LIBS="-lX11 -lGL -lXcursor -lXi" ;;
    Darwin) PLATFORM_LIBS="-Wl,-framework,Cocoa -Wl,-framework,OpenGL -Wl,-framework,IOKit -Wl,-framework,CoreVideo" ;;
    MINGW*|MSYS*|CYGWIN*) PLATFORM_LIBS="-luser32 -lgdi32 -lopengl32" ;;
    *) echo "Unknown platform: $(uname -s)"; exit 1 ;;
esac

../../build/clx mandelbrot.lua --size --modules sokol_clx -L../sokol --output mandelbrot $PLATFORM_LIBS

echo "Done. Run ./mandelbrot to explore."
