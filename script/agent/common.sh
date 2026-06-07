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

# Proposal scope (the C2 proposal executor) is WIDER than the C1 guard: this repo has no API/ABI-compat
# requirement, so a header edit is allowed and falsified by the full-suite oracle (test-all; see
# c2_proposal_phase.sh), not gated by a path taboo. Only the oracle's OWN measurement apparatus stays
# forbidden -- CMake / .clang-tidy define the build (the "ruler"), and script / doc / .agents have no
# build/test oracle to falsify them. This is AGENT_FORBID MINUS the now-allowed include/ headers.
AGENT_PROPOSAL_FORBID='^(.*CMakeLists\.txt|\.clang-tidy|script/|doc/design/|\.agents/)'

# agent_guard_path <repo-relative path> ; exit 0 = allowed, nonzero = forbidden.
agent_guard_path() {
  ! printf '%s' "$1" | rg -q "$AGENT_FORBID"
}

# agent_classify_path <repo-relative path> ; classify a path for capability-specific positive gates.
agent_classify_path() {
  case "$1" in
    include/* | app/include/*) echo public-header ;;
    app/*.h | app/**/*.h)      echo private-app-header ;;
    lib/*.h | lib/**/*.h)      echo private-lib-header ;;
    script/*)                  echo script ;;
    doc/design/*)              echo design-doc ;;
    .agents/*)                 echo skill ;;
    .clang-tidy | *CMakeLists.txt) echo build-config ;;
    test/*.cpp | test/**/*.cpp) echo test-cpp ;;
    test/* | test/**/*)        echo test-helper ;;
    lib/*.cpp | lib/**/*.cpp | app/*.cpp | app/**/*.cpp | src/*.cpp | src/**/*.cpp) echo private-cpp-source ;;
    *)                         echo unknown ;;
  esac
}

# agent_check_registered_test <repo-relative test .cpp> ; true iff the file is an existing registered
# Catch2 test source. A conservative false negative escalates to C3; a false positive would let C2 write
# tests that never run, so keep this intentionally strict.
agent_cmake_has_source() {
  local cmake="$1" rel="$2" target="${3:-}" src
  src="${rel#test/}"
  [ -r "$cmake" ] || return 1
  awk -v src="$src" -v target="$target" '
    function scan(line,    n, i, tok) {
      sub(/#.*/, "", line)
      gsub(/[()]/, " ", line)
      n = split(line, tok, /[ \t\r\n]+/)
      for (i = 1; i <= n; i++) {
        if (tok[i] == src) found = 1
      }
    }
    target == "" { scan($0); next }
    $0 ~ "^[ \t]*add_executable[ \t]*\\(" {
      line = $0
      sub(/#.*/, "", line)
      gsub(/[()]/, " ", line)
      n = split(line, tok, /[ \t\r\n]+/)
      in_target = (tok[2] == target)
    }
    in_target {
      scan($0)
      if ($0 ~ /\)/) in_target = 0
    }
    END { exit found ? 0 : 1 }
  ' "$cmake"
}

agent_check_registered_test() {
  local rel="$1" cmake
  [ "$(agent_classify_path "$rel")" = test-cpp ] || return 1
  [ -f "$AGENT_REPO/$rel" ] || return 1
  grep -Eq '\b(TEST_CASE|TEMPLATE_TEST_CASE|SCENARIO)\s*\(' "$AGENT_REPO/$rel" || return 1
  cmake="$AGENT_REPO/test/CMakeLists.txt"
  agent_cmake_has_source "$cmake" "$rel"
}

# agent_is_header <repo-relative-path> ; 0 iff it classifies as any header (public include/** OR a
# private lib/app header).
agent_is_header() {
  local c; c="$(agent_classify_path "$1")"
  case "$c" in public-header | private-app-header | private-lib-header) return 0 ;; *) return 1 ;; esac
}

