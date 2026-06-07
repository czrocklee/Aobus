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
export ROOT
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
v_test_all() { return 0; }
declare -gA VALIDATION_ARGSPEC=([test-core]="filter 1 1" [test-all]="any 0 -")
declare -gA VALIDATION_IS_ISOLATABLE=([test-core]="1" [test-all]="1")
EOF

export AOBUS_ROUTING_ENV="$ROUTING" AOBUS_VALIDATION_ENV="$VALID"

run_proposal() {
  local packet="$1"; shift
  env AOBUS_AGENT_REPO="$ROOT/repo" AOBUS_AGENT_WORK="$ROOT/work" "$@" \
    bash "$PROPOSAL" "$packet" > "$ROOT/run.log" 2>&1
  RC=$?
  LOG="$(cat "$ROOT/run.log")"
}

latest_proposal_dir() {
  ls -td "$ROOT/work"/proposal_* 2>/dev/null | head -1
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
v_test_all() { return 0; }
declare -gA VALIDATION_ARGSPEC=([test-core]="filter 1 1" [test-all]="any 0 -")
declare -gA VALIDATION_IS_ISOLATABLE=([test-core]="1" [test-all]="1")
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
adir="$(latest_proposal_dir)"
assert_eq "A: phase exit artifact records success" "$(sed -n 's/^exit_code=//p' "$adir/phase-exit.env")" "0"
[ -s "$adir/baseline.before.ccache" ] && ok "A: baseline before ccache stats emitted" || bad "A: baseline before ccache stats emitted"
[ -s "$adir/baseline.after.ccache" ] && ok "A: baseline after ccache stats emitted" || bad "A: baseline after ccache stats emitted"
[ -s "$adir/round1.worker.before.ccache" ] && ok "A: worker before ccache stats emitted" || bad "A: worker before ccache stats emitted"
[ -s "$adir/round1.worker.after.ccache" ] && ok "A: worker after ccache stats emitted" || bad "A: worker after ccache stats emitted"
[ -s "$adir/round1.validation.before.ccache" ] && ok "A: validation before ccache stats emitted" || bad "A: validation before ccache stats emitted"
[ -s "$adir/round1.validation.after.ccache" ] && ok "A: validation after ccache stats emitted" || bad "A: validation after ccache stats emitted"

echo "== A2: the worker prompt carries the reference-skills self-load hint =="
# The runner tells the worker it MAY consult .agents/skills/<topic>/SKILL.md in its work copy, so a
# test-writing plan needs no inlined conventions. Probe the assembled prompt the worker actually saw.
export PROMPT_PROBE="$ROOT/prompt_probe.txt"; : > "$PROMPT_PROBE"
mock_probe_worker() {
  cp "$AGENT_PROMPT_FILE" "$PROMPT_PROBE"
  local f
  f=$(grep -A 1 'ALLOWED INPUTS:' "$AGENT_PROMPT_FILE" | tail -n 1 | sed 's/^- //')
  mkdir -p "$(dirname "$AGENT_PROPOSAL_WORK/$f")"
  echo "edit" >> "$AGENT_PROPOSAL_WORK/$f"
}
export -f mock_probe_worker
run_proposal "$ROOT/p_ok.md" ROUTE_C2_PROPOSAL_WORKER=mock_probe_worker
assert_eq "A2: proposal still validates -> exit 0" "$RC" "0"
case "$(cat "$PROMPT_PROBE")" in *".agents/skills/"*) ok "A2: prompt includes reference-skills hint" ;; *) bad "A2: prompt includes reference-skills hint" ;; esac

