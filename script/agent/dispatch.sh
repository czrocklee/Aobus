#!/usr/bin/env bash
# script/agent/dispatch.sh — thin, harness-agnostic phase dispatcher (§6 of the design doc).
#
# Reads a Phase Packet (YAML frontmatter + markdown), enforces the contract, routes the worker phase
# to the registered runner, then INDEPENDENTLY re-validates through the allowlist — it never trusts
# the runner's self-report. The dispatcher is itself C0 logic: no model, pure routing + gating.
#
# Contract enforced before anything runs:
#   - capability + skill must map to a registered runner (else escalate, don't guess);
#   - `validation` must be an ID in the allowlist (script/agent/validation.env), not a shell string;
#   - every `inputs` entry must be a safe repo-relative path (no flags, traversal, or metacharacters).
#
# Usage: script/agent/dispatch.sh <packet.md>
# Exit:  0 = phase kept and the independent gate passed ; 2 = rejected / escalated ; 5 = config
#        missing ; 64 = bad packet / usage.
set -u

. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"
agent_load_routing    || exit 5
agent_load_validation || exit 5

PACKET="${1:?need a phase packet path}"
[ -r "$PACKET" ] || { echo "dispatch: packet not readable: $PACKET" >&2; exit 64; }

if [ "$(agent_packet_scalar "$PACKET" kind)" = "proposal" ]; then
  echo "dispatch: proposal packets must run via c2_proposal_phase.sh, not dispatch.sh -> reject" >&2
  exit 64
fi

agent_packet_validate "$PACKET" request || exit 64

skill="$(agent_packet_scalar "$PACKET" skill)"
cap="$(agent_packet_scalar "$PACKET" capability)"
valid="$(agent_packet_scalar "$PACKET" validation)"
esc="$(agent_packet_scalar "$PACKET" escalate_to)"
mapfile -t inputs < <(agent_packet_list "$PACKET" inputs)
mapfile -t vargs < <(agent_packet_list "$PACKET" validation_args)

echo "dispatch: skill=$skill capability=$cap validation=$valid escalate_to=${esc:-?} inputs=${#inputs[@]}"

# --- contract gates: reject early, before mutating the tree ---
if [ -z "$skill" ] || [ -z "$cap" ] || [ "${#inputs[@]}" -lt 1 ]; then
  echo "dispatch: packet missing required fields (skill/capability/inputs) -> reject" >&2; exit 64
fi
if ! agent_validation_exists "$valid"; then
  echo "dispatch: validation '$valid' is not in the allowlist -> reject" >&2; exit 2
fi
for f in "${inputs[@]}" "${vargs[@]}"; do
  agent_arg_safe "$f" || { echo "dispatch: unsafe field '$f' -> reject" >&2; exit 2; }
done

# The independent gate runs the validation against its declared args when present, otherwise against
# the input files (e.g. the files for a C1 lint phase).
declare -a gate_args
if [ "${#vargs[@]}" -gt 0 ]; then gate_args=("${vargs[@]}"); else gate_args=("${inputs[@]}"); fi

# Reject a mistyped / mis-counted validation arg up front, BEFORE the slow runner, per the allowlist's
# declared contract (per-arg enum/type). The independent gate (agent_validate) re-checks this too.
if ! agent_validation_args_ok "$valid" "${gate_args[@]}"; then
  echo "dispatch: validation args do not satisfy the '$valid' contract -> reject" >&2; exit 2
fi

# A C1 tidy phase edits exactly the packet input files, so its independent tidy gate must cover that
# same scope. Letting validation_args point elsewhere would validate the wrong file set after the runner
# mutates the tree.
if [ "$skill/$cap" = "use-clang-tidy/C1" ] && [ "${#vargs[@]}" -gt 0 ]; then
  if [ "${#vargs[@]}" -ne "${#inputs[@]}" ]; then
    echo "dispatch: C1 tidy validation_args must match inputs exactly -> reject" >&2; exit 2
  fi
  for f in "${inputs[@]}"; do
    found=0
    for g in "${vargs[@]}"; do [ "$f" = "$g" ] && { found=1; break; }; done
    [ "$found" -eq 1 ] || {
      echo "dispatch: C1 tidy validation_args must stay within inputs ('$f' missing) -> reject" >&2; exit 2; }
  done
fi

# --- route (skill, capability) -> runner ---
runner_rc=0
case "$skill/$cap" in
  use-clang-tidy/C1)
    "$AGENT_DIR/lint_phase.sh" "${inputs[@]}"; runner_rc=$? ;;
  *)
    echo "dispatch: no runner registered for '$skill/$cap' -> escalate ${esc:-C3}"; exit 2 ;;
esac

# --- independent gate via the allowlist (do not trust the runner's exit code alone) ---
echo "dispatch: independent gate -> v_$valid ${gate_args[*]}"
if [ "$runner_rc" -eq 0 ] && agent_validate "$valid" "${gate_args[@]}"; then
  echo "dispatch: PASS (runner kept + independent '$valid' gate clean)"; exit 0
fi
echo "dispatch: runner_rc=$runner_rc or '$valid' gate failed -> escalate ${esc:-C3}"
case "$skill/$cap" in
  use-clang-tidy/C1) phase_dir="lint" ;;
  *) phase_dir="dispatch" ;;
esac
echo "dispatch: packets under $AGENT_WORK/$phase_dir/escalate/"
exit 2
