#!/usr/bin/env bash
# Aobus agent-fleet — C1 "lint phase" runner.
#
# Delegates the MECHANICAL subset of clang-tidy fixing to a C1 (low-cost) worker behind deterministic
# safety gates. This is repo infrastructure, not skill content: the portable contract lives in
# .agents/skills/use-clang-tidy/SKILL.md ("Phase Contract — C1 delegation"); this script is the HOW.
#
# Hardening (Step C/D of doc/design/agent-fleet-tiering.md):
#   - routing externalized -> script/agent/routing.env (no model hardcoded here)
#   - repo lock: tree-mutating phases serialize (flock), so concurrent runs cannot clobber the tree
#   - multi-file scope: explicit files, or --changed to derive the changed C++ set
#   - C3 handoff: every escalation writes a Phase Packet markdown for a frontier reviewer
#
# Invariants kept from the pilot:
#   - ITERATE TO FIXPOINT: a fix can surface new warnings; loop until 0 or budget / no-progress.
#   - PROCESS ISOLATION: the worker edits a SANDBOX COPY in an isolated cwd; the dispatcher takes the
#     patch by diffing the copy (harness-diff) and applies it to the real tree under temporal
#     isolation (apply -> re-validate -> keep / rollback). The worker never reaches the real tree.
#
# Usage:
#   script/agent/lint_phase.sh <repo-relative-file> [<more files>...]
#   script/agent/lint_phase.sh --changed         # all changed C++ files (staged + modified + untracked)
#
# Exit: 0 = every file reached fixpoint (kept) ; 2 = at least one file escalated to C3 (packets
#       written) ; 4 = could not acquire repo lock ; 5 = routing table missing ; 64 = usage error.
set -u

. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"
agent_load_routing || exit 5

REPO="$AGENT_REPO"
WORK="$AGENT_WORK/lint"; mkdir -p "$WORK"
ESC_DIR="$WORK/escalate"; mkdir -p "$ESC_DIR"
MAX_CHURN="${MAX_CHURN:-80}"
MAX_ROUNDS="${MAX_ROUNDS:-4}"

run_tidy() { # $1=repo-relative file ; emit only warning/error lines
  ( cd "$REPO" && ./script/run-clang-tidy.sh "$1" 2>/dev/null ) | rg "warning:|error:" || true
}