# agent_proposal_input_ok <repo-relative-path> ; scope gate for the C2 proposal executor. Accepts a
# private cpp source, a public/private header (falsified by the full-suite oracle), or an EXISTING
# registered test source (so a behavior-change proposal can extend the test that pins the new behavior).
# Rejects the oracle-foundation paths (AGENT_PROPOSAL_FORBID), non-files, and symlinks.
agent_proposal_input_ok() {
  local p="$1"
  agent_arg_safe "$p" || return 1
  ! printf '%s' "$p" | rg -q "$AGENT_PROPOSAL_FORBID" || return 1
  [ -f "$AGENT_REPO/$p" ] || return 1
  [ ! -L "$AGENT_REPO/$p" ] || return 1
  case "$(agent_classify_path "$p")" in
    private-cpp-source | public-header | private-app-header | private-lib-header) return 0 ;;
    test-cpp) agent_check_registered_test "$p" ;;
    *) return 1 ;;
  esac
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

# agent_validate_in_repo <source-dir> <build-dir> <validation-id> [args...]
# Isolated validation wrapper for proposals.
agent_validate_in_repo() {
  local src="$1" bld="$2" id="$3"; shift 3
  src="$(cd "$src" 2>/dev/null && pwd -P)" || return 2
  mkdir -p "$bld"
  bld="$(cd "$bld" 2>/dev/null && pwd -P)" || return 2
  local fn
  fn="$(agent_validation_fn "$id")"
  if ! agent_validation_exists "$id"; then
    echo "agent: validation '$id' is not in the allowlist -> reject" >&2
    return 2
  fi
  
  if [ "${VALIDATION_IS_ISOLATABLE[$id]:-}" != "1" ]; then
    echo "agent: validation '$id' is not proposal-isolatable -> reject" >&2
    return 2
  fi
  
  local a
  for a in "$@"; do
    agent_arg_safe "$a" || { echo "agent: validation arg '$a' is unsafe -> reject" >&2; return 2; }
  done
  agent_validation_args_ok "$id" "$@" || return 2
  
  ( 
    export AOBUS_AGENT_REPO="$src"
    export BUILD_DIR="$bld"
    export CCACHE_BASEDIR="$src"
    export CCACHE_READONLY=1
    # Normalize file paths (including DW_AT_comp_dir and __FILE__ macros)
    # so ASan/GDB traces are consistent and Ccache hits across boundaries.
    export CFLAGS="${CFLAGS:-} -ffile-prefix-map=$src=."
    export CXXFLAGS="${CXXFLAGS:-} -ffile-prefix-map=$src=."
    cd "$src"
    if [ ! -f "$bld/CMakeCache.txt" ]; then
      cmake --preset linux-debug -B "$bld" -S "$src" >/dev/null 2>&1 || true
    fi
    "$fn" "$@" 
  )
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

agent_packet_keys() { # <file> ; one scalar/list key per line from frontmatter
  awk '
    /^---[ \t]*$/ {fmc++; next}
    fmc!=1 {next}
    /^[A-Za-z_][A-Za-z0-9_-]*:[ \t]*($|[^ ].*)/ {
      sub(/:.*/, "")
      print
    }
  ' "$1"
}

agent_has_key() {
  [ -n "$(agent_packet_scalar "$1" "$2")" ] || [ -n "$(agent_packet_list "$1" "$2")" ]
}

agent_key_allowed() {
  local key="$1"; shift
  local allowed
  for allowed in "$@"; do [ "$key" = "$allowed" ] && return 0; done
  return 1
}

# agent_packet_validate <file> [expected-kind] ; strict schema for mutating request packets.
agent_packet_validate() {
  local packet="$1" expected="${2:-}"
  local schema kind key pid_val
  schema="$(agent_packet_scalar "$packet" schema)"
  kind="$(agent_packet_scalar "$packet" kind)"
  [ "$schema" = "aobus-phase-packet/v1" ] || {
    echo "agent: packet schema must be aobus-phase-packet/v1 (got '${schema:-}') -> reject" >&2
    return 64
  }
  [ -n "$kind" ] || { echo "agent: packet kind is required -> reject" >&2; return 64; }
  [ -z "$expected" ] || [ "$kind" = "$expected" ] || {
    echo "agent: packet kind must be '$expected' (got '$kind') -> reject" >&2
    return 64
  }
  case "$kind" in
    request)
      for key in schema kind skill capability validation inputs; do
        agent_has_key "$packet" "$key" || {
          echo "agent: request packet missing '$key' -> reject" >&2
          return 64
        }
      done
      while IFS= read -r key; do
        agent_key_allowed "$key" schema kind skill capability validation validation_args inputs \
          escalate_to || {
          echo "agent: request packet has unknown key '$key' -> reject" >&2
          return 64
        }
      done < <(agent_packet_keys "$packet")
      ;;
    proposal)
      for key in schema kind skill capability mode inputs validation; do
        agent_has_key "$packet" "$key" || {
          echo "agent: proposal packet missing '$key' -> reject" >&2
          return 64
        }
      done
      while IFS= read -r key; do
        agent_key_allowed "$key" schema kind skill capability mode validation validation_args inputs \
          escalate_to id intent || {
          echo "agent: proposal packet has unknown key '$key' -> reject" >&2
          return 64
        }
      done < <(agent_packet_keys "$packet")
      if [ "$(agent_packet_scalar "$packet" skill)" != "execute-plan" ]; then
        echo "agent: proposal skill must be execute-plan -> reject" >&2
        return 64
      fi
      if [ "$(agent_packet_scalar "$packet" capability)" != "C2" ]; then
        echo "agent: proposal capability must be C2 -> reject" >&2
        return 64
      fi
      if [ "$(agent_packet_scalar "$packet" mode)" != "proposal" ]; then
        echo "agent: proposal mode must be proposal -> reject" >&2
        return 64
      fi
      if [ -z "$(agent_packet_body "$packet" | sed '/^[[:space:]]*$/d')" ]; then
        echo "agent: proposal body cannot be empty -> reject" >&2
        return 64
      fi
      pid_val="$(agent_packet_scalar "$packet" id)"   # id is optional; the runner mints one when absent
      if [ -n "$pid_val" ] && ! agent_id_ok "$pid_val"; then
        echo "agent: proposal id '$pid_val' is unsafe or reserved -> reject" >&2
        return 64
      fi
      case "$(agent_packet_scalar "$packet" intent)" in   # optional; defaults to 'refactor' in the runner
        '' | refactor | behavior-change) ;;
        *) echo "agent: proposal intent must be 'refactor' or 'behavior-change' -> reject" >&2; return 64 ;;
      esac
      ;;
    escalation)
      for key in schema kind skill capability escalate_to validation inputs; do
        agent_has_key "$packet" "$key" || {
          echo "agent: escalation packet missing '$key' -> reject" >&2
          return 64
        }
      done
      ;;
    council | council-dossier)
      : ;;
    *)
      echo "agent: unknown packet kind '$kind' -> reject" >&2
      return 64
      ;;
  esac
  return 0
}

