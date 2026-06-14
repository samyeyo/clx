#!/bin/bash
set -e
cd "$(dirname "$0")"

[ -f "../sokol/sokol_clx.a" ] || (cd ../sokol && ./build.sh)

[ -f "../../build/clx" ] || { echo "Building clx..."; (cd ../.. && ./build.sh); }

../../build/clx mandelbrot.lua --size --modules sokol_clx -L../sokol --output mandelbrot -lX11 -lGL -lXcursor -lXi

echo "Done. Run ./mandelbrot to explore."
