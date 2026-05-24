#!/usr/bin/env bash
# ============================================================================
# run_integration_test.sh
# Automates validation of individual Aobus Lint checks against their fixtures.
# ============================================================================
set -euo pipefail

# Setup relative paths
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
BUILD_DIR="/tmp/build/debug-clang-tidy"
PLUGIN="$BUILD_DIR/lint/libAobusLintPlugin.so"

echo "=== [1] Building AobusLintPlugin Module ==="
cmake --build "$BUILD_DIR" --target AobusLintPlugin -j$(nproc)

if [[ ! -f "$PLUGIN" ]]; then
    echo "ERROR: Plugin shared library not found at $PLUGIN" >&2
    exit 1
fi

get_check_alias() {
    local fixture_name="$1"
    case "$fixture_name" in
        CApiGlobalQualificationCheckFixture.cpp) echo "aobus-readability-c-api-global-qualification" ;;
        ConcreteFinalCheckFixture.cpp) echo "aobus-modernize-concrete-final" ;;
        ControlBlockSpacingCheckFixture.cpp) echo "aobus-readability-control-block-spacing" ;;
        ForbidNodiscardCheckFixture.cpp) echo "aobus-modernize-forbid-nodiscard" ;;
        ForbidTrailingReturnCheckFixture.cpp) echo "aobus-modernize-forbid-trailing-return" ;;
        IdentifierNamingExtensionsCheckFixture.cpp) echo "aobus-readability-identifier-naming-extensions" ;;
        LambdaParamsCheckFixture.cpp) echo "aobus-modernize-lambda-params" ;;
        LocalInitializationStyleCheckFixture.cpp) echo "aobus-modernize-local-initialization-style" ;;
        BracedInitializationCheckFixture.cpp) echo "aobus-modernize-braced-initialization" ;;
        MemberOrderCheckFixture.cpp) echo "aobus-readability-member-order" ;;
        OptionalNamingAndUsageCheckFixture.cpp) echo "aobus-readability-optional-naming-and-usage" ;;
        RedundantNamespaceQualificationCheckFixture.cpp) echo "aobus-readability-redundant-namespace-qualification" ;;
        StdCLibraryQualificationCheckFixture.cpp) echo "aobus-readability-std-c-library-qualification" ;;
        ThreadingPolicyCheckFixture.cpp) echo "aobus-threading-policy" ;;
        UnusedSuppressionStyleCheckFixture.cpp) echo "aobus-readability-unused-suppression-style" ;;
        UseIfInitStatementCheckFixture.cpp) echo "aobus-readability-use-if-init-statement" ;;
        *) return 1 ;;
    esac
}

FIXTURE_DIR="$SCRIPT_DIR/fixture"
VERIFIER="$SCRIPT_DIR/verify_diagnostics.py"

echo "=== [2] Running Per-Check Integration Tests ==="

for FIXTURE in "$FIXTURE_DIR"/*Fixture.cpp; do
    FNAME=$(basename "$FIXTURE")
    ALIAS=$(get_check_alias "$FNAME") || continue

    echo "--- Testing: $ALIAS ($FNAME) ---"

    ACTUAL="/tmp/actual_$FNAME.txt"

    EXTRA_CHECKS="-*,$ALIAS" "$PROJECT_ROOT/script/run-clang-tidy.sh" \
        -p "$BUILD_DIR" \
        "$FIXTURE" > "$ACTUAL" 2>&1 || true

    "$VERIFIER" "$FIXTURE" --check "$ALIAS" --input "$ACTUAL"

    echo "  [Diagnostic Verification: PASSED]"

    # Testing Auto-Fix Capability
    FIXED_FILE="/tmp/fixed_$FNAME"
    cp "$FIXTURE" "$FIXED_FILE"

    cp "$FIXTURE_DIR/TestHelpers.h" "/tmp/TestHelpers.h"

    EXTRA_CHECKS="-*,$ALIAS" "$PROJECT_ROOT/script/run-clang-tidy.sh" \
        -p "$BUILD_DIR" \
        --fix \
        "$FIXED_FILE" > /dev/null 2>&1 || true

    nix-shell --run "g++ -std=c++23 -fsyntax-only -I${PROJECT_ROOT}/include -I${PROJECT_ROOT}/lib -I/tmp $FIXED_FILE"

    echo "  [Auto-Fix & Compilation: PASSED]"

    rm -f "$ACTUAL" "$FIXED_FILE" "/tmp/TestHelpers.h"
done

echo "=== ALL INTEGRATION TESTS PASSED SUCCESSFULLY ==="
