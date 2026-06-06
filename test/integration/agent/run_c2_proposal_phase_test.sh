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

# include/** headers are NO LONGER a flat scope rejection -- they are blast-radius-gated by the runner
# (sections W/X). A .h OUTSIDE include/ classifies as 'unknown' and stays rejected.
check_scope "lib/Foo.h"; assert_eq "K: reject lib/Foo.h (header outside include/)" "$RC" "2"
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

# A valid-but-unrecorded id has no kept audit entry -> exit 2.
env AOBUS_AGENT_WORK="$AW" bash "$RR" "p-absent" "accept" >/dev/null 2>&1
assert_eq "P: recorder rejects an unrecorded phase id (exit 2)" "$?" "2"
# The reserved 'unknown' sentinel and unsafe ids are refused up front by agent_id_ok -> exit 64.
env AOBUS_AGENT_WORK="$AW" bash "$RR" "unknown" "accept" >/dev/null 2>&1
assert_eq "P: recorder rejects the reserved 'unknown' id (exit 64)" "$?" "64"
env AOBUS_AGENT_WORK="$AW" bash "$RR" "bad id!" "accept" >/dev/null 2>&1
assert_eq "P: recorder rejects an unsafe phase id (exit 64)" "$?" "64"

echo "== Q: circuit breaker trips on a silent-wrong ONLY (Phase 7) =="
QW="$ROOT/work_breaker"; mkdir -p "$QW"
cat > "$QW/audit.log" <<'EOF'
{"ts":"t","phase_id":"q-val","skill":"execute-plan","capability":"C2","worker":"Mock Worker A","result":"proposal-validated","rounds":1,"churn":1,"assertion_delta":0,"reason":"ok"}
{"ts":"t","phase_id":"q-acc","skill":"execute-plan","capability":"C2","worker":"Mock Worker C","result":"proposal-validated","rounds":1,"churn":1,"assertion_delta":0,"reason":"ok"}
{"ts":"t","phase_id":"q-diag","skill":"execute-plan","capability":"C2","worker":"Mock Worker B","result":"proposal-diagnostic","rounds":3,"churn":1,"assertion_delta":0,"reason":"budget"}
EOF
trip_a="$QW/breaker/$(agent_breaker_slug "Mock Worker A").tripped"
trip_b="$QW/breaker/$(agent_breaker_slug "Mock Worker B").tripped"
trip_c="$QW/breaker/$(agent_breaker_slug "Mock Worker C").tripped"
env AOBUS_AGENT_WORK="$QW" bash "$RR" q-val reject "broke semantics" >/dev/null 2>&1
[ -f "$trip_a" ] && ok "Q: reject of a validated phase trips the breaker" || bad "Q: reject of a validated phase trips the breaker"
env AOBUS_AGENT_WORK="$QW" bash "$RR" q-acc accept "looks right" >/dev/null 2>&1
[ -f "$trip_c" ] && bad "Q: accept must NOT trip" || ok "Q: accept does not trip"
env AOBUS_AGENT_WORK="$QW" bash "$RR" q-diag reject "never passed validation" >/dev/null 2>&1
[ -f "$trip_b" ] && bad "Q: reject of a non-validated (diagnostic) phase must NOT trip" || ok "Q: reject of a diagnostic does not trip"

echo "== R: proposal runner refuses a breaker-tripped worker BEFORE the worker runs (Phase 7) =="
cat > "$VALID" <<'EOF'
#!/usr/bin/env bash
v_test_core() { return 0; }
declare -gA VALIDATION_ARGSPEC=([test-core]="filter 1 1")
declare -gA VALIDATION_IS_ISOLATABLE=([test-core]="1")
EOF
mkdir -p "$ROOT/work/breaker"
: > "$ROOT/work/breaker/unknown.tripped"   # label resolves to 'unknown' under the empty mock routing
rm -f "$ROOT/work/worker-ran.marker"
mock_breaker_canary() { echo ran > "$ROOT/work/worker-ran.marker"; echo edit >> "$AGENT_PROPOSAL_WORK/lib/foo.cpp"; }
export ROUTE_C2_PROPOSAL_WORKER="mock_breaker_canary"; export -f mock_breaker_canary
before_hash="$(sha256sum "$ROOT/repo/lib/foo.cpp" | awk '{print $1}')"
run_proposal "$ROOT/p_base.md"
assert_eq "R: breaker-tripped runner exits 2" "$RC" "2"
[ -f "$ROOT/work/worker-ran.marker" ] && bad "R: worker must NOT run when breaker tripped" || ok "R: worker did not run (pre-flight refusal)"
case "$LOG" in *"breaker-tripped"*) ok "R: reports breaker refusal" ;; *) bad "R: reports breaker refusal" ;; esac
assert_eq "R: real tree untouched" "$(sha256sum "$ROOT/repo/lib/foo.cpp" | awk '{print $1}')" "$before_hash"
rm -f "$ROOT/work/breaker/unknown.tripped"

