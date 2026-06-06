#!/usr/bin/env bash
# ============================================================================
# run_c2_proposal_phase_test.sh
# Deterministic, offline coverage for the C2 proposal executor skeleton.
# ============================================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROPOSAL="$(cd "$SCRIPT_DIR/../../.." && pwd)/script/agent/c2_proposal_phase.sh"
DISPATCH="$(cd "$SCRIPT_DIR/../../.." && pwd)/script/agent/dispatch.sh"

PASS=0; FAIL=0
ok()  { PASS=$((PASS + 1)); printf '  ok   %s\n' "$1"; }
bad() { FAIL=$((FAIL + 1)); printf '  FAIL %s\n' "$1"; }
assert_eq() { [ "$2" = "$3" ] && ok "$1" || bad "$1 (got [$2], want [$3])"; }
assert_rc() { local e="$1" d="$2"; shift 2; "$@" >/dev/null 2>&1; local r=$?; [ "$r" -eq "$e" ] && ok "$d" || bad "$d (rc=$r, want $e)"; }

ROOT="$(mktemp -d)"
# trap 'rm -rf "$ROOT"' EXIT

# --- test doubles ------------------------------------------------------------
ROUTING="$ROOT/mock-routing.env"
cat > "$ROUTING" <<'EOF'
#!/usr/bin/env bash
EOF

VALID="$ROOT/mock-validation.env"
cat > "$VALID" <<'EOF'
#!/usr/bin/env bash
v_test_core() { return 0; }
declare -gA VALIDATION_ARGSPEC=([test-core]="filter 1 1")
declare -gA VALIDATION_IS_ISOLATABLE=([test-core]="1")
EOF

export AOBUS_ROUTING_ENV="$ROUTING" AOBUS_VALIDATION_ENV="$VALID"

run_proposal() {
  local packet="$1"; shift
  env AOBUS_AGENT_REPO="$ROOT/repo" AOBUS_AGENT_WORK="$ROOT/work" "$@" \
    bash "$PROPOSAL" "$packet" > "$ROOT/run.log" 2>&1
  RC=$?
  LOG="$(cat "$ROOT/run.log")"
}

run_dispatch() {
  local packet="$1"; shift
  env AOBUS_AGENT_REPO="$ROOT/repo" AOBUS_AGENT_WORK="$ROOT/work" "$@" \
    bash "$DISPATCH" "$packet" > "$ROOT/run.log" 2>&1
  RC=$?
  LOG="$(cat "$ROOT/run.log")"
}

mkdir -p "$ROOT/repo/lib"
touch "$ROOT/repo/lib/foo.cpp"

echo "== A: valid proposal packet reaches the skeleton -> PASS =="
cat > "$ROOT/p_ok.md" <<'EOF'
---
schema: aobus-phase-packet/v1
kind: proposal
skill: execute-plan
capability: C2
mode: proposal
validation: test-core
validation_args:
  - [audio]
inputs:
  - lib/foo.cpp
---
Implement it.
EOF

# Mock validation and worker for Phase 6 execution in tests A-M
cat > "$VALID" <<'EOF'
#!/usr/bin/env bash
v_test_core() { return 0; }
declare -gA VALIDATION_ARGSPEC=([test-core]="filter 1 1")
declare -gA VALIDATION_IS_ISOLATABLE=([test-core]="1")
EOF
export AOBUS_VALIDATION_ENV="$VALID"

export ROUTE_C2_PROPOSAL_WORKER="mock_am_worker"
mock_am_worker() {
  local f
  f=$(grep -A 1 'ALLOWED INPUTS:' "$AGENT_PROMPT_FILE" | tail -n 1 | sed 's/^- //')
  mkdir -p "$(dirname "$AGENT_PROPOSAL_WORK/$f")"
  echo "edit" >> "$AGENT_PROPOSAL_WORK/$f"
}
export -f mock_am_worker

run_proposal "$ROOT/p_ok.md"; assert_eq "A: valid proposal -> exit 0" "$RC" "0"
case "$LOG" in *"proposal: running baseline validation"*) ok "A: reaches skeleton" ;; *) bad "A: reaches skeleton" ;; esac

