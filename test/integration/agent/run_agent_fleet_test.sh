#!/usr/bin/env bash
# ============================================================================
# run_agent_fleet_test.sh
# Deterministic, offline unit coverage for the agent-fleet shared helpers
# (script/agent/common.sh + validation.env): the safety gates (arg sanitizer,
# validation allowlist, path guard), the harness-diff churn count, and the
# Phase Packet schema round-trip. No model and no clang-tidy are invoked, so
# this is CI-safe; the end-to-end lint/dispatch/commit chains are exercised
# separately against a live worker.
# ============================================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../../.." && pwd)"
# shellcheck disable=SC1091
source "$REPO/script/agent/common.sh"
# shellcheck disable=SC1091
source "$REPO/script/agent/validation.env"

PASS=0; FAIL=0
ok()  { PASS=$((PASS + 1)); printf '  ok   %s\n' "$1"; }
bad() { FAIL=$((FAIL + 1)); printf '  FAIL %s\n' "$1"; }
# assert_rc <expected-rc> <desc> <cmd...>
assert_rc() { local e="$1" d="$2"; shift 2; "$@" >/dev/null 2>&1; local r=$?; [ "$r" -eq "$e" ] && ok "$d" || bad "$d (rc=$r, want $e)"; }
assert_eq() { [ "$2" = "$3" ] && ok "$1" || bad "$1 (got [$2], want [$3])"; }

echo "== arg sanitizer =="
for a in "lib/x.cpp" "[audio],[model]" "a_b.cpp" "x/y/z.h"; do assert_rc 0 "safe: $a" agent_arg_safe "$a"; done
for a in "--all" "../etc" "a;b" '$(x)' "a b" "" "a|b" "a>b"; do assert_rc 1 "unsafe: $a" agent_arg_safe "$a"; done

echo "== path guard =="
for p in lib/a.cpp app/b.cpp test/c.cpp; do assert_rc 0 "allowed: $p" agent_guard_path "$p"; done
for p in include/a.h lib/CMakeLists.txt .clang-tidy script/x.sh doc/design/d.md .agents/s; do assert_rc 1 "forbidden: $p" agent_guard_path "$p"; done

echo "== validation allowlist =="
assert_rc 2 "unknown id rejected" agent_validate no-such-id foo
assert_rc 2 "unsafe arg rejected" agent_validate tidy '$(evil)'
assert_eq "v_tidy is a function" "$(type -t v_tidy)" "function"
assert_rc 0 "hyphen id 'tidy' resolves"       agent_validation_exists tidy
assert_rc 0 "hyphen id 'test-core' resolves"  agent_validation_exists test-core
assert_rc 0 "hyphen id 'build-debug' resolves" agent_validation_exists build-debug
assert_rc 1 "unknown id does not resolve"     agent_validation_exists no-such-id
assert_eq "id->fn maps hyphen to underscore" "$(agent_validation_fn test-core)" "v_test_core"

VT="$(mktemp -d)"; mkdir -p "$VT/script"
vtidy_in() { ( cd "$1" && v_tidy lib/foo.cpp ); }
printf '#!/usr/bin/env bash\nexit 7\n' > "$VT/script/run-clang-tidy.sh"; chmod +x "$VT/script/run-clang-tidy.sh"
assert_rc 7 "v_tidy fails when run-clang-tidy exits nonzero" vtidy_in "$VT"
printf '#!/usr/bin/env bash\necho "warning: fake diagnostic"\n' > "$VT/script/run-clang-tidy.sh"; chmod +x "$VT/script/run-clang-tidy.sh"
assert_rc 1 "v_tidy fails when tidy emits warnings" vtidy_in "$VT"
printf '#!/usr/bin/env bash\nexit 0\n' > "$VT/script/run-clang-tidy.sh"; chmod +x "$VT/script/run-clang-tidy.sh"
assert_rc 0 "v_tidy passes when tidy exits clean with no warnings" vtidy_in "$VT"
rm -rf "$VT"