echo "== A4: bwrap path view gives the worker stable repo/build/out paths =="
export BWRAP_WORKER_ENV_PROBE="$ROOT/bwrap_worker_env_probe.txt"; : > "$BWRAP_WORKER_ENV_PROBE"
export BWRAP_WORKER_PROMPT_PROBE="$ROOT/bwrap_worker_prompt_probe.txt"; : > "$BWRAP_WORKER_PROMPT_PROBE"
before_bwrap_hash="$(sha256sum "$ROOT/repo/lib/foo.cpp" | awk '{print $1}')"
mock_bwrap_worker() {
  cp "$AGENT_PROMPT_FILE" "$BWRAP_WORKER_PROMPT_PROBE"
  {
    printf 'BUILD_DIR=%s\n' "${BUILD_DIR:-}"
    printf 'AGENT_PROPOSAL_BUILD_DIR=%s\n' "${AGENT_PROPOSAL_BUILD_DIR:-}"
    printf 'AOBUS_AGENT_REPO=%s\n' "${AOBUS_AGENT_REPO:-}"
    printf 'AGENT_PROPOSAL_WORK=%s\n' "${AGENT_PROPOSAL_WORK:-}"
    printf 'AGENT_PROMPT_FILE=%s\n' "${AGENT_PROMPT_FILE:-}"
    printf 'AGENT_PROPOSAL_OUT=%s\n' "${AGENT_PROPOSAL_OUT:-}"
  } > "$BWRAP_WORKER_ENV_PROBE"
  local f
  f=$(grep -A 1 'ALLOWED INPUTS:' "$AGENT_PROMPT_FILE" | tail -n 1 | sed 's/^- //')
  mkdir -p "$(dirname "$AGENT_PROPOSAL_WORK/$f")"
  echo "edit" >> "$AGENT_PROPOSAL_WORK/$f"
}
export -f mock_bwrap_worker
if command -v bwrap >/dev/null 2>&1; then
  run_proposal "$ROOT/p_ok.md" ROUTE_C2_PROPOSAL_WORKER=mock_bwrap_worker
  assert_eq "A4: bwrap path-view proposal validates -> exit 0" "$RC" "0"
  bwrap_build_dir="$(sed -n 's/^BUILD_DIR=//p' "$BWRAP_WORKER_ENV_PROBE")"
  bwrap_declared_build_dir="$(sed -n 's/^AGENT_PROPOSAL_BUILD_DIR=//p' "$BWRAP_WORKER_ENV_PROBE")"
  bwrap_repo="$(sed -n 's/^AOBUS_AGENT_REPO=//p' "$BWRAP_WORKER_ENV_PROBE")"
  bwrap_work="$(sed -n 's/^AGENT_PROPOSAL_WORK=//p' "$BWRAP_WORKER_ENV_PROBE")"
  bwrap_prompt="$(sed -n 's/^AGENT_PROMPT_FILE=//p' "$BWRAP_WORKER_ENV_PROBE")"
  bwrap_out="$(sed -n 's/^AGENT_PROPOSAL_OUT=//p' "$BWRAP_WORKER_ENV_PROBE")"
  assert_eq "A4: bwrap BUILD_DIR is main-cache-shaped build view" "$bwrap_build_dir" "/tmp/build/debug"
  assert_eq "A4: bwrap BUILD_DIR equals AGENT_PROPOSAL_BUILD_DIR" "$bwrap_build_dir" "$bwrap_declared_build_dir"
  assert_eq "A4: bwrap AOBUS_AGENT_REPO is the stable repo view" "$bwrap_repo" "$ROOT/repo"
  assert_eq "A4: bwrap AGENT_PROPOSAL_WORK is the stable repo view" "$bwrap_work" "$ROOT/repo"
  assert_eq "A4: bwrap prompt file uses stable out view" "$bwrap_prompt" "/agent/out/prompt.md"
  assert_eq "A4: bwrap output dir uses stable out view" "$bwrap_out" "/agent/out"
  case "$(cat "$BWRAP_WORKER_PROMPT_PROBE")" in *"BUILD_DIR=/tmp/build/debug"*) ok "A4: prompt names the bwrap build view" ;; *) bad "A4: prompt names the bwrap build view" ;; esac
  case "$(cat "$BWRAP_WORKER_PROMPT_PROBE")" in *"build-work is reserved for the harness oracle"*) ok "A4: prompt reserves build-work for harness validation" ;; *) bad "A4: prompt reserves build-work for harness validation" ;; esac
  assert_eq "A4: real repo remains untouched by bwrap worker edit" "$(sha256sum "$ROOT/repo/lib/foo.cpp" | awk '{print $1}')" "$before_bwrap_hash"
else
  ok "A4: bwrap worker path-view skipped (bwrap missing)"
fi

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
touch "$ROOT/repo/lib/notes.txt"
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

