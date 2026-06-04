#!/usr/bin/env bash
# script/agent/commit_flow.sh — C0 pre-commit orchestration (the §6 commit-flow chain).
#
# Sequences the mechanical pre-commit phases over the CHANGED working set, then hands off to C3. This
# is C0 routing only and DELIBERATELY DOES NOT COMMIT: no git commit / add / checkout / reset / stash.
# The commit decision, message, and semantic review stay with the frontier (the manage-git-flow skill,
# capability C3). commit_flow just makes the changed C++ review-ready and reports what C3 needs.
#
# Chain:
#   C0  clang-format the changed C++ files (targeted; report what changed)
#   C1  lint phase over the changed C++ set, via a Phase Packet + dispatch.sh (fix to fixpoint)
#   C0  the dispatcher's own independent tidy gate (allowlist) is the validation
#   ->  PASS: print a hand-off summary for C3 review + commit
#       NEEDS C3: list the escalation packets / guarded paths and stop
#
# Usage: script/agent/commit_flow.sh
# Exit:  0 = changed C++ is formatted + lint-clean, ready for C3 ; 2 = something needs C3 ; 5 = config.
set -u

. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"
agent_load_validation || exit 5

WORK="$AGENT_WORK/commit"; mkdir -p "$WORK"

# --- C0: targeted clang-format on the changed C++ files ---
echo "== C0 format =="
mapfile -t fmt < <(agent_changed_cpp)
if [ "${#fmt[@]}" -eq 0 ]; then
  echo "  no changed C++ files -> nothing to format"
else
  for f in "${fmt[@]}"; do
    [ -f "$AGENT_REPO/$f" ] && { clang-format -i "$AGENT_REPO/$f"; echo "  formatted $f"; }
  done
fi

# --- recompute the changed set after formatting ---
mapfile -t files < <(agent_changed_cpp)
if [ "${#files[@]}" -eq 0 ]; then echo "== no changed C++ -> nothing to lint; tree is C++-clean =="; exit 0; fi
echo "== changed C++ set (${#files[@]}): ${files[*]} =="

# Forbidden paths cannot go through a C1 worker; surface them for C3 directly.
declare -a lintable=() forbidden=()
for f in "${files[@]}"; do
  if agent_guard_path "$f"; then lintable+=("$f"); else forbidden+=("$f"); fi
done
[ "${#forbidden[@]}" -gt 0 ] && printf '  C3-only (guarded path): %s\n' "${forbidden[*]}"

# --- C1: lint phase, driven through a Phase Packet + the dispatcher (which re-validates via tidy) ---
rc_dispatch=0
if [ "${#lintable[@]}" -gt 0 ]; then
  packet="$WORK/commit-lint.packet.md"
  {
    echo "---"
    echo "schema: aobus-phase-packet/v1"
    echo "kind: request"
    echo "skill: use-clang-tidy"
    echo "capability: C1"
    echo "validation: tidy"
    echo "escalate_to: C3"
    echo "inputs:"
    printf '  - %s\n' "${lintable[@]}"
    echo "---"
    echo "# Pre-commit C1 lint over the changed set"
  } > "$packet"
  echo "== C1 lint phase (dispatch $packet) =="
  "$AGENT_DIR/dispatch.sh" "$packet"; rc_dispatch=$?
fi

# --- hand-off to C3 (commit_flow never commits) ---
echo "==================== commit-flow summary ===================="
if [ "$rc_dispatch" -eq 0 ] && [ "${#forbidden[@]}" -eq 0 ]; then
  echo "READY FOR C3: changed C++ is clang-formatted and lint-clean (tidy gate passed)."
  echo "Next (C3 / manage-git-flow): semantic review, commit message, commit. commit-flow does NOT commit."
  exit 0
fi
echo "NEEDS C3:"
[ "$rc_dispatch" -ne 0 ] && echo "  - C1 lint escalated; packets under $AGENT_WORK/lint/escalate/"
[ "${#forbidden[@]}" -gt 0 ] && printf '  - guarded path needs manual review: %s\n' "${forbidden[*]}"
exit 2
