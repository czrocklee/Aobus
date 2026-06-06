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

echo "== path classifier and registered-test gate =="
assert_eq "classify public header" "$(agent_classify_path include/a.h)" "public-header"
assert_eq "classify script"        "$(agent_classify_path script/x.sh)" "script"
assert_eq "classify design doc"    "$(agent_classify_path doc/design/x.md)" "design-doc"
assert_eq "classify skill"         "$(agent_classify_path .agents/skills/x/SKILL.md)" "skill"
assert_eq "classify build config"  "$(agent_classify_path test/CMakeLists.txt)" "build-config"
assert_eq "classify test cpp"      "$(agent_classify_path test/unit/FooTest.cpp)" "test-cpp"
assert_eq "classify source cpp"    "$(agent_classify_path lib/audio/Foo.cpp)" "private-cpp-source"
assert_eq "classify unknown"       "$(agent_classify_path README.md)" "unknown"
RT="$(mktemp -d)"
OLD_AGENT_REPO="$AGENT_REPO"; AGENT_REPO="$RT"
mkdir -p "$RT/test/unit"
mkdir -p "$RT/test/other"
printf 'add_executable(ao_test unit/FooTest.cpp other/DupeTest.cpp unit/MyOtherTest.cpp)\n' > "$RT/test/CMakeLists.txt"
printf '#include <catch2/catch_test_macros.hpp>\nTEST_CASE("x", "[x]") { CHECK(true); }\n' > "$RT/test/unit/FooTest.cpp"
printf 'TEST_CASE("y") {}\n' > "$RT/test/unit/UnregisteredTest.cpp"
printf 'TEST_CASE("dupe", "[x]") {}\n' > "$RT/test/unit/DupeTest.cpp"
printf 'TEST_CASE("other", "[x]") {}\n' > "$RT/test/unit/OtherTest.cpp"
assert_rc 0 "registered Catch2 test accepted" agent_check_registered_test test/unit/FooTest.cpp
assert_rc 1 "unregistered test rejected"      agent_check_registered_test test/unit/UnregisteredTest.cpp
assert_rc 1 "same-basename different-dir test rejected" agent_check_registered_test test/unit/DupeTest.cpp
assert_rc 1 "substring CMake source match rejected"      agent_check_registered_test test/unit/OtherTest.cpp
assert_rc 1 "non-test source rejected"        agent_check_registered_test lib/foo.cpp
assert_rc 0 "C2 test scope accepts one registered test" agent_scope_ok c2-test-augment test/unit/FooTest.cpp
assert_rc 1 "C2 test scope rejects source"             agent_scope_ok c2-test-augment lib/foo.cpp
assert_rc 0 "C2 test validation accepts test-core" agent_c2_test_validation_ok test-core
assert_rc 0 "C2 test validation accepts test-gtk"  agent_c2_test_validation_ok test-gtk
assert_rc 1 "C2 test validation rejects tidy"     agent_c2_test_validation_ok tidy
AGENT_REPO="$OLD_AGENT_REPO"; rm -rf "$RT"

echo "== validation allowlist =="
assert_rc 2 "unknown id rejected" agent_validate no-such-id foo
assert_rc 2 "unsafe arg rejected" agent_validate tidy '$(evil)'
assert_eq "v_tidy is a function" "$(type -t v_tidy)" "function"
assert_rc 0 "hyphen id 'tidy' resolves"       agent_validation_exists tidy
assert_rc 0 "hyphen id 'test-core' resolves"  agent_validation_exists test-core
assert_rc 0 "hyphen id 'build-debug' resolves" agent_validation_exists build-debug
assert_rc 1 "unknown id does not resolve"     agent_validation_exists no-such-id
assert_eq "id->fn maps hyphen to underscore" "$(agent_validation_fn test-core)" "v_test_core"