# include/** AND private lib/app headers are NO LONGER a flat scope rejection -- they are accepted and
# falsified by the single forced full-suite oracle (sections W/Z). A non-source extension classifies as
# 'unknown' and stays rejected at the input gate.
check_scope "lib/notes.txt"; assert_eq "K: reject lib/notes.txt (unknown, non-source ext)" "$RC" "2"
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

echo "== N: agent_validate_in_repo runs the oracle under the bwrap path view =="
cat > "$VALID" <<'EOF'
#!/usr/bin/env bash
v_test_core() {
  echo "src=$AOBUS_AGENT_REPO build=$BUILD_DIR arg=$1" > "$ROOT/work/v_out.txt"
  echo "ccache=${CCACHE_DIR:-} readonly=${CCACHE_READONLY:-unset} basedir=${CCACHE_BASEDIR:-} deps=${AOBUS_CMAKE_DEPS_DIR:-} fetch=${FETCHCONTENT_BASE_DIR:-}" > "$ROOT/work/v_cache.txt"
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

mkdir -p "$ROOT/work" "$ROOT/repo/.cache/ccache" "$ROOT/bin"
cat > "$ROOT/bin/cmake" <<EOF
#!/usr/bin/env bash
printf '%s\n' "\$@" >> "$ROOT/work/cmake_args.txt"
exit 0
EOF
chmod +x "$ROOT/bin/cmake"
rm -f "$ROOT/work/cmake_args.txt" "$ROOT/work/v_out.txt" "$ROOT/work/v_cache.txt"
# The proposal oracle always runs under bwrap: the staged copy ($ROOT/repo here) is presented at the
# stable real-repo path and the build dir at the main-cache-shaped view, so plain ccache hits across
# sandboxes -- no source/build path leaks into the cache key, so no compiler-arg rewriter is needed.
run_validate() {
  AGENT_REPO="$ROOT/repo" PATH="$ROOT/bin:$PATH" AOBUS_AGENT_CCACHE_PROGRAM="$ROOT/bin/fakeccache" \
    agent_validate_in_repo "$ROOT/repo" "$ROOT/bld" test-core "[x]"
}
if command -v bwrap >/dev/null 2>&1; then
  assert_rc 0 "test-core is isolatable" run_validate
  assert_eq "oracle sees stable real-repo-shaped paths" "$(cat "$ROOT/work/v_out.txt")" "src=$ROOT/repo build=/tmp/build/debug arg=[x]"
  assert_eq "oracle gets bwrap-mapped writable caches" "$(cat "$ROOT/work/v_cache.txt")" "ccache=$ROOT/repo/.cache/ccache readonly=unset basedir=$ROOT/repo deps=/tmp/build/debug/_deps fetch=/tmp/build/debug/_deps"
  assert_rc 0 "configure uses the plain ccache launcher (no srcroot rewriter)" grep -qx -- "-DCCACHE_PROGRAM=$ROOT/bin/fakeccache" "$ROOT/work/cmake_args.txt"
  assert_rc 0 "configure uses the bwrap build view" grep -qx -- "/tmp/build/debug" "$ROOT/work/cmake_args.txt"
  assert_rc 0 "configure uses the stable source view" grep -qx -- "$ROOT/repo" "$ROOT/work/cmake_args.txt"
  assert_rc 0 "configure uses the bwrap CMake dependency base" grep -qx -- "-DFETCHCONTENT_BASE_DIR=/tmp/build/debug/_deps" "$ROOT/work/cmake_args.txt"
  [ ! -e "$ROOT/repo/.cache/build-root" ] && ok "no ccache-srcroot build alias is created" || bad "no ccache-srcroot build alias is created"
else
  ok "agent_validate_in_repo bwrap checks skipped (bwrap missing)"
fi

assert_rc 2 "tidy is not proposal-isolatable" agent_validate_in_repo "$ROOT/repo" "$ROOT/bld" tidy "lib/foo.cpp"
assert_rc 2 "unknown id rejected" agent_validate_in_repo "$ROOT/repo" "$ROOT/bld" bogus "arg"
assert_rc 2 "wrong arg type rejected" agent_validate_in_repo "$ROOT/repo" "$ROOT/bld" test-core "lib/foo.cpp"

echo "== O: Runner Loop (Phase 6) =="

cat > "$VALID" <<'EOF'
#!/usr/bin/env bash
v_test_all() { return 1; }
declare -gA VALIDATION_ARGSPEC=([test-all]="any 0 -")
declare -gA VALIDATION_IS_ISOLATABLE=([test-all]="1")
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
odir="$(latest_proposal_dir)"
assert_eq "O: baseline failure exit artifact records rejection" "$(sed -n 's/^exit_code=//p' "$odir/phase-exit.env")" "2"
[ -s "$odir/baseline.before.ccache" ] && ok "O: baseline failure before ccache stats emitted" || bad "O: baseline failure before ccache stats emitted"
[ -s "$odir/baseline.after.ccache" ] && ok "O: baseline failure after ccache stats emitted" || bad "O: baseline failure after ccache stats emitted"
[ ! -e "$odir/round1.worker.before.ccache" ] && ok "O: baseline failure does not emit worker stats" || bad "O: baseline failure does not emit worker stats"

cat > "$VALID" <<'EOF'
#!/usr/bin/env bash
v_test_all() {
  if [ "$(cat "$BUILD_DIR/fail" 2>/dev/null || echo 0)" = "1" ]; then
    return 1
  fi
  return 0
}
declare -gA VALIDATION_ARGSPEC=([test-all]="any 0 -")
declare -gA VALIDATION_IS_ISOLATABLE=([test-all]="1")
EOF

export ROUTE_C2_PROPOSAL_WORKER="mock_worker"
mock_worker() {
  echo "edit" >> "$AGENT_PROPOSAL_WORK/lib/foo.cpp"
  mkdir -p "$AGENT_PROPOSAL_OUT/build-work"
  echo "1" > "$AGENT_PROPOSAL_OUT/build-work/fail"
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

# Every round is rejected wholesale for the out-of-scope file, so NO usable in-scope patch is ever
# produced -> exit 2 (rejected), NOT exit 1 (which means "an in-scope patch was produced but never green").
run_proposal "$ROOT/p_base.md"; assert_eq "O: out-of-scope-only -> no usable patch -> exit 2" "$RC" "2"

# A real worker is an external subprocess; model a crash as a non-zero return that made no edits (using
# `exit` here would terminate the runner's own shell rather than the worker). No patch -> exit 2.
mock_worker_crash() {
  return 1
}
export ROUTE_C2_PROPOSAL_WORKER="mock_worker_crash"
export -f mock_worker_crash

run_proposal "$ROOT/p_base.md"; assert_eq "O: worker crash (no patch) -> exit 2" "$RC" "2"

# A worker MUST NOT be able to widen its own scope by tampering AGENT_PROPOSAL_INPUTS_FILE: the runner
# re-derives the scope authority from its trusted in-memory packet inputs before the guard runs.
mkdir -p "$ROOT/repo/script"; echo "orig" > "$ROOT/repo/script/evil.sh"  # exists -> a worker edit is a 'modify', not an 'add'
mock_worker_tamper() {
  printf 'script/evil.sh\n' >> "$AGENT_PROPOSAL_INPUTS_FILE"   # try to widen the allow-list
  echo "pwn" >> "$AGENT_PROPOSAL_WORK/script/evil.sh"          # edit a forbidden, out-of-scope file
}
export ROUTE_C2_PROPOSAL_WORKER="mock_worker_tamper"
export -f mock_worker_tamper

run_proposal "$ROOT/p_base.md"
assert_eq "O2: tampered-scope forbidden edit -> exit 2" "$RC" "2"
case "$LOG" in *"out-of-scope edit on 'script/evil.sh'"*) ok "O2: guard ignores worker-tampered inputs file" ;; *) bad "O2: guard ignores worker-tampered inputs file"; printf '%s\n' "$LOG" | tail -5 ;; esac
rm -rf "$ROOT/repo/script"

