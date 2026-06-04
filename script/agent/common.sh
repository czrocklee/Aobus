#!/usr/bin/env bash
# script/agent/common.sh — shared primitives for Aobus agent-fleet phase runners.
#
# Sourced by lint_phase.sh (and any future dispatcher). Pure helpers: sourcing has no side effects
# beyond defining functions and the AGENT_* path variables below.

# Repo root from this file's location (independent of the caller's cwd).
AGENT_REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
AGENT_DIR="$AGENT_REPO/script/agent"
AGENT_WORK="${AOBUS_AGENT_WORK:-/tmp/aobus-agent}"

# Deterministic guard: repo-relative paths a cheap (C1/C2) worker must never edit. A hit means
# escalate to C3, never auto-fix. Keep in sync with the Phase Contract in use-clang-tidy/SKILL.md.
AGENT_FORBID='^(include/|.*CMakeLists\.txt|\.clang-tidy|script/|doc/design/|\.agents/)'

# agent_guard_path <repo-relative path> ; exit 0 = allowed, nonzero = forbidden.
agent_guard_path() {
  ! printf '%s' "$1" | rg -q "$AGENT_FORBID"
}

# agent_load_routing ; source the routing table (override file via AOBUS_ROUTING_ENV).
agent_load_routing() {
  local rt="${AOBUS_ROUTING_ENV:-$AGENT_DIR/routing.env}"
  [ -r "$rt" ] || { echo "agent: routing table not found: $rt" >&2; return 1; }
  # shellcheck disable=SC1090
  . "$rt"
}

# agent_load_validation ; source the validation allowlist (override via AOBUS_VALIDATION_ENV).
agent_load_validation() {
  local vt="${AOBUS_VALIDATION_ENV:-$AGENT_DIR/validation.env}"
  [ -r "$vt" ] || { echo "agent: validation allowlist not found: $vt" >&2; return 1; }
  # shellcheck disable=SC1090
  . "$vt"
}

# agent_arg_safe <arg> ; 0 = safe to pass as a positional argument to an allowlisted command.
# Args are never eval'd (the allowlist passes them as quoted positionals), so this is defense in
# depth: block flag injection, path traversal, and any byte outside [A-Za-z0-9._/,:[]-].
agent_arg_safe() {
  case "$1" in -* | *..* | "") return 1 ;; esac
  [ -z "$(printf '%s' "$1" | tr -d 'A-Za-z0-9._/,:[]-')" ]
}

# A validation id (e.g. test-core) maps to a function v_<id-with-hyphens-as-underscores> (v_test_core).
agent_validation_fn()     { printf 'v_%s' "${1//-/_}"; }
agent_validation_exists() { [ "$(type -t "$(agent_validation_fn "$1")" 2>/dev/null)" = "function" ]; }

# agent_validate <id> [arg...] ; resolve an ALLOWLISTED validation and run it from the repo root.
# Rejects unknown IDs and unsafe args; never evaluates a shell string from a packet. Returns the
# validation's exit code (0 = pass).
agent_validate() {
  local id="$1"; shift
  local fn; fn="$(agent_validation_fn "$id")"
  if ! agent_validation_exists "$id"; then
    echo "agent: validation '$id' is not in the allowlist -> reject" >&2; return 2
  fi
  local a
  for a in "$@"; do
    agent_arg_safe "$a" || { echo "agent: validation arg '$a' is unsafe -> reject" >&2; return 2; }
  done
  ( cd "$AGENT_REPO" && "$fn" "$@" )
}

