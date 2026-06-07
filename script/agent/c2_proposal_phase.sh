#!/usr/bin/env bash
# script/agent/c2_proposal_phase.sh — C2 proposal executor skeleton
#
# Usage: script/agent/c2_proposal_phase.sh <packet.md>
# Exit:  0 = validated ; 1 = diagnostic (an in-scope patch was produced but never validated within the
#        round budget) ; 2 = rejected / no usable in-scope patch ; 5 = config/routing/table missing ;
#        64 = bad packet / usage. (The forced `test-all` oracle is always isolatable, so the
#        not-isolatable path never fires here.)

set -euo pipefail

. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"
agent_load_routing    || exit 5
agent_load_validation || exit 5

PACKET="${1:?need a proposal packet path}"
[ -r "$PACKET" ] || { echo "proposal: packet not readable: $PACKET" >&2; exit 64; }

agent_packet_validate "$PACKET" proposal || exit 64

# The harness owns the phase id: honor the packet's optional `id` when supplied
# (already charset-validated by agent_packet_validate), otherwise mint a unique one. Never the old
# "unknown" sentinel, which collided across id-less runs in audit.log / review-outcomes / breaker keys.
pid="$(agent_packet_scalar "$PACKET" id)"
[ -n "$pid" ] || pid="$(agent_phase_id proposal)"
echo "proposal: phase id $pid"
pskill="$(agent_packet_scalar "$PACKET" skill || echo "execute-plan")"
pcap="$(agent_packet_scalar "$PACKET" capability || echo "C2")"
pintent="$(agent_packet_scalar "$PACKET" intent)"; pintent="${pintent:-refactor}"

mapfile -t inputs < <(agent_packet_list "$PACKET" inputs)

declare -A seen_inputs
for f in "${inputs[@]}"; do
  if [ -n "${seen_inputs["$f"]:-}" ]; then
    echo "proposal: duplicate input '$f' -> reject" >&2
    exit 2
  fi
  seen_inputs["$f"]=1

  if ! agent_proposal_input_ok "$f"; then
    echo "proposal: input '$f' is out of scope or invalid -> reject" >&2
    exit 2
  fi
done

# Circuit breaker: if this worker's route was paused by a prior silent-wrong, refuse before any heavy
# staging/validation and escalate to C3 (the breaker is cleared via review_stats.sh --reset).
if agent_breaker_tripped "${ROUTE_C2_PROPOSAL_LABEL:-unknown}"; then
  echo "proposal: worker '${ROUTE_C2_PROPOSAL_LABEL:-unknown}' route is breaker-tripped -> refuse, escalate to C3" >&2
  agent_audit_entry "$pid" "${pskill:-execute-plan}" "${pcap:-C2}" "${ROUTE_C2_PROPOSAL_LABEL:-unknown}" "proposal-rejected" "0" "0" "0" "breaker tripped for this worker route"
  exit 2
fi

# The proposal oracle is FIXED to the full suite -- the C2 executor no longer selects a test subset from
# the changed paths. The whole suite runs in seconds and any change ultimately reaches the app, so a
# per-change blast-radius gate bought complexity, not safety; running everything also closes the gap
# where a core change silently broke an app-only test. Every proposal validates against `test-all`
# (build + run the WHOLE core AND GTK suites); the packet's `validation` field cannot weaken it. We
# still flag a header touched with NO test delta as a residual risk for C3, but it no longer steers the
# oracle.
body="$(agent_packet_body "$PACKET")"
vid="test-all"
declare -a vargs=()

hdr_touched=false
for f in "${inputs[@]}"; do
  if agent_is_header "$f"; then hdr_touched=true; break; fi
done

# Output / Sandbox setup
work_root="${AOBUS_AGENT_WORK:-}"
snapshot_work_root=false
if [ -z "$work_root" ]; then
  if agent_snapshot_disabled; then
    work_root="/tmp/aobus-c2"
  else
    work_root="$(agent_btrfs_work_root)"
    mkdir -p "$work_root"
    if agent_can_snapshot "$AGENT_REPO" "$work_root/.snapshot_probe"; then
      snapshot_work_root=true
    else
      if agent_snapshot_required; then
        echo "proposal: snapshot staging is required but unavailable" >&2
        exit 2
      fi
      work_root="/tmp/aobus-c2"
    fi
  fi
