#!/usr/bin/env bash
# ============================================================================
# run_review_stats_test.sh
# Deterministic, offline coverage for review_stats.sh (rolling C3-review stats
# + circuit-breaker control). No model, no network.
# ============================================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../../.." && pwd)"
RS="$REPO/script/agent/review_stats.sh"
COMMON="$REPO/script/agent/common.sh"

PASS=0; FAIL=0
ok()  { PASS=$((PASS + 1)); printf '  ok   %s\n' "$1"; }
bad() { FAIL=$((FAIL + 1)); printf '  FAIL %s\n' "$1"; }
assert_eq() { [ "$2" = "$3" ] && ok "$1" || bad "$1 (got [$2], want [$3])"; }
assert_rc() { local e="$1" d="$2"; shift 2; "$@" >/dev/null 2>&1; local r=$?; [ "$r" -eq "$e" ] && ok "$d" || bad "$d (rc=$r, want $e)"; }
grepq() { echo "$1" | grep -Eq "$2"; }

ROOT="$(mktemp -d)"
trap 'rm -rf "$ROOT"' EXIT
W="$ROOT/work"; mkdir -p "$W"
export AOBUS_AGENT_WORK="$W"
# shellcheck disable=SC1090
source "$COMMON"   # for agent_breaker_slug / agent_breaker_trip / agent_breaker_dir (uses AGENT_WORK=$W)

echo "== A: empty work dir -> no phases, no breakers =="
OUT="$(bash "$RS")"
grepq "$OUT" 'no audited phases yet' && ok "A: reports no phases" || bad "A: reports no phases"
grepq "$OUT" '=== tripped breakers ===' && ok "A: has breaker section" || bad "A: has breaker section"
echo "$OUT" | sed -n '/tripped breakers/,$p' | grep -q '(none)' && ok "A: no breakers" || bad "A: no breakers"

echo "== B: aggregation joins audit + outcomes by phase_id =="
cat > "$W/audit.log" <<'EOF'
{"ts":"t","phase_id":"a1","skill":"execute-plan","capability":"C2","worker":"DS Pro","result":"proposal-validated","rounds":1,"churn":7,"assertion_delta":0,"reason":"ok"}
{"ts":"t","phase_id":"a2","skill":"execute-plan","capability":"C2","worker":"DS Pro","result":"proposal-validated","rounds":1,"churn":7,"assertion_delta":0,"reason":"ok"}
{"ts":"t","phase_id":"a3","skill":"execute-plan","capability":"C2","worker":"DS Pro","result":"proposal-validated","rounds":1,"churn":7,"assertion_delta":0,"reason":"ok"}
{"ts":"t","phase_id":"d1","skill":"execute-plan","capability":"C2","worker":"DS Pro","result":"proposal-diagnostic","rounds":3,"churn":3,"assertion_delta":0,"reason":"budget"}
{"ts":"t","phase_id":"b1","skill":"use-clang-tidy","capability":"C1","worker":"codex","result":"keep","rounds":1,"churn":2,"assertion_delta":0,"reason":"ok"}
EOF
cat > "$W/review-outcomes.log" <<'EOF'
{"ts":"t","phase_id":"a1","verdict":"accept","reason":"x"}
{"ts":"t","phase_id":"a2","verdict":"modify","reason":"x"}
{"ts":"t","phase_id":"a3","verdict":"reject","reason":"x"}
{"ts":"t","phase_id":"d1","verdict":"reject","reason":"never passed"}
{"ts":"t","phase_id":"b1","verdict":"accept","reason":"x"}
EOF
OUT="$(bash "$RS")"
# DS Pro/C2: VALD=3 (a1,a2,a3), ACPT=1, MOD=1, REJ=2 (a3 validated + d1 diagnostic), SLNT=1 (a3 only), 33.3%
grepq "$OUT" '^DS Pro +C2 +3 +1 +1 +2 +1 +33\.3%' && ok "B: DS Pro row correct (silent-wrong = validated+reject only)" \
  || { bad "B: DS Pro row"; echo "$OUT"; }
# codex/C1: VALD=1 (keep), ACPT=1, no silent-wrong -> a clean 0.0% (more informative than n/a, which
# is reserved for workers with zero validated phases).
grepq "$OUT" '^codex +C1 +1 +1 +0 +0 +0 +0\.0%' && ok "B: codex row correct" || { bad "B: codex row"; echo "$OUT"; }

echo "== C: a worker label containing spaces is handled (regression for word-splitting) =="
# Exactly one DS Pro row, not one row per space-separated token.
n_rows="$(echo "$OUT" | grep -cE '^DS Pro +C2 ')"
assert_eq "C: exactly one 'DS Pro' row" "$n_rows" "1"

echo "== D: tripped-breaker listing =="
agent_breaker_trip "DS Pro" "a3" "silent-wrong: proposal-validated then C3 reject" >/dev/null
OUT="$(bash "$RS")"
echo "$OUT" | sed -n '/tripped breakers/,$p' | grep -q "$(agent_breaker_slug "DS Pro")" \
  && ok "D: tripped breaker is listed by slug" || { bad "D: tripped breaker listed"; echo "$OUT"; }