# Phase Packet (v1) is YAML frontmatter + markdown body. These read the constrained frontmatter we
# author (no general YAML parser): a key we own, simple scalars, and one-level "- item" lists.
agent_packet_scalar() { # <file> <key>
  awk -v k="$2" '/^---[ \t]*$/{fmc++; next} fmc==1 && index($0,k":")==1 {sub("^"k":[ \t]*",""); print; exit}' "$1"
}
agent_packet_list() { # <file> <key> ; one item per line
  awk -v k="$2" '
    /^---[ \t]*$/ {fmc++; next}
    fmc!=1 {next}
    index($0,k":")==1 {inl=1; next}
    inl && /^[ \t]+-[ \t]/ {sub(/^[ \t]+-[ \t]*/,""); print; next}
    inl && /^[^ \t]/ {inl=0}
  ' "$1"
}
agent_packet_body() { # <file> ; the markdown body (everything after the closing frontmatter ---)
  awk '/^---[ \t]*$/ {fmc++; next} fmc>=2 {print}' "$1"
}

# agent_repo_lock ; serialize all tree-mutating phases on this repo with an exclusive flock on fd 9.
# Call once, early. The lock is held for the process lifetime. Returns 4 if it cannot be acquired
# within AGENT_LOCK_WAIT seconds (default 120).
agent_repo_lock() {
  local lock="$AGENT_WORK/.repo.lock"
  mkdir -p "$AGENT_WORK"
  exec 9>"$lock"
  if ! flock -w "${AGENT_LOCK_WAIT:-120}" 9; then
    echo "agent: another phase holds the repo lock ($lock) -> abort" >&2
    return 4
  fi
}

# agent_changed_cpp ; echo repo-relative changed C++ files (staged + modified + untracked), rename-safe.
agent_changed_cpp() {
  ( cd "$AGENT_REPO" && git status --porcelain -z |
    while IFS= read -r -d '' entry; do
      local st="${entry:0:2}" f="${entry:3}"
      [[ "$st" =~ ^[RC] ]] && { IFS= read -r -d '' f || break; }
      case "$f" in *.cpp | *.h | *.hpp) printf '%s\n' "$f" ;; esac
    done )
}

# agent_harness_diff <orig> <modified> <out-patch> ; the dispatcher computes the patch itself, never
# trusting a model-authored diff. Writes a unified diff to <out-patch> and echoes the changed-line
# count (added/removed body lines, excluding the +++/--- header) to stdout.
agent_harness_diff() {
  diff -u "$1" "$2" > "$3" || true
  awk '/^[+-]/ && !/^[+-][+-]/ {c++} END{print c+0}' "$3"
}

# agent_emit_packet <out.md> <escalated_from> <target_rel> <reason> <diag_file> [<patch_file>]
# Writes a self-contained Phase Packet (§6) for a C3 reviewer and echoes its path. The real tree is
# always rolled back before this is called, so the packet documents an un-applied situation.
agent_emit_packet() {
  local out="$1" capfrom="$2" rel="$3" reason="$4" diag="$5" patch="${6:-}"
  {
    echo "---"
    echo "schema: aobus-phase-packet/v1"
    echo "kind: escalation"
    echo "skill: ${AGENT_PACKET_SKILL:-use-clang-tidy}"
    echo "capability: $capfrom"
    echo "escalate_to: C3"
    echo "validation: ${AGENT_PACKET_VALIDATION:-tidy}"
    echo "inputs:"
    echo "  - $rel"
    echo "---"
    echo "# Phase Packet — escalation to C3"
    echo
    echo "- skill: \`.agents/skills/use-clang-tidy\`"
    echo "- escalated_from: $capfrom"
    echo "- capability_needed: C3 (frontier reasoning)"
    echo "- target: \`$rel\`"
    echo "- reason: $reason"
    echo "- validation: \`./script/run-clang-tidy.sh $rel\` (zero warnings is the gate)"
    echo "- real-tree state: rolled back to pre-phase (no partial edits applied)"
    echo
    echo "## Residual diagnostics"
    echo '```'
    if [ -r "$diag" ] && [ -s "$diag" ]; then cat "$diag"; else echo "(none captured)"; fi
    echo '```'
    if [ -n "$patch" ] && [ -s "$patch" ]; then
      echo
      echo "## Last rejected C1 attempt (for reference; NOT applied)"
      echo '```diff'
      cat "$patch"
      echo '```'
    fi
  } > "$out"
  echo "$out"
}