agent_phase_id() { printf '%s-%s-%s' "$1" "$(date -u +%Y%m%d-%H%M%S)" "$$"; }

# A phase id keys the audit log, review-outcomes log, breaker files, and the review_stats joins, so it
# must be a safe, non-reserved token. Reject the empty string and the literal "unknown" (the old
# id-less sentinel that collided across runs), plus anything outside the filename/grep-safe charset.
agent_id_ok() {
  case "${1:-}" in
    '' | unknown) return 1 ;;
    *[!A-Za-z0-9._:-]*) return 1 ;;
    *) return 0 ;;
  esac
}

agent_json_escape() {
  local s="$1"
  s="${s//\\/\\\\}"
  s="${s//\"/\\\"}"
  s="${s//$'\n'/\\n}"
  s="${s//$'\r'/\\r}"
  s="${s//$'\t'/\\t}"
  printf '%s' "$s"
}

# agent_audit_entry <phase-id> <skill> <cap> <worker> <result> <rounds> <churn> <assertion-delta> <reason>
agent_audit_entry() {
  local id="$1" skill="$2" cap="$3" worker="$4" result="$5" rounds="$6" churn="$7" delta="$8" reason="$9"
  mkdir -p "$AGENT_WORK"
  printf '{"ts":"%s","phase_id":"%s","skill":"%s","capability":"%s","worker":"%s","result":"%s","rounds":%s,"churn":%s,"assertion_delta":%s,"reason":"%s"}\n' \
    "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$(agent_json_escape "$id")" "$(agent_json_escape "$skill")" \
    "$(agent_json_escape "$cap")" "$(agent_json_escape "$worker")" "$(agent_json_escape "$result")" \
    "${rounds:-0}" "${churn:-0}" "${delta:-0}" "$(agent_json_escape "$reason")" >> "$AGENT_WORK/audit.log"
}

