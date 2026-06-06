#!/usr/bin/env bash
set -e
set -o pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

if [[ -z "${IN_NIX_SHELL:-}" ]]; then
    SCRIPT_PATH="$SCRIPT_DIR/$(basename "${BASH_SOURCE[0]}")"
    exec nix-shell "$PROJECT_ROOT/shell.nix" --run "cd $(printf '%q' "$PROJECT_ROOT") && $(printf '%q ' "$SCRIPT_PATH" "$@")"
fi

BUILD_DIR="/tmp/build/debug"
SUITE="all"
TEST_FILTER=""
LIST_ONLY="false"
DO_BUILD="true"

usage() {
    cat <<'EOF'
Usage: run-tests.sh [options] [--] [catch2_filter]

Run Aobus tests using the existing build directory.
Pass any valid Catch2 filter string as the last argument (or after --).
Note: Use quotes around your filter to avoid shell globbing, e.g., "[layout],[model]"

=== Options ===
  -p, --path <dir>      Build directory (default: /tmp/build/debug)
  --suite <suite>       Test suite to run: core, gtk, all (default: all)
  --core                Shortcut for --suite core
  --gtk                 Shortcut for --suite gtk
  -l, --list            List matching tests instead of running them
  -n, --no-build        Skip incremental build before running tests
  -h, --help            Show this help

=== Examples ===
  # Build and run all tests
  ./script/run-tests.sh

  # Run without building (faster if you know binaries are current)
  ./script/run-tests.sh -n

  # Run only GTK tests with specific tags (OR logic)
  ./script/run-tests.sh --gtk "[layout],[presets]"

  # Run only core tests with specific tags (AND logic)
  ./script/run-tests.sh --core "[audio][backend]"

  # List all layout tests
  ./script/run-tests.sh --gtk --list "[layout]"
EOF
    exit 0
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -p|--path) BUILD_DIR="$2"; shift 2 ;;
        --suite) SUITE="$2"; shift 2 ;;
        --core) SUITE="core"; shift ;;
        --gtk) SUITE="gtk"; shift ;;
        -l|--list) LIST_ONLY="true"; shift ;;
        -n|--no-build) DO_BUILD="false"; shift ;;
        -h|--help) usage ;;
        --) shift; TEST_FILTER="$*"; break ;;
        -*) echo "Unknown option: $1"; usage ;;
        *) TEST_FILTER="$*"; break ;;
    esac
done

if [[ "$DO_BUILD" == "true" ]]; then
    if [[ ! -d "$BUILD_DIR" ]]; then
        echo "Error: Build directory $BUILD_DIR does not exist."
        echo "Please run ./build.sh debug first to configure the project."
        exit 1
    fi
    echo "====================================="
    echo "Building tests in $BUILD_DIR..."
    echo "====================================="
    cmake --build "$BUILD_DIR" --parallel --target ao_test ao_test_gtk
fi

run_core() {
    local CORE_TEST_BIN="$BUILD_DIR/test/ao_test"
    if [[ ! -f "$CORE_TEST_BIN" ]]; then
        echo "Error: Core test binary not found at $CORE_TEST_BIN"
        echo "Please build the tests first, e.g., with ./build.sh debug"
        exit 1
    fi

    local CMD=("$CORE_TEST_BIN")
    if [[ "$LIST_ONLY" == "true" ]]; then
        CMD+=(--list-tests --verbosity high)
    fi
    if [[ -n "$TEST_FILTER" ]]; then
        CMD+=("$TEST_FILTER")
    fi

    echo "====================================="
    echo "Running Core Tests"
    echo "CMD: ${CMD[*]}"
    echo "====================================="
    "${CMD[@]}"
}

run_gtk() {
    local GTK_TEST_BIN="$BUILD_DIR/test/ao_test_gtk"
    if [[ ! -f "$GTK_TEST_BIN" ]]; then
        echo "Error: GTK test binary not found at $GTK_TEST_BIN"
        echo "Please build the tests first, e.g., with ./build.sh debug"
        exit 1
    fi

    local CMD=()
    if [[ -n "${DISPLAY:-}" ]] || [[ -n "${WAYLAND_DISPLAY:-}" ]]; then
        CMD=("$GTK_TEST_BIN")
    elif command -v xvfb-run >/dev/null 2>&1; then
        CMD=(xvfb-run -a "$GTK_TEST_BIN")
    else
        echo "Error: GTK tests require either a display (DISPLAY/WAYLAND_DISPLAY) or xvfb-run."
        exit 1
    fi

    if [[ "$LIST_ONLY" == "true" ]]; then
        CMD+=(--list-tests --verbosity high)
    fi
    if [[ -n "$TEST_FILTER" ]]; then
        CMD+=("$TEST_FILTER")
    fi

    echo "====================================="
    echo "Running GTK Tests"
    echo "CMD: ${CMD[*]}"
    echo "====================================="
    "${CMD[@]}"
}

case "$SUITE" in
    core) run_core ;;
    gtk) run_gtk ;;
    all) 
        run_core
        echo ""
        run_gtk 
        ;;
    *)
        echo "Error: Invalid suite '$SUITE'. Allowed values: core, gtk, all."
        exit 1
        ;;
esac
