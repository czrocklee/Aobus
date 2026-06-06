#!/usr/bin/env bash
# script/agent/c2_proposal_phase.sh — C2 proposal executor skeleton
#
# Usage: script/agent/c2_proposal_phase.sh <packet.md>
# Exit:  0 = success ; 1 = diagnostic ; 2 = rejected / invalidated ; 64 = usage

set -euo pipefail

. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"
agent_load_routing    || exit 2
agent_load_validation || exit 2

PACKET="${1:?need a proposal packet path}"
[ -r "$PACKET" ] || { echo "proposal: packet not readable: $PACKET" >&2; exit 64; }

agent_packet_validate "$PACKET" proposal || exit 64

# The harness owns the phase id (as test_phase.sh does): honor the packet's optional `id` when supplied
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

# Extract packet info
vid="$(agent_packet_scalar "$PACKET" validation)"
declare -a vargs
mapfile -t vargs < <(agent_packet_list "$PACKET" validation_args)
body="$(agent_packet_body "$PACKET")"

# Derive the validation oracle from the ACTUAL scope -- the packet cannot weaken it. A single Catch2
# filter cannot cover a header's blast radius, so any header in scope forces the whole-core oracle
# (test-core-all) and is bounded by a blast-radius budget; a change that reaches the GTK/app frontend or
# exceeds the budget escalates to C3 BEFORE the worker runs. cpp-only changes keep the packet's filter.
declare -a hdr_inputs=()
for f in "${inputs[@]}"; do agent_is_header "$f" && hdr_inputs+=("$f"); done
hdr_touched=false
blast_n=0
if [ "${#hdr_inputs[@]}" -gt 0 ]; then
  hdr_touched=true
  declare -a blast
  mapfile -t blast < <(agent_proposal_compute_blast_radius "${hdr_inputs[@]}")
  blast_n="${#blast[@]}"
  budget="${PROPOSAL_BLAST_MAX:-12}"
  echo "proposal: header(s) in scope -> blast radius $blast_n TU(s) (budget $budget)"
  if ! agent_proposal_blast_core_only ${blast[@]+"${blast[@]}"}; then
    echo "proposal: header blast radius reaches GTK/app targets beyond the core oracle -> escalate to C3" >&2
    agent_audit_entry "$pid" "${pskill:-execute-plan}" "${pcap:-C2}" "${ROUTE_C2_PROPOSAL_LABEL:-unknown}" "proposal-rejected" "0" "0" "0" "header blast radius not core-only; escalate to C3"
    exit 2
  fi
  if [ "$blast_n" -gt "$budget" ]; then
    echo "proposal: header blast radius $blast_n exceeds budget $budget -> escalate to C3" >&2
    agent_audit_entry "$pid" "${pskill:-execute-plan}" "${pcap:-C2}" "${ROUTE_C2_PROPOSAL_LABEL:-unknown}" "proposal-rejected" "0" "0" "0" "blast radius $blast_n > budget $budget; escalate to C3"
    exit 2
  fi
  vid="test-core-all"   # harness-forced; the packet's validation cannot downgrade a header change
  vargs=()
fi

# Output / Sandbox setup
out_dir="${AOBUS_AGENT_WORK:-/tmp/aobus-c2}/proposal_$$"
mkdir -p "$out_dir"
agent_guard_output_dir "$AGENT_REPO" "$out_dir" || { echo "proposal: safe out dir failed" >&2; exit 2; }

# Save initial tree hash
orig_hash="$(agent_tree_hash "$AGENT_REPO")"
verify_tree_immutability() {
  local h; h="$(agent_tree_hash "$AGENT_REPO")"
  if [ "$h" != "$orig_hash" ]; then
    echo "proposal: FATAL: real repo tree was mutated during execution!" >&2
    agent_audit_entry "$pid" "${pskill:-execute-plan}" "${pcap:-C2}" "${ROUTE_C2_PROPOSAL_LABEL:-unknown}" "proposal-rejected" "${round:-0}" "0" "0" "real repo tree mutated"
    exit 2
  fi
}
trap 'verify_tree_immutability; chmod -R u+w "$out_dir/base" 2>/dev/null || true; rm -rf "$out_dir/base" "$out_dir/work"' EXIT

# Staging
bdir="$out_dir/base"
wdir="$out_dir/work"
agent_stage_repo_copy "$AGENT_REPO" "$bdir"
agent_clean_ignored "$bdir" "$AGENT_REPO"   # drop gitignored runtime noise so base == work for source
chmod -R a-w "$bdir" # Base is strictly read-only
agent_stage_repo_copy "$AGENT_REPO" "$wdir"