# agent_audit_field_for <phase-id> <field> ; print the JSON scalar value of <field> from the LAST
# audit.log line for <phase-id> (e.g. result, worker, capability). Empty if no such entry. Reuses the
# same phase-id match shape as record_review.sh so the join key is consistent across the harness.
agent_audit_field_for() {
  local id="$1" field="$2" log="$AGENT_WORK/audit.log"
  [ -r "$log" ] || return 0
  grep -F "\"phase_id\":\"$(agent_json_escape "$id")\"" "$log" 2>/dev/null | tail -n 1 |
    grep -oE "\"$field\":\"[^\"]*\"" | tail -n 1 | sed -E "s/^\"$field\":\"(.*)\"$/\1/"
}

# --- Circuit breaker -------------------------------------------------------------------------------
# The "first production silent-wrong pauses that route" rule (§Step A) made automatic. A breaker is a
# per-worker-label flag file under $AGENT_WORK/breaker/. record_review.sh trips it on a silent-wrong (a
# validated/kept phase that C3 later rejects); runners refuse a tripped worker and escalate to C3;
# review_stats.sh --reset clears it after a postmortem.
agent_breaker_dir() { printf '%s' "$AGENT_WORK/breaker"; }

# agent_breaker_slug <label> ; map a worker label to a safe flat filename.
agent_breaker_slug() {
  local s="$1"
  s="$(printf '%s' "$s" | tr '[:upper:]' '[:lower:]')"
  s="${s//[^a-z0-9]/-}"        # collapse every non-alnum to '-'
  s="$(printf '%s' "$s" | sed -E 's/-+/-/g; s/^-//; s/-$//')"
  printf '%s' "${s:-unknown}"
}

# agent_breaker_tripped <label> ; rc 0 iff this worker's breaker is tripped.
agent_breaker_tripped() {
  [ -f "$(agent_breaker_dir)/$(agent_breaker_slug "$1").tripped" ]
}