# Under the bwrap path view the worker sees its work copy AT the real-repo path, so the real tree is
# structurally unreachable: a worker that targets the repo path only mutates its own sandbox. The real
# repo therefore stays byte-identical and the in-scope edit still validates.
before_mut_hash="$(sha256sum "$ROOT/repo/lib/foo.cpp" | awk '{print $1}')"
mock_worker_mutate_real() {
  echo "mutate" >> "$AOBUS_AGENT_REPO/lib/foo.cpp"   # the repo path the worker sees == its sandbox copy
  echo "edit"   >> "$AGENT_PROPOSAL_WORK/lib/foo.cpp"
}
export ROUTE_C2_PROPOSAL_WORKER="mock_worker_mutate_real"
export -f mock_worker_mutate_real

run_proposal "$ROOT/p_base.md"; assert_eq "O: bwrap shadows the real repo -> worker repo-path writes stay sandboxed -> exit 0" "$RC" "0"
assert_eq "O: real repo tree untouched by a worker targeting the repo path" "$(sha256sum "$ROOT/repo/lib/foo.cpp" | awk '{print $1}')" "$before_mut_hash"

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
v_test_all() { return 0; }
declare -gA VALIDATION_ARGSPEC=([test-core]="filter 1 1" [test-all]="any 0 -")
declare -gA VALIDATION_IS_ISOLATABLE=([test-core]="1" [test-all]="1")
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
v_test_all() { return 0; }
declare -gA VALIDATION_ARGSPEC=([test-core]="filter 1 1" [test-all]="any 0 -")
declare -gA VALIDATION_IS_ISOLATABLE=([test-core]="1" [test-all]="1")
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

