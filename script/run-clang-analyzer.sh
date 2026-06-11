#!/usr/bin/env bash
set -uo pipefail
# Note: not using -e because background job exit codes propagate through wait.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

if [[ -z "${IN_NIX_SHELL:-}" ]]; then
    SCRIPT_PATH="$SCRIPT_DIR/$(basename "${BASH_SOURCE[0]}")"
    exec nix-shell "$PROJECT_ROOT/shell.nix" --run "cd $(printf '%q' "$PROJECT_ROOT") && $(printf '%q ' "$SCRIPT_PATH" "$@")"
fi

BUILD_DIR="/tmp/build/debug-clang-analyzer"
DEBUG_MODE=false
FAIL_ON_DIAGNOSTICS=false
INCLUDE_EXTERNAL_DIAGNOSTICS=false
ALPHA_MODE=false
OUTPUT_FILE=""
JOBS=$(( $(nproc) > 1 ? $(nproc) - 1 : 1 ))

usage() {
    cat <<'EOF'
Usage: run-clang-analyzer.sh [options] [<file>...]

Run Clang Static Analyzer checks through clang-tidy using the Aobus build database.
Analyzer diagnostics are report-only by default; tool failures still return non-zero.

=== Scope (choose one) ================================================

  (none)                Changed files — local main..HEAD + working tree + staged + untracked
  <file>...             Explicit list of files to analyze
  --all                 Every .cpp/.h/.hpp under lib/ app/ include/ test/ tool/lint/
  --folder <d>          All files under <d> (repeatable: --folder lib --folder app)
  --commit <r>          Changed files since <r> + working tree + untracked

=== Output control ====================================================

  (none)                Full analyzer diagnostics to stdout
  -o <file>             Write analyzer diagnostics to <file>

=== Other =============================================================

  --check <name>        Run only the specified analyzer check
                        (e.g. clang-analyzer-core.NullDereference)
  --fail-on-diagnostics Return non-zero when analyzer diagnostics are found
  --include-external-diagnostics
                        Include diagnostics whose primary location is outside the repo
  --alpha               Also enable clang-analyzer-alpha.* experimental checks
  --debug               Show debug info (config, system includes)
  -j <N>                Parallel jobs (default: nproc - 1)
  -p <dir>              Build directory with compile_commands.json
                        (default: /tmp/build/debug-clang-analyzer)
  -h, --help            Show this help

=== Examples ==========================================================

  # Analyze changed files
  ./script/run-clang-analyzer.sh

  # Analyze production code
  ./script/run-clang-analyzer.sh --folder lib --folder app --folder include

  # Analyze everything and write a report
  ./script/run-clang-analyzer.sh --all -o /tmp/aobus-analyzer.txt

  # Make diagnostics fail the command, useful after a baseline is clean
  ./script/run-clang-analyzer.sh --all --fail-on-diagnostics

  # Include experimental analyzer checks for manual investigation
  ./script/run-clang-analyzer.sh --alpha --folder lib
EOF
    exit 0
}

ALL_MODE=false
FOLDER_DIRS=()
COMMIT_REF=""
CHECK_ONLY=""
FILES=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        -p) BUILD_DIR="$2"; shift 2 ;;
        -p*) BUILD_DIR="${1#-p}"; shift ;;
        -j) JOBS="$2"; shift 2 ;;
        -j*) JOBS="${1#-j}"; shift ;;
        --check) CHECK_ONLY="$2"; shift 2 ;;
        --debug) DEBUG_MODE=true; shift ;;
        --fail-on-diagnostics) FAIL_ON_DIAGNOSTICS=true; shift ;;
        --include-external-diagnostics) INCLUDE_EXTERNAL_DIAGNOSTICS=true; shift ;;
        --alpha) ALPHA_MODE=true; shift ;;
        -o) OUTPUT_FILE="$2"; shift 2 ;;
        --all) ALL_MODE=true; shift ;;
        --folder) FOLDER_DIRS+=("$2"); shift 2 ;;
        --commit) COMMIT_REF="$2"; shift 2 ;;
        -h|--help) usage ;;
        *) FILES+=("$1"); shift ;;
    esac
done

if [[ "$JOBS" =~ ^[0-9]+$ ]]; then
    if (( JOBS < 1 )); then
        echo "ERROR: -j must be at least 1." >&2
        exit 1
    fi
else
    echo "ERROR: -j requires a positive integer." >&2
    exit 1
fi

if [[ -n "$OUTPUT_FILE" ]]; then
    : > "$OUTPUT_FILE"
fi

COMPILE_DB="$BUILD_DIR/compile_commands.json"

# --- Ensure prerequisites ---------------------------------------------------
cmake_cache="$BUILD_DIR/CMakeCache.txt"