else
  mkdir -p "$work_root"
  if agent_can_snapshot "$AGENT_REPO" "$work_root/.snapshot_probe"; then
    snapshot_work_root=true
  else
    if agent_snapshot_required; then
      echo "proposal: snapshot staging is required but unavailable for AOBUS_AGENT_WORK=$work_root" >&2
      exit 2
    fi
  fi
fi

if [ "$snapshot_work_root" = true ]; then
  agent_btrfs_sweep "$work_root"
fi
out_dir="$work_root/proposal_$(date +%s)_$$_$RANDOM"
mkdir -p "$out_dir"
agent_write_proposal_marker "$out_dir"
agent_guard_output_dir "$AGENT_REPO" "$out_dir" || { echo "proposal: safe out dir failed" >&2; exit 2; }

# Save initial tree hash
orig_hash="$(agent_tree_hash "$AGENT_REPO")"
verify_tree_immutability() {
  local h; h="$(agent_tree_hash "$AGENT_REPO")"
  if [ "$h" != "$orig_hash" ]; then
    echo "proposal: FATAL: real repo tree was mutated during execution!" >&2
    agent_audit_entry "$pid" "${pskill:-execute-plan}" "${pcap:-C2}" "${ROUTE_C2_PROPOSAL_LABEL:-unknown}" "proposal-rejected" "${round:-0}" "0" "0" "real repo tree mutated"
    return 2
  fi
  return 0
}

record_phase_exit() {
  local rc="$1"
  {
    printf 'exit_code=%s\n' "$rc"
    printf 'phase_id=%s\n' "$pid"
    printf 'round=%s\n' "${round:-0}"
    printf 'patch_produced=%s\n' "${patch_produced:-false}"
  } > "$out_dir/phase-exit.env" 2>/dev/null || true
}

record_ccache_stats() {
  local label="$1"
  local cache_dir="$wdir/.cache/ccache"
  local stats_file="$out_dir/$label.ccache"
  {
    printf 'CCACHE_DIR=%s\n' "$cache_dir"
    if command -v ccache >/dev/null 2>&1; then
      CCACHE_DIR="$cache_dir" ccache -s
    else
      echo "ccache: unavailable"
    fi
  } > "$stats_file" 2>&1 || true
}

proposal_exit_trap() {
  local rc=$?
  record_phase_exit "$rc"
  verify_tree_immutability || rc=$?
  agent_stage_repo_teardown "$out_dir/base" "$out_dir/work" || true
  exit "$rc"
}
trap proposal_exit_trap EXIT

# Staging
bdir="$out_dir/base"
wdir="$out_dir/work"
agent_stage_repo_copy "$AGENT_REPO" "$bdir" ro
if ! agent_is_btrfs_subvolume "$bdir"; then
  agent_clean_ignored "$bdir" "$AGENT_REPO"
  mkdir -p "$bdir/logs" "$bdir/.cache"
  chmod -R a-w "$bdir"
  chmod u+w "$bdir" "$bdir/logs" "$bdir/.cache" 2>/dev/null || true
fi

stage_work_copy() {
  agent_stage_repo_copy "$AGENT_REPO" "$wdir"
  work_snapshot=false
  if agent_is_btrfs_subvolume "$wdir"; then
    work_snapshot=true
  else
    agent_clean_ignored "$wdir" "$AGENT_REPO"
  fi
  mkdir -p "$wdir/logs" "$wdir/.cache"
}
stage_work_copy

# Baseline Validation
echo "proposal: running baseline validation..."
record_ccache_stats "baseline.before"
if ! AGENT_VALIDATE_CCACHE_DIR="$wdir/.cache/ccache" agent_validate_in_repo "$bdir" "$out_dir/build-base" "$vid" "${vargs[@]}" > "$out_dir/baseline.log" 2>&1; then
  record_ccache_stats "baseline.after"
  echo "proposal: baseline validation failed before any edits" >&2
  agent_audit_entry "$pid" "${pskill:-execute-plan}" "${pcap:-C2}" "${ROUTE_C2_PROPOSAL_LABEL:-unknown}" "proposal-rejected" "0" "0" "0" "baseline validation failed"
  exit 2
