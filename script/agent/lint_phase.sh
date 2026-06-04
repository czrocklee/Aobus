#!/usr/bin/env bash
# Aobus agent-fleet — C1 "lint phase" dispatcher (pilot).
#
# Delegates the MECHANICAL subset of clang-tidy fixing to a low-cost model (capability tier C1),
# behind deterministic safety gates. This is repo infrastructure, not skill content: the portable
# contract lives in .agents/skills/use-clang-tidy/SKILL.md ("Phase Contract — C1 delegation"); this
# script is the HOW that enforces it.
#
# Flow:  C0 run tidy -> C1 worker edits a SANDBOX COPY (isolated cwd) -> harness-diff (sandbox vs orig)
#        -> deterministic guard -> temporal-isolation apply to real tree -> re-validate ->
#        iterate to fixpoint; on 0 warnings KEEP (hand to C3 review), else rollback + escalate C3.
#
# Two rules learned from the pilot and enforced here:
#   - ITERATE TO FIXPOINT: a fix can surface new warnings; loop until 0 or budget/no-progress.
#   - PROCESS ISOLATION: agentic CLIs edit files in their cwd directly. The worker runs in a sandbox
#     dir holding only a COPY of the target; it edits the copy; the dispatcher diffs the copy. The
#     worker can never reach the real tree (the harness, and the CLI's own perms, block external dirs).
#
# Usage: script/agent/lint_phase.sh <repo-relative-file>
set -u

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WORK="${AOBUS_AGENT_WORK:-/tmp/aobus-agent}/lint"; mkdir -p "$WORK"
TARGET_REL="${1:?need repo-relative target file}"
TARGET="$REPO/$TARGET_REL"

# --- §4 fleet routing (the one thing to maintain when the fleet changes; externalize later) ---
WORKER_INVOKE_MODEL="opencode-go/deepseek-v4-flash"   # C1 lint worker: DeepSeek V4 Flash via opencode
run_worker_in() { # $1=sandbox cwd  $2=prompt ; worker edits files inside the sandbox only
  ( cd "$1" && timeout "${WORKER_TIMEOUT:-240}" opencode run -m "$WORKER_INVOKE_MODEL" "$2" ) </dev/null
}

# --- deterministic guard config ---
MAX_CHURN="${MAX_CHURN:-80}"
MAX_ROUNDS="${MAX_ROUNDS:-4}"
FORBID='^(include/|.*CMakeLists\.txt|\.clang-tidy|script/|doc/design/)'

if printf '%s' "$TARGET_REL" | rg -q "$FORBID"; then
  echo "GUARD REJECT: '$TARGET_REL' is a forbidden path -> escalate C3"; exit 3
fi

ROLLBACK="$WORK/$(basename "$TARGET").rollback"
cp "$TARGET" "$ROLLBACK"

run_tidy() { ( cd "$REPO" && ./script/run-clang-tidy.sh "$TARGET_REL" 2>/dev/null ) | rg "warning:|error:" || true; }

prev_n=999999
for ((round=1; round<=MAX_ROUNDS; round++)); do
  echo "==================== round $round ===================="
  DIAG="$(run_tidy)"; N=$(printf '%s\n' "$DIAG" | rg -c "warning:|error:" || echo 0)
  printf '%s\n' "$DIAG"; echo "diagnostics=$N"

  if [ "$N" -eq 0 ]; then
    echo "FIXPOINT: 0 warnings after $((round-1)) fix round(s) -> KEEP (hand to C3 review)"; exit 0
  fi
  if [ "$N" -ge "$prev_n" ]; then
    echo "NO PROGRESS ($prev_n -> $N) -> rollback + escalate C3"; cp "$ROLLBACK" "$TARGET"; exit 1
  fi
  prev_n="$N"

  # --- sandbox: a copy of the target at its relative path; the worker may edit only inside here ---
  SANDBOX="$(mktemp -d)"; SBX_FILE="$SANDBOX/$TARGET_REL"
  mkdir -p "$(dirname "$SBX_FILE")"; cp "$TARGET" "$SBX_FILE"
  read -r -d '' PROMPT <<EOF
You are a non-interactive C++ lint-fix worker for Aobus (C++26, clang-tidy enforced).
The file "$TARGET_REL" (in the current working directory) has these clang-tidy diagnostics:

$DIAG

Edit that file IN PLACE to fix ONLY these diagnostics. Follow Aobus conventions
(e.g. constants are kCamelCase like kAddend). Do NOT refactor, rename unrelated symbols,
create other files, or touch anything else.
EOF

  echo "--- C1 worker ($WORKER_INVOKE_MODEL), round $round, sandbox=$SANDBOX ---"
  run_worker_in "$SANDBOX" "$PROMPT" >"$WORK/round$round.log" 2>&1

  # --- harness-diff: whatever the worker did to the sandbox copy (no model-authored diff trusted) ---
  diff -u "$TARGET" "$SBX_FILE" > "$WORK/round$round.patch" || true
  CHURN=$(awk '/^[+-]/ && !/^[+-][+-]/ {c++} END{print c+0}' "$WORK/round$round.patch")
  echo "harness-diff: $CHURN changed lines"
  if [ "$CHURN" -eq 0 ]; then echo "worker made no change -> rollback + escalate C3"; rm -rf "$SANDBOX"; cp "$ROLLBACK" "$TARGET"; exit 1; fi
  if [ "$CHURN" -gt "$MAX_CHURN" ]; then echo "GUARD REJECT: churn $CHURN > $MAX_CHURN -> rollback + escalate"; rm -rf "$SANDBOX"; cp "$ROLLBACK" "$TARGET"; exit 3; fi

  # temporal isolation: apply to the real tree; the next loop re-validates via tidy
  cp "$SBX_FILE" "$TARGET"; rm -rf "$SANDBOX"
done

FINAL=$(run_tidy | rg -c "warning:|error:" || echo 0)
echo "ROUND BUDGET ($MAX_ROUNDS) EXHAUSTED, residual=$FINAL -> rollback + escalate C3"
cp "$ROLLBACK" "$TARGET"; exit 1