echo "== B: missing opening frontmatter marker is rejected =="
cat > "$ROOT/p_no_front.md" <<'EOF'
schema: aobus-phase-packet/v1
kind: proposal
skill: execute-plan
capability: C2
mode: proposal
validation: test-core
inputs:
  - lib/foo.cpp
---
# The plan
EOF
run_proposal "$ROOT/p_no_front.md"
assert_eq "B: missing frontmatter -> exit 64" "$RC" "64"

echo "== C: kind: request with mode: proposal is rejected by proposal runner =="
cat > "$ROOT/p_request.md" <<'EOF'
---
schema: aobus-phase-packet/v1
kind: request
skill: execute-plan
capability: C2
mode: proposal
validation: test-core
inputs:
  - lib/foo.cpp
---
# The plan
EOF
run_proposal "$ROOT/p_request.md"
assert_eq "C: request packet -> exit 64" "$RC" "64"
case "$LOG" in *"kind must be 'proposal'"*) ok "C: complains about kind" ;; *) bad "C: complains about kind" ;; esac

echo "== D: unknown proposal keys are rejected =="
cat > "$ROOT/p_unknown_key.md" <<'EOF'
---
schema: aobus-phase-packet/v1
kind: proposal
skill: execute-plan
capability: C2
mode: proposal
validation: test-core
inputs:
  - lib/foo.cpp
unknown_key: true
---
# The plan
EOF
run_proposal "$ROOT/p_unknown_key.md"
assert_eq "D: unknown key -> exit 64" "$RC" "64"
case "$LOG" in *"unknown key 'unknown_key'"*) ok "D: complains about unknown key" ;; *) bad "D: complains about unknown key" ;; esac

echo "== E: wrong skill is rejected =="
cat > "$ROOT/p_wrong_skill.md" <<'EOF'
---
schema: aobus-phase-packet/v1
kind: proposal
skill: use-clang-tidy
capability: C2
mode: proposal
validation: test-core
inputs:
  - lib/foo.cpp
---
# The plan
EOF
run_proposal "$ROOT/p_wrong_skill.md"
assert_eq "E: wrong skill -> exit 64" "$RC" "64"
case "$LOG" in *"skill must be execute-plan"*) ok "E: complains about skill" ;; *) bad "E: complains about skill" ;; esac

echo "== F: wrong capability is rejected =="
cat > "$ROOT/p_wrong_cap.md" <<'EOF'
---
schema: aobus-phase-packet/v1
kind: proposal
skill: execute-plan
capability: C1
mode: proposal
validation: test-core
inputs:
  - lib/foo.cpp
---
# The plan
EOF
run_proposal "$ROOT/p_wrong_cap.md"
assert_eq "F: wrong capability -> exit 64" "$RC" "64"
case "$LOG" in *"capability must be C2"*) ok "F: complains about capability" ;; *) bad "F: complains about capability" ;; esac

echo "== G: wrong mode is rejected =="
cat > "$ROOT/p_wrong_mode.md" <<'EOF'
---
schema: aobus-phase-packet/v1
kind: proposal
skill: execute-plan
capability: C2
mode: normal
validation: test-core
inputs:
  - lib/foo.cpp
---
# The plan
EOF
run_proposal "$ROOT/p_wrong_mode.md"
assert_eq "G: wrong mode -> exit 64" "$RC" "64"
case "$LOG" in *"mode must be proposal"*) ok "G: complains about mode" ;; *) bad "G: complains about mode" ;; esac

echo "== H: missing required field (inputs) is rejected =="
cat > "$ROOT/p_missing.md" <<'EOF'
---
schema: aobus-phase-packet/v1
kind: proposal
skill: execute-plan
capability: C2
mode: proposal
validation: test-core
---
# The plan
EOF
run_proposal "$ROOT/p_missing.md"
assert_eq "H: missing inputs -> exit 64" "$RC" "64"
case "$LOG" in *"missing 'inputs'"*) ok "H: complains about inputs" ;; *) bad "H: complains about inputs" ;; esac

