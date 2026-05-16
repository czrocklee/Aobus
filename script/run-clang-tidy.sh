#!/usr/bin/env bash
set -uo pipefail
# Note: not using -e because background job exit codes propagate through wait

BUILD_DIR="/tmp/build/debug-clang-tidy"
FIX_MODE=false
SUMMARY_MODE=false
OUTPUT_FILE=""
JOBS=$(nproc)

usage() {
    cat <<'EOF'
Usage: run-clang-tidy.sh [options] [<file>...]

Run clang-tidy on Aobus C++ source files using project-wide configs.
Each file is classified as STRICT (lib/app/include) or RELAXED (test/).

=== Scope (choose one) ================================================

  (none)                Changed files — git diff + working tree + staged + untracked
  <file>...             Explicit list of files to check
  --all                 Every .cpp/.h/.hpp under lib/ app/ include/ test/
  --folder <d>          All files under <d> (repeatable: --folder lib --folder test)
  --commit <r>          Changed files since <r> + working tree + untracked

=== Output control ====================================================

  (none)                Full clang-tidy output to stdout
  --summary             Group warnings by check name with line numbers, no full text
  -o <file>             Write full clang-tidy output to <file>

=== Other =============================================================

  --fix                 Apply fixes automatically (use with caution)
  -j <N>                Parallel jobs (default: nproc)
  -p <dir>              Build directory with compile_commands.json
                        (default: /tmp/build/debug-clang-tidy)
  -h, --help            Show this help

=== Examples ==========================================================

  # Check everything
  ./script/run-clang-tidy.sh --all

  # Check a folder with summary
  ./script/run-clang-tidy.sh --folder test/unit/audio --summary

  # Check changes since a commit, write to file
  ./script/run-clang-tidy.sh --commit HEAD~3 -o report.txt

  # Check specific files with auto-fix
  ./script/run-clang-tidy.sh --fix lib/audio/Foo.cpp lib/audio/Foo.h

  # Check production code only
  ./script/run-clang-tidy.sh --folder lib --folder app --folder include --summary

=== Config per mode ===================================================

  STRICT                  All checks active (lib/app/include)
  RELAXED                 Magic numbers, cognitive complexity, identifier length,
                          reinterpret_cast, vararg, C arrays, optional-access
                          are suppressed (test/)
EOF
    exit 0
}

ALL_MODE=false
FOLDER_DIRS=()
COMMIT_REF=""
FILES=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        -p) BUILD_DIR="$2"; shift 2 ;;
        -p*) BUILD_DIR="${1#-p}"; shift ;;
        -j) JOBS="$2"; shift 2 ;;
        -j*) JOBS="${1#-j}"; shift ;;
        --fix) FIX_MODE=true; shift ;;
        --summary) SUMMARY_MODE=true; shift ;;
        -o) OUTPUT_FILE="$2"; shift 2 ;;
        --all) ALL_MODE=true; shift ;;
        --folder) FOLDER_DIRS+=("$2"); shift 2 ;;
        --commit) COMMIT_REF="$2"; shift 2 ;;
        -h|--help) usage ;;
        *) FILES+=("$1"); shift ;;
    esac
done

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
COMPILE_DB="$BUILD_DIR/compile_commands.json"
PLUGIN="$BUILD_DIR/lint/libAobusLintPlugin.so"

# --- Ensure prerequisites ---------------------------------------------------
cmake_cache="$BUILD_DIR/CMakeCache.txt"

if [[ ! -f "$COMPILE_DB" ]]; then
    echo "compile_commands.json missing, running cmake configure..."
    if [[ ! -f "$cmake_cache" ]]; then
        nix-shell --run "cmake -S '$PROJECT_ROOT' --preset linux-debug -B '$BUILD_DIR' \
            -DAOBUS_ENABLE_CLANG_TIDY=ON" 2>&1 | tail -5
    else
        nix-shell --run "cmake '$PROJECT_ROOT' -B '$BUILD_DIR' \
            -DAOBUS_ENABLE_CLANG_TIDY=ON" 2>&1 | tail -5
    fi
    echo "Configure done."
fi

if [[ ! -f "$PLUGIN" ]]; then
    echo "AobusLintPlugin missing, building..."
    nix-shell --run "cmake --build '$BUILD_DIR' --target AobusLintPlugin -j$(nproc)" 2>&1 | tail -5
    echo "Plugin built."
fi

