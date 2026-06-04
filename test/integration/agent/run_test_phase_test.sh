#!/usr/bin/env bash
# ============================================================================
# run_test_phase_test.sh
# Deterministic, offline coverage for the C2 "test phase" runner
# (script/agent/test_phase.sh): packet-driven augmentation of an existing test
# file under the same safety envelope as the lint phase — process isolation
# (worker edits a sandbox copy), harness-diff, deterministic guard + churn
# budget, temporal-isolation apply, allowlist re-validation, rollback, and
# error-feedback rounds. No real model and no build: the C2 worker is mocked
# via AOBUS_ROUTING_ENV (behavior switched by C2_MODE), the validation via
# AOBUS_VALIDATION_ENV, and the tree is a throwaway under AOBUS_AGENT_REPO.
# ============================================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TESTPHASE="$(cd "$SCRIPT_DIR/../../.." && pwd)/script/agent/test_phase.sh"

PASS=0; FAIL=0
ok()  { PASS=$((PASS + 1)); printf '  ok   %s\n' "$1"; }
bad() { FAIL=$((FAIL + 1)); printf '  FAIL %s\n' "$1"; }
assert_eq() { [ "$2" = "$3" ] && ok "$1" || bad "$1 (got [$2], want [$3])"; }
has()  { case "$2" in *"$3"*) ok "$1" ;; *) bad "$1 (missing [$3])" ;; esac; }
hasnt(){ case "$2" in *"$3"*) bad "$1 (unexpected [$3])" ;; *) ok "$1" ;; esac; }

ROOT="$(mktemp -d)"
trap 'rm -rf "$ROOT"' EXIT

REL="lib/foo.cpp"
export T_REL="$REL"   # test_phase does not export the rel path; the mock derives it from here

# Mock C2 routing: one worker whose behavior is selected by C2_MODE. It edits ONLY its sandbox copy.
ROUTING="$ROOT/mock-routing.env"
cat > "$ROUTING" <<'EOF'
#!/usr/bin/env bash
ROUTE_C1_LABEL="-"; ROUTE_C2_LABEL="mock-c2"; ROUTE_C3_LABEL="-"
route_c2_worker() {
  local t="$AGENT_SANDBOX/${T_REL:?}"
  case "${C2_MODE:-good}" in
    good)  printf 'PASSMARK\n' >> "$t" ;;                          # one-round pass
    noop)  : ;;                                                     # no change
    churn) for _ in $(seq 30); do echo filler; done >> "$t" ;;     # blow the churn budget
    fail)  printf 'FAILMARK\n' >> "$t" ;;                          # never satisfies validation
    flaky) case "$AGENT_PROMPT" in                                 # fail round 1, pass once told it failed
             *FAILED*) printf 'PASSMARK\n' >> "$t" ;;
             *)        printf 'FAILMARK\n' >> "$t" ;;
           esac ;;
  esac
}
route_c2_alt() { printf 'PASSMARK\nALTWORKER\n' >> "$AGENT_SANDBOX/${T_REL:?}"; }  # a 2nd selectable C2 worker
EOF

# Mock validation allowlist: v_test_core passes iff the (applied) tree carries PASSMARK. It ignores the
# Catch2-filter arg, exactly like a real build+run validates tree STATE, not the arg. On failure it
# EMITS a diagnostic, like a real build/test — that output is what test_phase feeds back to the next
# round, so a silent mock would never exercise the error-feedback path.
VALID="$ROOT/mock-validation.env"
cat > "$VALID" <<'EOF'
#!/usr/bin/env bash
v_test_core() {
  grep -q 'PASSMARK' "$AOBUS_AGENT_REPO/${T_REL:?}" && return 0
  echo "test failed: PASSMARK assertion not satisfied"; return 1
}
EOF

# A spec-bearing allowlist (declares a per-arg contract) to exercise test_phase's up-front arg gate.
VALID_SPEC="$ROOT/mock-validation-spec.env"
cat > "$VALID_SPEC" <<'EOF'
#!/usr/bin/env bash
v_test_core() { grep -q 'PASSMARK' "$AOBUS_AGENT_REPO/${T_REL:?}" && return 0; echo "test failed"; return 1; }
declare -gA VALIDATION_ARGSPEC=([test-core]="filter 1 1")
EOF

