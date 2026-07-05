#!/bin/bash
set -e
cd "$(dirname "$0")"

# Build sokol module if needed
[ -f "../sokol/sokol_clx.a" ] || (cd ../sokol && ./build.sh)

# Build clx if needed
[ -f "../../build/clx" ] || { echo "Building clx..."; (cd ../.. && ./build.sh); }

# Platform-specific linker flags
case "$(uname -s)" in
    Linux)  PLATFORM_LIBS="-lX11 -lGL -lXcursor -lXi" ;;
    Darwin) PLATFORM_LIBS="-Wl,-framework,Cocoa -Wl,-framework,OpenGL -Wl,-framework,IOKit -Wl,-framework,CoreVideo" ;;
    MINGW*|MSYS*|CYGWIN*) PLATFORM_LIBS="-luser32 -lgdi32 -lopengl32" ;;
    *) echo "Unknown platform: $(uname -s)"; exit 1 ;;
esac

# Compile pong (--size shrinks the final binary)
../../build/clx pong.lua --size --modules sokol_clx -L../sokol --output pong $PLATFORM_LIBS

echo "Done. Run ./pong to play."