echo "== E: --reset clears one breaker; second reset is a no-op =="
assert_rc 0 "E: --reset succeeds for a tripped worker" bash "$RS" --reset "DS Pro"
[ -f "$(agent_breaker_dir)/$(agent_breaker_slug "DS Pro").tripped" ] && bad "E: breaker file removed" || ok "E: breaker file removed"
assert_rc 2 "E: --reset again exits 2 (nothing to reset)" bash "$RS" --reset "DS Pro"

echo "== F: --reset-all clears every breaker =="
agent_breaker_trip "DS Pro" "a3" "x" >/dev/null
agent_breaker_trip "codex" "z9" "x" >/dev/null
assert_rc 0 "F: --reset-all succeeds when breakers exist" bash "$RS" --reset-all
OUT="$(bash "$RS")"; echo "$OUT" | sed -n '/tripped breakers/,$p' | grep -q '(none)' \
  && ok "F: all breakers cleared" || bad "F: all breakers cleared"
assert_rc 2 "F: --reset-all again exits 2" bash "$RS" --reset-all

echo "== G: usage errors =="
assert_rc 64 "G: unknown option exits 64" bash "$RS" --bogus
assert_rc 64 "G: --reset without a label exits 64" bash "$RS" --reset

# The section-B logs are still in place ($W is untouched by the breaker sections). For DS Pro the
# validated+reviewed evals in audit order are a1(accept), a2(modify), a3(reject) -> flags 0 0 1; d1 is
# diagnostic (never validated) so it is excluded. Lifetime silent-wrong rate is 1/3 = 33.3%.
echo "== H: default output carries NO rolling column =="
OUT="$(bash "$RS")"
grepq "$OUT" 'ROLL\(' && bad "H: default has no rolling column" || ok "H: default has no rolling column"

echo "== I: --window N adds a rolling silent-wrong rate over the last N validated+reviewed evals =="
OUT="$(bash "$RS" --window 2)"
grepq "$OUT" 'ROLL\(last 2\)' && ok "I: rolling header present" || { bad "I: rolling header"; echo "$OUT"; }
# last 2 of [0,0,1] = [0,1] -> 1/2 = 50.0% (recent regression vs the 33.3% lifetime average)
grepq "$OUT" '^DS Pro +C2 .*33\.3% +1/2=50\.0%' && ok "I: DS Pro rolling(2)=1/2=50.0%" || { bad "I: DS Pro rolling(2)"; echo "$OUT"; }
# codex has a single validated+reviewed eval (b1 accept) -> 0/1 in any window
grepq "$OUT" '^codex +C1 .*0/1=0\.0%' && ok "I: codex rolling(2)=0/1=0.0%" || { bad "I: codex rolling(2)"; echo "$OUT"; }

echo "== J: a window >= history collapses to the lifetime rate =="
OUT="$(bash "$RS" --window 9)"
grepq "$OUT" '^DS Pro +C2 .*33\.3% +1/3=33\.3%' && ok "J: window>=count equals lifetime" || { bad "J: window>=count"; echo "$OUT"; }

echo "== K: window=1 isolates the most recent eval (a3 reject) =="
OUT="$(bash "$RS" --window 1)"
grepq "$OUT" '^DS Pro +C2 .*1/1=100\.0%' && ok "K: window 1 -> last eval only" || { bad "K: window 1"; echo "$OUT"; }

echo "== L: a worker with zero validated+reviewed evals in the window shows n/a =="
# Fresh work dir: one validated phase, but NOT yet reviewed -> excluded from the window denominator.
WL="$ROOT/work-pending"; mkdir -p "$WL"
cat > "$WL/audit.log" <<'EOF'
{"ts":"t","phase_id":"p1","skill":"execute-plan","capability":"C2","worker":"DS Pro","result":"proposal-validated","rounds":1,"churn":4,"assertion_delta":0,"reason":"ok"}
EOF
OUT="$(AOBUS_AGENT_WORK="$WL" bash "$RS" --window 3)"
grepq "$OUT" '^DS Pro +C2 .* n/a$' && ok "L: pending (unreviewed) eval -> rolling n/a" || { bad "L: pending eval n/a"; echo "$OUT"; }

echo "== M: --window rejects non-positive / non-numeric / missing args =="
assert_rc 64 "M: --window 0 exits 64" bash "$RS" --window 0
assert_rc 64 "M: --window abc exits 64" bash "$RS" --window abc
assert_rc 64 "M: --window without arg exits 64" bash "$RS" --window

echo "============================================================"
if [ "$FAIL" -eq 0 ]; then
  echo "PASS=$PASS FAIL=$FAIL"
  echo "=== ALL REVIEW-STATS TESTS PASSED ==="
  exit 0
else
  echo "PASS=$PASS FAIL=$FAIL"
  echo "=== REVIEW-STATS TESTS FAILED ==="
  exit 1
fi
