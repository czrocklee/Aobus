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
agent_packet_validate "$PACKET" request || exit 64

SKILL="$(agent_packet_scalar "$PACKET" skill)"
CAP="$(agent_packet_scalar "$PACKET" capability)"
mapfile -t INPUTS < <(agent_packet_list "$PACKET" inputs)
VALID="$(agent_packet_scalar "$PACKET" validation)"
mapfile -t VARGS < <(agent_packet_list "$PACKET" validation_args)
PLAN="$(agent_packet_body "$PACKET")"
ANCHOR="$(agent_packet_scalar "$PACKET" target_anchor)"
REL="${INPUTS[0]:-}"
[ -n "$REL" ] || { echo "test_phase: packet has no inputs -> reject" >&2; exit 64; }
TARGET="$AGENT_REPO/$REL"

WORK="$AGENT_WORK/test"; mkdir -p "$WORK"
ESC="$WORK/escalate"; mkdir -p "$ESC"
MAX_ROUNDS="${MAX_ROUNDS:-3}"; MAX_CHURN="${MAX_CHURN:-120}"
PHASE_ID="$(agent_phase_id test)"
PHASE_DIR="$WORK/$PHASE_ID"; mkdir -p "$PHASE_DIR"
export AGENT_PACKET_SKILL="$SKILL" AGENT_PACKET_VALIDATION="$VALID"
C2_LABEL="${ROUTE_C2_LABEL:-unknown}"

# --- contract gates: reject early, before touching the tree ---
[ "$CAP" = C2 ] || { echo "test_phase: capability must be C2 (got '${CAP:-}') -> reject" >&2; exit 64; }
case "$SKILL" in
  improve-test-coverage | write-unit-test) ;;
  *) echo "test_phase: unsupported skill '$SKILL' -> reject" >&2; exit 64 ;;
esac
[ "${#INPUTS[@]}" -eq 1 ] || { echo "test_phase: exactly one input is required -> reject" >&2; exit 2; }
[ "${#VARGS[@]}" -eq 1 ] || { echo "test_phase: exactly one validation_args filter is required -> reject" >&2; exit 2; }
[ -n "$ANCHOR" ] || { echo "test_phase: target_anchor is required for C2 test augmentation -> reject" >&2; exit 64; }
if ! agent_validation_exists "$VALID"; then
  echo "test_phase: validation '$VALID' is not in the allowlist -> reject" >&2; exit 2
fi
if ! agent_c2_test_validation_ok "$VALID"; then
  echo "test_phase: validation must be test-core or test-gtk for C2 test augmentation -> reject" >&2; exit 2
fi
VFN="$(agent_validation_fn "$VALID")"
agent_arg_safe "$REL" || { echo "test_phase: unsafe target '$REL' -> reject" >&2; exit 2; }
agent_arg_safe "$ANCHOR" || { echo "test_phase: unsafe target_anchor '$ANCHOR' -> reject" >&2; exit 2; }
for a in "${VARGS[@]}"; do
  agent_arg_safe "$a" || { echo "test_phase: unsafe validation arg '$a' -> reject" >&2; exit 2; }
done
if ! agent_validation_args_ok "$VALID" "${VARGS[@]}"; then
  echo "test_phase: validation args do not satisfy the '$VALID' contract -> reject" >&2; exit 2
fi
packet_out="$ESC/$(basename "$REL").packet.md"
if ! agent_guard_path "$REL"; then
  agent_audit_entry "$PHASE_ID" "$SKILL" C2 "$C2_LABEL" escalate-guard 0 0 0 "forbidden path"
  echo "test_phase: GUARD REJECT '$REL' -> escalate C3 (packet: $(agent_emit_packet "$packet_out" C2 \
    "$REL" "forbidden path: a C2 worker may not edit it" /dev/null))"; exit 2
fi
[ -f "$TARGET" ] || { echo "test_phase: target missing on disk: $REL -> escalate C3"; exit 2; }
if ! agent_check_registered_test_for_validation "$REL" "$VALID"; then
  agent_audit_entry "$PHASE_ID" "$SKILL" C2 "$C2_LABEL" reject-scope 0 0 0 "target is not a registered Catch2 test for validation"
  echo "test_phase: target must be one existing registered Catch2 test file for '$VALID' -> reject" >&2; exit 2
fi
[ -n "$PLAN" ] || { echo "test_phase: packet has an empty plan body -> reject" >&2; exit 64; }
if rg -Fq "$ANCHOR" "$TARGET"; then
  agent_audit_entry "$PHASE_ID" "$SKILL" C2 "$C2_LABEL" reject-anchor 0 0 0 "target_anchor already present"
  echo "test_phase: target_anchor '$ANCHOR' is already present in $REL -> reject" >&2; exit 2
fi
if ! agent_test_filter_nonempty "$VALID" "${VARGS[0]}"; then
  agent_audit_entry "$PHASE_ID" "$SKILL" C2 "$C2_LABEL" reject-filter 0 0 0 "filter matched no tests"
  echo "test_phase: validation filter '${VARGS[0]}' matches no tests -> reject" >&2; exit 2
fi

agent_repo_lock || exit 4
ROLLBACK="$WORK/$(basename "$REL").rollback"; cp "$TARGET" "$ROLLBACK"
PRE_ASSERTIONS="$(agent_count_assertions "$TARGET")"