echo "== routing defaults =="
routing_value() {
  local key="$1"
  (
    # shellcheck disable=SC1091
    source "$REPO/script/agent/routing.env"
    case "$key" in
      c2_worker) printf '%s' "$ROUTE_C2_WORKER" ;;
      c2_label)  printf '%s' "$ROUTE_C2_LABEL" ;;
      fallback)  declare -f route_c2_worker ;;
    esac
  )
}
assert_eq "C2 default worker is Gemini agy" "$(routing_value c2_worker)" "route_c2_worker_gpro"
assert_eq "C2 default label is Gemini agy"  "$(routing_value c2_label)"  "Gemini 3.1 Pro (high) via agy"
case "$(routing_value fallback)" in
  *route_c2_worker_gpro*) ok "C2 fallback alias routes to Gemini agy" ;;
  *) bad "C2 fallback alias routes to Gemini agy" ;;
esac

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

echo "== test list gates =="
v_mock_list_gate() { :; }
v_mock_list_gate_list() {
  case "$1" in
    "[empty]") printf 'Matching test cases:\n0 matching test cases\n' ;;
    "[hit]")   printf 'Matching test cases:\n  anchor case\n    %s/test/unit/FooTest.cpp:7\n      [hit][anchor]\n1 matching test cases\n' "$AGENT_REPO" ;;
    *)         printf 'mock test\n' ;;
  esac
}
LIST_REPO="$(mktemp -d)"
OLD_AGENT_REPO="$AGENT_REPO"; AGENT_REPO="$LIST_REPO"
assert_rc 1 "empty Catch2 list output rejected" agent_test_filter_nonempty mock-list-gate "[empty]"
assert_rc 0 "nonempty Catch2 list output accepted" agent_test_filter_nonempty mock-list-gate "[hit]"
hit_out="$(v_mock_list_gate_list "[hit]")"
assert_rc 0 "list block binds target and anchor" agent_test_list_mentions_target_anchor "$hit_out" test/unit/FooTest.cpp anchor
assert_rc 1 "list block rejects missing anchor" agent_test_list_mentions_target_anchor "$hit_out" test/unit/FooTest.cpp other-anchor
AGENT_REPO="$OLD_AGENT_REPO"; rm -rf "$LIST_REPO"

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
assert_rc 0 "escalation packet validates" agent_packet_validate "$PK" escalation

# packet body parsing on a hand-written request packet (frontmatter + body)
PKR="$T/req.md"
printf -- '---\nschema: aobus-phase-packet/v1\nkind: request\nskill: improve-test-coverage\ncapability: C2\nvalidation: test-core\ntarget_anchor: anchor\nvalidation_args:\n  - [base64]\ninputs:\n  - test/unit/utility/Base64Test.cpp\n---\nLINE ONE of plan\nLINE TWO of plan\n' > "$PKR"
assert_eq "validation_args list" "$(agent_packet_list "$PKR" validation_args)" "[base64]"
assert_eq "body first line"      "$(agent_packet_body "$PKR" | head -1)"       "LINE ONE of plan"
assert_eq "body line count"      "$(agent_packet_body "$PKR" | grep -c .)"      "2"
assert_rc 0 "request packet validates" agent_packet_validate "$PKR" request
printf -- '---\nschema: bogus\nkind: request\nskill: x\ncapability: C2\nvalidation: test-core\ninputs:\n  - test/x.cpp\n---\n' > "$T/bad-schema.md"
assert_rc 64 "bad schema rejected" agent_packet_validate "$T/bad-schema.md" request
printf -- '---\nschema: aobus-phase-packet/v1\nkind: request\nskill: x\ncapability: C2\nvalidation: test-core\nunknown: y\ninputs:\n  - test/x.cpp\n---\n' > "$T/unknown-key.md"
assert_rc 64 "unknown request key rejected" agent_packet_validate "$T/unknown-key.md" request

