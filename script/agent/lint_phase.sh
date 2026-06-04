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
#   - multi-candidate (Step D): per round, fan out ROUTE_C1_CANDIDATES in PARALLEL, each in its own
#     sandbox; guard + rank the resulting patches DETERMINISTICALLY (fewest files, least churn), then
#     pay the slow validation on only the top-K. One candidate = the cheapest single-worker path.
#
# Invariants kept from the pilot:
#   - ITERATE TO FIXPOINT: a fix can surface new warnings; loop until 0 or budget / no-progress.
#   - PROCESS ISOLATION: the worker edits a SANDBOX COPY in an isolated cwd; the dispatcher takes the
#     patch by diffing the copy (harness-diff) and applies it to the real tree under temporal
#     isolation (apply -> re-validate -> keep / rollback). The worker never reaches the real tree.
#   - VALIDATE ONLY THE BEST: candidate generation is near-free, but tidy validation is minutes; rank
#     first, validate at most MAX_VALIDATE candidates per round, keep the first that makes progress.
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
MAX_VALIDATE="${MAX_VALIDATE:-2}"   # K: most candidates to pay slow validation on, per round (§5.1)

# Resolve the C1 candidate set from the routing table; fall back to the single worker so an older
# routing.env (or a mock that only defines route_c1_worker) keeps working unchanged.
if declare -p ROUTE_C1_CANDIDATES >/dev/null 2>&1 && [ "${#ROUTE_C1_CANDIDATES[@]}" -gt 0 ]; then
  CANDS=("${ROUTE_C1_CANDIDATES[@]}")
else
  CANDS=(route_c1_worker)
fi

# Human-readable label for a C1 worker function (ROUTE_C1_LABELS from routing.env); falls back to the
# function name when the routing table (or a test mock) defines no label for it.
c1_label() {
  if declare -p ROUTE_C1_LABELS >/dev/null 2>&1; then printf '%s' "${ROUTE_C1_LABELS[$1]:-$1}"; else printf '%s' "$1"; fi
}

