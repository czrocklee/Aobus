#!/usr/bin/env bash
# ============================================================================
# run_integration_test.sh
# Automates validation of the custom Aobus Lint Plugin against the unified fixture.
# ============================================================================
set -euo pipefail

# Setup relative paths
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
BUILD_DIR="/tmp/build/debug"
PLUGIN="$BUILD_DIR/lint/libAobusLintPlugin.so"

FIXTURE="$SCRIPT_DIR/LintAllCheckFixture.cpp"
EXPECTED="$SCRIPT_DIR/expected_diagnostics.txt"
ACTUAL="$SCRIPT_DIR/actual_diagnostics.txt"
FIXED_FILE="$SCRIPT_DIR/LintAllCheckFixture.fixed.cpp"

echo "=== [1] Building AobusLintPlugin Module ==="
cmake --build "$BUILD_DIR" --target AobusLintPlugin -j$(nproc)

if [[ ! -f "$PLUGIN" ]]; then
    echo "ERROR: Plugin shared library not found at $PLUGIN" >&2
    exit 1
fi

# Fetch Nix-shell system include paths dynamically
ISYSTEM_ARGS=()
while IFS= read -r line; do
    line=$(echo "$line" | xargs)
    [[ -n "$line" && -d "$line" ]] && ISYSTEM_ARGS+=("--extra-arg-before=-isystem${line}")
done < <(clang++ -E -x c++ - -v < /dev/null 2>&1 | grep "^ /nix")

echo "=== [2] Running Custom Linter Checks on Fixture ==="
# Execute clang-tidy using the loaded custom plugin
# Enable only aobus custom checks to isolate integration diagnostics
clang-tidy -p="$BUILD_DIR" \
    -load="$PLUGIN" \
    -checks="-*,aobus-*" \
    -header-filter=".*" \
    "${ISYSTEM_ARGS[@]}" \
    "$FIXTURE" > "$ACTUAL" 2>&1 || true

echo "=== [3] Verifying Linter Diagnostics (Diff Check) ==="
# Clean up path variables in actual output to make diffs deterministic
sed -i "s|${PROJECT_ROOT}/||g" "$ACTUAL"

if diff -u "$EXPECTED" "$ACTUAL"; then
    echo "SUCCESS: Diagnostics matched expected output exactly!"
else
    echo "ERROR: Diagnostics mismatch found!" >&2
    echo "Please inspect differences above." >&2
    exit 1
fi

echo "=== [4] Testing Auto-Fix Capability (-fix) ==="
cp "$FIXTURE" "$FIXED_FILE"
clang-tidy -p="$BUILD_DIR" \
    -load="$PLUGIN" \
    -checks="-*,aobus-*" \
    "${ISYSTEM_ARGS[@]}" \
    -fix \
    "$FIXED_FILE" > /dev/null 2>&1 || true

echo "=== [5] Validating Compilation of Auto-Fixed File ==="
# Attempt compilation of the auto-fixed file to ensure fix-its did not break syntax.
nix-shell --run "g++ -std=c++23 -fsyntax-only -I${PROJECT_ROOT}/include -I${PROJECT_ROOT}/lib $FIXED_FILE"

echo "=== INTEGRATION TEST PASSED SUCCESSFULLY ==="
# Clean up temporary artifacts
rm -f "$ACTUAL" "$FIXED_FILE"