echo "== validation arg contract (Step C: per-arg enum/type) =="
# agent_argtype_re: a repo path token and a Catch2 tag expression are distinguishable types.
amatch() { printf '%s' "$2" | grep -Eq "$(agent_argtype_re "$1")"; }
assert_rc 0 "type path matches a repo path"         amatch path "lib/audio/Foo.cpp"
assert_rc 1 "type path rejects a tag filter"        amatch path "[audio]"
assert_rc 0 "type filter matches a tag"             amatch filter "[audio]"
assert_rc 0 "type filter matches OR tags"           amatch filter "[layout],[model]"
assert_rc 0 "type filter matches an exclude tag"    amatch filter "~[slow]"
assert_rc 1 "type filter rejects a path"            amatch filter "lib/foo.cpp"
assert_rc 1 "unknown type is a misconfig"           agent_argtype_re bogus
# agent_validation_args_ok against the REAL allowlist specs (tidy=path 1 -, test-core=filter 1 1, ...).
assert_rc 0 "tidy accepts >=1 path"                 agent_validation_args_ok tidy lib/a.cpp app/b.cpp
assert_rc 2 "tidy rejects 0 args (min 1)"           agent_validation_args_ok tidy
assert_rc 2 "tidy rejects a filter (wrong type)"    agent_validation_args_ok tidy "[audio]"
assert_rc 0 "test-core accepts one filter"          agent_validation_args_ok test-core "[audio]"
assert_rc 2 "test-core rejects a path (wrong type)" agent_validation_args_ok test-core lib/foo.cpp
assert_rc 2 "test-core rejects two filters (max 1)" agent_validation_args_ok test-core "[a]" "[b]"
assert_rc 2 "test-core rejects 0 args (min 1)"      agent_validation_args_ok test-core
assert_rc 0 "build-debug tolerates extra/no args"   agent_validation_args_ok build-debug x y
assert_rc 0 "unspec'd id falls back to allow"       agent_validation_args_ok no-such-id whatever

echo "== harness diff churn =="
T="$(mktemp -d)"; printf 'a\nb\nc\n' > "$T/o"; printf 'a\nB\nc\nd\n' > "$T/m"
assert_eq "churn counts changed body lines" "$(agent_harness_diff "$T/o" "$T/m" "$T/p")" "3"

echo "== candidate ranking (Step D) =="
# agent_patch_files: one '+++ ' header per file touched
printf -- '--- a\n+++ b\n@@ -1 +1 @@\n-x\n+y\n'            > "$T/pf1"
printf -- '--- a\n+++ b\n--- c\n+++ d\n'                   > "$T/pf2"
assert_eq "patch-files counts single file"  "$(agent_patch_files "$T/pf1")" "1"
assert_eq "patch-files counts two files"    "$(agent_patch_files "$T/pf2")" "2"
# agent_rank_candidates: fewest files, then least churn, then id (deterministic, stable)
ranked="$(printf '1 30 a\n1 10 b\n2 5 c\n1 10 a2\n' | agent_rank_candidates | tr '\n' ' ')"
assert_eq "rank: files asc, churn asc, id asc" "$ranked" "a2 b a c "
assert_eq "rank: single candidate is itself"   "$(printf '1 7 only\n' | agent_rank_candidates)" "only"
# a low-churn winner outranks a correct-but-sprawling rewrite (the whole point of ranking)
assert_eq "rank: surgical beats rewrite"       "$(printf '1 4 surgical\n1 90 rewrite\n' | agent_rank_candidates | head -1)" "surgical"

echo "== packet schema round-trip =="
PK="$T/pk.md"
agent_emit_packet "$PK" C1 "lib/tag/Open.cpp" "test reason" /dev/null >/dev/null
assert_eq "scalar schema"      "$(agent_packet_scalar "$PK" schema)"      "aobus-phase-packet/v1"
assert_eq "scalar skill"       "$(agent_packet_scalar "$PK" skill)"       "use-clang-tidy"
assert_eq "scalar capability"  "$(agent_packet_scalar "$PK" capability)"  "C1"
assert_eq "scalar validation"  "$(agent_packet_scalar "$PK" validation)"  "tidy"
assert_eq "scalar escalate_to" "$(agent_packet_scalar "$PK" escalate_to)" "C3"
assert_eq "list inputs"        "$(agent_packet_list "$PK" inputs)"        "lib/tag/Open.cpp"

# packet body parsing on a hand-written request packet (frontmatter + body)
PKR="$T/req.md"
printf -- '---\nschema: aobus-phase-packet/v1\nskill: improve-test-coverage\ncapability: C2\nvalidation: test-core\nvalidation_args:\n  - [base64]\ninputs:\n  - test/unit/utility/Base64Test.cpp\n---\nLINE ONE of plan\nLINE TWO of plan\n' > "$PKR"
assert_eq "validation_args list" "$(agent_packet_list "$PKR" validation_args)" "[base64]"
assert_eq "body first line"      "$(agent_packet_body "$PKR" | head -1)"       "LINE ONE of plan"
assert_eq "body line count"      "$(agent_packet_body "$PKR" | grep -c .)"      "2"
rm -rf "$T"

echo "============================================================"
echo "PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ] || { echo "=== AGENT FLEET UNIT TESTS FAILED ==="; exit 1; }
echo "=== ALL AGENT FLEET UNIT TESTS PASSED ==="