echo "test phase: target=$REL validation=$VALID args=[${VARGS[*]}] worker=[$C2_LABEL]"
echo "--- baseline validate: $VFN ${VARGS[*]} ---"
if ! baseline_out="$( cd "$AGENT_REPO" && "$VFN" "${VARGS[@]}" 2>&1 )"; then
  printf '%s\n' "$baseline_out" | tail -6
  agent_audit_entry "$PHASE_ID" "$SKILL" C2 "$C2_LABEL" reject-baseline 0 0 0 "baseline validation failed"
  echo "BASELINE VALIDATION FAILED -> escalate C3 diagnosis"; exit 2
fi
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

  echo "--- C2 worker [$C2_LABEL], round $round, sandbox=$sandbox ---"
  "${ROUTE_C2_WORKER:-route_c2_worker}" > "$WORK/$(basename "$REL").round$round.log" 2>&1

  patch="$WORK/$(basename "$REL").round$round.patch"
  churn="$(agent_harness_diff "$TARGET" "$sbx" "$patch")"
  echo "harness-diff: $churn changed lines"
  if [ "$churn" -eq 0 ]; then
    rm -rf "$sandbox"; cp "$ROLLBACK" "$TARGET"
    agent_audit_entry "$PHASE_ID" "$SKILL" C2 "$C2_LABEL" escalate-noop "$round" 0 0 "worker produced no change"
    echo "NO-OP worker -> rollback + escalate C3 (packet: $(agent_emit_packet "$packet_out" C2 "$REL" \
      "worker produced no change" /dev/null))"; exit 2
  fi
  if [ "$churn" -gt "$MAX_CHURN" ]; then
    rm -rf "$sandbox"; cp "$ROLLBACK" "$TARGET"
    agent_audit_entry "$PHASE_ID" "$SKILL" C2 "$C2_LABEL" escalate-churn "$round" "$churn" 0 "churn over budget"
    echo "CHURN GUARD ($churn > $MAX_CHURN) -> rollback + escalate C3 (packet: $(agent_emit_packet \
      "$packet_out" C2 "$REL" "churn $churn > budget $MAX_CHURN (possible over-reach)" /dev/null "$patch"))"; exit 2
  fi

  # Temporal isolation: apply, then validate via the allowlist (build + run the filtered test).
  cp "$sbx" "$TARGET"; rm -rf "$sandbox"
  echo "--- validate: $VFN ${VARGS[*]} ---"
  vout="$PHASE_DIR/round$round.validation.log"
  if out="$( cd "$AGENT_REPO" && "$VFN" "${VARGS[@]}" 2>&1 )"; then
    printf '%s\n' "$out" > "$vout"
    if ! rg -Fq "$ANCHOR" "$TARGET"; then
      last_err="validation passed, but target_anchor '$ANCHOR' was not present in $REL"
      printf '%s\n' "$last_err"
      cp "$ROLLBACK" "$TARGET"
      continue
    fi
    if ! agent_test_filter_mentions_target_anchor "$VALID" "${VARGS[0]}" "$REL" "$ANCHOR"; then
      last_err="validation passed, but filter '${VARGS[0]}' did not list target_anchor '$ANCHOR' from $REL"
      printf '%s\n' "$last_err"
      cp "$ROLLBACK" "$TARGET"
      continue
    fi
    post_assertions="$(agent_count_assertions "$TARGET")"
    delta=$((post_assertions - PRE_ASSERTIONS))
    if [ "$delta" -lt 0 ]; then
      cp "$ROLLBACK" "$TARGET"
      agent_audit_entry "$PHASE_ID" "$SKILL" C2 "$C2_LABEL" reject-assertion-delta "$round" "$churn" "$delta" "assertion count decreased"
      echo "ASSERTION DELTA ($delta) decreased -> rollback + escalate C3"; exit 2
    fi
    oracle="increased"
    [ "$delta" -eq 0 ] && oracle="zero-delta"
    dossier="$PHASE_DIR/review.md"
    manifest="$PHASE_DIR/manifest.json"
    agent_emit_review_dossier "$dossier" "$PACKET" "$REL" "$patch" "$vout" "$PHASE_ID" \
      "$C2_LABEL" "$round" "$churn" "$PRE_ASSERTIONS" "$post_assertions" "$oracle" >/dev/null
    agent_write_manifest "$manifest" "$patch" "$vout" "$dossier" "$WORK/$(basename "$REL").round$round.log"
    agent_audit_entry "$PHASE_ID" "$SKILL" C2 "$C2_LABEL" keep "$round" "$churn" "$delta" "$oracle"
    printf '%s\n' "$out" | tail -3
    echo "VALIDATION PASS after $round round(s) -> KEEP (C3 review dossier: $dossier)"; exit 0
  fi
  printf '%s\n' "$out" > "$vout"
  last_err="$(printf '%s\n' "$out" | tail -40)"
  printf '%s\n' "$out" | tail -6
  cp "$ROLLBACK" "$TARGET"   # discard the failed attempt; next round re-seeds from clean + error feedback
done

cp "$ROLLBACK" "$TARGET"
agent_audit_entry "$PHASE_ID" "$SKILL" C2 "$C2_LABEL" escalate-budget "$MAX_ROUNDS" 0 0 "round budget exhausted"
echo "ROUND BUDGET ($MAX_ROUNDS) EXHAUSTED -> rollback + escalate C3 (packet: $(agent_emit_packet \
  "$packet_out" C2 "$REL" "could not produce a passing test in $MAX_ROUNDS rounds" /dev/null))"
exit 2
