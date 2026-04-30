#!/usr/bin/env bash
#Build script for RockStudio
#Usage :./ build.sh[debug | release | pgo1 | pgo2 | profile][--clean][--tidy][--clang]

set -e

#Default to debug build
BUILD_TYPE="debug"
CLEAN="false"
ENABLE_TIDY="false"
USE_CLANG="false"
ENABLE_ASAN="false"
VERBOSE="false"

show_usage() {
    echo "Usage: $0 [debug|release|pgo1|pgo2|profile] [--clean] [--tidy] [--clang] [--asan] [--verbose]"
    echo "  debug     - Debug build (default, no sanitizers)"
    echo "  release   - Release build (optimized, no sanitizers)"
    echo "  pgo1      - PGO step 1: instrumented build for profile generation"
    echo "  pgo2      - PGO step 2: optimized build using collected profile"
    echo "  profile   - Optimized build with debug symbols and frame pointers (for perf)"
    echo "  --clean   - Clean build directory before building"
    echo "  --tidy    - Enable clang-tidy during the configure/build (implies --clang)"
    echo "  --clang   - Build with clang/clang++ in a dedicated build directory"
    echo "  --asan    - Enable address/undefined sanitizers (Debug only, default: off)"
    echo "  --verbose - Show full build lines"
}

for ARG in "$@";
do
    case "$ARG" in
        debug|release|pgo1|pgo2|profile)
            BUILD_TYPE="$ARG"
            ;;
        clean|--clean)
            CLEAN="true"
            ;;
        --tidy)
            ENABLE_TIDY="true"
            ;;
        --asan)
            ENABLE_ASAN="true"
            ;;
        --clang)
            USE_CLANG="true"
            ;;
        --verbose)
            VERBOSE="true"
            ;;
        -h|--help)
            show_usage
            exit 0
            ;;
        *)
            show_usage
            exit 1
            ;;
    esac
done

#Validate build type
if [[ "$BUILD_TYPE" != "debug" && "$BUILD_TYPE" != "release" && "$BUILD_TYPE" != "pgo1" && "$BUILD_TYPE" != "pgo2" && "$BUILD_TYPE" != "profile" ]]; then
    show_usage
    exit 1
fi

#Convert to CMake preset name
case "$BUILD_TYPE" in
    debug)   PRESET="linux-debug" ;;
    release) PRESET="linux-release" ;;
    pgo1)    PRESET="linux-pgo-profile" ;;
    pgo2)    PRESET="linux-pgo-optimize" ;;
    profile) PRESET="profile" ;;
esac

#Build directory
SOURCE_DIR="$(cd "$(dirname "$0")" && pwd)"
TIDY_SUFFIX=""
COMPILER_SUFFIX=""
ASAN_SUFFIX=""
COMPILER_NAME="gcc"

if [[ "$ENABLE_TIDY" == "true" ]]; then
    USE_CLANG="true"
    TIDY_SUFFIX="-tidy"
fi

if [[ "$USE_CLANG" == "true" ]]; then
    COMPILER_SUFFIX="-clang"
    COMPILER_NAME="clang"
fi

if [[ "$ENABLE_ASAN" == "true" ]]; then
    ASAN_SUFFIX="-asan"
fi

case "$BUILD_TYPE" in
    debug|release|profile)
        BUILD_DIR="/tmp/build/${BUILD_TYPE}${COMPILER_SUFFIX}${TIDY_SUFFIX}${ASAN_SUFFIX}"
        ;;
    pgo1|pgo2)
#PGO generate / use steps must share a build tree so profile data stays available.
        BUILD_DIR="/tmp/build/pgo${COMPILER_SUFFIX}${TIDY_SUFFIX}"
        ;;
esac

#Clean if requested
if [[ "$CLEAN" == "true" ]]; then
    echo "Cleaning build directory ($BUILD_DIR)..."
    rm -rf "$BUILD_DIR"
fi

#Ensure nix - shell is available
if ! command -v nix-shell &> /dev/null; then
    echo "Error: nix-shell is required"
    exit 1
fi

#Configure
echo "Configuring RockStudio with preset '$PRESET' in '$BUILD_DIR'..."
CONFIGURE_COMMAND="cmake -S '$SOURCE_DIR' --preset '$PRESET' -B '$BUILD_DIR'"
BUILD_COMMAND="cmake --build '$BUILD_DIR' --parallel"
TEST_COMMAND="$BUILD_DIR/test/rs_test"
TEST_LINUX_COMMAND="$BUILD_DIR/test/rs_test_linux"

if [[ "$VERBOSE" == "true" ]]; then
    CONFIGURE_COMMAND+=" -DCMAKE_VERBOSE_MAKEFILE=ON"
    BUILD_COMMAND+=" --verbose"
else
    CONFIGURE_COMMAND+=" -DCMAKE_VERBOSE_MAKEFILE=OFF"
fi

if [[ "$USE_CLANG" == "true" ]]; then
    echo "clang enabled for this build."
    CONFIGURE_COMMAND="CC=clang CXX=clang++ $CONFIGURE_COMMAND"
    BUILD_COMMAND="CC=clang CXX=clang++ $BUILD_COMMAND"
    TEST_COMMAND="CC=clang CXX=clang++ $TEST_COMMAND"
fi

if [[ "$ENABLE_TIDY" == "true" ]]; then
    echo "clang-tidy enabled for this build (implies clang toolchain)."
    CONFIGURE_COMMAND+=" -DROCKSTUDIO_ENABLE_CLANG_TIDY=ON"
fi

if [[ "$ENABLE_ASAN" == "true" ]]; then
    echo "ASan/UBSan enabled for this build."
    CONFIGURE_COMMAND+=" -DROCKSTUDIO_ENABLE_ASAN=ON"
fi

nix-shell --run "$CONFIGURE_COMMAND"

#Build
echo "Building RockStudio..."
nix-shell --run "$BUILD_COMMAND"

#Run tests(only for debug and release)
if [[ "$BUILD_TYPE" == "debug" || "$BUILD_TYPE" == "release" ]]; then
    echo "Running tests..."
    nix-shell --run "$TEST_COMMAND && $TEST_LINUX_COMMAND"
fi

#PGO instructions
if [[ "$BUILD_TYPE" == "pgo1" ]]; then
    NEXT_COMMAND="./build.sh pgo2"
    if [[ "$USE_CLANG" == "true" ]]; then
        NEXT_COMMAND+=" --clang"
    fi
    if [[ "$ENABLE_TIDY" == "true" ]]; then
        NEXT_COMMAND+=" --tidy"
    fi

    echo ""
    echo "============================================"
    echo "PGO Step 1 complete."
    echo ""
    echo "Next: Run the app to generate profile data:"
    echo "  cd $BUILD_DIR && ./RockStudio"
    echo "  # Use the app normally, then close it"
    echo ""
    echo "Then run:"
    echo "  $NEXT_COMMAND"
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
echo "  compiler: $COMPILER_NAME"
echo "  clang-tidy: $ENABLE_TIDY"
echo "  asan: $ENABLE_ASAN"
echo "  verbose: $VERBOSE"
echo "  tests: rs_test + rs_test_linux"
