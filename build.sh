#!/bin/sh

BUILD_TYPE="Release"
DO_INSTALL=false

for arg in "$@"; do
    case $arg in
        clean)
            sudo rm /usr/local/bin/clx /usr/local/include/clx.h /usr/local/include/clx_runtime.h /usr/local/include/clx_simd.h /usr/local/lib/libclx.a /usr/local/lib/libclx_size.a /usr/local/lib/libclx_lua.a /usr/local/lib/libclx.so 2>/dev/null || true
            rm -rf build
            exit 0
            ;;
        uninstall)
            if [ ! -f "build/install_manifest.txt" ]; then
                echo "Cannot find build/install_manifest.txt. Is the project installed?"
                exit 1
            fi
            echo "Uninstalling..."
    if [ -w /usr/local/bin ] 2>/dev/null || [ "$OSTYPE" = "msys" ] || [ "$OSTYPE" = "win32" ]; then
                xargs rm -v < build/install_manifest.txt
            else
                xargs sudo rm -v < build/install_manifest.txt
            fi
            echo "Uninstallation complete."
            exit 0
            ;;
        debug)
            BUILD_TYPE="Debug"
            ;;
        install)
            DO_INSTALL=true
            ;;
    esac
done

ARCH_ARGS=""
# Forward any CLX_* environment variables (CLX_ARCH, etc.) to CMake
for var in $(env | cut -d= -f1 | grep '^CLX_'); do
    eval val=\$$var
    if [ -n "$val" ]; then
        ARCH_ARGS="$ARCH_ARGS -D$var=$val"
    fi
done

cmake -S . -B build -DCMAKE_BUILD_TYPE=$BUILD_TYPE $ARCH_ARGS
cmake --build build --config $BUILD_TYPE

if [ "$DO_INSTALL" = true ]; then
    if [ -w /usr/bin ] 2>/dev/null || [ "$OSTYPE" = "msys" ] || [ "$OSTYPE" = "win32" ]; then
        cmake --install build --config $BUILD_TYPE
    else
        sudo cmake --install build --config $BUILD_TYPE
    fi
fi