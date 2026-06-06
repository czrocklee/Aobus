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

pid="$(agent_packet_scalar "$PACKET" id || echo "unknown")"
pskill="$(agent_packet_scalar "$PACKET" skill || echo "execute-plan")"
pcap="$(agent_packet_scalar "$PACKET" capability || echo "C2")"

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

# Extract packet info
vid="$(agent_packet_scalar "$PACKET" validation)"
declare -a vargs
mapfile -t vargs < <(agent_packet_list "$PACKET" validation_args)
body="$(agent_packet_body "$PACKET")"

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
    agent_audit_entry "${pid:-unknown}" "${pskill:-execute-plan}" "${pcap:-C2}" "${ROUTE_C2_PROPOSAL_LABEL:-unknown}" "proposal-rejected" "${round:-0}" "0" "0" "real repo tree mutated"
    exit 2
  fi
}
trap 'verify_tree_immutability; chmod -R u+w "$out_dir/base" 2>/dev/null || true; rm -rf "$out_dir/base" "$out_dir/work"' EXIT

# Staging
bdir="$out_dir/base"
wdir="$out_dir/work"
agent_stage_repo_copy "$AGENT_REPO" "$bdir"
chmod -R a-w "$bdir" # Base is strictly read-only
agent_stage_repo_copy "$AGENT_REPO" "$wdir"

# Baseline Validation
echo "proposal: running baseline validation..."
if ! agent_validate_in_repo "$bdir" "$out_dir/build-base" "$vid" "${vargs[@]}" > "$out_dir/baseline.log" 2>&1; then
  echo "proposal: baseline validation failed before any edits" >&2
  agent_audit_entry "${pid:-unknown}" "${pskill:-execute-plan}" "${pcap:-C2}" "${ROUTE_C2_PROPOSAL_LABEL:-unknown}" "proposal-rejected" "0" "0" "0" "baseline validation failed"
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
  echo "$churn" > "$out_dir/churn.txt"
  awk '{print $2}' "$out_dir/changes.tsv" > "$out_dir/changed-files.txt"
  
  cat > "$out_dir/manifest.json" <<EOF
{
  "phase_id": "$(agent_json_escape "$pid")",
  "skill": "$(agent_json_escape "$pskill")",
  "capability": "$(agent_json_escape "$pcap")",
  "status": "$status",
  "rounds": $round,
  "churn": ${churn:-0},
  "validation": "$(agent_json_escape "$vid")"
}
EOF

  cat > "$out_dir/review.md" <<EOF
# Proposal Review Dossier
**Status:** $status
**Validation:** \`$vid\` (args: \`${vargs[*]:-}\`)
**Rounds:** $round / $max_rounds
**Churn:** ${churn:-0} lines

## Plan
$(cat "$plan_file")

## Changed Files
$(sed 's/^/- /' "$out_dir/changed-files.txt")
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

  agent_harness_diff_tree "$bdir" "$wdir" "$out_dir/patch"
  churn="$(awk '/^[+-]/ && !/^[+-][+-]/ {c++} END{print c+0}' "$out_dir/patch")"
  if [ "${churn:-0}" -eq 0 ]; then
    echo "proposal: worker made no changes" >&2
    round=$((round + 1))
    continue
  fi
  if [ "$churn" -gt "$churn_limit" ]; then
    echo "proposal: churn limit exceeded" >&2
    agent_audit_entry "${pid:-unknown}" "${pskill:-execute-plan}" "${pcap:-C2}" "${ROUTE_C2_PROPOSAL_LABEL:-unknown}" "proposal-rejected" "$round" "$churn" "0" "churn limit exceeded"
    exit 2
  fi

  # Validate
  echo "proposal: validating changes..."
  if agent_validate_in_repo "$wdir" "$out_dir/build-work" "$vid" "${vargs[@]}" > "$out_dir/round$round.validation.log" 2>&1; then
    echo "proposal: validation passed"
    emit_artifacts "validated"
    agent_audit_entry "${pid:-unknown}" "${pskill:-execute-plan}" "${pcap:-C2}" "${ROUTE_C2_PROPOSAL_LABEL:-unknown}" "proposal-validated" "$round" "$churn" "0" "validation passed"
    exit 0
  else
    echo "proposal: validation failed"
    round=$((round + 1))
  fi
done

echo "proposal: round budget exhausted"
emit_artifacts "diagnostic-budget-exhausted"
agent_audit_entry "${pid:-unknown}" "${pskill:-execute-plan}" "${pcap:-C2}" "${ROUTE_C2_PROPOSAL_LABEL:-unknown}" "proposal-diagnostic" "$round" "${churn:-0}" "0" "round budget exhausted"
exit 1