echo "== S: sanitizer validations are isolatable + arg-spec'd (Phase 4) =="
cat > "$VALID" <<'EOF'
#!/usr/bin/env bash
v_test_core_asan() { echo "asan arg=$1" > "$ROOT/work/asan_out.txt"; return 0; }
v_test_core_tsan() { return 0; }
declare -gA VALIDATION_ARGSPEC=([test-core-asan]="filter 1 1" [test-core-tsan]="filter 1 1")
declare -gA VALIDATION_IS_ISOLATABLE=([test-core-asan]="1" [test-core-tsan]="1")
EOF
# shellcheck disable=SC1090
source "$VALID"
mkdir -p "$ROOT/work"
assert_rc 0 "S: test-core-asan is proposal-isolatable" agent_validate_in_repo "$ROOT/repo" "$ROOT/bld_sa" test-core-asan "[base64]"
assert_eq "S: asan validation got its filter" "$(cat "$ROOT/work/asan_out.txt")" "asan arg=[base64]"
assert_rc 0 "S: test-core-tsan is proposal-isolatable" agent_validate_in_repo "$ROOT/repo" "$ROOT/bld_st" test-core-tsan "[x]"
assert_rc 2 "S: asan rejects a path arg (wants a filter)" agent_validate_in_repo "$ROOT/repo" "$ROOT/bld_sb" test-core-asan "lib/foo.cpp"
assert_rc 2 "S: asan rejects a bad arg count" agent_validate_in_repo "$ROOT/repo" "$ROOT/bld_sc" test-core-asan "[a]" "[b]"

echo "== T: agent_clean_ignored drops gitignored runtime paths, keeps tracked source =="
GT="$ROOT/gitrepo"; mkdir -p "$GT/logs" "$GT/lib"
( cd "$GT" && git init -q && printf '*.log\n' > .gitignore )
echo noise > "$GT/logs/app.log"; echo src > "$GT/lib/foo.cpp"
agent_clean_ignored "$GT" "$GT"
[ -f "$GT/logs/app.log" ] && bad "T: gitignored *.log removed" || ok "T: gitignored *.log removed"
[ -f "$GT/lib/foo.cpp" ] && ok "T: tracked source kept" || bad "T: tracked source kept"
echo keep > "$ROOT/repo/lib/keep_marker.cpp"
agent_clean_ignored "$ROOT/repo" "$ROOT/repo"   # not a git repo -> no-op
[ -f "$ROOT/repo/lib/keep_marker.cpp" ] && ok "T: no-op on a non-git tree" || bad "T: no-op on a non-git tree"
rm -f "$ROOT/repo/lib/keep_marker.cpp"

echo "== U: proposal VALIDATES despite a worker writing a gitignored runtime file (Finding A e2e) =="
GR="$ROOT/gitrepo2"; mkdir -p "$GR/lib"
( cd "$GR" && git init -q && printf '*.log\n' > .gitignore && echo orig > lib/bar.cpp && git add -A \
    && git -c user.email=t@t -c user.name=t commit -qm init )
cat > "$VALID" <<'EOF'
#!/usr/bin/env bash
v_test_core() { return 0; }
declare -gA VALIDATION_ARGSPEC=([test-core]="filter 1 1")
declare -gA VALIDATION_IS_ISOLATABLE=([test-core]="1")
EOF
mock_worker_with_log() {
  echo edit >> "$AGENT_PROPOSAL_WORK/lib/bar.cpp"          # in-scope source edit
  mkdir -p "$AGENT_PROPOSAL_WORK/logs"
  echo "runtime noise" >> "$AGENT_PROPOSAL_WORK/logs/run.log"  # gitignored -> must be ignored, not rejected
}
export ROUTE_C2_PROPOSAL_WORKER="mock_worker_with_log"; export -f mock_worker_with_log
cat > "$ROOT/p_gi.md" <<'EOF'
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
  - lib/bar.cpp