run_tidy() { # $1=repo-relative file ; emit only warning/error lines
  # AOBUS_LINT_TIDY overrides the (slow, nix-shell) clang-tidy call with a script that takes the same
  # repo-relative file and prints diagnostics — the deterministic offline test seam for this runner.
  if [ -n "${AOBUS_LINT_TIDY:-}" ]; then "$AOBUS_LINT_TIDY" "$1"; return; fi
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

  local rollback="$WORK/$(basename "$rel").rollback"      # pre-phase content (full-revert on escalate)
  local round_base="$WORK/$(basename "$rel").roundbase"   # start-of-round content (per-candidate retry)
  cp "$target" "$rollback"

  local round diag n diag2 n2
  diag="$(run_tidy "$rel")"; n=$(printf '%s\n' "$diag" | rg -c "warning:|error:" || echo 0)
  printf '%s\n' "$diag" > "$diagfile"

  for ((round = 1; round <= MAX_ROUNDS; round++)); do
    echo "  ---- round $round (warnings=$n) ----"
    if [ "$n" -eq 0 ]; then
      echo "  FIXPOINT: 0 warnings after $((round - 1)) round(s) -> KEEP (hand to C3 review)"
      return 0
    fi
    printf '%s\n' "$diag" | sed 's/^/    /'
    cp "$target" "$round_base"   # baseline the round so a failed candidate reverts here, not pre-phase

    local prompt
    prompt="$(printf '%s\n' \
      "You are a non-interactive C++ lint-fix worker for Aobus (C++26, clang-tidy enforced)." \
      "The file \"$rel\" (in the current working directory) has these clang-tidy diagnostics:" \
      "" "$diag" "" \
      "Edit that file IN PLACE to fix ONLY these diagnostics. Follow Aobus conventions" \
      "(e.g. constants are kCamelCase like kAddend). Do NOT refactor, rename unrelated symbols," \
      "create other files, or touch anything else.")"

    # --- fan the candidate set out IN PARALLEL: each worker edits only its own sandbox copy ---
    local -a c_sbx=() c_patch=() c_churn=() rank_in=()
    local ci=0 w
    for w in "${CANDS[@]}"; do
      local s; s="$(mktemp -d)"; mkdir -p "$(dirname "$s/$rel")"; cp "$target" "$s/$rel"
      c_sbx[ci]="$s"
      ( AGENT_SANDBOX="$s"; AGENT_REL="$rel"; AGENT_PROMPT="$prompt"; "$w" >"$WORK/$(basename "$rel").round$round.cand$ci.log" 2>&1 ) &
      ci=$((ci + 1))
    done
    wait
    echo "    fanned out $ci candidate(s) [${CANDS[*]}]"

    # --- collect + deterministic guard (churn budget; the sandbox already pins scope to one file) ---
    local survivors=0 noop=0 over=0 rej_patch="" idx
    for ((idx = 0; idx < ci; idx++)); do
      local p="$WORK/$(basename "$rel").round$round.cand$idx.patch" ch fl
      ch="$(agent_harness_diff "$target" "${c_sbx[idx]}/$rel" "$p")"
      fl="$(agent_patch_files "$p")"
      c_patch[idx]="$p"; c_churn[idx]="$ch"
      echo "    cand$idx [$(c1_label "${CANDS[idx]}")]: files=$fl churn=$ch"
      if [ "$ch" -eq 0 ]; then noop=$((noop + 1)); rm -rf "${c_sbx[idx]}"; c_sbx[idx]=""; continue; fi
      if [ "$ch" -gt "$MAX_CHURN" ]; then over=$((over + 1)); rej_patch="$p"; rm -rf "${c_sbx[idx]}"; c_sbx[idx]=""; continue; fi
      survivors=$((survivors + 1)); rank_in+=("$fl $ch $idx")
    done

    if [ "$survivors" -eq 0 ]; then
      cp "$rollback" "$target"
      echo "  NO VIABLE CANDIDATE (no-op=$noop, over-churn=$over > $MAX_CHURN) -> rollback + escalate C3 (packet: \
$(agent_emit_packet "$packet" C1 "$rel" "all $ci candidate(s) rejected (no-op=$noop, over-churn=$over)" "$diagfile" "$rej_patch"))"
      return 2
    fi

    # --- rank deterministically (fewest files, least churn), then pay validation on at most top-K ---
    local -a ranked; mapfile -t ranked < <(printf '%s\n' "${rank_in[@]}" | agent_rank_candidates)
    echo "    ranked (best first): ${ranked[*]}; validating up to $MAX_VALIDATE"

    local applied=0 vcount=0 rid s
    for rid in "${ranked[@]}"; do
      [ "$vcount" -ge "$MAX_VALIDATE" ] && break
      vcount=$((vcount + 1))
      cp "${c_sbx[rid]}/$rel" "$target"
      diag2="$(run_tidy "$rel")"; n2=$(printf '%s\n' "$diag2" | rg -c "warning:|error:" || echo 0)
      echo "    validate #$vcount: cand$rid [$(c1_label "${CANDS[rid]}")] (churn=${c_churn[rid]}) -> warnings $n -> $n2"
      if [ "$n2" -eq 0 ]; then
        for s in "${c_sbx[@]}"; do [ -n "$s" ] && rm -rf "$s"; done
        echo "  FIXPOINT: cand$rid cleared all warnings in round $round -> KEEP (hand to C3 review)"
        return 0
      fi
      if [ "$n2" -lt "$n" ]; then
        n="$n2"; diag="$diag2"; printf '%s\n' "$diag" > "$diagfile"; applied=1
        echo "      progress -> accept cand$rid; re-fan-out next round on the residual"
        break
      fi
      cp "$round_base" "$target"   # this candidate did not help; revert to round start, try the next
    done
    for s in "${c_sbx[@]}"; do [ -n "$s" ] && rm -rf "$s"; done

    if [ "$applied" -eq 0 ]; then
      cp "$rollback" "$target"
      echo "  NO PROGRESS: none of top-$vcount candidate(s) reduced warnings ($n) -> rollback + escalate C3 (packet: \
$(agent_emit_packet "$packet" C1 "$rel" "no candidate of top-$MAX_VALIDATE reduced warnings ($n): C1 cannot converge" "$diagfile" "${c_patch[${ranked[0]}]}"))"
      return 2
    fi
  done

  cp "$rollback" "$target"
  echo "  BUDGET EXHAUSTED ($MAX_ROUNDS rounds), residual=$n -> rollback + escalate C3 (packet: \
$(agent_emit_packet "$packet" C1 "$rel" "round budget $MAX_ROUNDS exhausted, residual=$n" "$diagfile"))"
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

echo "lint phase: ${#FILES[@]} file(s); candidates=[${CANDS[*]}]; validate top-$MAX_VALIDATE/round"
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
