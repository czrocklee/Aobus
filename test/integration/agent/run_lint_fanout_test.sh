#!/usr/bin/env bash
# ============================================================================
# run_lint_fanout_test.sh
# Deterministic, offline end-to-end coverage for the C1 lint phase's Step D
# multi-candidate path (script/agent/lint_phase.sh): parallel candidate fan-out,
# deterministic ranking (fewest files, least churn), validate-top-K, and the
# escalation paths. No real model and no clang-tidy run — the worker is mocked
# through AOBUS_ROUTING_ENV and the slow tidy call through AOBUS_LINT_TIDY, and
# the runner is pointed at a throwaway tree via AOBUS_AGENT_REPO. This makes the
# whole control flow CI-safe and reproducible; the live multi-model run is
# exercised separately against real workers.
# ============================================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LINT="$(cd "$SCRIPT_DIR/../../.." && pwd)/script/agent/lint_phase.sh"

PASS=0; FAIL=0
ok()  { PASS=$((PASS + 1)); printf '  ok   %s\n' "$1"; }
bad() { FAIL=$((FAIL + 1)); printf '  FAIL %s\n' "$1"; }
assert_eq() { [ "$2" = "$3" ] && ok "$1" || bad "$1 (got [$2], want [$3])"; }

ROOT="$(mktemp -d)"
trap 'rm -rf "$ROOT"' EXIT

# --- shared test doubles -----------------------------------------------------
# Fake tidy: one "warning:" per remaining BAD marker in the (repo-relative) file.
TIDY="$ROOT/fake-tidy.sh"
cat > "$TIDY" <<'EOF'
#!/usr/bin/env bash
f="$AOBUS_AGENT_REPO/$1"
c=$(grep -c 'BAD' "$f" 2>/dev/null); c=${c:-0}
for ((i = 0; i < c; i++)); do echo "warning: BAD marker [aobus-fake]"; done
EOF
chmod +x "$TIDY"

# Mock routing: three deterministic "workers" editing only their sandbox copy.
#   surgical  — minimal in-place fix (low churn)
#   rewrite   — correct but sprawling full rewrite (high churn; still under budget)
#   noop      — changes nothing
ROUTING="$ROOT/mock-routing.env"
cat > "$ROUTING" <<'EOF'
#!/usr/bin/env bash
ROUTE_C1_LABEL="mock"
route_c1_surgical() { sed -i 's/BAD //g' "$AGENT_SANDBOX/$SD_REL"; }
route_c1_rewrite()  { for _ in $(seq 20); do echo clean; done > "$AGENT_SANDBOX/$SD_REL"; }
route_c1_noop()     { :; }
route_c1_worker()   { sed -i 's/BAD //g' "$AGENT_SANDBOX/$SD_REL"; }   # legacy single-worker name
EOF

SD_REL="lib/foo.cpp"
export SD_REL AOBUS_LINT_TIDY="$TIDY" AOBUS_ROUTING_ENV="$ROUTING"

# seed_target — fresh throwaway repo tree with a 4-line target (2 BAD markers).
seed_target() {
  local repo="$1"
  rm -rf "$repo"; mkdir -p "$repo/$(dirname "$SD_REL")"
  printf 'keep-marker-1\nremove BAD here\nkeep-marker-2\nremove BAD too\n' > "$repo/$SD_REL"
}

run_lint() { # <candidates-line> <extra-env...> ; sets RC and CONTENT
  local cands="$1"; shift
  local repo="$ROOT/repo"; seed_target "$repo"
  printf '%s\n' "$cands" >> "$ROUTING"   # append the per-scenario candidate set
  AOBUS_AGENT_REPO="$repo" AOBUS_AGENT_WORK="$ROOT/work" "$@" \
    bash "$LINT" "$SD_REL" > "$ROOT/run.log" 2>&1
  RC=$?
  CONTENT="$(cat "$repo/$SD_REL")"
  # strip the appended candidate line so the next scenario starts clean
  sed -i '/^ROUTE_C1_CANDIDATES=/d' "$ROUTING"
}

echo "== A: rank picks the surgical (low-churn) candidate over a correct rewrite =="
# rewrite launched FIRST (idx0), surgical SECOND (idx1): ranking, not launch order, must decide.
run_lint 'ROUTE_C1_CANDIDATES=(route_c1_rewrite route_c1_surgical)'
assert_eq "A: fixpoint -> exit 0" "$RC" "0"
case "$CONTENT" in
  *keep-marker-1*keep-marker-2*) ok "A: surgical result kept (original markers survive)" ;;
  *) bad "A: surgical result kept (got: $(printf '%s' "$CONTENT" | tr '\n' '|'))" ;;
esac
case "$CONTENT" in *BAD*) bad "A: BAD marker cleared" ;; *) ok "A: BAD marker cleared" ;; esac

echo "== B: all candidates no-op -> escalate, tree restored =="
run_lint 'ROUTE_C1_CANDIDATES=(route_c1_noop route_c1_noop)'
assert_eq "B: no viable candidate -> exit 2" "$RC" "2"
assert_eq "B: tree restored to original" "$CONTENT" "$(printf 'keep-marker-1\nremove BAD here\nkeep-marker-2\nremove BAD too')"
[ -f "$ROOT/work/lint/escalate/foo.cpp.packet.md" ] && ok "B: escalation packet written" || bad "B: escalation packet written"

echo "== C: churn guard rejects an over-budget rewrite -> escalate =="
run_lint 'ROUTE_C1_CANDIDATES=(route_c1_rewrite)' env MAX_CHURN=3
assert_eq "C: over-churn -> exit 2" "$RC" "2"
case "$CONTENT" in *keep-marker-1*) ok "C: tree restored (over-churn never applied)" ;; *) bad "C: tree restored" ;; esac

echo "== D: legacy routing (no ROUTE_C1_CANDIDATES) falls back to single worker =="
run_lint ''   # no candidate array -> CANDS must fall back to (route_c1_worker)
assert_eq "D: single-worker fallback -> exit 0" "$RC" "0"
case "$CONTENT" in *keep-marker-1*) ok "D: fallback worker fixed the file" ;; *) bad "D: fallback worker fixed the file" ;; esac
case "$CONTENT" in *BAD*) bad "D: BAD marker cleared" ;; *) ok "D: BAD marker cleared" ;; esac

echo "============================================================"
echo "PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ] || { echo "=== LINT FAN-OUT TESTS FAILED ==="; exit 1; }
echo "=== ALL LINT FAN-OUT TESTS PASSED ==="