# System include paths (NixOS)
ISYSTEM_ARGS=()
while IFS= read -r line; do
    line=$(echo "$line" | xargs)
    [[ -n "$line" && -d "$line" ]] && ISYSTEM_ARGS+=("--extra-arg-before=-isystem${line}")
done < <(clang++ -E -x c++ - -v < /dev/null 2>&1 | grep "^ /nix")

# --- Config ----------------------------------------------------------------
# Each check group is listed on its own line with a rationale comment.
# The final string is comma-separated for clang-tidy's -checks parameter.
STRICT_CHECKS="$(
    c='-*'                                    # start from empty — only enable what we list

    # === enabled check groups ===
    c+=',aobus-*'                             # Aobus custom checks (naming, spacing, etc.)
    c+=',bugprone-*'                          # bug-prone pattern detection
    c+=',performance-*'                       # performance issues
    c+=',cppcoreguidelines-*'                 # C++ Core Guidelines
    c+=',misc-*'                              # miscellaneous useful checks
    c+=',modernize-*'                         # modern C++ usage
    c+=',readability-*'                       # readability improvements
    c+=',portability-*'                       # portability concerns

    # === disabled: false positives or project-preference ===
    c+=',-bugprone-easily-swappable-parameters'         # frequent false positives
    c+=',-cppcoreguidelines-avoid-magic-numbers'        # handled by aobus readability check
    c+=',-cppcoreguidelines-avoid-const-or-ref-data-members'  # common pattern for views
    c+=',-cppcoreguidelines-pro-bounds-pointer-arithmetic'    # common in layout/audio code
    c+=',-misc-no-recursion'                            # recursion is idiomatic in some modules
    c+=',-misc-non-private-member-variables-in-classes' # impl structs use this pattern
    c+=',-modernize-use-trailing-return-type'           # style choice: prefer leading type
    c+=',-modernize-use-nodiscard'                      # not enforced globally
    c+=',-modernize-use-auto'                           # handled by aobus custom check
    c+=',-performance-unnecessary-value-param'          # too noisy for interface types
    c+=',-performance-move-const-arg'                   # conflicts with const-correctness
    c+=',-readability-redundant-member-init'            # explicit init is intentional
    c+=',-readability-convert-member-functions-to-static'  # prefers explicit design intent
    c+=',-portability-avoid-pragma-once'                # project standard is #pragma once
    c+=',-clang-diagnostic-note'                        # always suppressed
    c+=',-clang-diagnostic-*'                           # compiler diagnostics not our domain

    echo "$c"
)"

CONFIG_CHECKS='-*,aobus-*,readability-identifier-length,readability-identifier-naming,readability-magic-numbers,readability-qualified-auto,readability-function-cognitive-complexity'

CONFIG_BASE="
{Checks: '${CONFIG_CHECKS}',
 CheckOptions: [
  {key: 'readability-identifier-length.MinimumVariableNameLength', value: 2},
  {key: 'readability-identifier-length.MinimumParameterNameLength', value: 2},
  {key: 'readability-identifier-length.IgnoredVariableNames', value: '^[_]([^_].*)?\$'},
  {key: 'readability-identifier-length.IgnoredBindingNames', value: '^[_]\$'},
  {key: 'readability-identifier-naming.ClassCase', value: 'CamelCase'},
  {key: 'readability-identifier-naming.StructCase', value: 'CamelCase'},
  {key: 'readability-identifier-naming.EnumCase', value: 'CamelCase'},
  {key: 'readability-identifier-naming.ScopedEnumConstantCase', value: 'CamelCase'},
  {key: 'readability-identifier-naming.ConstexprVariableCase', value: 'CamelCase'},
  {key: 'readability-identifier-naming.ConstexprVariablePrefix', value: 'k'},
  {key: 'readability-identifier-naming.FunctionCase', value: 'camelBack'},
  {key: 'readability-identifier-naming.MethodCase', value: 'camelBack'},
  {key: 'readability-identifier-naming.MethodIgnoredRegexp', value: '^property_.*|^signal_.*|^vfunc_.*|^on_.*'},
  {key: 'readability-identifier-naming.PublicMemberCase', value: 'camelBack'},
  {key: 'readability-identifier-naming.TypeAliasCase', value: 'CamelCase'},
  {key: 'readability-identifier-naming.TypeAliasIgnoredRegexp', value: '^(difference_type|value_type|pointer|reference|iterator_category)\$'},
  {key: 'readability-magic-numbers.IgnorePowersOf2IntegerValues', value: true},
  {key: 'readability-magic-numbers.IgnoredIntegerValues', value: '24;1000;1000U;60;100'},
  {key: 'readability-qualified-auto.AllowedTypes', value: 'std::array<.*>::(const_)?iterator;std::string_view::(const_)?iterator;.*::iterator;.*Iterator'},
  {key: 'readability-function-cognitive-complexity.Threshold', value: 30},
  {key: 'misc-include-cleaner.IgnoreHeaders', value: '.*yaml-cpp.*'}
 ]}"

