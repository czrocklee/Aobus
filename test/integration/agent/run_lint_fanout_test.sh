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

echo "== E: signature-gate rejects a patch with a declaration qualifier change =="
# Worker fixes the BAD marker but also adds 'noexcept' to the declaration — the sig-gate must
# reject it and escalate to C3, leaving the real tree unchanged.
ROUTING_SIG="$ROOT/mock-routing-sig.env"
cat > "$ROUTING_SIG" <<'EOF'
#!/usr/bin/env bash
route_c1_sig_changer() {
  # "Fix" the BAD marker by rewriting the file — but also sneaks in noexcept.
  printf 'void foo() noexcept;\n' > "$AGENT_SANDBOX/$SD_REL"
}
route_c1_worker() { route_c1_sig_changer; }
ROUTE_C1_CANDIDATES=(route_c1_sig_changer)
EOF
_repo_e="$ROOT/repo_e"; mkdir -p "$_repo_e/$(dirname "$SD_REL")"
# File has a BAD marker so tidy emits a warning -> worker is invoked.
printf 'void foo(); // BAD\n' > "$_repo_e/$SD_REL"
AOBUS_AGENT_REPO="$_repo_e" AOBUS_AGENT_WORK="$ROOT/work_e" AOBUS_ROUTING_ENV="$ROUTING_SIG" \
  AOBUS_LINT_TIDY="$TIDY" bash "$LINT" "$SD_REL" > "$ROOT/run_e.log" 2>&1
RC_E=$?
assert_eq "E: sig-gate candidate -> escalate exit 2" "$RC_E" "2"
CONTENT_E="$(cat "$_repo_e/$SD_REL")"
assert_eq "E: real tree restored (sig-gate never applied)" "$CONTENT_E" "void foo(); // BAD"

echo "== F: shard parallelism (C1_SHARD_JOBS=2) processes two files concurrently =="
_repo_f="$ROOT/repo_f"; mkdir -p "$_repo_f/lib"
printf 'remove BAD here\n' > "$_repo_f/lib/a.cpp"
printf 'remove BAD here\n' > "$_repo_f/lib/b.cpp"
ROUTING_F="$ROOT/mock-routing-f.env"
cat > "$ROUTING_F" <<'EOF'
#!/usr/bin/env bash
route_c1_worker() { sed -i 's/BAD //g' "$AGENT_SANDBOX/$AGENT_REL"; }
ROUTE_C1_CANDIDATES=(route_c1_worker)
EOF
export SD_A="lib/a.cpp" SD_B="lib/b.cpp"
AOBUS_AGENT_REPO="$_repo_f" AOBUS_AGENT_WORK="$ROOT/work_f" AOBUS_ROUTING_ENV="$ROUTING_F" \
  C1_SHARD_JOBS=2 AOBUS_LINT_TIDY="$TIDY" bash "$LINT" "$SD_A" "$SD_B" > "$ROOT/run_f.log" 2>&1
RC_F=$?
assert_eq "F: two-shard run -> exit 0" "$RC_F" "0"
CONTENT_FA="$(cat "$_repo_f/lib/a.cpp")"; CONTENT_FB="$(cat "$_repo_f/lib/b.cpp")"
case "$CONTENT_FA" in *BAD*) bad "F: lib/a.cpp BAD cleared" ;; *) ok "F: lib/a.cpp BAD cleared" ;; esac
case "$CONTENT_FB" in *BAD*) bad "F: lib/b.cpp BAD cleared" ;; *) ok "F: lib/b.cpp BAD cleared" ;; esac

echo "== G: agent_lint_input_ok rejects symlinks and path-escape attempts =="
_repo_g="$ROOT/repo_g"; mkdir -p "$_repo_g/lib"
printf 'clean\n' > "$_repo_g/lib/real.cpp"
ln -s "$_repo_g/lib/real.cpp" "$_repo_g/lib/sym.cpp"
AOBUS_AGENT_REPO="$_repo_g" AOBUS_AGENT_WORK="$ROOT/work_g" AOBUS_ROUTING_ENV="$ROUTING_F" \
  AOBUS_LINT_TIDY="$TIDY" bash "$LINT" "lib/sym.cpp" > "$ROOT/run_g.log" 2>&1
RC_G=$?
assert_eq "G: symlink input -> skip (exit 0, not escalate)" "$RC_G" "0"
grep -q 'symlink -> skip' "$ROOT/run_g.log" && ok "G: symlink skip message emitted" || bad "G: symlink skip message emitted"

echo "============================================================"
echo "PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ] || { echo "=== LINT FAN-OUT TESTS FAILED ==="; exit 1; }
echo "=== ALL LINT FAN-OUT TESTS PASSED ==="
