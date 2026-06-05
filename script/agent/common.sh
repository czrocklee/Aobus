#!/usr/bin/env bash
# script/agent/common.sh — shared primitives for Aobus agent-fleet phase runners.
#
# Sourced by lint_phase.sh (and any future dispatcher). Pure helpers: sourcing has no side effects
# beyond defining functions and the AGENT_* path variables below.

# Repo root from this file's location (independent of the caller's cwd). AOBUS_AGENT_REPO overrides it
# so a runner can be exercised against a throwaway tree in /tmp (the deterministic test seam; same
# spirit as AOBUS_AGENT_WORK / AOBUS_ROUTING_ENV).
AGENT_REPO="${AOBUS_AGENT_REPO:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
AGENT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
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

# Per-arg TYPE -> ERE. A validation's declared arg type (in VALIDATION_ARGSPEC) maps here; this is the
# enum/type layer on top of the generic agent_arg_safe charset gate. Add a row to introduce a new type
# (an enum is just a literal alternation, e.g. 'core|gtk').
agent_argtype_re() {
  case "$1" in
    path)   printf '%s' '^[A-Za-z0-9][A-Za-z0-9._/-]*$' ;;                      # a repo-relative path token
    filter) printf '%s' '^(~?\[[A-Za-z0-9_-]+\])+(,~?\[[A-Za-z0-9_-]+\])*$' ;;  # a Catch2 tag expression
    any)    printf '%s' '.*' ;;                                                 # no further restriction
    *)      return 1 ;;                                                         # unknown type = misconfig
  esac
}

# agent_validation_args_ok <id> [arg...] ; 0 = the args satisfy the validation's declared contract in
# VALIDATION_ARGSPEC ("<type> <min> <max>", max '-' = unbounded): arity within [min,max] and every arg of
# <type>. This is the per-arg enum/type gate that rejects a MISTYPED or MIS-COUNTED packet before any slow
# validation runs. If no spec is declared for <id> (or VALIDATION_ARGSPEC is absent, e.g. a test mock) it
# returns 0 — the generic agent_arg_safe charset gate still applies upstream.
agent_validation_args_ok() {
  local id="$1"; shift
  declare -p VALIDATION_ARGSPEC >/dev/null 2>&1 || return 0
  local spec="${VALIDATION_ARGSPEC[$id]:-}"; [ -n "$spec" ] || return 0
  local type min max; read -r type min max <<<"$spec"
  local n=$#
  if [ "$n" -lt "$min" ]; then
    echo "agent: validation '$id' needs >= $min arg(s), got $n -> reject" >&2; return 2
  fi
  if [ "$max" != "-" ] && [ "$n" -gt "$max" ]; then
    echo "agent: validation '$id' takes <= $max arg(s), got $n -> reject" >&2; return 2
  fi
  local re; re="$(agent_argtype_re "$type")" || {
    echo "agent: validation '$id' has unknown arg type '$type' -> reject" >&2; return 2; }
  local a
  for a in "$@"; do
    printf '%s' "$a" | grep -Eq "$re" || {
      echo "agent: validation '$id' arg '$a' is not of type '$type' -> reject" >&2; return 2; }
  done
  return 0
}

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
  agent_validation_args_ok "$id" "$@" || return 2
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

# agent_tree_hash [dir] ; a stable fingerprint of a directory (default AGENT_REPO), `.git` excluded.
# council.sh (§11) gives each READ-ONLY committee member a disposable COPY of the repo as its cwd and
# snapshots this hash of that copy immediately before and after the member runs: an unequal hash means
# the member mutated files, a protocol violation (its job is an OPINION, never a patch) — the runner then
# discards that member's output and flags it. Per-member copies make the check attributable under
# parallel fan-out. The fingerprint folds in a TYPED MANIFEST (type, octal mode, path, and symlink
# target) plus regular-file CONTENT, so additions, deletions, edits, chmods, AND symlink retargets all
# change the result — a member cannot escape the canary by flipping a bit, an exec bit, or repointing a
# symlink out of the copy. The manifest is one `find -printf` pass (no per-file fork); content is a
# single batched `sha256sum`. An empty or missing dir yields a stable hash.
agent_tree_hash() {
  local d="${1:-$AGENT_REPO}"
  ( cd "$d" 2>/dev/null || exit 0
    find . -path ./.git -prune -o -printf '%y %m %p -> %l\n' 2>/dev/null | sort
    find . -path ./.git -prune -o -type f -print0 2>/dev/null | sort -z | xargs -0 -r sha256sum 2>/dev/null
  ) | sha256sum | awk '{print $1}'
}

# agent_harness_diff <orig> <modified> <out-patch> ; the dispatcher computes the patch itself, never
# trusting a model-authored diff. Writes a unified diff to <out-patch> and echoes the changed-line
# count (added/removed body lines, excluding the +++/--- header) to stdout.
agent_harness_diff() {
  diff -u "$1" "$2" > "$3" || true
  awk '/^[+-]/ && !/^[+-][+-]/ {c++} END{print c+0}' "$3"
}

# agent_patch_files <patch> ; number of distinct files a unified diff touches (one '+++ ' header per
# file). A candidate that strays outside its single-file scope shows up as >1 here.
agent_patch_files() { awk '/^\+\+\+ /{c++} END{print c+0}' "$1"; }

# agent_rank_candidates ; deterministic candidate ranking (§5.1 step 2). Reads "<files> <churn> <id>"
# lines on stdin and emits the <id>s ranked best-first: fewest files touched, then least churn, then
# id (so the order is stable and reproducible). The whole point is to pick a winner WITHOUT spending
# the slow validation or any frontier attention; semantic tie-breaking (when deterministic dimensions
# are level) is a C3 concern handled upstream, not here.
agent_rank_candidates() { sort -k1,1n -k2,2n -k3,3 | awk '{print $3}'; }

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
