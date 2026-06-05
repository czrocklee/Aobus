#!/usr/bin/env bash
# ============================================================================
# run_dispatch_test.sh
# Deterministic, offline coverage for the harness-agnostic phase dispatcher
# (script/agent/dispatch.sh, §6 of the design doc). The dispatcher is pure C0
# logic — Phase Packet parsing, contract gating, (skill,capability)->runner
# routing, and an INDEPENDENT allowlist re-validation that never trusts the
# runner's own exit code. Everything here runs without a real model or
# clang-tidy: routing is mocked via AOBUS_ROUTING_ENV, the validation allowlist
# via AOBUS_VALIDATION_ENV, the lint phase's internal tidy via AOBUS_LINT_TIDY,
# and the tree is a throwaway under AOBUS_AGENT_REPO. Covers the PASS path and
# every reject/escalate branch, including the keystone: a runner that "succeeds"
# but is overridden by the independent gate.
# ============================================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DISPATCH="$(cd "$SCRIPT_DIR/../../.." && pwd)/script/agent/dispatch.sh"

PASS=0; FAIL=0
ok()  { PASS=$((PASS + 1)); printf '  ok   %s\n' "$1"; }
bad() { FAIL=$((FAIL + 1)); printf '  FAIL %s\n' "$1"; }
assert_eq() { [ "$2" = "$3" ] && ok "$1" || bad "$1 (got [$2], want [$3])"; }

ROOT="$(mktemp -d)"
trap 'rm -rf "$ROOT"' EXIT

# --- test doubles ------------------------------------------------------------
# Mock routing: a single surgical C1 worker (edits only its sandbox copy).
ROUTING="$ROOT/mock-routing.env"
cat > "$ROUTING" <<'EOF'
#!/usr/bin/env bash
ROUTE_C1_LABEL="mock"
route_c1_worker() { sed -i 's/BAD //g' "$AGENT_SANDBOX/$AGENT_REL"; }
ROUTE_C2_LABEL="-"
EOF

# Mock validation allowlist: v_tidy records the args the INDEPENDENT gate got
# (so a test can prove gate args come from validation_args, not inputs), can be
# forced to fail via GATE_FAIL, and otherwise passes iff no BAD markers remain.
VALID="$ROOT/mock-validation.env"
cat > "$VALID" <<'EOF'
#!/usr/bin/env bash
v_tidy() {
  printf '%s\n' "$*" >> "${GATE_ARGS_LOG:?}"
  [ -z "${GATE_FAIL:-}" ] || return 1
  local f n total=0
  for f in "$@"; do
    n=$(grep -c 'BAD' "$AOBUS_AGENT_REPO/$f" 2>/dev/null); total=$((total + ${n:-0}))
  done
  [ "$total" -eq 0 ]
}
declare -gA VALIDATION_ARGSPEC=([tidy]="path 1 -")  # -gA: sourced inside a loader fn (Step C arg gate)
EOF

# Fake lint-phase tidy: one "warning:" per remaining BAD marker. This drives the
# runner's OWN convergence loop, independently of the dispatcher's gate.
TIDY="$ROOT/fake-tidy.sh"
cat > "$TIDY" <<'EOF'
#!/usr/bin/env bash
f="$AOBUS_AGENT_REPO/$1"
c=$(grep -c 'BAD' "$f" 2>/dev/null); c=${c:-0}
for ((i = 0; i < c; i++)); do echo "warning: BAD marker [aobus-fake]"; done
EOF
chmod +x "$TIDY"

GATE_ARGS_LOG="$ROOT/gate-args.log"; : > "$GATE_ARGS_LOG"
export AOBUS_ROUTING_ENV="$ROUTING" AOBUS_VALIDATION_ENV="$VALID" \
       AOBUS_LINT_TIDY="$TIDY" GATE_ARGS_LOG

REL="lib/foo.cpp"

seed_repo() { # <repo> <bad|clean>
  local repo="$1" mode="$2"
  rm -rf "$repo"; mkdir -p "$repo/$(dirname "$REL")"
  if [ "$mode" = bad ]; then
    printf 'keep-1\nremove BAD here\nkeep-2\n' > "$repo/$REL"
  else
    printf 'keep-1\nkeep-2\n' > "$repo/$REL"
  fi
}

run_dispatch() { # <packet> <bad|clean> [VAR=val...] ; sets RC, LOG
  local packet="$1" mode="$2"; shift 2
  local repo="$ROOT/repo"; seed_repo "$repo" "$mode"
  env AOBUS_AGENT_REPO="$repo" AOBUS_AGENT_WORK="$ROOT/work" "$@" \
    bash "$DISPATCH" "$packet" > "$ROOT/run.log" 2>&1
  RC=$?
  LOG="$(cat "$ROOT/run.log")"
}

echo "== A: well-formed C1 lint packet -> runner converges + independent gate clean -> PASS =="
cat > "$ROOT/p_ok.md" <<'EOF'
---
schema: aobus-phase-packet/v1
skill: use-clang-tidy
capability: C1
validation: tidy
escalate_to: C3
inputs:
  - lib/foo.cpp
---
# fix the BAD markers
EOF
run_dispatch "$ROOT/p_ok.md" bad
assert_eq "A: dispatch PASS -> exit 0" "$RC" "0"
case "$LOG" in *"gate clean"*) ok "A: reports independent-gate PASS" ;; *) bad "A: reports independent-gate PASS" ;; esac
case "$(cat "$ROOT/repo/$REL")" in *BAD*) bad "A: BAD cleared in real tree" ;; *) ok "A: BAD cleared in real tree" ;; esac

