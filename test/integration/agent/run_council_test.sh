#!/usr/bin/env bash
# ============================================================================
# run_council_test.sh
# Deterministic, offline coverage for the C3 "council" orchestrator
# (script/agent/council.sh): the multi-model plan/review committee. No real
# frontier model and no network: the committee members are mocked via
# AOBUS_ROUTING_ENV (behaviour switched by COUNCIL_MUTATE / COUNCIL_SILENT /
# COUNCIL_ROSTER), and the repo is a throwaway tree under AOBUS_AGENT_REPO.
#
# What is exercised is the C0 plumbing council.sh owns — fan-out, the read-only
# tree-immutability canary (attributable per member because each runs in its own
# repo copy), blind-R1 isolation, quorum handling, and dossier assembly. The C3
# acts (the members' judgments and the chair's R4 synthesis) are NOT tested —
# they are not deterministic, which is the whole reason C3 has no validation gate.
# ============================================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
COUNCIL="$(cd "$SCRIPT_DIR/../../.." && pwd)/script/agent/council.sh"
# Source common.sh to unit-check the canary primitive directly (scenario J). Harmless: it only defines
# helpers + AGENT_* path vars; council.sh is still driven as a subprocess for the e2e scenarios.
. "$(cd "$SCRIPT_DIR/../../.." && pwd)/script/agent/common.sh"

PASS=0; FAIL=0
ok()  { PASS=$((PASS + 1)); printf '  ok   %s\n' "$1"; }
bad() { FAIL=$((FAIL + 1)); printf '  FAIL %s\n' "$1"; }
assert_eq() { [ "$2" = "$3" ] && ok "$1" || bad "$1 (got [$2], want [$3])"; }
has()  { case "$2" in *"$3"*) ok "$1" ;; *) bad "$1 (missing [$3])" ;; esac; }
hasnt(){ case "$2" in *"$3"*) bad "$1 (unexpected [$3])" ;; *) ok "$1" ;; esac; }

ROOT="$(mktemp -d)"
trap 'rm -rf "$ROOT"' EXIT

# A small throwaway repo the members get a (disposable) copy of. Never mutated by the test directly;
# the mutate scenario edits a member's COPY (under the out dir), not this tree.
REPO="$ROOT/repo"; mkdir -p "$REPO/lib"
printf 'int main() { return 0; }\n' > "$REPO/lib/foo.cpp"
printf '# Aobus\n' > "$REPO/README.md"

# Mock routing: three council members. Each writes a canned opinion to its slot. COUNCIL_SILENT makes a
# member emit nothing (absent); COUNCIL_MUTATE makes a member edit its read-only copy (a violation);
# COUNCIL_ROSTER overrides the roster (to force a degraded quorum).
ROUTING="$ROOT/mock-routing.env"
cat > "$ROUTING" <<'EOF'
#!/usr/bin/env bash
ROUTE_C3_LABEL="Mock Chair"
_mock_member() {
  local mid="$1" phase="${AGENT_COUNCIL_SLOT%%.*}"   # draft | challenge | revised
  case " ${COUNCIL_SILENT:-} " in *" $mid "*) return 0 ;; esac                       # produce no output
  case " ${COUNCIL_MUTATE:-} "    in *" $mid "*) printf 'stray\n' >> "$AGENT_COUNCIL_CWD/STRAY" ;; esac
  case " ${COUNCIL_MUTATE_R2:-} " in *" $mid "*) [ "$phase" = challenge ] && printf 'stray\n' >> "$AGENT_COUNCIL_CWD/STRAY" ;; esac
  case " ${COUNCIL_FAIL:-} " in *" $mid "*)       # emit PARTIAL output, then exit non-zero (crash/timeout)
    printf 'partial output from %s then crash\n' "$mid" > "$AGENT_COUNCIL_OUT/$AGENT_COUNCIL_SLOT"; return 3 ;;
  esac
  {
    printf '# opinion from %s — slot %s\n' "$mid" "$AGENT_COUNCIL_SLOT"
    printf 'canned council opinion body for %s\n' "$mid"
  } > "$AGENT_COUNCIL_OUT/$AGENT_COUNCIL_SLOT"
}
route_c3_member_m1() { _mock_member m1; }
route_c3_member_m2() { _mock_member m2; }
route_c3_member_m3() { _mock_member m3; }
if [ -n "${COUNCIL_ROSTER:-}" ]; then
  read -r -a ROUTE_C3_MEMBERS <<<"$COUNCIL_ROSTER"