# agent_breaker_trip <label> <phase-id> <reason> ; create the breaker flag (idempotent: keeps the
# first trip's record). Returns 0 on a fresh trip, 1 if it was already tripped.
agent_breaker_trip() {
  local label="$1" id="$2" reason="$3" dir; dir="$(agent_breaker_dir)"
  local f="$dir/$(agent_breaker_slug "$label").tripped"
  mkdir -p "$dir"
  if [ -f "$f" ]; then return 1; fi
  printf '{"ts":"%s","worker":"%s","phase_id":"%s","reason":"%s"}\n' \
    "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$(agent_json_escape "$label")" \
    "$(agent_json_escape "$id")" "$(agent_json_escape "$reason")" > "$f"
  return 0
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
    # Exclude common noise paths that are not part of the source identity.
    local skip=( "(" -path "./.git" -o -path "./build" -o -path "./build/*" -o -path "./build-*" -o -path "./.cache" -o -path "./logs" ")" )
    find . "${skip[@]}" -prune -o -printf '%y %m %p -> %l\n' 2>/dev/null | sort
    find . "${skip[@]}" -prune -o -type f -print0 2>/dev/null | sort -z | xargs -0 -r sha256sum 2>/dev/null
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
    echo "- skill: \`.agents/skills/${AGENT_PACKET_SKILL:-use-clang-tidy}\`"
    echo "- escalated_from: $capfrom"
    echo "- capability_needed: C3 (frontier reasoning)"
    echo "- target: \`$rel\`"
    echo "- reason: $reason"
    echo "- validation: \`${AGENT_PACKET_VALIDATION:-tidy}\`"
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

# agent_guard_output_dir <repo> <output>
agent_guard_output_dir() {
  local repo="$1" out="$2"
  local r o
  r="$(cd "$repo" 2>/dev/null && pwd -P)" || return 1
  mkdir -p "$out"
  o="$(cd "$out" 2>/dev/null && pwd -P)" || return 1
  case "$o/" in "$r/"*) return 1 ;; esac
  return 0
}

# agent_stage_repo_copy <source-repo> <destination>
agent_stage_repo_copy() {
  local src="$1" dst="$2"
  mkdir -p "$dst"
  rsync -a --exclude='.git/' --exclude='build*/' --exclude='.cache/' --exclude='logs/' "$src/" "$dst/"
}

# agent_clean_ignored <tree-dir> <repo-with-rules>
# Remove from <tree-dir> every path that <repo>'s gitignore rules would ignore (logs, runtime
# artifacts, etc.). A C2 proposal operates on TRACKED source; a worker's build/test run inevitably
# writes gitignored runtime files (e.g. logs/app.log) into its work copy, and those must NOT register
# as in/out-of-scope changes. Cleaning BOTH the base and work copies identically keeps the diff about
# source only. No-op when <repo> is not a git repo (offline tests use plain temp trees).
agent_clean_ignored() {
  local tree="$1" repo="$2"
  git -C "$repo" rev-parse --is-inside-work-tree >/dev/null 2>&1 || return 0
  # `git check-ignore` exits 1 when NOTHING matches; that is normal, not an error. Capture via a
  # substitution with `|| true` so a caller's `set -e`/`pipefail` is not tripped by the empty case.
  local ignored
  ignored="$( ( cd "$tree" 2>/dev/null && find . -mindepth 1 \( -type f -o -type d \) -printf '%P\n' 2>/dev/null ) \
                | git -C "$repo" check-ignore --stdin 2>/dev/null || true )"
  local p
  while IFS= read -r p; do
    [ -n "$p" ] && rm -rf "${tree:?}/$p"
  done <<< "$ignored"
  # Prune directories left empty (e.g. logs/ after removing logs/*.log) so an empty dir does not
  # register as a spurious add. Base and work are cleaned identically, so this stays symmetric.
  find "${tree:?}" -mindepth 1 -type d -empty -delete 2>/dev/null || true
  return 0
}

# agent_tree_manifest <dir> <out.tsv>
agent_tree_manifest() {
  local d="$1" out="$2"
  mkdir -p "$(dirname "$out")"
  out="$(cd "$(dirname "$out")" 2>/dev/null && pwd -P)/$(basename "$out")" || return 2
  d="$(cd "$d" 2>/dev/null && pwd -P)" || return 2
  ( cd "$d" && 
    local skip=( "(" -path "./.git" -o -path "./build" -o -path "./build/*" -o -path "./build-*" -o -path "./.cache" -o -path "./logs" ")" )
    find . "${skip[@]}" -prune -o -printf '%y\t%m\t%p\t%l\n' 2>/dev/null | sed 's|^\([^\t]*\t[^\t]*\t\)\./|\1|' | sort > "$out.base"
    find . "${skip[@]}" -prune -o -type f -print0 2>/dev/null | sort -z | xargs -0 -r sha256sum 2>/dev/null | sed 's|^\([a-f0-9]\{64\}\)[ \t]\+\./|\1\t|' > "$out.hashes"
    
    awk -F'\t' -v OFS='\t' '
      NR==FNR { hash[$2] = $1; next }
      {
        if ($1 == "f") {
          h = hash[$3];
          if (h == "") h = "-";
          print $1, $2, $3, $4, h;
        } else {
          print $1, $2, $3, $4, "-";
        }
      }
    ' "$out.hashes" "$out.base" > "$out"
    rm -f "$out.base" "$out.hashes"
  )
}