echo "== B: validation id not in allowlist -> reject (exit 2), runner never runs =="
cat > "$ROOT/p_badvalid.md" <<'EOF'
---
skill: use-clang-tidy
capability: C1
validation: rm-rf-slash
inputs:
  - lib/foo.cpp
---
EOF
run_dispatch "$ROOT/p_badvalid.md" bad
assert_eq "B: non-allowlisted validation -> exit 2" "$RC" "2"
case "$LOG" in *"not in the allowlist"*) ok "B: rejected at allowlist gate" ;; *) bad "B: rejected at allowlist gate" ;; esac
case "$(cat "$ROOT/repo/$REL")" in *BAD*) ok "B: tree untouched (runner never ran)" ;; *) bad "B: tree untouched (runner never ran)" ;; esac

echo "== C: packet missing a required field (no inputs) -> reject (exit 64) =="
cat > "$ROOT/p_noinputs.md" <<'EOF'
---
skill: use-clang-tidy
capability: C1
validation: tidy
---
EOF
run_dispatch "$ROOT/p_noinputs.md" bad
assert_eq "C: missing inputs -> exit 64" "$RC" "64"
case "$LOG" in *"missing required fields"*) ok "C: rejected for missing fields" ;; *) bad "C: rejected for missing fields" ;; esac

echo "== D: unsafe input path (traversal) -> reject (exit 2) =="
cat > "$ROOT/p_unsafe.md" <<'EOF'
---
skill: use-clang-tidy
capability: C1
validation: tidy
inputs:
  - ../../etc/passwd
---
EOF
run_dispatch "$ROOT/p_unsafe.md" bad
assert_eq "D: unsafe input -> exit 2" "$RC" "2"
case "$LOG" in *"unsafe field"*) ok "D: rejected at arg-safety gate" ;; *) bad "D: rejected at arg-safety gate" ;; esac

echo "== E: (skill,capability) with no registered runner -> escalate (exit 2) =="
cat > "$ROOT/p_norunner.md" <<'EOF'
---
skill: diagnose-issue
capability: C3
validation: tidy
inputs:
  - lib/foo.cpp
---
EOF
run_dispatch "$ROOT/p_norunner.md" bad
assert_eq "E: no runner -> exit 2" "$RC" "2"
case "$LOG" in *"no runner registered"*) ok "E: escalates an unrouted phase" ;; *) bad "E: escalates an unrouted phase" ;; esac

echo "== F: runner keeps (rc 0) but the INDEPENDENT gate fails -> escalate (exit 2) =="
# Clean tree -> the lint runner reaches fixpoint immediately and returns 0; the
# dispatcher must STILL escalate because its own allowlist gate is red (forced
# here via GATE_FAIL, simulating a gate that disagrees with the runner's
# self-report). This is the whole reason the gate is independent.
run_dispatch "$ROOT/p_ok.md" clean GATE_FAIL=1
assert_eq "F: independent gate overrides runner -> exit 2" "$RC" "2"
case "$LOG" in *"gate failed -> escalate"*) ok "F: dispatcher distrusts the runner self-report" ;; *) bad "F: dispatcher distrusts the runner self-report" ;; esac

echo "== G: C1 tidy validation_args may feed the gate only when they match inputs exactly =="
: > "$GATE_ARGS_LOG"
cat > "$ROOT/p_vargs.md" <<'EOF'
---
skill: use-clang-tidy
capability: C1
validation: tidy
inputs:
  - lib/foo.cpp
validation_args:
  - lib/foo.cpp
---
EOF
run_dispatch "$ROOT/p_vargs.md" clean
assert_eq "G: dispatch PASS -> exit 0" "$RC" "0"
assert_eq "G: gate received the matching validation_args" "$(tail -n 1 "$GATE_ARGS_LOG")" "lib/foo.cpp"

echo "== H: C1 tidy validation_args outside inputs are rejected before the runner =="
cat > "$ROOT/p_scope_escape.md" <<'EOF'
---
skill: use-clang-tidy
capability: C1
validation: tidy
inputs:
  - lib/foo.cpp
validation_args:
  - sub/other.cpp
---
EOF
run_dispatch "$ROOT/p_scope_escape.md" bad
assert_eq "H: out-of-scope validation_args -> exit 2" "$RC" "2"
case "$LOG" in *"validation_args must stay within inputs"*) ok "H: rejected at the scope gate" ;; *) bad "H: rejected at the scope gate" ;; esac
case "$(cat "$ROOT/repo/$REL")" in *BAD*) ok "H: tree untouched (runner never ran)" ;; *) bad "H: tree untouched (runner never ran)" ;; esac

echo "== I: a mistyped validation arg is rejected up front, before the runner (Step C) =="
# tidy wants paths; a Catch2 filter as its validation_arg must be rejected by the dispatcher's
# up-front arg-contract gate, BEFORE the lint runner touches the tree.
cat > "$ROOT/p_mistyped.md" <<'EOF'
---
skill: use-clang-tidy
capability: C1
validation: tidy
inputs:
  - lib/foo.cpp
validation_args:
  - [audio]
---
EOF
run_dispatch "$ROOT/p_mistyped.md" bad
assert_eq "I: mistyped arg -> exit 2" "$RC" "2"
case "$LOG" in *"do not satisfy the 'tidy' contract"*) ok "I: rejected at the arg-contract gate" ;; *) bad "I: rejected at the arg-contract gate" ;; esac
case "$(cat "$ROOT/repo/$REL")" in *BAD*) ok "I: tree untouched (runner never ran)" ;; *) bad "I: tree untouched (runner never ran)" ;; esac

echo "============================================================"
echo "PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ] || { echo "=== DISPATCH TESTS FAILED ==="; exit 1; }
echo "=== ALL DISPATCH TESTS PASSED ==="