export AOBUS_ROUTING_ENV="$ROUTING" AOBUS_VALIDATION_ENV="$VALID"

reseed() { # fresh throwaway tree with a baseline "existing test file"
  local repo="$1"; rm -rf "$repo"; mkdir -p "$repo/lib"
  printf '// existing test\nTEST_CASE("x") {}\n' > "$repo/$REL"
}

run_tp() { # <packet> [VAR=val...] ; sets RC, LOG, BODY
  local packet="$1"; shift
  local repo="$ROOT/repo"; reseed "$repo"; rm -rf "$ROOT/work"
  env AOBUS_AGENT_REPO="$repo" AOBUS_AGENT_WORK="$ROOT/work" "$@" \
    bash "$TESTPHASE" "$packet" > "$ROOT/run.log" 2>&1
  RC=$?
  LOG="$(cat "$ROOT/run.log")"
  BODY="$(cat "$repo/$REL" 2>/dev/null || true)"
}
pkt() { cat > "$ROOT/p.md"; }   # write the packet from a heredoc on stdin
ESC="$ROOT/work/test/escalate"

echo "== A: valid packet, worker passes in one round -> KEEP (exit 0) =="
pkt <<'EOF'
---
schema: aobus-phase-packet/v1
skill: improve-test-coverage
capability: C2
validation: test-core
validation_args:
  - [base64]
inputs:
  - lib/foo.cpp
---
Add a base64 round-trip assertion.
EOF
run_tp "$ROOT/p.md"
assert_eq "A: pass -> exit 0" "$RC" "0"
has  "A: reports validation pass" "$LOG" "VALIDATION PASS"
has  "A: edit landed in real tree" "$BODY" "PASSMARK"

echo "== B: validation id not in allowlist -> reject (exit 2) =="
pkt <<'EOF'
---
skill: improve-test-coverage
capability: C2
validation: drop-tables
inputs:
  - lib/foo.cpp
---
plan
EOF
run_tp "$ROOT/p.md"
assert_eq "B: non-allowlisted validation -> exit 2" "$RC" "2"
has  "B: rejected at allowlist gate" "$LOG" "not in the allowlist"

echo "== C: packet with no inputs -> reject (exit 64) =="
pkt <<'EOF'
---
skill: improve-test-coverage
capability: C2
validation: test-core
---
plan
EOF
run_tp "$ROOT/p.md"
assert_eq "C: no inputs -> exit 64" "$RC" "64"

echo "== D: empty plan body -> reject (exit 64) =="
pkt <<'EOF'
---
skill: improve-test-coverage
capability: C2
validation: test-core
inputs:
  - lib/foo.cpp
---
EOF
run_tp "$ROOT/p.md"
assert_eq "D: empty plan -> exit 64" "$RC" "64"
has  "D: rejected for empty plan" "$LOG" "empty plan body"

echo "== E: unsafe validation_args -> reject (exit 2) =="
pkt <<'EOF'
---
skill: improve-test-coverage
capability: C2
validation: test-core
validation_args:
  - -rf
inputs:
  - lib/foo.cpp
---
plan
EOF
run_tp "$ROOT/p.md"
assert_eq "E: unsafe validation arg -> exit 2" "$RC" "2"
has  "E: rejected at arg-safety gate" "$LOG" "unsafe validation arg"

echo "== F: guarded target path -> escalate (exit 2) + packet =="
pkt <<'EOF'
---
skill: improve-test-coverage
capability: C2
validation: test-core
inputs:
  - include/foo.h
---
plan
EOF
run_tp "$ROOT/p.md"
assert_eq "F: guarded path -> exit 2" "$RC" "2"
has  "F: reports guard reject" "$LOG" "GUARD REJECT"
[ -f "$ESC/foo.h.packet.md" ] && ok "F: escalation packet written" || bad "F: escalation packet written"