# agent_tree_changes <base-dir> <work-dir> <out.tsv>
agent_tree_changes() {
  local b="$1" w="$2" out="$3"
  agent_tree_manifest "$b" "$out.base"
  agent_tree_manifest "$w" "$out.work"
  
  awk -F'\t' '
    FNR==NR { type[$3]=$1; mode[$3]=$2; sym[$3]=$4; hash[$3]=$5; base_seen[$3]=1; next }
    FNR!=NR {
      w_type=$1; w_mode=$2; w_sym=$4; w_hash=$5; path=$3
      if (!base_seen[path] && path != ".") {
        print "add\t" path
      } else if (path != ".") {
        if (type[path] != w_type) { print "type\t" path }
        else if (w_type == "f") {
          if (hash[path] != w_hash) { print "modify\t" path }
          else if (mode[path] != w_mode) { print "mode\t" path }
        } else if (w_type == "l") {
          if (sym[path] != w_sym) { print "symlink\t" path }
        }
      }
      work_seen[path]=1
    }
    END {
      for (path in base_seen) {
        if (!work_seen[path] && path != ".") print "delete\t" path
      }
    }
  ' "$out.base" "$out.work" | sort > "$out.raw"
  
  while IFS=$'\t' read -r change path; do
    if [ "$change" = "modify" ] || [ "$change" = "add" ]; then
      if grep -Iq "" "$w/$path" 2>/dev/null; then
         :
      elif [ -s "$w/$path" ]; then
         change="binary"
      fi
    fi
    printf '%s\t%s\n' "$change" "$path"
  done < "$out.raw" > "$out"
  rm -f "$out.base" "$out.work" "$out.raw"
}

# agent_harness_diff_tree <base-dir> <work-dir> <out.patch>
agent_harness_diff_tree() {
  local b="$1" w="$2" out="$3"
  git diff --no-index --src-prefix=a/ --dst-prefix=b/ "$b" "$w" > "$out" || true
}

# agent_proposal_changes_ok <inputs-file> <changes.tsv> ; 0 iff every change is a `modify` of a path
# listed in <inputs-file>. SECURITY: <inputs-file> is the scope authority, so the caller MUST pass a
# trusted file the worker cannot write (the C2 runner regenerates it from its in-memory packet inputs
# right before calling this, since the worker is handed its own copy via AGENT_PROPOSAL_INPUTS_FILE).
agent_proposal_changes_ok() {
  local inputs_file="$1" changes_file="$2"
  local ok=1
  while IFS=$'\t' read -r change path; do
    if [ "$change" != "modify" ]; then
      echo "agent: unsupported change '$change' on '$path' -> reject" >&2
      ok=0
    else
      if ! grep -Fxq "$path" "$inputs_file"; then
        echo "agent: out-of-scope edit on '$path' -> reject" >&2
        ok=0
      fi
    fi
  done < "$changes_file"
  return "$((1 - ok))"
}

# agent_changes_touch_registered_test <changes.tsv> ; 0 iff at least one changed path is an existing
# registered test source. Enforces the behavior-change intent obligation deterministically (the runner
# never infers "did behavior change" from worker output -- the planner declares it, this confirms a test
# moved with it).
agent_changes_touch_registered_test() {
  local change path
  while IFS=$'\t' read -r change path; do
    [ -n "$path" ] || continue
    agent_check_registered_test "$path" && return 0
  done < "$1"
  return 1
}