fi
record_ccache_stats "baseline.after"

# Worker Loop
inputs_file="$out_dir/inputs.txt"
printf "%s\n" "${inputs[@]}" > "$inputs_file"

max_rounds="${MAX_PROPOSAL_ROUNDS:-3}"
churn_limit="${PROPOSAL_CHURN_MAX:-2000}"

plan_file="$out_dir/plan.md"
echo "$body" > "$plan_file"

export AGENT_PROPOSAL_WORK="$wdir"
export AGENT_PROPOSAL_BUILD_DIR="$out_dir/build-worker"
export AGENT_PROMPT_FILE="$out_dir/prompt.md"
export AGENT_PROPOSAL_INPUTS_FILE="$inputs_file"
export AGENT_PROPOSAL_PLAN_FILE="$plan_file"
export AGENT_PROPOSAL_OUT="$out_dir"
proposal_work_host="$AGENT_PROPOSAL_WORK"
proposal_build_host="$AGENT_PROPOSAL_BUILD_DIR"
proposal_out_host="$AGENT_PROPOSAL_OUT"
proposal_work_view="$(agent_bwrap_repo_view_path "$proposal_work_host")"
proposal_build_view="$(agent_bwrap_build_view_path "$AGENT_PROPOSAL_BUILD_DIR")"
proposal_out_view="$(agent_bwrap_out_view_path)"

emit_artifacts() {
  local status="$1"
  local adelta=0
  agent_changes_touch_registered_test "$out_dir/changes.tsv" 2>/dev/null && adelta=1
  local risk_note=""
  if [ "$hdr_touched" = true ] && [ "$adelta" -eq 0 ]; then
    risk_note=" RISK: a header changed with NO test delta (intent=$pintent) -- the full suite passed, but C3 must confirm behavior is preserved on paths the existing tests do not exercise."
  fi
  echo "${churn:-0}" > "$out_dir/churn.txt"
  awk '{print $2}' "$out_dir/changes.tsv" > "$out_dir/changed-files.txt"
  
  cat > "$out_dir/manifest.json" <<EOF
{
  "phase_id": "$(agent_json_escape "$pid")",
  "skill": "$(agent_json_escape "$pskill")",
  "capability": "$(agent_json_escape "$pcap")",
  "status": "$status",
  "intent": "$(agent_json_escape "$pintent")",
  "header_touched": $hdr_touched,
  "assertion_delta": $adelta,
  "rounds": $round,
  "churn": ${churn:-0},
  "validation": "$(agent_json_escape "$vid")"
}
EOF

  local oracle_caveat
  case "$vid" in
    test-core-asan) oracle_caveat="ASan/UBSan reported no finding on the paths the \`${vargs[*]:-}\` tests exercise. This is NOT proof of memory-safety on unexercised paths." ;;
    test-core-tsan) oracle_caveat="TSan reported no data race on the paths the \`${vargs[*]:-}\` tests exercise. This is NOT proof of race-freedom; a single-threaded test exercises no concurrency at all." ;;
    *) oracle_caveat="A passing \`$vid\` filter need not semantically exercise every changed line." ;;
  esac

  cat > "$out_dir/review.md" <<EOF