echo "== I: missing body is rejected =="
cat > "$ROOT/p_no_body.md" <<'EOF'
---
schema: aobus-phase-packet/v1
kind: proposal
skill: execute-plan
capability: C2
mode: proposal
validation: test-core
inputs:
  - lib/foo.cpp
---
EOF
run_proposal "$ROOT/p_no_body.md"
assert_eq "I: missing body -> exit 64" "$RC" "64"
case "$LOG" in *"body cannot be empty"*) ok "I: complains about body" ;; *) bad "I: complains about body" ;; esac

echo "== J: dispatch.sh rejects proposal packets =="
run_dispatch "$ROOT/p_ok.md"
assert_eq "J: dispatch rejects proposal -> exit 64" "$RC" "64"
case "$LOG" in *"must run via c2_proposal_phase.sh"*) ok "J: complains about dispatch" ;; *) bad "J: complains about dispatch" ;; esac

echo "== K: scope gate rejections =="

mkdir -p "$ROOT/repo/include" "$ROOT/repo/lib" "$ROOT/repo/script/agent" "$ROOT/repo/doc/design" "$ROOT/repo/.agents/skills/foo" "$ROOT/repo/test"
touch "$ROOT/repo/include/Foo.h"
touch "$ROOT/repo/lib/Foo.h"
touch "$ROOT/repo/script/foo.sh"
touch "$ROOT/repo/script/agent/foo.sh"
touch "$ROOT/repo/doc/design/foo.md"
touch "$ROOT/repo/.agents/skills/foo/SKILL.md"
touch "$ROOT/repo/.clang-tidy"
touch "$ROOT/repo/CMakeLists.txt"
touch "$ROOT/repo/lib/CMakeLists.txt"
mkdir -p "$ROOT/repo/lib/dir" "$ROOT/repo/app/nested"
touch "$ROOT/repo/app/foo.cpp" "$ROOT/repo/app/nested/bar.cpp"
ln -s "$ROOT/repo/lib/foo.cpp" "$ROOT/repo/lib/symlink.cpp"

check_scope() {
  local p="$1"
  cat > "$ROOT/p_scope.md" <<EOF
---
schema: aobus-phase-packet/v1
kind: proposal
skill: execute-plan
capability: C2
mode: proposal
validation: test-core
validation_args:
  - [audio]
inputs:
  - $p
---
# The plan
EOF
  run_proposal "$ROOT/p_scope.md"
}

check_scope "include/Foo.h"; assert_eq "K: reject include/Foo.h" "$RC" "2"
check_scope "lib/Foo.h"; assert_eq "K: reject lib/Foo.h" "$RC" "2"
check_scope "script/foo.sh"; assert_eq "K: reject script/foo.sh" "$RC" "2"
check_scope "script/agent/foo.sh"; assert_eq "K: reject script/agent/foo.sh" "$RC" "2"
check_scope "doc/design/foo.md"; assert_eq "K: reject doc/design/foo.md" "$RC" "2"
check_scope ".agents/skills/foo/SKILL.md"; assert_eq "K: reject .agents/skills/foo/SKILL.md" "$RC" "2"
check_scope ".clang-tidy"; assert_eq "K: reject .clang-tidy" "$RC" "2"
check_scope "CMakeLists.txt"; assert_eq "K: reject CMakeLists.txt" "$RC" "2"
check_scope "lib/CMakeLists.txt"; assert_eq "K: reject lib/CMakeLists.txt" "$RC" "2"
check_scope "missing.cpp"; assert_eq "K: reject missing file" "$RC" "2"
check_scope "lib/dir"; assert_eq "K: reject directory" "$RC" "2"
check_scope "lib/symlink.cpp"; assert_eq "K: reject symlink" "$RC" "2"

echo "== L: duplicate inputs are rejected =="
cat > "$ROOT/p_dup.md" <<EOF
---
schema: aobus-phase-packet/v1
kind: proposal
skill: execute-plan
capability: C2
mode: proposal
validation: test-core
validation_args:
  - [audio]
inputs:
  - lib/foo.cpp
  - lib/foo.cpp