else
  ROUTE_C3_MEMBERS=(route_c3_member_m1 route_c3_member_m2 route_c3_member_m3)
fi
declare -gA ROUTE_C3_MEMBER_LABELS=(
  [route_c3_member_m1]="Mock M1" [route_c3_member_m2]="Mock M2" [route_c3_member_m3]="Mock M3")
EOF

pkt() { cat > "$ROOT/p.md"; }   # write the packet from a heredoc on stdin
N=0
new_out() { N=$((N + 1)); OUTDIR="$ROOT/out.$N"; rm -rf "$OUTDIR"; mkdir -p "$OUTDIR"; }
run_in() { # <packet> [VAR=val...] ; uses current $OUTDIR ; sets RC LOG DOSS
  local packet="$1"; shift
  rm -rf "$ROOT/work"
  env AOBUS_AGENT_REPO="$REPO" AOBUS_AGENT_WORK="$ROOT/work" AOBUS_ROUTING_ENV="$ROUTING" \
      AGENT_COUNCIL_OUT="$OUTDIR" "$@" bash "$COUNCIL" "$packet" > "$ROOT/run.log" 2>&1
  RC=$?
  LOG="$(cat "$ROOT/run.log")"
  DOSS="$(cat "$OUTDIR/dossier.md" 2>/dev/null || true)"
}

PLAN_PKT() { pkt <<'EOF'
---
schema: aobus-phase-packet/v1
kind: council
mode: plan
inputs:
  - lib/foo.cpp
---
Should Aobus model track durations as int seconds or std::chrono::milliseconds?
EOF
}

echo "== A: happy path (3 members + chair draft) -> full dossier, quorum ok, exit 0 =="
new_out
printf '# chair draft\nthe chair plan body\n' > "$OUTDIR/draft.chair.md"
PLAN_PKT
run_in "$ROOT/p.md"
assert_eq "A: exit 0" "$RC" "0"
[ -f "$OUTDIR/dossier.md" ] && ok "A: dossier written" || bad "A: dossier written"
has "A: quorum ok"            "$DOSS" "quorum: ok"
has "A: chair draft section"  "$DOSS" "R1 draft — the chair"
has "A: R1 drafts section"    "$DOSS" "## R1 — blind drafts"
has "A: R2 challenges section" "$DOSS" "## R2 — challenges"
has "A: R3 revised section"   "$DOSS" "## R3 — revised drafts"
has "A: m1 seated"            "$DOSS" "### Mock M1"
has "A: m2 seated"            "$DOSS" "### Mock M2"
has "A: m3 seated"            "$DOSS" "### Mock M3"
[ -f "$OUTDIR/draft.m1.md" ]     && ok "A: draft artifact"     || bad "A: draft artifact"
[ -f "$OUTDIR/challenge.m1.md" ] && ok "A: challenge artifact" || bad "A: challenge artifact"
[ -f "$OUTDIR/revised.m1.md" ]   && ok "A: revised artifact"   || bad "A: revised artifact"

echo "== B: a member mutates its copy -> read-only VIOLATION, discarded, others survive =="
new_out
PLAN_PKT
run_in "$ROOT/p.md" COUNCIL_MUTATE=m2
assert_eq "B: exit 0 (council still completes)" "$RC" "0"
has   "B: violation logged"          "$DOSS" "VIOLATION"
has   "B: violation reason"          "$DOSS" "mutated its working copy"
hasnt "B: violator not seated (no draft section)" "$DOSS" "### Mock M2"
[ ! -f "$OUTDIR/draft.m2.md" ] && ok "B: violator's draft discarded" || bad "B: violator's draft discarded"
has   "B: m1 survives"               "$DOSS" "### Mock M1"
has   "B: m3 survives"               "$DOSS" "### Mock M3"