---
Edit bar.cpp.
EOF
env AOBUS_AGENT_REPO="$GR" AOBUS_AGENT_WORK="$ROOT/work_gi" bash "$PROPOSAL" "$ROOT/p_gi.md" > "$ROOT/run_gi.log" 2>&1
assert_eq "U: validated despite gitignored worker artifact -> exit 0" "$?" "0"
case "$(cat "$ROOT/run_gi.log")" in *"validation passed"*) ok "U: reached validation (log not treated as out-of-scope)" ;; *) bad "U: reached validation"; cat "$ROOT/run_gi.log" ;; esac

echo "== V: phase id resolution (harness mints a unique id; never the 'unknown' sentinel) =="
# Reuse the A-section mock worker/validation. The audit log lives at \$AOBUS_AGENT_WORK/audit.log.
export ROUTE_C2_PROPOSAL_WORKER="mock_am_worker"
# V1: an id-less proposal still audits under a real, generated 'proposal-*' id (not "unknown"/empty).
WV="$ROOT/work_idless"; mkdir -p "$WV"
env AOBUS_AGENT_REPO="$ROOT/repo" AOBUS_AGENT_WORK="$WV" bash "$PROPOSAL" "$ROOT/p_ok.md" > "$ROOT/run_idless.log" 2>&1
assert_eq "V: id-less proposal -> exit 0" "$?" "0"
grep -Eq '"phase_id":"proposal-[0-9]{8}-[0-9]{6}-[0-9]+"' "$WV/audit.log" \
  && ok "V: id-less run mints a generated phase id" || { bad "V: generated phase id"; cat "$WV/audit.log"; }
grep -q '"phase_id":"unknown"' "$WV/audit.log" && bad "V: must never audit as 'unknown'" || ok "V: never audits as 'unknown'"
grep -q '"phase_id":""' "$WV/audit.log" && bad "V: must never audit an empty id" || ok "V: never audits an empty id"

# V2: an explicit, valid packet id is honored verbatim.
cat > "$ROOT/p_id.md" <<'EOF'
---
schema: aobus-phase-packet/v1
kind: proposal
id: base64-refactor-001
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
WV2="$ROOT/work_id"; mkdir -p "$WV2"
env AOBUS_AGENT_REPO="$ROOT/repo" AOBUS_AGENT_WORK="$WV2" bash "$PROPOSAL" "$ROOT/p_id.md" > "$ROOT/run_id.log" 2>&1
assert_eq "V: explicit-id proposal -> exit 0" "$?" "0"
grep -q '"phase_id":"base64-refactor-001"' "$WV2/audit.log" \
  && ok "V: explicit packet id is honored" || { bad "V: explicit id honored"; cat "$WV2/audit.log"; }

# V3: a malformed / reserved packet id is rejected at the schema gate (exit 64), before any staging.
for badid in "bad id!" "unknown"; do
  cat > "$ROOT/p_badid.md" <<EOF
---
schema: aobus-phase-packet/v1
kind: proposal
id: $badid
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
  run_proposal "$ROOT/p_badid.md"
  assert_eq "V: packet id '$badid' rejected (exit 64)" "$RC" "64"
done

echo "== W: header input accepted; oracle FORCED to test-core-all (atomicity) + dossier markers =="
# A low-fan-out core header: its only includer is a lib source -> blast radius 1, core-only, in budget.
mkdir -p "$ROOT/repo/include/ao/util" "$ROOT/repo/lib/util"
printf '#pragma once\nint kWidget();\n' > "$ROOT/repo/include/ao/util/Widget.h"
printf '#include <ao/util/Widget.h>\nint kWidget(){return 0;}\n' > "$ROOT/repo/lib/util/Widget.cpp"
# The packet asks for the narrow test-core (made to FAIL); the runner must force test-core-all (PASS) for
# a header change. exit 0 therefore proves the packet could not downgrade the oracle.
cat > "$VALID" <<'EOF'
#!/usr/bin/env bash
v_test_core()     { return 1; }
v_test_core_all() { return 0; }
declare -gA VALIDATION_ARGSPEC=([test-core]="filter 1 1" [test-core-all]="any 0 -")
declare -gA VALIDATION_IS_ISOLATABLE=([test-core]="1" [test-core-all]="1")
EOF
mock_hdr_worker() { echo "// private nested helper" >> "$AGENT_PROPOSAL_WORK/include/ao/util/Widget.h"; }
export ROUTE_C2_PROPOSAL_WORKER="mock_hdr_worker"; export -f mock_hdr_worker
cat > "$ROOT/p_hdr.md" <<'EOF'
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
  - include/ao/util/Widget.h
