#!/bin/bash
set -e
cd "$(dirname "$0")"

echo "Building sokol_clx module..."
g++ -std=c++20 -Os -fvisibility=hidden -ffunction-sections -fdata-sections -I../../include -I./sokol -c sokol_clx.cpp -o sokol_clx.o
strip --strip-unneeded sokol_clx.o 2>/dev/null || true
ar rcs sokol_clx.a sokol_clx.o
rm -f sokol_clx.o
echo "Created sokol_clx.a"
