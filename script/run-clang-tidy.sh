#!/usr/bin/env bash
set -uo pipefail
# Note: not using -e because background job exit codes propagate through wait

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

if [[ -z "${IN_NIX_SHELL:-}" ]]; then
    SCRIPT_PATH="$SCRIPT_DIR/$(basename "${BASH_SOURCE[0]}")"
    exec nix-shell "$PROJECT_ROOT/shell.nix" --run "cd $(printf '%q' "$PROJECT_ROOT") && $(printf '%q ' "$SCRIPT_PATH" "$@")"
fi

BUILD_DIR="/tmp/build/debug-clang-tidy"
FIX_MODE=false
DEBUG_MODE=false
OUTPUT_FILE=""
JOBS=$(nproc)

usage() {
    cat <<'EOF'
Usage: run-clang-tidy.sh [options] [<file>...]

Run clang-tidy on Aobus C++ source files using project-wide configs.
Each file is classified as STRICT (lib/app/include) or RELAXED (test/).

=== Scope (choose one) ================================================

  (none)                Changed files — local main..HEAD + working tree + staged + untracked
  <file>...             Explicit list of files to check
  --all                 Every .cpp/.h/.hpp under lib/ app/ include/ test/
  --folder <d>          All files under <d> (repeatable: --folder lib --folder test)
  --commit <r>          Changed files since <r> + working tree + untracked

=== Output control ====================================================

  (none)                Full clang-tidy output to stdout
  -o <file>             Write full clang-tidy output to <file>

=== Other =============================================================

  --fix                 Apply fixes automatically (use with caution)
  --debug               Show debug info (config, system includes)
  -j <N>                Parallel jobs (default: nproc)
  -p <dir>              Build directory with compile_commands.json
                        (default: /tmp/build/debug-clang-tidy)
  -h, --help            Show this help

=== Examples ==========================================================

  # Check everything
  ./script/run-clang-tidy.sh --all

  # Check a folder
  ./script/run-clang-tidy.sh --folder test/unit/audio

  # Check changes since a commit, write to file
  ./script/run-clang-tidy.sh --commit HEAD~3 -o report.txt

  # Check specific files with auto-fix
  ./script/run-clang-tidy.sh --fix lib/audio/Foo.cpp lib/audio/Foo.h

  # Check production code only
  ./script/run-clang-tidy.sh --folder lib --folder app --folder include

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
        --debug) DEBUG_MODE=true; shift ;;
        -o) OUTPUT_FILE="$2"; shift 2 ;;
        --all) ALL_MODE=true; shift ;;
        --folder) FOLDER_DIRS+=("$2"); shift 2 ;;
        --commit) COMMIT_REF="$2"; shift 2 ;;
        -h|--help) usage ;;
        *) FILES+=("$1"); shift ;;
    esac
done

# Truncate output file if specified to start with a fresh slate for this run.
# Since individual runs of STRICT and RELAXED batches will append to it,
# we only truncate it once at the very start of the script.
if [[ -n "$OUTPUT_FILE" ]]; then
    : > "$OUTPUT_FILE"
fi

COMPILE_DB="$BUILD_DIR/compile_commands.json"
PLUGIN="$BUILD_DIR/lint/libAobusLintPlugin.so"

# --- Ensure prerequisites ---------------------------------------------------
cmake_cache="$BUILD_DIR/CMakeCache.txt"

if [[ ! -f "$COMPILE_DB" ]]; then
    echo "compile_commands.json missing, running cmake configure..."
    if [[ ! -f "$cmake_cache" ]]; then
        cmake -S "$PROJECT_ROOT" --preset linux-debug -B "$BUILD_DIR" \
            -DAOBUS_ENABLE_CLANG_TIDY=ON 2>&1 | tail -5
    else
        cmake "$PROJECT_ROOT" -B "$BUILD_DIR" \
            -DAOBUS_ENABLE_CLANG_TIDY=ON 2>&1 | tail -5
    fi
    echo "Configure done."
    echo "Building targets to generate necessary headers (gperf)..."
    if ! cmake --build "$BUILD_DIR" -j$(nproc) 2>&1 | tail -5; then
        echo "ERROR: Build failed during header generation." >&2
        exit 1
    fi
    echo "Build done."
