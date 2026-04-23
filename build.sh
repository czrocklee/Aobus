#!/usr/bin/env bash
# Build script for RockStudio
# Usage: ./build.sh [debug|release|pgo1|pgo2] [clean]

set -e

# Default to debug build
BUILD_TYPE="${1:-debug}"
CLEAN="${2:-}"

# Validate build type
if [[ "$BUILD_TYPE" != "debug" && "$BUILD_TYPE" != "release" && "$BUILD_TYPE" != "pgo1" && "$BUILD_TYPE" != "pgo2" && "$BUILD_TYPE" != "profile" ]]; then
    echo "Usage: $0 [debug|release|pgo1|pgo2|profile] [clean]"
    echo "  debug   - Debug build (default, with sanitizers)"
    echo "  release - Release build (optimized, no sanitizers)"
    echo "  pgo1    - PGO step 1: instrumented build for profile generation"
    echo "  pgo2    - PGO step 2: optimized build using collected profile"
    echo "  profile - Optimized build with debug symbols and frame pointers (for perf)"
    echo "  clean   - Clean build directory before building"
    exit 1
fi

# Convert to CMake preset name
case "$BUILD_TYPE" in
    debug)   PRESET="linux-debug" ;;
    release) PRESET="linux-release" ;;
    pgo1)    PRESET="linux-pgo-profile" ;;
    pgo2)    PRESET="linux-pgo-optimize" ;;
    profile) PRESET="profile" ;;
esac

# Build directory
BUILD_DIR="/tmp/build"
PGO_DIR="/tmp/pgo-build"
PROFILE_DIR="/tmp/profile"
SOURCE_DIR="$(cd "$(dirname "$0")" && pwd)"

# Select build directory based on preset
if [[ "$BUILD_TYPE" == pgo* ]]; then
    BUILD_DIR="$PGO_DIR"
elif [[ "$BUILD_TYPE" == "profile" ]]; then
    BUILD_DIR="$PROFILE_DIR"
fi

# Clean if requested
if [[ "$CLEAN" == "clean" ]]; then
    echo "Cleaning build directory ($BUILD_DIR)..."
    rm -rf "$BUILD_DIR"
fi

# Ensure nix-shell is available
if ! command -v nix-shell &> /dev/null; then
    echo "Error: nix-shell is required"
    exit 1
fi

# Configure
echo "Configuring RockStudio with preset '$PRESET'..."
nix-shell --run "cmake --preset $PRESET"

# Build
echo "Building RockStudio..."
nix-shell --run "cmake --build '$BUILD_DIR' --parallel"

# Run tests (only for debug and release)
if [[ "$BUILD_TYPE" == "debug" || "$BUILD_TYPE" == "release" ]]; then
    echo "Running tests..."
    nix-shell --run "$BUILD_DIR/rs_test"
fi

# PGO instructions
if [[ "$BUILD_TYPE" == "pgo1" ]]; then
    echo ""
    echo "============================================"
    echo "PGO Step 1 complete."
    echo ""
    echo "Next: Run the app to generate profile data:"
    echo "  cd $BUILD_DIR && ./RockStudio"
    echo "  # Use the app normally, then close it"
    echo ""
    echo "Then run:"
    echo "  ./build.sh pgo2"
    echo "============================================"
elif [[ "$BUILD_TYPE" == "pgo2" ]]; then
    echo ""
    echo "============================================"
    echo "PGO Step 2 complete."
    echo ""
    echo "Optimized binary: $BUILD_DIR/RockStudio"
    echo "Run: cd $BUILD_DIR && perf record -g -- ./RockStudio"
    echo "============================================"
fi

echo ""
echo "All done!"
echo "  Preset: $PRESET"
echo "  Build dir: $BUILD_DIR"