echo "== C: blindness — R1 prompt carries NO peer text; R2/R3 do =="
new_out
PLAN_PKT
run_in "$ROOT/p.md"
P_DRAFT="$(cat "$OUTDIR/prompt.draft.m1.txt")"
P_CHAL="$(cat "$OUTDIR/prompt.challenge.m1.txt")"
P_REV="$(cat "$OUTDIR/prompt.revise.m1.txt")"
hasnt "C: R1 draft prompt has no peer drafts"  "$P_DRAFT" "=== Draft from"
hasnt "C: R1 draft prompt has no critiques"    "$P_DRAFT" "CRITIQUES"
has   "C: R2 challenge prompt shows peers"      "$P_CHAL"  "=== Draft from"
has   "C: R3 revise prompt shows own draft"     "$P_REV"   "=== YOUR DRAFT ==="
has   "C: R3 revise prompt shows critiques"      "$P_REV"   "CRITIQUES RAISED"
has   "C: inputs surfaced into the prompt (#7)"  "$P_DRAFT" "Pay particular attention to these files: lib/foo.cpp"
has   "C: R3 prompt includes a PEER's challenge (#3)" "$P_REV" "--- from Mock M2 ---"
hasnt "C: R3 prompt excludes the member's OWN challenge (#3)" "$P_REV" "--- from Mock M1 ---"

echo "== D: bad mode (not plan|review) -> reject (exit 64) =="
new_out
pkt <<'EOF'
---
kind: council
mode: design
---
plan something
EOF
run_in "$ROOT/p.md"
assert_eq "D: bad mode -> exit 64" "$RC" "64"
has "D: reports bad mode" "$LOG" "mode must be"

echo "== E: unsafe input path -> reject (exit 2) =="
new_out
pkt <<'EOF'
---
kind: council
mode: plan
inputs:
  - ../etc/passwd
---
plan something
EOF
run_in "$ROOT/p.md"
assert_eq "E: unsafe input -> exit 2" "$RC" "2"
has "E: reports unsafe input" "$LOG" "unsafe input"

echo "== F: single-member roster -> quorum degraded, challenge/revise skipped, dossier still emitted =="
new_out
PLAN_PKT
run_in "$ROOT/p.md" COUNCIL_ROSTER="route_c3_member_m1"
assert_eq "F: exit 0" "$RC" "0"
has   "F: quorum degraded"          "$DOSS" "quorum: degraded"
has   "F: skip logged"              "$LOG"  "skipping challenge/revise"
hasnt "F: no challenge round"       "$DOSS" "## R2 — challenges"
has   "F: lone draft still present" "$DOSS" "### Mock M1"

echo "== G: a silent member is recorded absent; council proceeds with survivors =="
new_out
PLAN_PKT
run_in "$ROOT/p.md" COUNCIL_SILENT=m3
assert_eq "G: exit 0" "$RC" "0"
has   "G: absence logged"      "$DOSS" "ABSENT"
has   "G: absence reason"      "$DOSS" "produced no output"
hasnt "G: silent member not seated" "$DOSS" "### Mock M3"
has   "G: survivor m1 seated"  "$DOSS" "### Mock M1"
has   "G: quorum still ok (2 drafts)" "$DOSS" "quorum: ok"

echo "== H: mode selects the prompt template (plan vs review) =="
new_out
PLAN_PKT
run_in "$ROOT/p.md"
has "H: plan prompt"   "$(cat "$OUTDIR/prompt.draft.m1.txt")" "implementation PLAN"
new_out
pkt <<'EOF'
---
kind: council
mode: review
---
Review this change: lib/foo.cpp now returns int where ao::Result was expected.
EOF
run_in "$ROOT/p.md"
assert_eq "H: review exit 0" "$RC" "0"
has "H: review prompt" "$(cat "$OUTDIR/prompt.draft.m1.txt")" "CODE REVIEW"