if [[ ! -f "$COMPILE_DB" ]]; then
    echo "compile_commands.json missing, running cmake configure..."
    if [[ ! -f "$cmake_cache" ]]; then
        cmake -S "$PROJECT_ROOT" --preset linux-debug -B "$BUILD_DIR" 2>&1 | tail -5
    else
        cmake "$PROJECT_ROOT" -B "$BUILD_DIR" 2>&1 | tail -5
    fi
    echo "Configure done."
    echo "Building targets to generate necessary headers (gperf)..."
    if ! cmake --build "$BUILD_DIR" -j"$(nproc)" 2>&1 | tail -5; then
        echo "ERROR: Build failed during header generation." >&2
        exit 1
    fi
    echo "Build done."
fi

# System include paths (NixOS)
ISYSTEM_ARGS=()
while IFS= read -r line; do
    line=$(echo "$line" | xargs)
    [[ -n "$line" && -d "$line" ]] && ISYSTEM_ARGS+=("--extra-arg-before=-isystem${line}")
done < <(clang++ -E -x c++ - -v < /dev/null 2>&1 | grep "^ /nix")

ANALYZER_CHECKS="$(
    c='-*'
    c+=',clang-analyzer-core.*'
    c+=',clang-analyzer-cplusplus.*'
    c+=',clang-analyzer-deadcode.*'
    c+=',clang-analyzer-nullability.*'
    c+=',clang-analyzer-optin.cplusplus.*'
    c+=',clang-analyzer-optin.performance.*'
    c+=',clang-analyzer-optin.portability.*'
    c+=',clang-analyzer-security.*'
    c+=',clang-analyzer-unix.*'
    c+=',clang-analyzer-valist.*'

    if $ALPHA_MODE; then
        c+=',clang-analyzer-alpha.*'
    fi

    echo "$c"
)"

if [[ -n "$CHECK_ONLY" ]]; then
    ANALYZER_CHECKS="-*,$CHECK_ONLY"
fi

classify_file() {
    local f="$1"
    [[ "$f" != /* ]] && f="$PWD/$f"
    if [[ ! -f "$f" ]]; then
        echo "IGNORE $f"
        return
    fi
    f=$(realpath -e "$f" 2>/dev/null || realpath "$f" 2>/dev/null || echo "$f")

    if [[ "$f" == */test/integration/lint/* ]]; then
        echo "IGNORE $f"
        return
    fi
    if [[ "$f" == */test/main.cpp ]]; then
        echo "IGNORE $f"
        return
    fi

    echo "ANALYZE $f"
}

