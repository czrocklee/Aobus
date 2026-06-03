#!/usr/bin/env bash
# ============================================================================
# run_integration_test.sh
# Automates validation of individual Aobus Lint checks against their fixtures.
# ============================================================================
set -euo pipefail

# Setup relative paths
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export PROJECT_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
export BUILD_DIR="/tmp/build/debug-clang-tidy"
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
        NodiscardUsageCheckFixture.cpp) echo "aobus-modernize-nodiscard-usage" ;;
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
        UseRangesContainsCheckFixture.cpp) echo "aobus-modernize-use-ranges-contains" ;;
        UseRangesProjectionCheckFixture.cpp) echo "aobus-modernize-use-ranges-projection" ;;
        UseRangesAnyOfCheckFixture.cpp) echo "aobus-modernize-use-ranges-any-of" ;;
        UseEraseIfCheckFixture.cpp) echo "aobus-modernize-use-erase-if" ;;
        UseRangesMinMaxCheckFixture.cpp) echo "aobus-modernize-use-ranges-min-max" ;;
        UseStartsWithCheckFixture.cpp) echo "aobus-modernize-use-starts-with" ;;
        UseStdNumbersCheckFixture.cpp) echo "aobus-modernize-use-std-numbers" ;;
        IncludeConventionCheckFixture.cpp) echo "aobus-include-convention" ;;
        SpdxLicenseHeaderCheckFixture.cpp) echo "aobus-license-header" ;;
        SpdxLicenseHeaderCheckMissingFixture.cpp) echo "aobus-license-header" ;;
        UseCtadCheckFixture.cpp) echo "aobus-modernize-use-ctad" ;;
        PointerNamingConventionFixture.cpp) echo "aobus-readability-pointer-naming-convention" ;;
        *) return 1 ;;
    esac
}

expects_auto_fix() {
    local alias="$1"
    case "$alias" in
        aobus-modernize-braced-initialization | \
            aobus-modernize-concrete-final | \
            aobus-modernize-lambda-params | \
            aobus-modernize-use-erase-if | \
            aobus-modernize-use-ranges-min-max | \
            aobus-modernize-use-starts-with | \
            aobus-modernize-use-std-numbers | \
            aobus-readability-control-block-spacing | \
            aobus-readability-redundant-namespace-qualification | \
            aobus-readability-use-if-init-statement)
            return 0
            ;;
        *)
            return 1
            ;;
    esac
}

export FIXTURE_DIR="$SCRIPT_DIR/fixture"
export VERIFIER="$SCRIPT_DIR/verify_diagnostics.py"
RUN_TMP_ROOT=$(mktemp -d "/tmp/aobus_lint_integration_XXXXXX")
FAILED=1

cleanup() {
    if [[ "${FAILED:-0}" -eq 0 ]]; then
        rm -rf "$RUN_TMP_ROOT"
    fi
}
trap cleanup EXIT

run_diag() {
    local FIXTURE="$1"
    local FNAME=$(basename "$FIXTURE")
    local ALIAS=$(get_check_alias "$FNAME") || return 0

    local TEST_TMP=$(mktemp -d "$RUN_TMP_ROOT/${FNAME}.diag.XXXXXX")
    local ACTUAL="$TEST_TMP/actual.txt"
    local LOG_FILE="$TEST_TMP/run.log"

    if {
        echo "--- Testing: $ALIAS ($FNAME) ---"
        EXTRA_CHECKS="-*,$ALIAS" "$PROJECT_ROOT/script/run-clang-tidy.sh" \
            -p "$BUILD_DIR" \
            "$FIXTURE" > "$ACTUAL" 2>&1 || true

        if ! "$VERIFIER" "$FIXTURE" --check "$ALIAS" --input "$ACTUAL"; then
            echo "ERROR: Diagnostic Verification FAILED for $FNAME"
            return 1
        fi
        echo "DIAG_PASS"
    } > "$LOG_FILE" 2>&1; then
        echo "  [DIAG PASS] $FNAME"
        return 0
    else
        echo "  [DIAG FAIL] $FNAME (See $LOG_FILE)"
        return 1
    fi
}