echo "== C2 review artifacts =="
AF="$T/assert.cpp"
printf 'TEST_CASE("x") { CHECK(true); REQUIRE(true); }\n' > "$AF"
assert_eq "assertion count catches CHECK/REQUIRE" "$(agent_count_assertions "$AF")" "2"
AP="$T/change.patch"; printf 'diff\n' > "$AP"
VL="$T/validation.log"; printf 'validation ok\n' > "$VL"
DOS="$T/review.md"; MAN="$T/manifest.json"
agent_emit_review_dossier "$DOS" "$PKR" "test/unit/utility/Base64Test.cpp" "$AP" "$VL" phase-1 worker 1 2 1 2 increased >/dev/null
[ -s "$DOS" ] && ok "review dossier emitted" || bad "review dossier emitted"
case "$(cat "$DOS")" in *"C2 Review Dossier"* ) ok "review dossier has title" ;; *) bad "review dossier has title" ;; esac
agent_write_manifest "$MAN" "$AP" "$VL" "$DOS"
[ -s "$MAN" ] && ok "manifest emitted" || bad "manifest emitted"
OLD_AGENT_WORK="$AGENT_WORK"; AGENT_WORK="$T/work"
agent_audit_entry phase-1 improve-test-coverage C2 worker keep 1 2 1 increased
[ -s "$AGENT_WORK/audit.log" ] && ok "audit entry emitted" || bad "audit entry emitted"
AGENT_WORK="$OLD_AGENT_WORK"
assert_eq "json escape handles newline and tab" "$(agent_json_escape $'a\nb\tc')" 'a\nb\tc'
RW="$T/review-work"
mkdir -p "$RW"
printf '{"phase_id":"phase-1","result":"keep"}\n' > "$RW/audit.log"
AOBUS_AGENT_WORK="$RW" "$REPO/script/agent/record_review.sh" phase-1 accept "looks good" >/dev/null
[ -s "$RW/review-outcomes.log" ] && ok "review outcome recorded" || bad "review outcome recorded"
AOBUS_AGENT_WORK="$RW" "$REPO/script/agent/record_review.sh" phase-1 accept "looks good again" >/dev/null
assert_rc 2 "conflicting review outcome rejected" env AOBUS_AGENT_WORK="$RW" "$REPO/script/agent/record_review.sh" phase-1 reject "changed mind"
assert_rc 2 "phantom review outcome rejected" env AOBUS_AGENT_WORK="$RW" "$REPO/script/agent/record_review.sh" missing accept "no keep"
rm -rf "$T"

echo "== tree and copy helpers (Phase 3) =="

TC="$(mktemp -d)"
mkdir -p "$TC/repo/.git" "$TC/repo/build-debug" "$TC/repo/.cache" "$TC/repo/lib"
echo "git" > "$TC/repo/.git/config"
echo "build" > "$TC/repo/build-debug/out"
echo "cache" > "$TC/repo/.cache/hit"
echo "cpp" > "$TC/repo/lib/foo.cpp"
ln -s "foo.cpp" "$TC/repo/lib/sym.cpp"

assert_rc 1 "reject output inside repo" agent_guard_output_dir "$TC/repo" "$TC/repo/out"
assert_rc 1 "reject output same as repo" agent_guard_output_dir "$TC/repo" "$TC/repo"
assert_rc 0 "accept output outside repo" agent_guard_output_dir "$TC/repo" "$TC/out"

agent_stage_repo_copy "$TC/repo" "$TC/copy"
assert_eq "file is copied" "$(cat "$TC/copy/lib/foo.cpp")" "cpp"
[ -d "$TC/copy/.git" ] && bad ".git was copied" || ok ".git not copied"
[ -d "$TC/copy/build-debug" ] && bad "build dir was copied" || ok "build dir not copied"
[ -d "$TC/copy/.cache" ] && bad ".cache was copied" || ok ".cache not copied"