---
# The plan
EOF
run_proposal "$ROOT/p_dup.md"
assert_eq "L: reject duplicate inputs" "$RC" "2"
case "$LOG" in *"duplicate input"*) ok "L: complains about duplicate" ;; *) bad "L: complains about duplicate" ;; esac

echo "== M: scope gate acceptances =="
mkdir -p "$ROOT/repo/app/nested"
touch "$ROOT/repo/app/foo.cpp"
touch "$ROOT/repo/app/nested/bar.cpp"

check_scope "lib/foo.cpp"; assert_eq "M: accept lib/foo.cpp" "$RC" "0"
check_scope "app/foo.cpp"; assert_eq "M: accept app/foo.cpp" "$RC" "0"
check_scope "app/nested/bar.cpp"; assert_eq "M: accept app/nested/bar.cpp" "$RC" "0"

echo "== N: agent_validate_in_repo (Phase 4) =="
cat > "$VALID" <<'EOF'
#!/usr/bin/env bash
v_test_core() {
  echo "src=$AOBUS_AGENT_REPO build=$BUILD_DIR arg=$1" > "$ROOT/work/v_out.txt"
  return 0
}
v_tidy() { return 0; }
declare -gA VALIDATION_ARGSPEC=([test-core]="filter 1 1" [tidy]="path 1 -")
declare -gA VALIDATION_IS_ISOLATABLE=([test-core]="1")
EOF

# shellcheck disable=SC1091
source "$SCRIPT_DIR/../../../script/agent/common.sh"
# shellcheck disable=SC1091
source "$VALID"

mkdir -p "$ROOT/work"
assert_rc 0 "test-core is isolatable" agent_validate_in_repo "$ROOT/repo" "$ROOT/bld" test-core "[x]"
assert_eq "validation receives environment" "$(cat "$ROOT/work/v_out.txt")" "src=$ROOT/repo build=$ROOT/bld arg=[x]"

assert_rc 2 "tidy is not proposal-isolatable" agent_validate_in_repo "$ROOT/repo" "$ROOT/bld" tidy "lib/foo.cpp"
assert_rc 2 "unknown id rejected" agent_validate_in_repo "$ROOT/repo" "$ROOT/bld" bogus "arg"
assert_rc 2 "wrong arg type rejected" agent_validate_in_repo "$ROOT/repo" "$ROOT/bld" test-core "lib/foo.cpp"

echo "== O: Runner Loop (Phase 6) =="

cat > "$VALID" <<'EOF'
#!/usr/bin/env bash
v_test_core() { return 1; }
declare -gA VALIDATION_ARGSPEC=([test-core]="filter 1 1")
declare -gA VALIDATION_IS_ISOLATABLE=([test-core]="1")
EOF

cat > "$ROOT/p_base.md" <<'EOF'
---
schema: aobus-phase-packet/v1
kind: proposal
skill: execute-plan
capability: C2
mode: proposal
validation: test-core
validation_args:
  - [x]
inputs:
  - lib/foo.cpp
---
Task
EOF

run_proposal "$ROOT/p_base.md"; assert_eq "O: baseline validation failure exits 2" "$RC" "2"

cat > "$VALID" <<'EOF'
#!/usr/bin/env bash
v_test_core() {
  if [ "$(cat "$BUILD_DIR/fail" 2>/dev/null || echo 0)" = "1" ]; then
    return 1
  fi
  return 0
}
declare -gA VALIDATION_ARGSPEC=([test-core]="filter 1 1")
declare -gA VALIDATION_IS_ISOLATABLE=([test-core]="1")
EOF

export ROUTE_C2_PROPOSAL_WORKER="mock_worker"
mock_worker() {
  echo "edit" >> "$AGENT_PROPOSAL_WORK/lib/foo.cpp"
  mkdir -p "$BUILD_DIR"
  echo "1" > "$BUILD_DIR/fail"
}
export -f mock_worker

export MAX_PROPOSAL_ROUNDS=2

run_proposal "$ROOT/p_base.md"; assert_eq "O: budget exhausted exits 1" "$RC" "1"

