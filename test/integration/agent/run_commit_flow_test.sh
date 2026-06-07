#!/usr/bin/env bash
# ============================================================================
# run_commit_flow_test.sh
# Deterministic, offline coverage for the C0 pre-commit orchestration chain
# (script/agent/commit_flow.sh): targeted clang-format over the changed C++ set,
# partition guarded paths off to C3, run the C1 lint phase through a Phase Packet
# + dispatch.sh (which re-validates via the tidy allowlist), then hand off. The
# defining safety property is that commit_flow DOES NOT COMMIT OR STAGE — it only
# makes the changed C++ review-ready. No real model, clang-tidy, or clang-format:
# routing/validation/tidy are mocked (AOBUS_ROUTING_ENV / AOBUS_VALIDATION_ENV /
# AOBUS_LINT_TIDY), clang-format is a PATH stub, and the tree is a throwaway git
# repo under AOBUS_AGENT_REPO.
# ============================================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
COMMITFLOW="$(cd "$SCRIPT_DIR/../../.." && pwd)/script/agent/commit_flow.sh"

PASS=0; FAIL=0
ok()  { PASS=$((PASS + 1)); printf '  ok   %s\n' "$1"; }
bad() { FAIL=$((FAIL + 1)); printf '  FAIL %s\n' "$1"; }
assert_eq() { [ "$2" = "$3" ] && ok "$1" || bad "$1 (got [$2], want [$3])"; }
has()  { case "$2" in *"$3"*) ok "$1" ;; *) bad "$1 (missing [$3])" ;; esac; }
hasnt(){ case "$2" in *"$3"*) bad "$1 (unexpected [$3])" ;; *) ok "$1" ;; esac; }

ROOT="$(mktemp -d)"
trap 'rm -rf "$ROOT"' EXIT
REPO="$ROOT/repo"

# --- test doubles ------------------------------------------------------------
# Mock C1 routing: a surgical worker (sed) editing only its sandbox copy; C1_NOOP=1 makes it converge
# nowhere so the lint phase escalates.
ROUTING="$ROOT/mock-routing.env"
cat > "$ROUTING" <<'EOF'
#!/usr/bin/env bash
ROUTE_C1_LABEL="mock"
route_c1_worker() { [ "${C1_NOOP:-}" = 1 ] && return 0; sed -i 's/BAD //g' "$AGENT_SANDBOX/$AGENT_REL"; }
EOF

# Mock validation allowlist: the dispatcher's independent tidy gate — passes iff no BAD markers remain.
VALID="$ROOT/mock-validation.env"
cat > "$VALID" <<'EOF'
#!/usr/bin/env bash
v_tidy() {
  local f n total=0
  for f in "$@"; do n=$(grep -c 'BAD' "$AOBUS_AGENT_REPO/$f" 2>/dev/null); total=$((total + ${n:-0})); done
  [ "$total" -eq 0 ]
}
EOF

# Fake lint-phase tidy: one warning per remaining BAD marker (drives the C1 convergence loop).
TIDY="$ROOT/fake-tidy.sh"
cat > "$TIDY" <<'EOF'
#!/usr/bin/env bash
f="$AOBUS_AGENT_REPO/$1"
c=$(grep -c 'BAD' "$f" 2>/dev/null); c=${c:-0}
for ((i = 0; i < c; i++)); do echo "warning: BAD marker [aobus-fake]"; done
EOF
chmod +x "$TIDY"

# No-op clang-format stub so the C0 format step never depends on a real toolchain.
mkdir -p "$ROOT/bin"
cat > "$ROOT/bin/clang-format" <<'EOF'
#!/usr/bin/env bash
exit 0
EOF
chmod +x "$ROOT/bin/clang-format"

export AOBUS_ROUTING_ENV="$ROUTING" AOBUS_VALIDATION_ENV="$VALID" AOBUS_LINT_TIDY="$TIDY"

init_repo() { # fresh git repo with a committed clean baseline
  rm -rf "$REPO"; mkdir -p "$REPO/lib" "$REPO/include"
  git -C "$REPO" init -q
  git -C "$REPO" config user.email t@t.local
  git -C "$REPO" config user.name tester
  printf 'int clean() { return 0; }\n' > "$REPO/lib/foo.cpp"
  printf '#pragma once\n' > "$REPO/include/bar.h"
  git -C "$REPO" add -A && git -C "$REPO" commit -q -m baseline
}
dirty_lintable() { printf 'int f() { return BAD 0; }\n' > "$REPO/lib/foo.cpp"; }       # tracked, lintable
dirty_guarded()  { printf '#pragma once\nint x();\n'    > "$REPO/include/bar.h"; }       # tracked, guarded path

commits()  { git -C "$REPO" rev-list --count HEAD; }
staged()   { git -C "$REPO" diff --cached --name-only; }
worktree() { git -C "$REPO" status --porcelain; }

run_cf() { # [VAR=val...] ; sets RC, LOG ; repo must already be seeded
  rm -rf "$ROOT/work"
  env AOBUS_AGENT_REPO="$REPO" AOBUS_AGENT_WORK="$ROOT/work" PATH="$ROOT/bin:$PATH" "$@" \
    bash "$COMMITFLOW" > "$ROOT/run.log" 2>&1
  RC=$?
  LOG="$(cat "$ROOT/run.log")"
}

echo "== A: clean repo (no changed C++) -> nothing to do (exit 0) =="
init_repo
run_cf
assert_eq "A: clean tree -> exit 0" "$RC" "0"
has  "A: reports tree is C++-clean" "$LOG" "tree is C++-clean"

echo "== B: changed lintable C++ converges + tidy gate clean -> READY FOR C3 (exit 0), never commits =="
init_repo; dirty_lintable
run_cf
assert_eq "B: ready -> exit 0" "$RC" "0"
has  "B: reports ready for C3" "$LOG" "READY FOR C3"
hasnt "B: BAD fixed in the working tree" "$(cat "$REPO/lib/foo.cpp")" "BAD"
assert_eq "B: did NOT create a commit" "$(commits)" "1"
assert_eq "B: did NOT stage anything" "$(staged)" ""
[ -n "$(worktree)" ] && ok "B: left the change in the working tree" || bad "B: left the change in the working tree"

echo "== C: a guarded path in the changed set -> NEEDS C3 (exit 2), even if lintable part is clean =="
init_repo; dirty_lintable; dirty_guarded
run_cf
assert_eq "C: guarded path -> exit 2" "$RC" "2"
has  "C: reports NEEDS C3" "$LOG" "NEEDS C3"
has  "C: names the guarded path" "$LOG" "include/bar.h"
assert_eq "C: still did NOT commit" "$(commits)" "1"

echo "== D: lintable C++ that cannot converge -> C1 escalates -> NEEDS C3 (exit 2) =="
init_repo; dirty_lintable
run_cf C1_NOOP=1
assert_eq "D: lint escalated -> exit 2" "$RC" "2"
has  "D: reports C1 lint escalation" "$LOG" "C1 lint escalated"
assert_eq "D: still did NOT commit" "$(commits)" "1"

echo "============================================================"
echo "PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ] || { echo "=== COMMIT-FLOW TESTS FAILED ==="; exit 1; }
echo "=== ALL COMMIT-FLOW TESTS PASSED ==="