mkdir -p "$TC/work"
agent_stage_repo_copy "$TC/repo" "$TC/work"
echo "edit" >> "$TC/work/lib/foo.cpp"
echo "new" > "$TC/work/lib/bar.cpp"
rm "$TC/work/lib/sym.cpp"
ln -s "bar.cpp" "$TC/work/lib/sym.cpp"
chmod +x "$TC/work/lib/foo.cpp"

agent_tree_changes "$TC/copy" "$TC/work" "$TC/changes.tsv"
assert_rc 0 "modify detected" grep -q "modify	lib/foo.cpp" "$TC/changes.tsv"
assert_rc 0 "add detected" grep -q "add	lib/bar.cpp" "$TC/changes.tsv"
assert_rc 0 "symlink detected" grep -q "symlink	lib/sym.cpp" "$TC/changes.tsv"

mkdir -p "$TC/work2"
agent_stage_repo_copy "$TC/repo" "$TC/work2"
rm "$TC/work2/lib/foo.cpp"
agent_tree_changes "$TC/copy" "$TC/work2" "$TC/changes2.tsv"
assert_rc 0 "delete detected" grep -q "delete	lib/foo.cpp" "$TC/changes2.tsv"

mkdir -p "$TC/work3"
agent_stage_repo_copy "$TC/repo" "$TC/work3"
chmod +x "$TC/work3/lib/foo.cpp"
agent_tree_changes "$TC/copy" "$TC/work3" "$TC/changes3.tsv"
assert_rc 0 "mode detected" grep -q "mode	lib/foo.cpp" "$TC/changes3.tsv"

mkdir -p "$TC/work4"
agent_stage_repo_copy "$TC/repo" "$TC/work4"
printf '\0\1\2\3' > "$TC/work4/lib/foo.cpp"
agent_tree_changes "$TC/copy" "$TC/work4" "$TC/changes4.tsv"
assert_rc 0 "binary detected" grep -q "binary	lib/foo.cpp" "$TC/changes4.tsv"

# Proposal changes ok gate
echo "lib/foo.cpp" > "$TC/inputs.txt"
printf "modify\tlib/foo.cpp\n" > "$TC/c_ok.tsv"
assert_rc 0 "accept normal edit" agent_proposal_changes_ok "$TC/inputs.txt" "$TC/c_ok.tsv"

printf "modify\tlib/other.cpp\n" > "$TC/c_out.tsv"
assert_rc 1 "reject out of scope edit" agent_proposal_changes_ok "$TC/inputs.txt" "$TC/c_out.tsv"

printf "add\tlib/foo.cpp\n" > "$TC/c_add.tsv"
assert_rc 1 "reject file addition" agent_proposal_changes_ok "$TC/inputs.txt" "$TC/c_add.tsv"

printf "delete\tlib/foo.cpp\n" > "$TC/c_del.tsv"
assert_rc 1 "reject file deletion" agent_proposal_changes_ok "$TC/inputs.txt" "$TC/c_del.tsv"

printf "symlink\tlib/foo.cpp\n" > "$TC/c_sym.tsv"
assert_rc 1 "reject symlink change" agent_proposal_changes_ok "$TC/inputs.txt" "$TC/c_sym.tsv"

printf "mode\tlib/foo.cpp\n" > "$TC/c_mode.tsv"
assert_rc 1 "reject mode change" agent_proposal_changes_ok "$TC/inputs.txt" "$TC/c_mode.tsv"

printf "binary\tlib/foo.cpp\n" > "$TC/c_bin.tsv"
assert_rc 1 "reject binary change" agent_proposal_changes_ok "$TC/inputs.txt" "$TC/c_bin.tsv"

agent_harness_diff_tree "$TC/copy" "$TC/work" "$TC/patch"
[ -s "$TC/patch" ] && ok "patch generated" || bad "patch generated"

rm -rf "$TC"

echo "============================================================"
echo "PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ] || { echo "=== AGENT FLEET UNIT TESTS FAILED ==="; exit 1; }
echo "=== ALL AGENT FLEET UNIT TESTS PASSED ==="
