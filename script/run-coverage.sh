#!/usr/bin/env bash
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

if [[ -z "${IN_NIX_SHELL:-}" ]]; then
    SCRIPT_PATH="$SCRIPT_DIR/$(basename "${BASH_SOURCE[0]}")"
    exec nix-shell "$PROJECT_ROOT/shell.nix" --run "cd $(printf '%q' "$PROJECT_ROOT") && $(printf '%q ' "$SCRIPT_PATH" "$@")"
fi

BUILD_DIR="/tmp/build/coverage"
JOBS=$(nproc)
TEST_FILTER=""

usage() {
    cat <<'EOF'
Usage: run-coverage.sh [options] [test_filter]

Run coverage analysis on Aobus C++ source files.
This configures a dedicated coverage build tree, builds tests, runs them,
and then generates and parses gcov outputs to find uncovered lines.

=== Options ===
  -p <dir>              Build directory (default: /tmp/build/coverage)
  -j <N>                Parallel jobs (default: nproc)
  -h, --help            Show this help

=== Examples ===
  # Run all tests and generate coverage
  ./script/run-coverage.sh

  # Run specific tests and generate coverage for that subset
  ./script/run-coverage.sh "rt::SmartListEvaluator"
EOF
    exit 0
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -p) BUILD_DIR="$2"; shift 2 ;;
        -p*) BUILD_DIR="${1#-p}"; shift ;;
        -j) JOBS="$2"; shift 2 ;;
        -j*) JOBS="${1#-j}"; shift ;;
        -h|--help) usage ;;
        *) TEST_FILTER="$1"; shift ;;
    esac
done

# 1. Configure for coverage if needed
if [[ ! -f "$BUILD_DIR/CMakeCache.txt" ]]; then
    echo "Configuring coverage build in $BUILD_DIR..."
    cmake -S "$PROJECT_ROOT" --preset linux-debug -B "$BUILD_DIR" \
        -DCMAKE_CXX_FLAGS="--coverage" \
        -DCMAKE_EXE_LINKER_FLAGS="--coverage" \
        -DCMAKE_SHARED_LINKER_FLAGS="--coverage"
fi

# 2. Build
echo "Building tests..."
cmake --build "$BUILD_DIR" -j"$JOBS"

# 3. Clear old coverage data to avoid stale results
echo "Clearing old .gcda files..."
find "$BUILD_DIR" -name "*.gcda" -delete

# 4. Run Tests
echo "Running tests..."
if [[ -n "$TEST_FILTER" ]]; then
    "$BUILD_DIR/test/ao_test" "$TEST_FILTER"
else
    "$BUILD_DIR/test/ao_test"
fi

# 5. Generate and Parse Coverage
echo ""
echo "=== Coverage Summary ==="
echo ""

cd "$BUILD_DIR" || exit 1

# Find all gcda files (executed object files)
mapfile -t GCDA_FILES < <(find app lib -name "*.gcda" 2>/dev/null)

if [[ ${#GCDA_FILES[@]} -eq 0 ]]; then
    echo "No coverage data generated. Did the tests run successfully?"
    exit 1
fi

for gcda in "${GCDA_FILES[@]}"; do
    # Run gcov on the .gcda file
    # This generates a .gcov file in the current directory
    gcov_output=$(gcov "$gcda" 2>/dev/null)
    
    # Extract the original source file path and the percentage
    src_file=$(echo "$gcov_output" | grep -oP "(?<=File ').*(?=')" | head -n 1)
    
    # Skip non-project files (e.g., standard library headers)
    if [[ "$src_file" != "$PROJECT_ROOT"* ]]; then
        continue
    fi
    
    percent_line=$(echo "$gcov_output" | grep -oP "(?<=Lines executed:)[0-9.]+" | head -n 1)
    total_lines=$(echo "$gcov_output" | grep -oP "(?<=of )[0-9]+" | head -n 1)
    
    if [[ -z "$percent_line" || -z "$total_lines" ]]; then
        continue
    fi

    # Determine filename
    base_name=$(basename "$src_file")
    gcov_file="${base_name}.gcov"
    
    rel_src_file="${src_file#$PROJECT_ROOT/}"
    
    # Check if missing lines exist
    if [[ -f "$gcov_file" ]]; then
        missing_count=$(grep -c "^ *#####:" "$gcov_file" || true)
        if [[ "$missing_count" -gt 0 ]]; then
            echo -e "\033[33m$rel_src_file: ${percent_line}% ($total_lines lines) -> $missing_count missing lines\033[0m"
            # Print the first few missing lines with context
            grep -C 6 "^ *#####:" "$gcov_file" | head -n 40 | while IFS= read -r line; do
                echo "    $line"
            done
            if [[ "$missing_count" -gt 5 ]]; then
                echo "    ..."
            fi
        else
            echo -e "\033[32m$rel_src_file: ${percent_line}% ($total_lines lines) -> OK\033[0m"
        fi
        rm -f "$gcov_file"
    fi
done

# Cleanup remaining gcov files (std headers etc)
rm -f *.gcov

echo ""
echo "Coverage check completed."
