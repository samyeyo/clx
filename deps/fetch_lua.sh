#!/usr/bin/env bash
# ─────────────────────────────────────────────
# │ fetch_lua.sh · vendor Lua 5.5 sources    │
# ─────────────────────────────────────────────
#
# Downloads the official Lua 5.5 source tarball and unpacks it under
# deps/lua-5.5/ so clx can build the optional Lua VM bridge.
#
# Usage:
#   ./deps/fetch_lua.sh
#   ./deps/fetch_lua.sh /path/to/lua-5.5.0.tar.gz
#
# After running this, build Lua once to produce deps/lua-5.5/src/liblua.a:
#
#   cd deps/lua-5.5/src && make linux           # (or macos, mingw, ...)

set -euo pipefail

LUA_VERSION="${LUA_VERSION:-5.5.0}"
DEPS_DIR="${DEPS_DIR:-$(cd "$(dirname "$0")/.." && pwd)}"
CANONICAL_DIR="${DEPS_DIR}/lua-5.5"
TARGET_DIR="${CANONICAL_DIR}/src"

if [[ -f "${TARGET_DIR}/lua.h" ]]; then
    echo "Lua ${LUA_VERSION} already present at ${TARGET_DIR}, nothing to do."
    exit 0
fi

TARBALL="${1:-}"
if [[ -z "${TARBALL}" ]]; then
    TARBALL="/tmp/lua-${LUA_VERSION}.tar.gz"
    echo "Downloading Lua ${LUA_VERSION} from https://www.lua.org/ftp/lua-${LUA_VERSION}.tar.gz ..."
    if ! curl -fsSL "https://www.lua.org/ftp/lua-${LUA_VERSION}.tar.gz" -o "${TARBALL}"; then
        echo "Download failed. Please fetch the tarball manually and rerun:" >&2
        echo "  ./deps/fetch_lua.sh /path/to/lua-${LUA_VERSION}.tar.gz" >&2
        exit 1
    fi
fi

echo "Unpacking ${TARBALL} -> ${DEPS_DIR}/"
mkdir -p "${DEPS_DIR}"
tar -xzf "${TARBALL}" -C "${DEPS_DIR}"
EXTRACTED_DIR="${DEPS_DIR}/lua-${LUA_VERSION}"
if [[ ! -f "${TARGET_DIR}/lua.h" && "${EXTRACTED_DIR}" != "${CANONICAL_DIR}" && -d "${EXTRACTED_DIR}" ]]; then
    if [[ -e "${CANONICAL_DIR}" ]]; then
        echo "Error: both ${EXTRACTED_DIR} and ${CANONICAL_DIR} exist." >&2
        exit 1
    fi
    mv "${EXTRACTED_DIR}" "${CANONICAL_DIR}"
fi
if [[ ! -f "${TARGET_DIR}/lua.h" ]]; then
    echo "Warning: the archive did not produce the expected layout:" >&2
    echo "         ${TARGET_DIR}/lua.h" >&2
    exit 1
fi

echo
echo "Lua ${LUA_VERSION} sources ready at: ${TARGET_DIR}"
echo "Configure and build clx to use this external Lua source tree."
echo "The --dynamic bridge still requires libclx_lua.a built from src/runtime/vm/lua."