echo "== W: header input accepted; oracle FORCED to test-all (packet cannot weaken) + dossier markers =="
mkdir -p "$ROOT/repo/include/ao/util" "$ROOT/repo/lib/util"
printf '#pragma once\nint kWidget();\n' > "$ROOT/repo/include/ao/util/Widget.h"
printf '#include <ao/util/Widget.h>\nint kWidget(){return 0;}\n' > "$ROOT/repo/lib/util/Widget.cpp"
# The packet asks for the narrow test-core (made to FAIL); the runner forces the full-suite test-all
# (PASS) regardless of scope. exit 0 therefore proves the packet could not downgrade the oracle.
cat > "$VALID" <<'EOF'
#!/usr/bin/env bash
v_test_core() { return 1; }
v_test_all()  { return 0; }
declare -gA VALIDATION_ARGSPEC=([test-core]="filter 1 1" [test-all]="any 0 -")
declare -gA VALIDATION_IS_ISOLATABLE=([test-core]="1" [test-all]="1")
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
assert_eq "W: header proposal validates under forced test-all -> exit 0" "$?" "0"
man="$(ls "$WH"/proposal_*/manifest.json 2>/dev/null | head -1)"
if [ -n "$man" ]; then
  MAN="$(cat "$man")"
  case "$MAN" in *'"intent": "refactor"'*) ok "W: dossier records intent" ;; *) bad "W: dossier intent"; echo "$MAN" ;; esac
  case "$MAN" in *'"header_touched": true'*) ok "W: dossier marks header_touched" ;; *) bad "W: dossier header_touched" ;; esac
  case "$MAN" in *'"validation": "test-all"'*) ok "W: dossier records forced test-all oracle" ;; *) bad "W: dossier oracle"; echo "$MAN" ;; esac
else bad "W: manifest.json emitted"; fi

echo "== Y: intent=behavior-change requires a registered test change (deterministic obligation) =="
mkdir -p "$ROOT/repo/test/unit"
printf 'TEST_CASE("widget","[widget]"){}\n' > "$ROOT/repo/test/unit/WidgetTest.cpp"
printf 'add_executable(ao_test\n  unit/WidgetTest.cpp\n)\n' > "$ROOT/repo/test/CMakeLists.txt"
cat > "$VALID" <<'EOF'
#!/usr/bin/env bash
v_test_core() { return 0; }
v_test_all() { return 0; }
declare -gA VALIDATION_ARGSPEC=([test-core]="filter 1 1" [test-all]="any 0 -")
declare -gA VALIDATION_IS_ISOLATABLE=([test-core]="1" [test-all]="1")
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

