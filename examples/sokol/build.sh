#!/bin/bash
set -e
cd "$(dirname "$0")"

echo "Building sokol_clx module..."

# On macOS, sokol_app.h includes Objective-C headers — compile as Objective-C++
case "$(uname -s)" in
    Darwin) EXTRA_FLAGS="-x objective-c++" ;;
    *)      EXTRA_FLAGS="" ;;
esac

g++ -std=c++20 -Os -fvisibility=hidden -ffunction-sections -fdata-sections -I../../include -I./sokol $EXTRA_FLAGS -c sokol_clx.cpp -o sokol_clx.o
case "$(uname -s)" in
    Darwin) strip -x sokol_clx.o 2>/dev/null || true ;;
    *)      strip --strip-unneeded sokol_clx.o 2>/dev/null || true ;;
esac
ar rcs sokol_clx.a sokol_clx.o
rm -f sokol_clx.o
echo "Created sokol_clx.a"