echo "== I: AGENT_COUNCIL_OUT inside the repo -> reject before staging (#5) =="
mkdir -p "$REPO/inside-out"
PLAN_PKT
env AOBUS_AGENT_REPO="$REPO" AOBUS_AGENT_WORK="$ROOT/work" AOBUS_ROUTING_ENV="$ROUTING" \
    AGENT_COUNCIL_OUT="$REPO/inside-out" bash "$COUNCIL" "$ROOT/p.md" > "$ROOT/run.log" 2>&1
rc=$?; log="$(cat "$ROOT/run.log")"
assert_eq "I: OUT inside repo -> exit 2" "$rc" "2"
has "I: reports the reject reason" "$log" "inside the repo"
[ -e "$REPO/inside-out/copy.m1" ] && bad "I: must not stage before rejecting" || ok "I: nothing staged before reject"
rm -rf "$REPO/inside-out"

echo "== J: agent_tree_hash canary detects content, mode, AND symlink-retarget mutations (#6) =="
TH="$ROOT/th"; rm -rf "$TH"; mkdir -p "$TH/d"; printf 'x\n' > "$TH/d/f"; ln -s f "$TH/d/lnk"
h0="$(agent_tree_hash "$TH")"
printf 'y\n' >> "$TH/d/f"; h1="$(agent_tree_hash "$TH")"
[ "$h0" != "$h1" ] && ok "J: content change trips canary" || bad "J: content change trips canary"
printf 'x\n' > "$TH/d/f"; chmod +x "$TH/d/f"; h2="$(agent_tree_hash "$TH")"     # content restored, only mode differs
{ [ "$h2" != "$h0" ] && [ "$h2" != "$h1" ]; } && ok "J: chmod trips canary (content-only hash would miss it)" || bad "J: chmod trips canary"
ln -sfn elsewhere "$TH/d/lnk"; h3="$(agent_tree_hash "$TH")"
[ "$h3" != "$h2" ] && ok "J: symlink retarget trips canary" || bad "J: symlink retarget trips canary"

echo "== K: a member that exits non-zero with partial output is discarded, not seated (#2) =="
new_out
PLAN_PKT
run_in "$ROOT/p.md" COUNCIL_FAIL=m2
assert_eq "K: exit 0 (council completes on survivors)" "$RC" "0"
has   "K: failure logged"              "$DOSS" "FAILED"
has   "K: failure reason (non-zero rc)" "$DOSS" "exited non-zero"
hasnt "K: failed member not seated"    "$DOSS" "### Mock M2"
[ ! -f "$OUTDIR/draft.m2.md" ] && ok "K: partial output discarded" || bad "K: partial output discarded"
has   "K: survivor m1 seated"          "$DOSS" "### Mock M1"

echo "== L: a member clean in R1 that violates in R2 is quarantined entirely, incl. its R1 draft (#4) =="
new_out
PLAN_PKT
run_in "$ROOT/p.md" COUNCIL_MUTATE_R2=m2
assert_eq "L: exit 0" "$RC" "0"
has   "L: quarantine announced"        "$LOG"  "quarantined: m2"
has   "L: R2 violation logged"         "$DOSS" "VIOLATION  challenge m2"
hasnt "L: quarantined member's R1 draft purged from dossier" "$DOSS" "### Mock M2"
has   "L: clean members remain"        "$DOSS" "### Mock M1"
has   "L: clean members remain (m3)"   "$DOSS" "### Mock M3"

echo "============================================================"
echo "PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ] || { echo "=== COUNCIL TESTS FAILED ==="; exit 1; }
echo "=== ALL COUNCIL TESTS PASSED ==="