STRICT_CONFIG=$(echo "$CONFIG_BASE" | tr '\n' ' ' | sed 's/  */ /g')

RELAXED_CHECKS="$(
    c="$STRICT_CHECKS"

    # === additionally disabled for test code ===
    c+=',-bugprone-unchecked-optional-access'             # test code uses .value() liberally
    c+=',-bugprone-unused-return-value'                   # tests often discard return values
    c+=',-cppcoreguidelines-avoid-c-arrays'               # Catch2 API and test data arrays
    c+=',-cppcoreguidelines-avoid-magic-numbers'          # not enforced in tests
    c+=',-cppcoreguidelines-pro-type-reinterpret-cast'    # common pattern in test mocks
    c+=',-cppcoreguidelines-pro-type-vararg'              # debug helpers use printf-style
    c+=',-portability-template-virtual-member-function'   # test fixtures use vfunc_* pattern
    c+=',-readability-function-cognitive-complexity'      # test bodies are inherently complex
    c+=',-readability-identifier-length'                  # test locals (id, v, it) are fine
    c+=',-readability-magic-numbers'                      # not enforced in tests

    echo "$c"
)"
RELAXED_CONFIG="$STRICT_CONFIG"
# ---------------------------------------------------------------------------

classify_file() {
    local f="$1"
    [[ "$f" != /* ]] && f="$PWD/$f"
    f=$(realpath -e "$f" 2>/dev/null || realpath "$f" 2>/dev/null || echo "$f")
    if [[ "$f" == */test/* ]]; then
        echo "RELAXED $f"
    else
        echo "STRICT $f"
    fi
}

run_one() {
    local mode="$1" f="$2" tmp="$3"
    local checks="$STRICT_CHECKS" config="$STRICT_CONFIG"
    local header_filter="${PROJECT_ROOT}/(lib|app|include)/.*"

    if [[ "$mode" == "RELAXED" ]]; then
        checks="$RELAXED_CHECKS"
        config="$RELAXED_CONFIG"
        header_filter="${PROJECT_ROOT}/(test|include)/.*"
    fi

    local extra_args=()
    while IFS= read -r arg; do
        [[ -n "$arg" ]] && extra_args+=("$arg")
    done <<< "$ISYSTEM_ARGS_STR"
    [[ -f "$PLUGIN" ]] && extra_args+=("-load=$PLUGIN")
    $FIX_MODE && extra_args+=("-fix")

    clang-tidy -p "$BUILD_DIR" \
        -checks="$checks" \
        -config="$config" \
        -header-filter="$header_filter" \
        "${extra_args[@]}" \
        "$f" > "$tmp" 2>&1
}

# Serialize array for subshell inheritance
ISYSTEM_ARGS_STR=$(printf '%s\n' "${ISYSTEM_ARGS[@]}")

# --- File discovery ---------------------------------------------------------
cd "$PROJECT_ROOT"

if [[ ${#FILES[@]} -eq 0 ]]; then
    if $ALL_MODE; then
        echo "Checking all .cpp/.h/.hpp files in lib/ app/ include/ test/" >&2
        mapfile -t FILES < <(
            find lib app include test -type f \( -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \) \
                ! -name 'fakeit.hpp' | sort
        )
    elif [[ ${#FOLDER_DIRS[@]} -gt 0 ]]; then
        echo "Checking folders: ${FOLDER_DIRS[*]}" >&2
        mapfile -t FILES < <(
            find "${FOLDER_DIRS[@]}" -type f \( -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \) \
                ! -name 'fakeit.hpp' | sort
        )
    else
        # Auto-detect base branch (prefer origin/ to avoid diffing against self)
        if [[ -n "$COMMIT_REF" ]]; then
            BASE="$COMMIT_REF"
        else
            BASE=""
            for candidate in origin/main origin/master main master; do
                BASE=$(git rev-parse --abbrev-ref "$candidate" 2>/dev/null || echo "")
                [[ -n "$BASE" && "$BASE" != "$(git branch --show-current)" ]] && break
                BASE=""
            done
            [[ -z "$BASE" ]] && BASE="HEAD~1"
        fi
        echo "No files specified — using git diff ${BASE}..HEAD + working tree + staged + untracked" >&2
        mapfile -t FILES < <(
            {
                git diff --name-only "${BASE}..HEAD"
                git diff --name-only
                git diff --name-only --cached
                git ls-files --others --exclude-standard
            } 2>/dev/null | grep -E '\.(cpp|h|hpp)$' | sort -u || true
        )
    fi
    [[ ${#FILES[@]} -eq 0 ]] && { echo "No .cpp/.h/.hpp files found." >&2; exit 0; }
fi

# Classify all files
STRT_FILES=()
RELX_FILES=()
while IFS=' ' read -r mode path; do
    case "$mode" in
        STRICT)  STRT_FILES+=("$path") ;;
        RELAXED) RELX_FILES+=("$path") ;;
    esac
done < <(for f in "${FILES[@]}"; do classify_file "$f"; done)

# Export arrays for subprocesses, then run in parallel
run_batch() {
    local mode="$1"
    shift
    local files=("$@")
    [[ ${#files[@]} -eq 0 ]] && return

    echo "=== clang-tidy [$mode] — ${#files[@]} file(s) ==="

    local tmpdir
    tmpdir=$(mktemp -d /tmp/tidy-XXXXXX)
    local running=0 total=${#files[@]} done=0
    for f in "${files[@]}"; do
        local name="$f"
        name="${name#$PROJECT_ROOT/}"
        local tmp="$tmpdir/${name//\//_}.log"
        run_one "$mode" "$f" "$tmp" &
        ((running++))

        if ((running >= JOBS)); then
            wait -n 2>/dev/null || true
            ((running--))
            ((done++))
            printf "\r  [%d/%d]" $done $total >&2
        fi
    done
    # Drain remaining
    while ((running > 0)); do
        wait -n 2>/dev/null || true
        ((running--))
        ((done++))
        printf "\r  [%d/%d]" $done $total >&2
    done
    printf "\r  [%d/%d]\n" $done $total >&2

    if $SUMMARY_MODE; then
        # Collect per-file warnings grouped by check name with line numbers
        local warn_total=0 file_count=0
        for log in "$tmpdir"/*.log; do
            local n
            n=$(grep -c "warning:" "$log" 2>/dev/null; true)
            [[ $n -eq 0 ]] && continue

            ((file_count++))
            ((warn_total += n))

            local logname
            logname=$(basename "$log" .log | tr '_' '/')

            printf "\n  %s (%d warning(s))\n" "$logname" "$n"

            # Extract check names with line numbers, deduplicate per check+line
            grep -oP '^/[^:]+:\d+:\d+:.*?\[.*?\]' "$log" 2>/dev/null | \
              sed -n 's/^[^:]*:\([0-9]*\):.*\[\([^]]*\)\].*/\2:\1/p' | \
              sort -t: -k1,1 -k2,2n | \
              awk -F: '{
                check=$1; line=$2
                if (check != prev_check) {
                  if (prev_check != "") printf "\n"
                  printf "    [%s] L%s", check, line
                  prev_check=check
                  prev_line=line
                } else if (line != prev_line) {
                  printf ",%s", line
                  prev_line=line
                }
              }
              END { if (prev_check != "") printf "\n" }'
        done

        if [[ $warn_total -eq 0 ]]; then
            echo "  No warnings found."
        else
            echo ""
            echo "  ---"
            echo "  $file_count file(s), $warn_total warning(s) total"
        fi
    elif [[ -n "$OUTPUT_FILE" ]]; then
        # Append full output to file
        for log in "$tmpdir"/*.log; do
            if grep -q "warning:" "$log" 2>/dev/null; then
                cat "$log" >> "$OUTPUT_FILE"
            fi
        done
        local has_warn
        has_warn=$(grep -rl "warning:" "$tmpdir" 2>/dev/null | wc -l)
        if [[ $has_warn -eq 0 ]]; then
            echo "  No warnings found."
        else
            echo "  Output written to $OUTPUT_FILE"
        fi
    else
        # Print warnings from all logs to stdout
        local count=0
        for log in "$tmpdir"/*.log; do
            if grep -q "warning:" "$log" 2>/dev/null; then
                cat "$log"
                ((count++))
            fi
        done

        if [[ $count -eq 0 ]]; then
            echo "  No warnings found."
        fi
    fi

    rm -rf "$tmpdir"
}

run_batch STRICT  "${STRT_FILES[@]}"
run_batch RELAXED "${RELX_FILES[@]}"