run_one() {
    local f="$1" tmp="$2"
    local header_filter="${PROJECT_ROOT}/(lib|app|include|test|tool/lint)/.*"
    local checks="$ANALYZER_CHECKS"
    local extra_args=()
    while IFS= read -r arg; do
        [[ -n "$arg" ]] && extra_args+=("$arg")
    done <<< "$ISYSTEM_ARGS_STR"

    local rel_f="${f#$PROJECT_ROOT/}"
    if [[ -z "$CHECK_ONLY" && "${rel_f}" == test/* ]]; then
        checks="${checks},-clang-analyzer-cplusplus.NewDeleteLeaks"
    fi
    if [[ "${rel_f}" == *linux-gtk/* ]]; then
        extra_args+=("--extra-arg=-Wno-format-nonliteral")
    fi

    local config
    config="{Checks: '$checks'}"
    $DEBUG_MODE && echo "DEBUG CONFIG: $config"

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

ISYSTEM_ARGS_STR=$(printf '%s\n' "${ISYSTEM_ARGS[@]}")
$DEBUG_MODE && echo "DEBUG ISYSTEM_ARGS: $ISYSTEM_ARGS_STR"

# --- File discovery ---------------------------------------------------------
cd "$PROJECT_ROOT"

if [[ ${#FILES[@]} -eq 0 ]]; then
    if $ALL_MODE; then
        echo "Analyzing all .cpp/.h/.hpp files in lib/ app/ include/ test/ tool/lint/" >&2
        mapfile -t FILES < <(
            find lib app include test tool/lint -type f \( -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \) \
                ! -path '*/test/integration/lint/*' | sort
        )
    elif [[ ${#FOLDER_DIRS[@]} -gt 0 ]]; then
        echo "Analyzing folders: ${FOLDER_DIRS[*]}" >&2
        mapfile -t FILES < <(
            find "${FOLDER_DIRS[@]}" -type f \( -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \) \
                ! -path '*/test/integration/lint/*' | sort
        )
    else
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

ANALYZE_FILES=()
declare -A SEEN_CLASSIFIED_FILES=()
while IFS=' ' read -r mode path; do
    if [[ -n "${SEEN_CLASSIFIED_FILES[$path]+x}" ]]; then
        continue
    fi
    SEEN_CLASSIFIED_FILES["$path"]=1

    if [[ "$mode" == "ANALYZE" ]]; then
        ANALYZE_FILES+=("$path")
    fi
done < <(for f in "${FILES[@]}"; do classify_file "$f"; done)

if [[ ${#ANALYZE_FILES[@]} -eq 0 ]]; then
    echo "No analyzable .cpp/.h/.hpp files found." >&2
    exit 0
fi

DEDUPLICATOR_PY=$(cat <<'EOF'
import re
import sys
from pathlib import Path

seen = set()
cid = None
block = []
skip_block = False
diag_re = re.compile(r"^([^:]+):([0-9]+):([0-9]+):\s+(warning|error|note):\s+(.*)")
noise_re = re.compile(r"^([0-9]+ warnings? generated\.|Suppressed [0-9]+ warnings?(?: \([^)]+\))?\.?|Use -header-filter=.*)$")

project_root = Path(sys.argv[1]).resolve()
outfile = sys.argv[2]
count_file = Path(sys.argv[3])
include_external = sys.argv[4] == "true"
logs = sys.argv[5:]
diagnostic_count = 0

def normalize_diag_path(path):
    p = Path(path)
    if not p.is_absolute():
        p = project_root / p
    try:
        return str(p.resolve())
    except OSError:
        return str(p)

def flush(out):
    global cid, block, diagnostic_count
    if block and cid and cid not in seen:
        out.write("".join(block))
        seen.add(cid)
        diagnostic_count += 1
    block = []
    cid = None

def is_project_path(path):
    try:
        Path(normalize_diag_path(path)).relative_to(project_root)
        return True
    except ValueError:
        return False

out = open(outfile, "a", encoding="utf-8") if outfile else sys.stdout

for arg in logs:
    with open(arg, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            m = diag_re.match(line)
            if m:
                if m.group(4) in ("warning", "error"):
                    flush(out)
                    if not include_external and not is_project_path(m.group(1)):
                        skip_block = True
                        cid = None
                        continue
                    skip_block = False
                    normalized_path = normalize_diag_path(m.group(1))
                    cid = f"{normalized_path}:{m.group(2)}:{m.group(3)}:{m.group(4)}:{m.group(5)}"
                    block.append(line)
                elif cid is not None:
                    block.append(line)
                elif skip_block:
                    continue
            elif line.startswith("In file included from") or line.strip().startswith("from "):
                pass
            elif skip_block:
                continue
            elif noise_re.match(line):
                pass
            elif cid is not None:
                block.append(line)
            elif line.strip() and line not in seen:
                out.write(line)
                seen.add(line)

flush(out)
if outfile:
    out.close()
count_file.write_text(f"{diagnostic_count}\n", encoding="utf-8")
EOF
)

echo "=== clang-analyzer — ${#ANALYZE_FILES[@]} file(s) ==="

tmpdir=$(mktemp -d /tmp/analyzer-XXXXXX)
running=0
total=${#ANALYZE_FILES[@]}
done_count=0
overall_status=0
logs=()
index=0

for f in "${ANALYZE_FILES[@]}"; do
    name="$f"
    name="${name#$PROJECT_ROOT/}"
    tmp=$(printf '%s/%06d_%s.log' "$tmpdir" "$index" "${name//\//_}")
    logs+=("$tmp")
    ((index++))
    run_one "$f" "$tmp" &
    ((running++))

    if ((running >= JOBS)); then
        if ! wait -n 2>/dev/null; then
            overall_status=1
        fi
        ((running--))
        ((done_count++))
        printf "\r  [%d/%d]" "$done_count" "$total" >&2
    fi
done

while ((running > 0)); do
    if ! wait -n 2>/dev/null; then
        overall_status=1
    fi
    ((running--))
    ((done_count++))
    printf "\r  [%d/%d]" "$done_count" "$total" >&2
done
printf "\r  [%d/%d]\n" "$done_count" "$total" >&2

logs_with_diagnostics=()
for log in "${logs[@]}"; do
    if grep -q -E "^[^:]+:[0-9]+:[0-9]+:[[:space:]]+(warning|error):" "$log" 2>/dev/null; then
        logs_with_diagnostics+=("$log")
    fi
done

diagnostic_count=0
if [[ ${#logs_with_diagnostics[@]} -gt 0 ]]; then
    count_file="$tmpdir/diagnostic_count"
    python3 -c "$DEDUPLICATOR_PY" "$PROJECT_ROOT" "$OUTPUT_FILE" "$count_file" "$INCLUDE_EXTERNAL_DIAGNOSTICS" "${logs_with_diagnostics[@]}"
    diagnostic_count=$(< "$count_file")
    if [[ -n "$OUTPUT_FILE" ]]; then
        echo "Output written to $OUTPUT_FILE"
    fi
    if (( diagnostic_count == 0 )); then
        echo "No analyzer diagnostics found."
    fi
else
    echo "No analyzer diagnostics found."
fi

if (( overall_status != 0 )); then
    echo "Analyzer tool failure output:" >&2
    for log in "${logs[@]}"; do
        if [[ -s "$log" ]]; then
            echo "=== ${log#$tmpdir/} ===" >&2
            sed -n '1,120p' "$log" >&2
        fi
    done
fi

rm -rf "$tmpdir"

if (( diagnostic_count > 0 )); then
    echo "Analyzer diagnostics: $diagnostic_count"
    if $FAIL_ON_DIAGNOSTICS; then
        overall_status=1
    fi
fi

exit "$overall_status"