echo "== G: target missing on disk -> escalate (exit 2) =="
pkt <<'EOF'
---
skill: improve-test-coverage
capability: C2
validation: test-core
inputs:
  - lib/missing.cpp
---
plan
EOF
run_tp "$ROOT/p.md"
assert_eq "G: target missing -> exit 2" "$RC" "2"
has  "G: reports missing target" "$LOG" "target missing on disk"

echo "== H: no-op worker -> rollback + escalate (exit 2) + packet =="
pkt <<'EOF'
---
skill: improve-test-coverage
capability: C2
validation: test-core
inputs:
  - lib/foo.cpp
---
plan
EOF
run_tp "$ROOT/p.md" C2_MODE=noop
assert_eq "H: no-op -> exit 2" "$RC" "2"
has   "H: reports no-op worker" "$LOG" "NO-OP worker"
hasnt "H: tree restored (no stray marker)" "$BODY" "PASSMARK"
[ -f "$ESC/foo.cpp.packet.md" ] && ok "H: escalation packet written" || bad "H: escalation packet written"

echo "== I: over-churn worker -> rollback + escalate (exit 2) =="
run_tp "$ROOT/p.md" C2_MODE=churn MAX_CHURN=5
assert_eq "I: over-churn -> exit 2" "$RC" "2"
has   "I: reports churn guard" "$LOG" "CHURN GUARD"
hasnt "I: tree restored (over-churn never applied)" "$BODY" "filler"

echo "== J: worker never satisfies validation -> round budget exhausted (exit 2) =="
run_tp "$ROOT/p.md" C2_MODE=fail MAX_ROUNDS=2
assert_eq "J: budget exhausted -> exit 2" "$RC" "2"
has   "J: reports budget exhausted" "$LOG" "ROUND BUDGET"
hasnt "J: tree restored (failed attempt discarded)" "$BODY" "FAILMARK"

echo "== K: flaky worker fails round 1, passes round 2 on error feedback (exit 0) =="
run_tp "$ROOT/p.md" C2_MODE=flaky MAX_ROUNDS=2
assert_eq "K: eventual pass -> exit 0" "$RC" "0"
has   "K: passing edit landed" "$BODY" "PASSMARK"
hasnt "K: clean re-seed before the passing round" "$BODY" "FAILMARK"

echo "== L: validation_args that violate the arg contract are rejected up front (Step C) =="
# With a spec'd allowlist (test-core wants exactly one Catch2 filter), a path-typed arg is rejected
# BEFORE the lock/worker — the same up-front gate the dispatcher enforces.
pkt <<'EOF'
---
skill: improve-test-coverage
capability: C2
validation: test-core
validation_args:
  - lib/foo.cpp
inputs:
  - lib/foo.cpp
---
Add a test.
EOF
run_tp "$ROOT/p.md" AOBUS_VALIDATION_ENV="$VALID_SPEC"
assert_eq "L: mistyped validation arg -> exit 2" "$RC" "2"
has   "L: rejected at the arg-contract gate" "$LOG" "do not satisfy the 'test-core' contract"
hasnt "L: worker never ran (no marker)" "$BODY" "PASSMARK"

echo "== M: ROUTE_C2_WORKER selects the active C2 worker (default vs alternate) =="
pkt <<'EOF'
---
skill: improve-test-coverage
capability: C2
validation: test-core
validation_args:
  - [base64]
inputs:
  - lib/foo.cpp
---
Add a test.
EOF
run_tp "$ROOT/p.md" ROUTE_C2_WORKER=route_c2_alt
assert_eq "M: selected worker passes -> exit 0" "$RC" "0"
has   "M: the selected (alternate) worker ran"  "$BODY" "ALTWORKER"
run_tp "$ROOT/p.md"
hasnt "M: default worker is not the alternate"  "$BODY" "ALTWORKER"

echo "============================================================"
echo "PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ] || { echo "=== TEST-PHASE TESTS FAILED ==="; exit 1; }
echo "=== ALL TEST-PHASE TESTS PASSED ==="
