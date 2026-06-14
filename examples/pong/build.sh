#!/bin/bash
set -e
cd "$(dirname "$0")"

# Build sokol module if needed
[ -f "../sokol/sokol_clx.a" ] || (cd ../sokol && ./build.sh)

# Build clx if needed
[ -f "../../build/clx" ] || { echo "Building clx..."; (cd ../.. && ./build.sh); }

# Compile pong (--size shrinks the final binary)
../../build/clx pong.lua --size --modules sokol_clx -L../sokol --output pong -lX11 -lGL -lXcursor -lXi

echo "Done. Run ./pong to play."