# Proposal Review Dossier
**Status:** $status
**Validation:** \`$vid\` (args: \`${vargs[*]:-}\`)
**Rounds:** $round / $max_rounds
**Churn:** ${churn:-0} lines
**Intent:** $pintent
**Header touched:** $hdr_touched
**Registered test changed:** $([ "$adelta" -eq 1 ] && echo yes || echo "no (assertion-delta 0)")

## Plan
$(cat "$plan_file")

## Changed Files
$(sed 's/^/- /' "$out_dir/changed-files.txt")

## What this validation did NOT prove
$oracle_caveat$risk_note C3 must independently judge semantic correctness before accepting.
EOF
}

round=1
patch_produced=false   # set once a round yields an in-scope, non-empty, within-budget patch
while [ "$round" -le "$max_rounds" ]; do
  echo "proposal: Round $round"
  
  # Prepare Prompt
  {
    echo "TASK:"
    echo "$body"
    echo "ALLOWED INPUTS:"
    for f in "${inputs[@]}"; do echo "- $f"; done
    echo "REFERENCE SKILLS: for domain conventions, you MAY READ .agents/skills/<topic>/SKILL.md in your"
    echo "working directory (e.g. write-unit-test, improve-test-coverage). Do NOT edit anything under .agents/"
    echo "(it is guarded -- any edit there is rejected and invalidates the proposal)."
    echo "SELF-VALIDATION: if you run local build/test commands, keep them inside the proposal worker"
    echo "build dir: BUILD_DIR=$proposal_build_view. This path is namespace-mapped to proposal-local storage."
    echo "Do not use build-base or build-work; build-work is reserved for the harness oracle."
    if [ "$round" -gt 1 ] && [ -r "$out_dir/round$((round - 1)).validation.log" ]; then
      echo "FEEDBACK FROM PREVIOUS ROUND:"
      tail -n 50 "$out_dir/round$((round - 1)).validation.log"
    fi
  } > "$AGENT_PROMPT_FILE"

  export AGENT_PROPOSAL_ROUND="$round"
  export AOBUS_AGENT_REPO="$proposal_work_view"
  export BUILD_DIR="$proposal_build_view"
  if [ "$round" -gt 1 ] && [ -r "$out_dir/round$((round - 1)).validation.log" ]; then
    export AGENT_PROPOSAL_FEEDBACK_FILE="$out_dir/round$((round - 1)).validation.log"
  else
    unset AGENT_PROPOSAL_FEEDBACK_FILE
  fi

  worker_rc=0
  record_ccache_stats "round$round.worker.before"
  export -f "$ROUTE_C2_PROPOSAL_WORKER" 2>/dev/null || true
  (
    export AGENT_PROPOSAL_WORK="$proposal_work_view"
    export AGENT_PROPOSAL_BUILD_DIR="$proposal_build_view"
    export AGENT_PROPOSAL_OUT="$proposal_out_view"
    export AGENT_PROMPT_FILE="$proposal_out_view/prompt.md"
    export AGENT_PROPOSAL_INPUTS_FILE="$proposal_out_view/inputs.txt"
    export AGENT_PROPOSAL_PLAN_FILE="$proposal_out_view/plan.md"
    export AOBUS_AGENT_REPO="$proposal_work_view"
    export BUILD_DIR="$proposal_build_view"
    cache_parent="$proposal_work_host/.cache"
    mkdir -p "$cache_parent/ccache" "$cache_parent/cmake-deps"
    agent_bwrap_path_view_run "$proposal_work_host" "$proposal_work_view" "$proposal_build_host" \
      "$proposal_build_view" "$cache_parent" "$proposal_out_host" "$proposal_out_view" \
      bash -lc "$ROUTE_C2_PROPOSAL_WORKER"
  ) > "$out_dir/round$round.worker.log" 2>&1 || worker_rc=$?
  record_ccache_stats "round$round.worker.after"
  if [ "${worker_rc:-0}" -ne 0 ]; then
    echo "proposal: worker exited with non-zero status" >&2
  fi

  # Fallback rsync copies keep the historical gitignored cleanup behavior. Snapshot work copies keep the
  # full repo state, including inherited .git/.cache, and rely on the manifest noise filter.
  if [ "$work_snapshot" != true ]; then
    agent_clean_ignored "$wdir" "$AGENT_REPO"
  fi

  # Check Changes
  agent_tree_changes "$bdir" "$wdir" "$out_dir/changes.tsv"
  # Drop mode rows: the base copy is deliberately `chmod -R a-w` while the work copy is writable, so every
  # file shows a spurious mode delta; the harness patch (git diff --no-index) carries no mode either way.
  sed -i '/^mode\t/d' "$out_dir/changes.tsv"
  # Re-derive the scope authority from the TRUSTED in-memory inputs before gating. The worker was handed
  # $AGENT_PROPOSAL_INPUTS_FILE and could have appended a forbidden path to widen its own allow-list; it
  # has already exited, so rewriting the file here cannot race. The guard must never trust a
  # worker-writable file for what is in scope.
  printf "%s\n" "${inputs[@]}" > "$inputs_file"
  if ! agent_proposal_changes_ok "$inputs_file" "$out_dir/changes.tsv"; then
    echo "proposal: worker made out-of-scope edits or unsupported changes" >&2
    agent_stage_repo_teardown "$wdir"
    stage_work_copy
    round=$((round + 1))
    continue
  fi

  # Behavior-change proposals must pin the new behavior with a registered test change. This is a
  # DETERMINISTIC obligation derived from the planner's declared intent -- the runner never infers
  # "did behavior change" from worker output (undecidable for a C0 script).
  if [ "$pintent" = "behavior-change" ] && ! agent_changes_touch_registered_test "$out_dir/changes.tsv"; then
    echo "proposal: intent=behavior-change but no registered test was changed -> reject" >&2
    agent_audit_entry "$pid" "${pskill:-execute-plan}" "${pcap:-C2}" "${ROUTE_C2_PROPOSAL_LABEL:-unknown}" "proposal-rejected" "$round" "0" "0" "behavior-change without a test change"
    exit 2
  fi

  agent_harness_diff_tree "$bdir" "$wdir" "$out_dir/patch"
  churn="$(awk '/^[+-]/ && !/^[+-][+-]/ {c++} END{print c+0}' "$out_dir/patch")"
  if [ "${churn:-0}" -eq 0 ]; then
    echo "proposal: worker made no changes" >&2
    round=$((round + 1))
    continue
  fi
  if [ "$churn" -gt "$churn_limit" ]; then
    echo "proposal: churn limit exceeded" >&2
    agent_audit_entry "$pid" "${pskill:-execute-plan}" "${pcap:-C2}" "${ROUTE_C2_PROPOSAL_LABEL:-unknown}" "proposal-rejected" "$round" "$churn" "0" "churn limit exceeded"
    exit 2
  fi
  patch_produced=true   # an in-scope, non-empty, within-budget patch exists this round

  # Validate
  echo "proposal: validating changes..."
  validate_rc=0
  record_ccache_stats "round$round.validation.before"
  AGENT_VALIDATE_CCACHE_DIR="$wdir/.cache/ccache" agent_validate_in_repo "$wdir" "$out_dir/build-work" "$vid" "${vargs[@]}" > "$out_dir/round$round.validation.log" 2>&1 || validate_rc=$?
  record_ccache_stats "round$round.validation.after"
  if [ "$validate_rc" -eq 0 ]; then
    echo "proposal: validation passed"
    emit_artifacts "validated"
    agent_audit_entry "$pid" "${pskill:-execute-plan}" "${pcap:-C2}" "${ROUTE_C2_PROPOSAL_LABEL:-unknown}" "proposal-validated" "$round" "$churn" "0" "validation passed"
    exit 0
  else
    echo "proposal: validation failed"
    round=$((round + 1))
  fi
done

if [ "$patch_produced" = true ]; then
  echo "proposal: round budget exhausted (an in-scope patch was produced but never validated)" >&2
  emit_artifacts "diagnostic-budget-exhausted"
  agent_audit_entry "$pid" "${pskill:-execute-plan}" "${pcap:-C2}" "${ROUTE_C2_PROPOSAL_LABEL:-unknown}" "proposal-diagnostic" "$round" "${churn:-0}" "0" "round budget exhausted"
  exit 1
fi
echo "proposal: no usable in-scope patch was produced -> reject" >&2
emit_artifacts "rejected-no-patch"
agent_audit_entry "$pid" "${pskill:-execute-plan}" "${pcap:-C2}" "${ROUTE_C2_PROPOSAL_LABEL:-unknown}" "proposal-rejected" "$round" "${churn:-0}" "0" "no in-scope patch produced"
exit 2