# Baseline Validation
echo "proposal: running baseline validation..."
if ! agent_validate_in_repo "$bdir" "$out_dir/build-base" "$vid" "${vargs[@]}" > "$out_dir/baseline.log" 2>&1; then
  echo "proposal: baseline validation failed before any edits" >&2
  agent_audit_entry "$pid" "${pskill:-execute-plan}" "${pcap:-C2}" "${ROUTE_C2_PROPOSAL_LABEL:-unknown}" "proposal-rejected" "0" "0" "0" "baseline validation failed"
  exit 2
fi

# Worker Loop
inputs_file="$out_dir/inputs.txt"
printf "%s\n" "${inputs[@]}" > "$inputs_file"

max_rounds="${MAX_PROPOSAL_ROUNDS:-3}"
churn_limit="${PROPOSAL_CHURN_MAX:-2000}"

plan_file="$out_dir/plan.md"
echo "$body" > "$plan_file"

export AGENT_PROPOSAL_WORK="$wdir"
export AGENT_PROMPT_FILE="$out_dir/prompt.md"
export AGENT_PROPOSAL_INPUTS_FILE="$inputs_file"
export AGENT_PROPOSAL_PLAN_FILE="$plan_file"
export AGENT_PROPOSAL_OUT="$out_dir"

emit_artifacts() {
  local status="$1"
  local adelta=0
  agent_changes_touch_registered_test "$out_dir/changes.tsv" 2>/dev/null && adelta=1
  local risk_note=""
  if [ "$hdr_touched" = true ] && [ "$adelta" -eq 0 ]; then
    risk_note=" RISK: a header changed with NO test delta (intent=$pintent) -- C3 must confirm behavior is preserved across the ${blast_n} translation unit(s) in the blast radius."
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
  "blast_radius": ${blast_n:-0},
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
**Header touched:** $hdr_touched (blast radius: ${blast_n:-0} TU)
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
while [ "$round" -le "$max_rounds" ]; do
  echo "proposal: Round $round"
  
  # Prepare Prompt
  {
    echo "TASK:"
    echo "$body"
    echo "ALLOWED INPUTS:"
    for f in "${inputs[@]}"; do echo "- $f"; done
    if [ "$round" -gt 1 ] && [ -r "$out_dir/round$((round - 1)).validation.log" ]; then
      echo "FEEDBACK FROM PREVIOUS ROUND:"
      tail -n 50 "$out_dir/round$((round - 1)).validation.log"
    fi
  } > "$AGENT_PROMPT_FILE"

  export AGENT_PROPOSAL_ROUND="$round"
  if [ "$round" -gt 1 ] && [ -r "$out_dir/round$((round - 1)).validation.log" ]; then
    export AGENT_PROPOSAL_FEEDBACK_FILE="$out_dir/round$((round - 1)).validation.log"
  else
    unset AGENT_PROPOSAL_FEEDBACK_FILE
  fi

  if ! "$ROUTE_C2_PROPOSAL_WORKER" > "$out_dir/round$round.worker.log" 2>&1; then
    echo "proposal: worker exited with non-zero status" >&2
  fi

  # Drop gitignored runtime artifacts a worker's build/test run wrote into the work copy (e.g.
  # logs/app.log) so they don't register as out-of-scope changes; base was cleaned identically.
  agent_clean_ignored "$wdir" "$AGENT_REPO"

  # Check Changes
  agent_tree_changes "$bdir" "$wdir" "$out_dir/changes.tsv"
  sed -i '/^mode\t/d' "$out_dir/changes.tsv"
  if ! agent_proposal_changes_ok "$inputs_file" "$out_dir/changes.tsv"; then
    echo "proposal: worker made out-of-scope edits or unsupported changes" >&2
    chmod -R u+w "$wdir"
    rm -rf "$wdir"
    agent_stage_repo_copy "$AGENT_REPO" "$wdir"
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

  # Validate
  echo "proposal: validating changes..."
  if agent_validate_in_repo "$wdir" "$out_dir/build-work" "$vid" "${vargs[@]}" > "$out_dir/round$round.validation.log" 2>&1; then
    echo "proposal: validation passed"
    emit_artifacts "validated"
    agent_audit_entry "$pid" "${pskill:-execute-plan}" "${pcap:-C2}" "${ROUTE_C2_PROPOSAL_LABEL:-unknown}" "proposal-validated" "$round" "$churn" "0" "validation passed"
    exit 0
  else
    echo "proposal: validation failed"
    round=$((round + 1))
  fi
done

echo "proposal: round budget exhausted"
emit_artifacts "diagnostic-budget-exhausted"
agent_audit_entry "$pid" "${pskill:-execute-plan}" "${pcap:-C2}" "${ROUTE_C2_PROPOSAL_LABEL:-unknown}" "proposal-diagnostic" "$round" "${churn:-0}" "0" "round budget exhausted"
exit 1