# process_file <repo-relative file> ; 0 = fixpoint reached (kept), 2 = escalated (packet written).
process_file() {
  local rel="$1" target="$REPO/$1"
  local packet="$ESC_DIR/$(basename "$rel").packet.md"
  local diagfile="$WORK/$(basename "$rel").diag"
  echo "######## file: $rel ########"

  if [ ! -f "$target" ]; then echo "  missing on disk -> skip"; return 0; fi
  if ! agent_guard_path "$rel"; then
    : > "$diagfile"
    echo "  GUARD REJECT -> escalate C3 (packet: $(agent_emit_packet "$packet" C1 "$rel" \
      "forbidden path: a C1 worker may not edit it" "$diagfile"))"
    return 2
  fi

  local rollback="$WORK/$(basename "$rel").rollback"
  cp "$target" "$rollback"

  local prev_n=999999 round diag n sandbox sbx churn patch
  for ((round = 1; round <= MAX_ROUNDS; round++)); do
    echo "  ---- round $round ----"
    diag="$(run_tidy "$rel")"; n=$(printf '%s\n' "$diag" | rg -c "warning:|error:" || echo 0)
    printf '%s\n' "$diag" | sed 's/^/    /'; echo "    diagnostics=$n"
    printf '%s\n' "$diag" > "$diagfile"

    if [ "$n" -eq 0 ]; then
      echo "  FIXPOINT: 0 warnings after $((round - 1)) round(s) -> KEEP (hand to C3 review)"
      return 0
    fi
    if [ "$n" -ge "$prev_n" ]; then
      cp "$rollback" "$target"
      echo "  NO PROGRESS ($prev_n -> $n) -> rollback + escalate C3 (packet: $(agent_emit_packet \
        "$packet" C1 "$rel" "no progress ($prev_n -> $n warnings): C1 cannot converge" "$diagfile"))"
      return 2
    fi
    prev_n="$n"

    # Sandbox: an isolated cwd holding only a copy of the target at its repo-relative path. The
    # worker edits the copy; it can never reach the real tree.
    sandbox="$(mktemp -d)"; sbx="$sandbox/$rel"
    mkdir -p "$(dirname "$sbx")"; cp "$target" "$sbx"
    AGENT_SANDBOX="$sandbox"
    AGENT_PROMPT="$(printf '%s\n' \
      "You are a non-interactive C++ lint-fix worker for Aobus (C++26, clang-tidy enforced)." \
      "The file \"$rel\" (in the current working directory) has these clang-tidy diagnostics:" \
      "" "$diag" "" \
      "Edit that file IN PLACE to fix ONLY these diagnostics. Follow Aobus conventions" \
      "(e.g. constants are kCamelCase like kAddend). Do NOT refactor, rename unrelated symbols," \
      "create other files, or touch anything else.")"

    echo "    C1 worker [$ROUTE_C1_LABEL], round $round, sandbox=$sandbox"
    route_c1_worker > "$WORK/$(basename "$rel").round$round.log" 2>&1

    # harness-diff: whatever the worker did to the sandbox copy (no model-authored diff trusted).
    patch="$WORK/$(basename "$rel").round$round.patch"
    churn="$(agent_harness_diff "$target" "$sbx" "$patch")"
    echo "    harness-diff: $churn changed lines"
    if [ "$churn" -eq 0 ]; then
      rm -rf "$sandbox"; cp "$rollback" "$target"
      echo "  NO-OP worker -> rollback + escalate C3 (packet: $(agent_emit_packet "$packet" C1 \
        "$rel" "worker produced no change" "$diagfile"))"
      return 2
    fi
    if [ "$churn" -gt "$MAX_CHURN" ]; then
      rm -rf "$sandbox"; cp "$rollback" "$target"
      echo "  CHURN GUARD ($churn > $MAX_CHURN) -> rollback + escalate C3 (packet: $(agent_emit_packet \
        "$packet" C1 "$rel" "churn $churn > budget $MAX_CHURN (possible over-reach)" "$diagfile" "$patch"))"
      return 2
    fi

    # Temporal isolation: apply to the real tree; the next loop iteration re-validates via tidy.
    cp "$sbx" "$target"; rm -rf "$sandbox"
  done

  local final; final=$(run_tidy "$rel" | rg -c "warning:|error:" || echo 0)
  cp "$rollback" "$target"
  echo "  BUDGET EXHAUSTED ($MAX_ROUNDS rounds), residual=$final -> rollback + escalate C3 (packet: \
$(agent_emit_packet "$packet" C1 "$rel" "round budget $MAX_ROUNDS exhausted, residual=$final" "$diagfile"))"
  return 2
}

# ---- driver ----
agent_repo_lock || exit 4

declare -a FILES
if [ "${1:-}" = "--changed" ]; then
  mapfile -t FILES < <(agent_changed_cpp)
  [ "${#FILES[@]}" -eq 0 ] && { echo "no changed C++ files -> nothing to do"; exit 0; }
else
  [ "$#" -ge 1 ] || { echo "usage: $0 <repo-relative-file>... | --changed" >&2; exit 64; }
  FILES=("$@")
fi

echo "lint phase: ${#FILES[@]} file(s); worker=[$ROUTE_C1_LABEL]"
kept=0; escalated=0; declare -a ESC_LIST=()
for rel in "${FILES[@]}"; do
  if process_file "$rel"; then kept=$((kept + 1)); else escalated=$((escalated + 1)); ESC_LIST+=("$rel"); fi
done

echo "==================== summary ===================="
echo "kept (fixpoint): $kept    escalated (C3): $escalated"
if [ "$escalated" -gt 0 ]; then
  printf '  escalate: %s\n' "${ESC_LIST[@]}"
  echo "  packets in: $ESC_DIR/"
  exit 2
fi
exit 0
