#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export DEVKITPRO=/opt/devkitpro
export DEVKITA64="$DEVKITPRO/devkitA64"
export PATH="$DEVKITPRO/tools/bin:$DEVKITA64/bin:$PATH"

BUILD_DIR="$SCRIPT_DIR/build-switch-wsl"
mkdir -p "$BUILD_DIR"

cd "$SCRIPT_DIR"

echo "=== Configuring with CMake (devkitA64) ==="
cmake -S . -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=cmake/switch-devkitA64.cmake \
    -DWINE_NX_PE_BUILD_DIR="$SCRIPT_DIR/build-wine-arm64-pe" \
    -DWINE_NX_BOX64_DYNAREC=ON \
    -DWINE_NX_BOX64_INTERPRETER=OFF \
    -DCMAKE_BUILD_TYPE=Release \
    "$@"

echo "=== Building wine-nx-runtime-nro ==="
cmake --build "$BUILD_DIR" --target wine-nx-runtime-nro -j$(nproc)

echo "=== Built NRO files: ==="
ls -lh "$BUILD_DIR"/*.nro