---
Add a private nested helper class to Widget.h (behavior-preserving).
EOF
WH="$ROOT/work_hdr"; mkdir -p "$WH"
env AOBUS_AGENT_REPO="$ROOT/repo" AOBUS_AGENT_WORK="$WH" bash "$PROPOSAL" "$ROOT/p_hdr.md" > "$ROOT/run_hdr.log" 2>&1
assert_eq "W: header proposal validates under forced test-core-all -> exit 0" "$?" "0"
LOGH="$(cat "$ROOT/run_hdr.log")"
case "$LOGH" in *"blast radius 1 TU"*) ok "W: reports blast radius" ;; *) bad "W: reports blast radius"; echo "$LOGH" ;; esac
man="$(ls "$WH"/proposal_*/manifest.json 2>/dev/null | head -1)"
if [ -n "$man" ]; then
  MAN="$(cat "$man")"
  case "$MAN" in *'"intent": "refactor"'*) ok "W: dossier records intent" ;; *) bad "W: dossier intent"; echo "$MAN" ;; esac
  case "$MAN" in *'"header_touched": true'*) ok "W: dossier marks header_touched" ;; *) bad "W: dossier header_touched" ;; esac
  case "$MAN" in *'"blast_radius": 1'*) ok "W: dossier records blast_radius" ;; *) bad "W: dossier blast_radius" ;; esac
else bad "W: manifest.json emitted"; fi

echo "== X: header blast radius is bounded; over-budget / non-core escalates BEFORE the worker =="
# X1: a header pulled in by 3 lib TUs, budget 2 -> escalate (no worker run).
mkdir -p "$ROOT/repo/include/ao/core" "$ROOT/repo/lib/a" "$ROOT/repo/lib/b" "$ROOT/repo/lib/c"
printf '#pragma once\nint kWide();\n' > "$ROOT/repo/include/ao/core/Wide.h"
for d in a b c; do printf '#include <ao/core/Wide.h>\nint u_%s(){return kWide();}\n' "$d" > "$ROOT/repo/lib/$d/u.cpp"; done
cat > "$ROOT/p_wide.md" <<'EOF'
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
  - include/ao/core/Wide.h
---
Touch a widely-included header.
EOF
mock_canary_x() { echo ran > "$AOBUS_AGENT_WORK/ran"; echo edit >> "$AGENT_PROPOSAL_WORK/include/ao/core/Wide.h"; }
export ROUTE_C2_PROPOSAL_WORKER="mock_canary_x"; export -f mock_canary_x
WX="$ROOT/work_wide"; mkdir -p "$WX"; rm -f "$WX/ran"
env PROPOSAL_BLAST_MAX=2 AOBUS_AGENT_REPO="$ROOT/repo" AOBUS_AGENT_WORK="$WX" bash "$PROPOSAL" "$ROOT/p_wide.md" > "$ROOT/run_wide.log" 2>&1
assert_eq "X1: over-budget header escalates -> exit 2" "$?" "2"
[ -f "$WX/ran" ] && bad "X1: worker must NOT run when over budget" || ok "X1: worker did not run (pre-staging escalation)"
case "$(cat "$ROOT/run_wide.log")" in *"exceeds budget"*) ok "X1: reports budget escalation" ;; *) bad "X1: reports budget escalation" ;; esac
case "$(cat "$WX/audit.log" 2>/dev/null)" in *'"result":"proposal-rejected"'*) ok "X1: audited as proposal-rejected" ;; *) bad "X1: audited rejected" ;; esac

# X2: a header reached only by the GTK/app frontend is not covered by the core oracle -> escalate.
mkdir -p "$ROOT/repo/include/ao/ui" "$ROOT/repo/app"
printf '#pragma once\nint kPanel();\n' > "$ROOT/repo/include/ao/ui/Panel.h"
printf '#include <ao/ui/Panel.h>\nint kPanel(){return 0;}\n' > "$ROOT/repo/app/gui.cpp"
cat > "$ROOT/p_panel.md" <<'EOF'
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
  - include/ao/ui/Panel.h