fi

echo "Building AobusLintPlugin (incremental)..."
if ! cmake --build "$BUILD_DIR" --target AobusLintPlugin -j$(nproc) > /dev/null; then
    echo "ERROR: Failed to build AobusLintPlugin." >&2
    exit 1
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

CONFIG_BASE="
{Checks: 'PLACEHOLDER',
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
  {key: 'readability-identifier-naming.ConstexprVariableIgnoredRegexp', value: '^(rule|value|whitespace|op|name|atom)\$'},
  {key: 'readability-identifier-naming.ClassConstantIgnoredRegexp', value: '^(rule|value|whitespace|op|name|atom)\$'},
  {key: 'readability-identifier-naming.StaticConstantIgnoredRegexp', value: '^(rule|value|whitespace|op|name|atom)\$'},
  {key: 'readability-identifier-naming.FunctionCase', value: 'camelBack'},
  {key: 'readability-identifier-naming.MethodCase', value: 'camelBack'},
  {key: 'readability-identifier-naming.MethodIgnoredRegexp', value: '^property_.*|^signal_.*|^vfunc_.*|^on_.*'},
  {key: 'readability-identifier-naming.PublicMemberCase', value: 'camelBack'},
  {key: 'readability-identifier-naming.ProtectedMemberCase', value: 'camelBack'},
  {key: 'readability-identifier-naming.ProtectedMemberPrefix', value: '_'},
  {key: 'readability-identifier-naming.PrivateMemberCase', value: 'camelBack'},
  {key: 'readability-identifier-naming.PrivateMemberPrefix', value: '_'},
  {key: 'readability-identifier-naming.ParameterCase', value: 'camelBack'},
  {key: 'readability-identifier-naming.LocalVariableCase', value: 'camelBack'},
  {key: 'readability-identifier-naming.TypeAliasCase', value: 'CamelCase'},
  {key: 'readability-identifier-naming.TypeAliasIgnoredRegexp', value: '^(difference_type|value_type|pointer|reference|iterator_category|operand|operation)\$'},
  {key: 'readability-magic-numbers.IgnorePowersOf2IntegerValues', value: true},
  {key: 'readability-magic-numbers.IgnoredIntegerValues', value: '24;1000;1000U;60;100'},
  {key: 'readability-qualified-auto.AllowedTypes', value: 'std::array<.*>::(const_)?iterator;std::string_view::(const_)?iterator;.*::iterator;.*Iterator'},
  {key: 'readability-function-cognitive-complexity.Threshold', value: 30},
  {key: 'cppcoreguidelines-macro-usage.AllowedRegexp', value: '^DEBUG_*|^[A-Z_]+_LOG_[A-Z_]+\$'},
  {key: 'misc-include-cleaner.IgnoreHeaders', value: '.*yaml-cpp.*;.*ryml.*;.*c4/.*;.*boost/asio/.*;.*boost/interprocess/.*;.*boost/unordered/.*;.*/flat_(set|map);.*/errno.h;.*glib.*'}
 ]}"

# ---------------------------------------------------------------------------
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
# ---------------------------------------------------------------------------

