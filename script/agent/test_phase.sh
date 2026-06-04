#!/usr/bin/env bash
# Aobus agent-fleet — C2 "test phase" runner: implement a FIXED test plan (decided upstream by C3)
# into an EXISTING, already-registered Catch2 file, then build+run to validate.
#
# Scope, from the C2 eval (doc/design §9): this AUGMENTS an already-registered test file. It does NOT
# create new test files — Aobus registers tests explicitly in test/CMakeLists.txt (no glob), so a new
# file needs a CMakeLists edit, which is a guarded path -> that is a C3 task, not C2.
#
# Packet-driven (the contract is richer than lint's): the runner reads from the Phase Packet
#   inputs[0]        the existing test file to augment
#   validation       an allowlisted v_<id> (test-core / test-gtk)
#   validation_args  the Catch2 filter for that validation (e.g. [base64])
#   <body>           the test PLAN (what C3 decided to add)
#
# Same safety as lint_phase: process isolation (worker edits a sandbox copy), harness-diff,
# deterministic guard, temporal-isolation apply, rollback. The iteration signal is "build+run passes";
# a failing build/test is fed back to the worker for the next round, up to a budget.
#
# Usage: script/agent/test_phase.sh <packet.md>
# Exit:  0 = a passing test was produced (kept) ; 2 = escalated to C3 ; 4 = lock ; 5 = config ; 64 = bad packet.
set -u

. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"
agent_load_routing    || exit 5
agent_load_validation || exit 5

PACKET="${1:?need a phase packet path}"
[ -r "$PACKET" ] || { echo "test_phase: packet not readable: $PACKET" >&2; exit 64; }

mapfile -t INPUTS < <(agent_packet_list "$PACKET" inputs)
VALID="$(agent_packet_scalar "$PACKET" validation)"
mapfile -t VARGS < <(agent_packet_list "$PACKET" validation_args)
PLAN="$(agent_packet_body "$PACKET")"
REL="${INPUTS[0]:-}"
[ -n "$REL" ] || { echo "test_phase: packet has no inputs -> reject" >&2; exit 64; }
TARGET="$AGENT_REPO/$REL"

WORK="$AGENT_WORK/test"; mkdir -p "$WORK"
ESC="$WORK/escalate"; mkdir -p "$ESC"
MAX_ROUNDS="${MAX_ROUNDS:-3}"; MAX_CHURN="${MAX_CHURN:-120}"
export AGENT_PACKET_SKILL="improve-test-coverage" AGENT_PACKET_VALIDATION="$VALID"

# --- contract gates: reject early, before touching the tree ---
if ! agent_validation_exists "$VALID"; then
  echo "test_phase: validation '$VALID' is not in the allowlist -> reject" >&2; exit 2
fi
VFN="$(agent_validation_fn "$VALID")"
agent_arg_safe "$REL" || { echo "test_phase: unsafe target '$REL' -> reject" >&2; exit 2; }
for a in "${VARGS[@]}"; do
  agent_arg_safe "$a" || { echo "test_phase: unsafe validation arg '$a' -> reject" >&2; exit 2; }
done
if ! agent_validation_args_ok "$VALID" "${VARGS[@]}"; then
  echo "test_phase: validation args do not satisfy the '$VALID' contract -> reject" >&2; exit 2
fi
packet_out="$ESC/$(basename "$REL").packet.md"
if ! agent_guard_path "$REL"; then
  echo "test_phase: GUARD REJECT '$REL' -> escalate C3 (packet: $(agent_emit_packet "$packet_out" C2 \
    "$REL" "forbidden path: a C2 worker may not edit it" /dev/null))"; exit 2
fi
[ -f "$TARGET" ] || { echo "test_phase: target missing on disk: $REL -> escalate C3"; exit 2; }
[ -n "$PLAN" ] || { echo "test_phase: packet has an empty plan body -> reject" >&2; exit 64; }

agent_repo_lock || exit 4
ROLLBACK="$WORK/$(basename "$REL").rollback"; cp "$TARGET" "$ROLLBACK"

echo "test phase: target=$REL validation=$VALID args=[${VARGS[*]}] worker=[$ROUTE_C2_LABEL]"
last_err=""
for ((round = 1; round <= MAX_ROUNDS; round++)); do
  echo "==================== round $round ===================="
  sandbox="$(mktemp -d)"; sbx="$sandbox/$REL"
  mkdir -p "$(dirname "$sbx")"; cp "$TARGET" "$sbx"

  AGENT_SANDBOX="$sandbox"; AGENT_REL="$REL"   # AGENT_REL: agy-backed workers stage by it (contract)
  if [ -n "$last_err" ]; then
    AGENT_PROMPT="$(printf '%s\n\nThe previous attempt FAILED validation. Correct the edit. Build/test output:\n%s\n' "$PLAN" "$last_err")"
  else
    AGENT_PROMPT="$PLAN"
  fi

  echo "--- C2 worker [$ROUTE_C2_LABEL], round $round, sandbox=$sandbox ---"
  "${ROUTE_C2_WORKER:-route_c2_worker}" > "$WORK/$(basename "$REL").round$round.log" 2>&1

  patch="$WORK/$(basename "$REL").round$round.patch"
  churn="$(agent_harness_diff "$TARGET" "$sbx" "$patch")"
  echo "harness-diff: $churn changed lines"
  if [ "$churn" -eq 0 ]; then
    rm -rf "$sandbox"; cp "$ROLLBACK" "$TARGET"
    echo "NO-OP worker -> rollback + escalate C3 (packet: $(agent_emit_packet "$packet_out" C2 "$REL" \
      "worker produced no change" /dev/null))"; exit 2
  fi
  if [ "$churn" -gt "$MAX_CHURN" ]; then
    rm -rf "$sandbox"; cp "$ROLLBACK" "$TARGET"
    echo "CHURN GUARD ($churn > $MAX_CHURN) -> rollback + escalate C3 (packet: $(agent_emit_packet \
      "$packet_out" C2 "$REL" "churn $churn > budget $MAX_CHURN (possible over-reach)" /dev/null "$patch"))"; exit 2
  fi

  # Temporal isolation: apply, then validate via the allowlist (build + run the filtered test).
  cp "$sbx" "$TARGET"; rm -rf "$sandbox"
  echo "--- validate: $VFN ${VARGS[*]} ---"
  if out="$( cd "$AGENT_REPO" && "$VFN" "${VARGS[@]}" 2>&1 )"; then
    printf '%s\n' "$out" | tail -3
    echo "VALIDATION PASS after $round round(s) -> KEEP (hand to C3 review)"; exit 0
  fi
  last_err="$(printf '%s\n' "$out" | tail -40)"
  printf '%s\n' "$out" | tail -6
  cp "$ROLLBACK" "$TARGET"   # discard the failed attempt; next round re-seeds from clean + error feedback
done

cp "$ROLLBACK" "$TARGET"
echo "ROUND BUDGET ($MAX_ROUNDS) EXHAUSTED -> rollback + escalate C3 (packet: $(agent_emit_packet \
  "$packet_out" C2 "$REL" "could not produce a passing test in $MAX_ROUNDS rounds" /dev/null))"
exit 2