---
Touch a frontend-only header.
EOF
export ROUTE_C2_PROPOSAL_WORKER="mock_canary_x"
WP="$ROOT/work_panel"; mkdir -p "$WP"; rm -f "$WP/ran"
env AOBUS_AGENT_REPO="$ROOT/repo" AOBUS_AGENT_WORK="$WP" bash "$PROPOSAL" "$ROOT/p_panel.md" > "$ROOT/run_panel.log" 2>&1
assert_eq "X2: non-core (GTK/app) blast radius escalates -> exit 2" "$?" "2"
[ -f "$WP/ran" ] && bad "X2: worker must NOT run when non-core" || ok "X2: worker did not run"
case "$(cat "$ROOT/run_panel.log")" in *"core oracle"*) ok "X2: reports non-core escalation" ;; *) bad "X2: reports non-core escalation" ;; esac

echo "== Y: intent=behavior-change requires a registered test change (deterministic obligation) =="
mkdir -p "$ROOT/repo/test/unit"
printf 'TEST_CASE("widget","[widget]"){}\n' > "$ROOT/repo/test/unit/WidgetTest.cpp"
printf 'add_executable(ao_test\n  unit/WidgetTest.cpp\n)\n' > "$ROOT/repo/test/CMakeLists.txt"
cat > "$VALID" <<'EOF'
#!/usr/bin/env bash
v_test_core() { return 0; }
declare -gA VALIDATION_ARGSPEC=([test-core]="filter 1 1")
declare -gA VALIDATION_IS_ISOLATABLE=([test-core]="1")
EOF
cat > "$ROOT/p_bc.md" <<'EOF'
---
schema: aobus-phase-packet/v1
kind: proposal
intent: behavior-change
skill: execute-plan
capability: C2
mode: proposal
validation: test-core
validation_args:
  - [widget]
inputs:
  - lib/util/Widget.cpp
  - test/unit/WidgetTest.cpp
---
Change kWidget()'s return value and update the test that pins it.
EOF
# Y1: worker edits only the source, not the test -> obligation fails -> reject.
mock_bc_notest() { echo "// behavior change" >> "$AGENT_PROPOSAL_WORK/lib/util/Widget.cpp"; }
export ROUTE_C2_PROPOSAL_WORKER="mock_bc_notest"; export -f mock_bc_notest
WB1="$ROOT/work_bc1"; mkdir -p "$WB1"
env AOBUS_AGENT_REPO="$ROOT/repo" AOBUS_AGENT_WORK="$WB1" bash "$PROPOSAL" "$ROOT/p_bc.md" > "$ROOT/run_bc1.log" 2>&1
assert_eq "Y1: behavior-change without a test change -> exit 2" "$?" "2"
case "$(cat "$ROOT/run_bc1.log")" in *"no registered test was changed"*) ok "Y1: reports the missing test" ;; *) bad "Y1: reports missing test"; cat "$ROOT/run_bc1.log" ;; esac
# Y2: worker edits BOTH the source and the registered test -> obligation met -> validates.
mock_bc_withtest() {
  echo "// behavior change" >> "$AGENT_PROPOSAL_WORK/lib/util/Widget.cpp"
  printf 'TEST_CASE("widget2","[widget]"){}\n' >> "$AGENT_PROPOSAL_WORK/test/unit/WidgetTest.cpp"
}
export ROUTE_C2_PROPOSAL_WORKER="mock_bc_withtest"; export -f mock_bc_withtest
WB2="$ROOT/work_bc2"; mkdir -p "$WB2"
env AOBUS_AGENT_REPO="$ROOT/repo" AOBUS_AGENT_WORK="$WB2" bash "$PROPOSAL" "$ROOT/p_bc.md" > "$ROOT/run_bc2.log" 2>&1
assert_eq "Y2: behavior-change with a test change -> exit 0" "$?" "0"
man2="$(ls "$WB2"/proposal_*/manifest.json 2>/dev/null | head -1)"
[ -n "$man2" ] && case "$(cat "$man2")" in *'"assertion_delta": 1'*) ok "Y2: dossier records assertion_delta" ;; *) bad "Y2: assertion_delta"; cat "$man2" ;; esac

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