classify_file() {
    local f="$1"
    [[ "$f" != /* ]] && f="$PWD/$f"
    if [[ ! -f "$f" ]]; then
        echo "IGNORE $f"
        return
    fi
    f=$(realpath -e "$f" 2>/dev/null || realpath "$f" 2>/dev/null || echo "$f")

    # Fixture files: check when explicitly requested, skip in batch scans
    if [[ "$f" == */test/integration/lint/fixture/* ]]; then
        if $EXPLICIT_FILES_MODE; then
            echo "STRICT $f"
        else
            echo "IGNORE $f"
        fi
        return
    fi
    # Everything else under test/integration/lint (scripts, old fixtures, etc.)
    if [[ "$f" == */test/integration/lint/* ]]; then
        echo "IGNORE $f"
        return
    fi

    # General classification
    if [[ "$f" == */test/main.cpp ]]; then
        echo "IGNORE $f"
    elif [[ "$f" == */lint/* ]]; then
        echo "STRICT $f"
    elif [[ "$f" == */test/* ]]; then
        echo "RELAXED $f"
    else
        echo "STRICT $f"
    fi
}


run_one() {
    local mode="$1" f="$2" tmp="$3"
    local checks="${EXTRA_CHECKS:-$STRICT_CHECKS}"
    local header_filter="${EXTRA_HEADER_FILTER:-${PROJECT_ROOT}/(lib|app|include|lint)/.*}"

    if [[ "$mode" == "RELAXED" ]]; then
        checks="${EXTRA_CHECKS:-$RELAXED_CHECKS}"
        header_filter="${PROJECT_ROOT}/(test|include)/.*"
    fi

    # Relax certain rules for GTK code due to framework patterns
    local rel_f="${f#$PROJECT_ROOT/}"
    if [[ "${rel_f}" == app/linux-gtk/* ]]; then
        checks="${checks},-cppcoreguidelines-owning-memory"
    fi

    # Re-build config string with updated checks
    local config
    config="$(echo "$CONFIG_BASE" | sed "s/PLACEHOLDER/${checks}/" | tr '\n' ' ' | sed 's/  */ /g')"
    $DEBUG_MODE && echo "DEBUG CONFIG: $config"

    local extra_args=()
    while IFS= read -r arg; do
        [[ -n "$arg" ]] && extra_args+=("$arg")
    done <<< "$ISYSTEM_ARGS_STR"
    [[ -f "$PLUGIN" ]] && extra_args+=("-load=$PLUGIN")
    if $FIX_MODE; then
        local fix_file="${tmp%.log}.yaml"
        extra_args+=("-export-fixes=$fix_file")
    fi

    clang-tidy -p "$BUILD_DIR" ${EXTRA_TIDY_ARGS:-} \
        -config="$config" \
        -header-filter="$header_filter" \
        "${extra_args[@]}" \
        "$f" > "$tmp" 2>&1
    local status=$?
    if [[ $status -ne 0 ]]; then
        echo "FAILED [$status]: $f" >&2
        return $status
    fi
}


# Serialize array for subshell inheritance
ISYSTEM_ARGS_STR=$(printf '%s\n' "${ISYSTEM_ARGS[@]}")
$DEBUG_MODE && echo "DEBUG ISYSTEM_ARGS: $ISYSTEM_ARGS_STR"

# --- File discovery ---------------------------------------------------------
cd "$PROJECT_ROOT"

EXPLICIT_FILES_MODE=false
if [[ ${#FILES[@]} -gt 0 ]]; then
    EXPLICIT_FILES_MODE=true
fi

if [[ ${#FILES[@]} -eq 0 ]]; then
    if $ALL_MODE; then
        echo "Checking all .cpp/.h/.hpp files in lib/ app/ include/ test/" >&2
        mapfile -t FILES < <(
            find lib app include test -type f \( -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \) \
                ! -name 'fakeit.hpp' ! -name 'main.cpp' ! -path '*/test/integration/lint/*' | sort
        )
    elif [[ ${#FOLDER_DIRS[@]} -gt 0 ]]; then
        echo "Checking folders: ${FOLDER_DIRS[*]}" >&2
        mapfile -t FILES < <(
            find "${FOLDER_DIRS[@]}" -type f \( -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \) \
                ! -name 'fakeit.hpp' ! -name 'main.cpp' ! -path '*/test/integration/lint/*' | sort
        )
    else
        # Default to the local main branch; when already on main, use the previous commit.
        if [[ -n "$COMMIT_REF" ]]; then
            BASE="$COMMIT_REF"
        else
            current_branch=$(git branch --show-current)
            if [[ "$current_branch" != "main" ]] && git rev-parse --verify --quiet main >/dev/null; then
                BASE="main"
            else
                BASE="HEAD~1"
            fi
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
declare -A SEEN_CLASSIFIED_FILES=()
while IFS=' ' read -r mode path; do
    if [[ -n "${SEEN_CLASSIFIED_FILES[$path]+x}" ]]; then
        continue
    fi
    SEEN_CLASSIFIED_FILES["$path"]=1

    case "$mode" in
        STRICT)  STRT_FILES+=("$path") ;;
        RELAXED) RELX_FILES+=("$path") ;;
    esac
done < <(for f in "${FILES[@]}"; do classify_file "$f"; done)

DEDUPLICATOR_PY=$(cat <<'EOF'
import sys, re
from pathlib import Path

seen = set()
cid = None
block = []
diag_re = re.compile(r"^([^:]+):([0-9]+):([0-9]+):\s+(warning|error|note):\s+(.*)")
noise_re = re.compile(r"^([0-9]+ warnings? generated\.|Suppressed [0-9]+ warnings?(?: \([^)]+\))?\.?|Use -header-filter=.*)$")

project_root = Path(sys.argv[1]).resolve()

def normalize_diag_path(path):
    p = Path(path)
    if not p.is_absolute():
        p = project_root / p
    try:
        return str(p.resolve())
    except OSError:
        return str(p)

def flush(out):
    global cid, block, seen
    if block and cid and cid not in seen:
        out.write("".join(block))
        seen.add(cid)
    block = []
    cid = None

outfile = sys.argv[2]
logs = sys.argv[3:]

out = open(outfile, "a", encoding="utf-8") if outfile else sys.stdout

for arg in logs:
    with open(arg, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            m = diag_re.match(line)
            if m:
                if m.group(4) in ("warning", "error"):
                    flush(out)
                    normalized_path = normalize_diag_path(m.group(1))
                    cid = f"{normalized_path}:{m.group(2)}:{m.group(3)}:{m.group(4)}:{m.group(5)}"
                    block.append(line)
                else:
                    if cid is not None:
                        block.append(line)
            elif line.startswith("In file included from") or line.strip().startswith("from "):
                pass
            elif noise_re.match(line):
                pass
            else:
                if cid is not None:
                    block.append(line)
                elif line.strip() and line not in seen:
                    out.write(line)
                    seen.add(line)
flush(out)
if outfile:
    out.close()
EOF
)

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
    local logs=()
    local index=0
    for f in "${files[@]}"; do
        local name="$f"
        name="${name#$PROJECT_ROOT/}"
        local tmp
        tmp=$(printf '%s/%06d_%s.log' "$tmpdir" "$index" "${name//\//_}")
        logs+=("$tmp")
        ((index++))
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

    local logs_with_warnings=()
    for log in "${logs[@]}"; do
        if grep -q -E "^[^:]+:[0-9]+:[0-9]+:[[:space:]]+(warning|error):" "$log" 2>/dev/null; then
            logs_with_warnings+=("$log")
        fi
    done

    if [[ ${#logs_with_warnings[@]} -gt 0 ]]; then
        python3 -c "$DEDUPLICATOR_PY" "$PROJECT_ROOT" "$OUTPUT_FILE" "${logs_with_warnings[@]}"
        if [[ -n "$OUTPUT_FILE" ]]; then
            echo "  Output appended to $OUTPUT_FILE"
        fi
    else
        echo "  No warnings found."
    fi

    if $FIX_MODE; then
        echo "  Applying fixes safely..."
        clang-apply-replacements "$tmpdir"
    fi

    rm -rf "$tmpdir"
}

run_batch STRICT  "${STRT_FILES[@]}"
run_batch RELAXED "${RELX_FILES[@]}"