echo "== Z: headers no longer route per-domain or escalate on a core+app mix -> always test-all =="
# Z1: a private lib header (lib/**/*.h) validates under the single forced test-all oracle. The packet's
# narrow test-core (made to FAIL) cannot downgrade it; the forced test-all PASSES -> exit 0.
mkdir -p "$ROOT/repo/lib/util"
printf '#pragma once\nint kHelper();\n' > "$ROOT/repo/lib/util/Helper.h"
printf '#include "Helper.h"\nint kHelper(){return 0;}\n' > "$ROOT/repo/lib/util/Helper.cpp"
cat > "$VALID" <<'EOF'
#!/usr/bin/env bash
v_test_core() { return 1; }
v_test_all()  { return 0; }
declare -gA VALIDATION_ARGSPEC=([test-core]="filter 1 1" [test-all]="any 0 -")
declare -gA VALIDATION_IS_ISOLATABLE=([test-core]="1" [test-all]="1")
EOF
mock_libhdr_worker() { echo "// private helper" >> "$AGENT_PROPOSAL_WORK/lib/util/Helper.h"; }
export ROUTE_C2_PROPOSAL_WORKER="mock_libhdr_worker"; export -f mock_libhdr_worker
cat > "$ROOT/p_libhdr.md" <<'EOF'
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
  - lib/util/Helper.h
---
Refactor a private lib header (behavior-preserving).
EOF
WZ1="$ROOT/work_libhdr"; mkdir -p "$WZ1"
env AOBUS_AGENT_REPO="$ROOT/repo" AOBUS_AGENT_WORK="$WZ1" bash "$PROPOSAL" "$ROOT/p_libhdr.md" > "$ROOT/run_libhdr.log" 2>&1
assert_eq "Z1: private lib header validates under forced test-all -> exit 0" "$?" "0"
man_z1="$(ls "$WZ1"/proposal_*/manifest.json 2>/dev/null | head -1)"
[ -n "$man_z1" ] && case "$(cat "$man_z1")" in *'"validation": "test-all"'*) ok "Z1: forced test-all oracle" ;; *) bad "Z1: oracle"; cat "$man_z1" ;; esac

# Z2: a header set spanning BOTH core and app no longer escalates -- the full-suite oracle covers both,
# so the worker RUNS and the proposal validates (regression guard for the removed mixed-escalation gate).
mkdir -p "$ROOT/repo/app/widgets" "$ROOT/repo/app/linux-gtk"
printf '#pragma once\nint kRow();\n' > "$ROOT/repo/app/widgets/Row.h"
printf '#include "Row.h"\nint kRow(){return 0;}\n' > "$ROOT/repo/app/linux-gtk/Row.cpp"
cat > "$VALID" <<'EOF'
#!/usr/bin/env bash
v_test_all() { return 0; }
declare -gA VALIDATION_ARGSPEC=([test-all]="any 0 -")
declare -gA VALIDATION_IS_ISOLATABLE=([test-all]="1")
EOF
mock_mixed_worker() {
  echo "// helper" >> "$AGENT_PROPOSAL_WORK/lib/util/Helper.h"
  echo "// row"    >> "$AGENT_PROPOSAL_WORK/app/widgets/Row.h"
  echo ran > "$AOBUS_AGENT_WORK/ran"
}
export ROUTE_C2_PROPOSAL_WORKER="mock_mixed_worker"; export -f mock_mixed_worker
cat > "$ROOT/p_mixed.md" <<'EOF'
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
  - lib/util/Helper.h
  - app/widgets/Row.h
---
Touch both a core and an app header.
EOF
WZ2="$ROOT/work_mixed"; mkdir -p "$WZ2"; rm -f "$WZ2/ran"
env AOBUS_AGENT_REPO="$ROOT/repo" AOBUS_AGENT_WORK="$WZ2" bash "$PROPOSAL" "$ROOT/p_mixed.md" > "$ROOT/run_mixed.log" 2>&1
assert_eq "Z2: core+app header mix runs under test-all (no escalation) -> exit 0" "$?" "0"
[ -f "$WZ2/ran" ] && ok "Z2: worker ran (mixed no longer escalates)" || { bad "Z2: worker should run"; cat "$ROOT/run_mixed.log"; }
man_z2="$(ls "$WZ2"/proposal_*/manifest.json 2>/dev/null | head -1)"
[ -n "$man_z2" ] && case "$(cat "$man_z2")" in *'"validation": "test-all"'*) ok "Z2: forced test-all oracle" ;; *) bad "Z2: oracle"; cat "$man_z2" ;; esac

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
