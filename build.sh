#!/usr/bin/env bash
# Build script for RockStudio
# Usage: ./build.sh [debug|release] [clean]

set -e

# Default to debug build
BUILD_TYPE="${1:-debug}"
CLEAN="${2:-}"

# Validate build type
if [[ "$BUILD_TYPE" != "debug" && "$BUILD_TYPE" != "release" ]]; then
    echo "Usage: $0 [debug|release] [clean]"
    echo "  debug   - Debug build (default)"
    echo "  release - Release build"
    echo "  clean   - Clean build directory before building"
    exit 1
fi

# Convert to CMake build type
if [[ "$BUILD_TYPE" == "debug" ]]; then
    CMAKE_BUILD_TYPE="Debug"
else
    CMAKE_BUILD_TYPE="Release"
fi

# Build directory
BUILD_DIR="/tmp/build"

# Clean if requested
if [[ "$CLEAN" == "clean" ]]; then
    echo "Cleaning build directory..."
    rm -rf "$BUILD_DIR"
fi

# Ensure nix-shell is available
if ! command -v nix-shell &> /dev/null; then
    echo "Error: nix-shell is required"
    exit 1
fi

# Configure and build
echo "Building RockStudio ($CMAKE_BUILD_TYPE)..."
nix-shell --run "cmake -S . -B $BUILD_DIR -DCMAKE_BUILD_TYPE=$CMAKE_BUILD_TYPE -DCMAKE_EXPORT_COMPILE_COMMANDS=ON"
nix-shell --run "cmake --build $BUILD_DIR -j$(nproc)"

echo ""
echo "Build complete!"
echo "  Build type: $CMAKE_BUILD_TYPE"
echo "  Build dir:   $BUILD_DIR"