mock_worker_success() {
  echo "edit" >> "$AGENT_PROPOSAL_WORK/lib/foo.cpp"
}
export ROUTE_C2_PROPOSAL_WORKER="mock_worker_success"
export -f mock_worker_success

run_proposal "$ROOT/p_base.md"
assert_eq "O: success exits 0" "$RC" "0"
if [ "$RC" -ne 0 ]; then cat "$ROOT/run.log"; fi

mock_worker_out_of_scope() {
  echo "edit" >> "$AGENT_PROPOSAL_WORK/lib/foo.cpp"
  echo "edit" >> "$AGENT_PROPOSAL_WORK/lib/out.cpp"
}
export ROUTE_C2_PROPOSAL_WORKER="mock_worker_out_of_scope"
export -f mock_worker_out_of_scope

run_proposal "$ROOT/p_base.md"; assert_eq "O: out of scope exhausts budget exits 1" "$RC" "1"

mock_worker_crash() {
  exit 1
}
export ROUTE_C2_PROPOSAL_WORKER="mock_worker_crash"
export -f mock_worker_crash

run_proposal "$ROOT/p_base.md"; assert_eq "O: worker crash exhausts budget exits 1" "$RC" "1"

mock_worker_mutate_real() {
  echo "mutate" >> "$AOBUS_AGENT_REPO/lib/foo.cpp"
  echo "edit" >> "$AGENT_PROPOSAL_WORK/lib/foo.cpp"
}
export ROUTE_C2_PROPOSAL_WORKER="mock_worker_mutate_real"
export -f mock_worker_mutate_real

# Need to make repo writable for the worker to actually mutate it, since test setup might not matter, wait, repo is writable.
run_proposal "$ROOT/p_base.md"; assert_eq "O: tree mutation exits 2" "$RC" "2"

# restore file
sed -i '/mutate/d' "$ROOT/repo/lib/foo.cpp"

echo "== P: record_review.sh tests (Phase 7) =="
RR="$(cd "$SCRIPT_DIR/../../.." && pwd)/script/agent/record_review.sh"
mkdir -p "$ROOT/work_rr"
AW="$ROOT/work_rr"

# Create fake audit entries
cat > "$AW/audit.log" <<'EOF'
{"ts":"2023-01-01T00:00:00Z","phase_id":"p-val","result":"proposal-validated"}
{"ts":"2023-01-01T00:00:00Z","phase_id":"p-diag","result":"proposal-diagnostic"}
EOF

env AOBUS_AGENT_WORK="$AW" bash "$RR" "p-val" "accept" >/dev/null 2>&1
assert_eq "P: validated proposal can record accept" "$?" "0"

env AOBUS_AGENT_WORK="$AW" bash "$RR" "p-diag" "reject" >/dev/null 2>&1
assert_eq "P: diagnostic proposal can record reject" "$?" "0"

env AOBUS_AGENT_WORK="$AW" bash "$RR" "p-val" "modify" >/dev/null 2>&1
assert_eq "P: modify is accepted for proposal phases (conflict because already accepted, wait modify is a new test)" "$?" "2" # wait, we need a fresh one

cat >> "$AW/audit.log" <<'EOF'
{"ts":"2023-01-01T00:00:00Z","phase_id":"p-mod","result":"proposal-validated"}
EOF
env AOBUS_AGENT_WORK="$AW" bash "$RR" "p-mod" "modify" >/dev/null 2>&1
assert_eq "P: modify is accepted for proposal phases" "$?" "0"

env AOBUS_AGENT_WORK="$AW" bash "$RR" "unknown" "accept" >/dev/null 2>&1
assert_eq "P: proposal recorder rejects unknown phase IDs" "$?" "2"

echo "============================================================"
if [ "$FAIL" -eq 0 ]; then
  echo "PASS=$PASS FAIL=$FAIL"
  echo "=== ALL PROPOSAL TESTS PASSED ==="
  exit 0
else
  echo "PASS=$PASS FAIL=$FAIL"
  echo "=== PROPOSAL TESTS FAILED ==="
  exit 1
fi
