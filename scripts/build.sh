#!/usr/bin/env bash
# Build Vial module for Move Anything (ARM64)
#
# Uses CMake to build the Vial core engine and plugin wrapper.
# Automatically uses Docker for cross-compilation if needed.
# Set CROSS_PREFIX to skip Docker (e.g., for native ARM builds).
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGE_NAME="move-anything-helm-builder"

# Check if we need Docker
if [ -z "$CROSS_PREFIX" ] && [ ! -f "/.dockerenv" ]; then
    echo "=== Vial Module Build (via Docker) ==="
    echo ""

    # Build Docker image if needed
    if ! docker image inspect "$IMAGE_NAME" &>/dev/null; then
        echo "Building Docker image (first time only)..."
        docker build -t "$IMAGE_NAME" -f "$SCRIPT_DIR/Dockerfile" "$REPO_ROOT"
        echo ""
    fi

    # Run build inside container
    echo "Running build..."
    docker run --rm \
        -v "$REPO_ROOT:/build" \
        -u "$(id -u):$(id -g)" \
        -w /build \
        "$IMAGE_NAME" \
        ./scripts/build.sh

    echo ""
    echo "=== Done ==="
    exit 0
fi

# === Actual build (runs in Docker or with cross-compiler) ===
cd "$REPO_ROOT"

echo "=== Building Vial Module ==="

# Create build directory
mkdir -p build

# Run CMake configure with cross-compilation toolchain
echo "Configuring CMake..."
cmake -B build \
    -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-toolchain.cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -G Ninja \
    2>&1

# Build
echo "Building (this may take a while)..."
cmake --build build --target vial-move-plugin -j$(nproc) 2>&1

# Package
echo "Packaging..."
mkdir -p dist/vial

# Copy files to dist
cat src/module.json > dist/vial/module.json
[ -f src/help.json ] && cat src/help.json > dist/vial/help.json
[ -f LICENSE ] && cat LICENSE > dist/vial/LICENSE
[ -f NOTICE ]  && cat NOTICE  > dist/vial/NOTICE
cat src/ui.js > dist/vial/ui.js
cat build/dsp.so > dist/vial/dsp.so
chmod +x dist/vial/dsp.so

# Copy patches (both vial-data and backward-compat helm-data)
if [ -d "src/dsp/helm/patches" ]; then
    echo "Copying factory patches..."
    mkdir -p dist/vial/vial-data/patches
    cp -r "src/dsp/helm/patches/Factory Presets" dist/vial/vial-data/patches/
    if [ -f "data/Move Organ.helm" ]; then
        mkdir -p "dist/vial/vial-data/patches/Factory Presets/Keys"
        cp "data/Move Organ.helm" "dist/vial/vial-data/patches/Factory Presets/Keys/"
    fi
fi

# Create tarball for release
cd dist
tar -czvf vial-module.tar.gz vial/
cd ..

echo ""
echo "=== Build Complete ==="
echo "Output: dist/vial/"
echo "Tarball: dist/vial-module.tar.gz"
echo ""
echo "To install on Move:"
echo "  ./scripts/install.sh"