run_fix() {
    local FIXTURE="$1"
    local FNAME=$(basename "$FIXTURE")
    local ALIAS=$(get_check_alias "$FNAME") || return 0

    local TEST_TMP=$(mktemp -d "$RUN_TMP_ROOT/${FNAME}.fix.XXXXXX")
    local LOG_FILE="$TEST_TMP/run.log"

    if {
        echo "--- Auto-Fix: $ALIAS ($FNAME) ---"
        local FIXED_FILE="$TEST_TMP/$FNAME"
        cp "$FIXTURE" "$FIXED_FILE"
        cp "$FIXTURE_DIR/TestHelpers.h" "$TEST_TMP/TestHelpers.h"

        EXTRA_CHECKS="-*,$ALIAS" "$PROJECT_ROOT/script/run-clang-tidy.sh" \
            -p "$BUILD_DIR" \
            --fix \
            "$FIXED_FILE" > /dev/null 2>&1 || true

        if expects_auto_fix "$ALIAS" && cmp -s "$FIXTURE" "$FIXED_FILE"; then
            echo "ERROR: Auto-Fix did not modify $FNAME"
            return 1
        fi

        if ! g++ -std=c++26 -fsyntax-only -I"${PROJECT_ROOT}/include" -I"${PROJECT_ROOT}/lib" -I"$TEST_TMP" "$FIXED_FILE"; then
            echo "ERROR: Auto-Fix Compilation FAILED for $FNAME"
            return 1
        fi
    } >> "$LOG_FILE" 2>&1; then
        echo "  [FIX PASS] $FNAME"
        rm -rf "$TEST_TMP"
        return 0
    else
        echo "  [FIX FAIL] $FNAME (See $LOG_FILE)"
        return 1
    fi
}

run_apply_conflict_smoke() {
    local TEST_TMP
    TEST_TMP=$(mktemp -d "$RUN_TMP_ROOT/apply-conflict.XXXXXX")
    local SOURCE="$TEST_TMP/sample.cpp"
    local LOG_FILE="$TEST_TMP/run.log"

    if {
        echo "--- Replacement Insert Conflict Smoke ---"
        printf 'int main() {}\n' > "$SOURCE"

        cat > "$TEST_TMP/a.yaml" <<EOF
---
MainSourceFile:  '$SOURCE'
Diagnostics:
  - DiagnosticName:  test-a
    DiagnosticMessage:
      Message:         'insert a'
      FilePath:        '$SOURCE'
      FileOffset:      0
      Replacements:
        - FilePath:        '$SOURCE'
          Offset:          0
          Length:          0
          ReplacementText: '/*a*/'
    Level:           Warning
    BuildDirectory:  '$TEST_TMP'
...
EOF

        cat > "$TEST_TMP/b.yaml" <<EOF
---
MainSourceFile:  '$SOURCE'
Diagnostics:
  - DiagnosticName:  test-b
    DiagnosticMessage:
      Message:         'insert b'
      FilePath:        '$SOURCE'
      FileOffset:      0
      Replacements:
        - FilePath:        '$SOURCE'
          Offset:          0
          Length:          0
          ReplacementText: '/*b*/'
    Level:           Warning
    BuildDirectory:  '$TEST_TMP'
...
EOF

        clang-apply-replacements --ignore-insert-conflict "$TEST_TMP"
        grep -q '/\*a\*/' "$SOURCE"
        grep -q '/\*b\*/' "$SOURCE"
    } > "$LOG_FILE" 2>&1; then
        echo "  [APPLY PASS] insert conflict smoke"
        return 0
    else
        echo "  [APPLY FAIL] insert conflict smoke (See $LOG_FILE)"
        return 1
    fi
}

echo "=== [2] Diagnostic Verification (parallel) ==="
DIAG_FAILED=0
PIDS=()
for FIXTURE in "$FIXTURE_DIR"/*Fixture.cpp; do
    run_diag "$FIXTURE" &
    PIDS+=($!)
done
for PID in "${PIDS[@]}"; do
    if ! wait $PID; then
        DIAG_FAILED=1
    fi
done

echo "=== [3] Auto-Fix Verification (serial) ==="
FIX_FAILED=0
for FIXTURE in "$FIXTURE_DIR"/*Fixture.cpp; do
    if ! run_fix "$FIXTURE"; then
        FIX_FAILED=1
    fi
done

echo "=== [4] Replacement Application Smoke ==="
APPLY_FAILED=0
if ! run_apply_conflict_smoke; then
    APPLY_FAILED=1
fi

FAILED=$((DIAG_FAILED + FIX_FAILED + APPLY_FAILED))

if [[ $FAILED -ne 0 ]]; then
    echo "=== INTEGRATION TESTS FAILED ==="
    # Print only logs produced by this run; /tmp may contain stale failures.
    while IFS= read -r LOG; do
        if [[ -f "$LOG" ]] && grep -q "ERROR" "$LOG"; then
            echo ""
            cat "$LOG"
        fi
    done < <(find "$RUN_TMP_ROOT" -type f -name run.log | sort)
    exit 1
fi

echo "=== ALL INTEGRATION TESTS PASSED SUCCESSFULLY ==="
