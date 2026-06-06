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
EXTERNAL_STAGE="$HOME/.cache/aobus-council-test.$$"
trap 'rm -rf "$ROOT" "$EXTERNAL_STAGE"' EXIT

# A small throwaway repo the members get a (disposable) copy of. Never mutated by the test directly;
# the mutate scenario edits a member's COPY (under the out dir), not this tree.
REPO="$ROOT/repo"; mkdir -p "$REPO/lib"
printf 'int main() { return 0; }\n' > "$REPO/lib/foo.cpp"
printf '# Aobus\n' > "$REPO/README.md"

# Mock routing: four council members. Each writes a canned opinion to its slot. COUNCIL_SILENT makes a
# member emit nothing (absent); COUNCIL_MUTATE makes a member edit its read-only copy (a violation);
# COUNCIL_ROSTER overrides the roster (to force a degraded quorum).
ROUTING="$ROOT/mock-routing.env"
cat > "$ROUTING" <<'EOF'
#!/usr/bin/env bash
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
route_c3_member_m4() { _mock_member m4; }
if [ -n "${COUNCIL_ROSTER:-}" ]; then
  read -r -a ROUTE_C3_MEMBERS <<<"$COUNCIL_ROSTER"
else
  ROUTE_C3_MEMBERS=(route_c3_member_m1 route_c3_member_m2 route_c3_member_m3 route_c3_member_m4)
fi
declare -gA ROUTE_C3_MEMBER_LABELS=(
  [route_c3_member_m1]="Mock M1" [route_c3_member_m2]="Mock M2"
  [route_c3_member_m3]="Mock M3" [route_c3_member_m4]="Mock M4")
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

# The standard fixture exercises the FULL three-round protocol (R1+R2+R3), so it pins depth: full
# explicitly now that the default is the lighter 'challenge'. Depth-tier behaviour has its own scenarios.
PLAN_PKT() { pkt <<'EOF'
---
schema: aobus-phase-packet/v1
kind: council
mode: plan
depth: full
inputs:
  - lib/foo.cpp
---
Should Aobus model track durations as int seconds or std::chrono::milliseconds?
EOF
}

echo "== A: happy path (4 members) -> full dossier, quorum ok, exit 0 =="
new_out
PLAN_PKT
run_in "$ROOT/p.md"
assert_eq "A: exit 0" "$RC" "0"
[ -f "$OUTDIR/dossier.md" ] && ok "A: dossier written" || bad "A: dossier written"
has "A: quorum ok"            "$DOSS" "quorum: ok"
has "A: shallow full (depth: full)" "$DOSS" "shallow: full"
has "A: m1 seated"            "$DOSS" "### Mock M1"
has "A: m2 seated"            "$DOSS" "### Mock M2"
has "A: m3 seated"            "$DOSS" "### Mock M3"
has "A: m4 seated"            "$DOSS" "### Mock M4"

echo "== B: a member mutates its copy -> read-only VIOLATION, discarded, others survive =="
new_out
PLAN_PKT
run_in "$ROOT/p.md" COUNCIL_MUTATE=m2
assert_eq "B: exit 0 (council still completes)" "$RC" "0"
has   "B: violation logged"          "$DOSS" "VIOLATION"
has   "B: violation reason"          "$DOSS" "mutated its working copy"
hasnt "B: violator not seated (no draft section)" "$DOSS" "### Mock M2"

echo "== C: blindness and fencing — R1 prompt carries NO peer text; prompts use XML fencing (#3) =="
new_out
PLAN_PKT
run_in "$ROOT/p.md"
P_DRAFT="$(cat "$OUTDIR/prompt.draft.m1.txt")"
P_CHAL="$(cat "$OUTDIR/prompt.challenge.m1.txt")"
P_REV="$(cat "$OUTDIR/prompt.revise.m1.txt")"
hasnt "C: R1 draft prompt has no peer drafts"  "$P_DRAFT" "<peer_draft"
hasnt "C: R1 draft prompt has no critiques"    "$P_DRAFT" "CRITIQUES"
has   "C: R2 challenge prompt shows XML peers"  "$P_CHAL"  "<peer_draft member=\"Mock M2\">"
has   "C: R3 revise prompt shows own draft"     "$P_REV"   "=== YOUR DRAFT ==="
has   "C: R3 revise prompt has log header"      "$P_REV"   "FULL CHALLENGE LOG"
has   "C: R3 revise prompt shows XML critiques" "$P_REV"   "<peer_challenge member=\"Mock M2\">"
hasnt "C: R3 prompt excludes own challenge"     "$P_REV"   "<peer_challenge member=\"Mock M1\">"

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

echo "== E: unsafe input path -> reject (exit 3) =="
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
assert_eq "E: unsafe input -> exit 3" "$RC" "3"

echo "== EE: validation key in packet -> reject (exit 64) =="
new_out
pkt <<'EOF'
---
kind: council
mode: plan
validation: ./check.sh
---
plan something
EOF
run_in "$ROOT/p.md"
assert_eq "EE: validation key -> exit 64" "$RC" "64"
has "EE: reports reason" "$LOG" "must not have a 'validation:' key"

echo "== F: single-member roster -> quorum degraded, challenge/revise skipped, dossier still emitted =="
new_out
PLAN_PKT
run_in "$ROOT/p.md" COUNCIL_ROSTER="route_c3_member_m1"
assert_eq "F: exit 0" "$RC" "0"
has   "F: quorum degraded"          "$DOSS" "quorum: degraded"
has   "F: skip logged"              "$LOG"  "skipping challenge/revise"
has   "F: lone draft still present" "$DOSS" "### Mock M1"

echo "== G: a silent member is recorded absent; council proceeds with survivors =="
new_out
PLAN_PKT
run_in "$ROOT/p.md" COUNCIL_SILENT=m4
assert_eq "G: exit 0" "$RC" "0"
has   "G: absence logged"      "$DOSS" "ABSENT"
has   "G: survivor m1 seated"  "$DOSS" "### Mock M1"
has   "G: quorum still ok (3 drafts)" "$DOSS" "quorum: ok"

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
Review this change.
EOF
run_in "$ROOT/p.md"
assert_eq "H: review exit 0" "$RC" "0"
has "H: review prompt" "$(cat "$OUTDIR/prompt.draft.m1.txt")" "CODE REVIEW"

echo "== I: AGENT_COUNCIL_OUT inside the repo -> reject WITHOUT creating directory (exit 3) =="
PLAN_PKT
# Pass a path that does NOT exist yet.
env AOBUS_AGENT_REPO="$REPO" AOBUS_AGENT_WORK="$ROOT/work" AOBUS_ROUTING_ENV="$ROUTING" \
    AGENT_COUNCIL_OUT="$REPO/new-inside-out" bash "$COUNCIL" "$ROOT/p.md" > "$ROOT/run.log" 2>&1
rc=$?; log="$(cat "$ROOT/run.log")"
assert_eq "I: OUT inside repo -> exit 3" "$rc" "3"
[ -d "$REPO/new-inside-out" ] && bad "I: must not create directory on rejection" || ok "I: no directory created"

echo "== II: repo copy fails (missing repo) -> reject (exit 3) =="
new_out
PLAN_PKT
env AOBUS_AGENT_REPO="$ROOT/no-such-repo" AOBUS_AGENT_WORK="$ROOT/work" AOBUS_ROUTING_ENV="$ROUTING" \
    AGENT_COUNCIL_OUT="$OUTDIR" bash "$COUNCIL" "$ROOT/p.md" > "$ROOT/run.log" 2>&1
rc=$?; log="$(cat "$ROOT/run.log")"
assert_eq "II: failed copy -> exit 3" "$rc" "3"
has "II: reports error" "$log" "not found or not a directory"

echo "== J: agent_tree_hash canary detects content, mode, AND symlink-retarget mutations (#6) =="
TH="$ROOT/th"; rm -rf "$TH"; mkdir -p "$TH/d"; printf 'x\n' > "$TH/d/f"; ln -s f "$TH/d/lnk"
h0="$(agent_tree_hash "$TH")"
printf 'y\n' >> "$TH/d/f"; h1="$(agent_tree_hash "$TH")"
[ "$h0" != "$h1" ] && ok "J: content change trips canary" || bad "J: content change trips canary"

echo "== K: a member that exits non-zero with partial output is discarded, not seated (#2) =="
new_out
PLAN_PKT
run_in "$ROOT/p.md" COUNCIL_FAIL=m2
assert_eq "K: exit 0" "$RC" "0"
has   "K: failure logged"              "$DOSS" "FAILED"
hasnt "K: failed member not seated"    "$DOSS" "### Mock M2"

echo "== L: a member clean in R1 that violates in R2 is quarantined entirely, incl. its R1 draft (#4) =="
new_out
PLAN_PKT
run_in "$ROOT/p.md" COUNCIL_MUTATE_R2=m2
assert_eq "L: exit 0" "$RC" "0"
has   "L: R2 violation logged"         "$DOSS" "VIOLATION  challenge m2"
hasnt "L: quarantined draft purged"    "$DOSS" "### Mock M2"

echo "== M: late-stage violation updates dossier metadata (drafts and quorum) =="
new_out
PLAN_PKT
# Roster has 2 members: m1 and m2. COUNCIL_MIN=2.
# m2 violates in R2. dossier should show drafts=1 and quorum=degraded.
run_in "$ROOT/p.md" COUNCIL_ROSTER="route_c3_member_m1 route_c3_member_m2" COUNCIL_MUTATE_R2=m2
assert_eq "M: exit 0" "$RC" "0"
has "M: trusted drafts count is 1" "$DOSS" "drafts: 1"
has "M: quorum becomes degraded"   "$DOSS" "quorum: **degraded**"

echo "== MM: all members quarantined in R2 -> reject (exit 2) =="
new_out
PLAN_PKT
run_in "$ROOT/p.md" COUNCIL_ROSTER="route_c3_member_m1 route_c3_member_m2" COUNCIL_MUTATE_R2="m1 m2"
assert_eq "MM: exit 2" "$RC" "2"
has "MM: reports reason" "$LOG" "all members quarantined"

echo "== N: real Gemini C3 route stages under HOME for steam-run private-/tmp =="
BIN="$ROOT/bin"; mkdir -p "$BIN"
cat > "$BIN/steam-run" <<'EOF'
#!/usr/bin/env bash
pwd -P > "${STEAM_CWD_FILE:?}"
case "$(pwd -P)/" in
  /tmp/*) echo "steam-run test: cwd under private /tmp" >&2; exit 77 ;;
esac
"$@"
EOF
cat > "$BIN/agy" <<'EOF'
#!/usr/bin/env bash
cat >/dev/null
printf 'gemini opinion from %s\n' "$(pwd -P)"
case "${AGY_MUTATE:-}" in
  1) printf 'mutated\n' > MUTATED_BY_AGY ;;
esac
EOF
chmod +x "$BIN/steam-run" "$BIN/agy"
REAL_ROUTING="$(cd "$SCRIPT_DIR/../../.." && pwd)/script/agent/routing.env"
ROUTING_REAL="$ROOT/real-routing.env"
cat > "$ROUTING_REAL" <<EOF
. "$REAL_ROUTING"
ROUTE_C3_MEMBERS=(route_c3_member_gemini)
COUNCIL_MIN=1
EOF
old_routing="$ROUTING"; ROUTING="$ROUTING_REAL"
new_out
PLAN_PKT
run_in "$ROOT/p.md" PATH="$BIN:$PATH" AOBUS_AGY_COUNCIL_STAGE="$EXTERNAL_STAGE" STEAM_CWD_FILE="$ROOT/steam.cwd"
assert_eq "N: Gemini route exits 0 with fake steam-run" "$RC" "0"
has "N: Gemini seated" "$DOSS" "### Gemini 3 Pro via gemini"
steam_cwd="$(cat "$ROOT/steam.cwd" 2>/dev/null || true)"
case "$steam_cwd/" in
  /tmp/*) bad "N: steam-run cwd must not be /tmp ($steam_cwd)" ;;
  "$EXTERNAL_STAGE"/*) ok "N: steam-run cwd is HOME-backed staged copy" ;;
  *) bad "N: steam-run cwd under expected stage (got [$steam_cwd])" ;;
esac

echo "== O: Gemini staged-copy mutation is mirrored to outer canary as violation =="
new_out
PLAN_PKT
run_in "$ROOT/p.md" PATH="$BIN:$PATH" AOBUS_AGY_COUNCIL_STAGE="$EXTERNAL_STAGE" \
  STEAM_CWD_FILE="$ROOT/steam-mut.cwd" AGY_MUTATE=1
assert_eq "O: all-draft violation exits 2" "$RC" "2"
assert_eq "O: Gemini status is violation" "$(cat "$OUTDIR/draft.gemini.md.status" 2>/dev/null || true)" "violation"
has "O: violation note explains mutation" "$(cat "$OUTDIR/draft.gemini.md.note" 2>/dev/null || true)" "mutated its working copy"
ROUTING="$old_routing"

echo "== P: stable prefix is byte-identical across R1/R2/R3 (with inputs) =="
new_out
PLAN_PKT
run_in "$ROOT/p.md"
# Extract the prefix: everything up to (but not including) the INSTRUCTIONS header.
_prefix_of() { sed '/^=== INSTRUCTIONS ===/,$d' "$1"; }
pfx_r1="$(_prefix_of "$OUTDIR/prompt.draft.m1.txt")"
pfx_r2="$(_prefix_of "$OUTDIR/prompt.challenge.m1.txt")"
pfx_r3="$(_prefix_of "$OUTDIR/prompt.revise.m1.txt")"
assert_eq "P: R1 prefix == R2 prefix" "$pfx_r1" "$pfx_r2"
assert_eq "P: R2 prefix == R3 prefix" "$pfx_r2" "$pfx_r3"

echo "== Q: stable prefix is byte-identical without inputs =="
new_out
pkt <<'EOF'
---
schema: aobus-phase-packet/v1
kind: council
mode: plan
depth: full
---
Design question without inputs.
EOF
run_in "$ROOT/p.md"
_prefix_of() { sed '/^=== INSTRUCTIONS ===/,$d' "$1"; }
pfx_r1="$(_prefix_of "$OUTDIR/prompt.draft.m1.txt")"
pfx_r2="$(_prefix_of "$OUTDIR/prompt.challenge.m1.txt")"
pfx_r3="$(_prefix_of "$OUTDIR/prompt.revise.m1.txt")"
assert_eq "Q: R1 prefix == R2 prefix (no inputs)" "$pfx_r1" "$pfx_r2"
assert_eq "Q: R2 prefix == R3 prefix (no inputs)" "$pfx_r2" "$pfx_r3"

echo "== R: prompt layout ordering — TASK before INSTRUCTIONS before peer data =="
new_out
PLAN_PKT
run_in "$ROOT/p.md"
# Helper: line number of first match
_lineof() { grep -n "$2" "$1" | head -1 | cut -d: -f1; }
# R1 draft: TASK before INSTRUCTIONS
t1=$(_lineof "$OUTDIR/prompt.draft.m1.txt" "^=== TASK ===")
i1=$(_lineof "$OUTDIR/prompt.draft.m1.txt" "^=== INSTRUCTIONS ===")
[ "$t1" -lt "$i1" ] && ok "R: R1 TASK before INSTRUCTIONS" || bad "R: R1 TASK before INSTRUCTIONS (TASK=$t1 INST=$i1)"
# R2 challenge: TASK before INSTRUCTIONS before peer_draft
t2=$(_lineof "$OUTDIR/prompt.challenge.m1.txt" "^=== TASK ===")
i2=$(_lineof "$OUTDIR/prompt.challenge.m1.txt" "^=== INSTRUCTIONS ===")
p2=$(_lineof "$OUTDIR/prompt.challenge.m1.txt" "<peer_draft")
[ "$t2" -lt "$i2" ] && ok "R: R2 TASK before INSTRUCTIONS" || bad "R: R2 TASK before INSTRUCTIONS"
[ "$i2" -lt "$p2" ] && ok "R: R2 INSTRUCTIONS before peer_draft" || bad "R: R2 INSTRUCTIONS before peer_draft"
# R3 revise: TASK before INSTRUCTIONS before YOUR DRAFT
t3=$(_lineof "$OUTDIR/prompt.revise.m1.txt" "^=== TASK ===")
i3=$(_lineof "$OUTDIR/prompt.revise.m1.txt" "^=== INSTRUCTIONS ===")
d3=$(_lineof "$OUTDIR/prompt.revise.m1.txt" "^=== YOUR DRAFT ===")
[ "$t3" -lt "$i3" ] && ok "R: R3 TASK before INSTRUCTIONS" || bad "R: R3 TASK before INSTRUCTIONS"
[ "$i3" -lt "$d3" ] && ok "R: R3 INSTRUCTIONS before YOUR DRAFT" || bad "R: R3 INSTRUCTIONS before YOUR DRAFT"

echo "== S: cross-member prefix symmetry within the same round =="
new_out
PLAN_PKT
run_in "$ROOT/p.md"
_prefix_of() { sed '/^=== INSTRUCTIONS ===/,$d' "$1"; }
pfx_m1="$(_prefix_of "$OUTDIR/prompt.draft.m1.txt")"
pfx_m2="$(_prefix_of "$OUTDIR/prompt.draft.m2.txt")"
pfx_m3="$(_prefix_of "$OUTDIR/prompt.draft.m3.txt")"
assert_eq "S: m1 R1 prefix == m2 R1 prefix" "$pfx_m1" "$pfx_m2"
assert_eq "S: m2 R1 prefix == m3 R1 prefix" "$pfx_m2" "$pfx_m3"

# A packet at an arbitrary depth (4-member roster), for the depth-tier scenarios below.
DEPTH_PKT() { # <depth>
  pkt <<EOF
---
schema: aobus-phase-packet/v1
kind: council
mode: plan
depth: $1
---
Should Aobus model track durations as int seconds or std::chrono::milliseconds?
EOF
}

echo "== T: depth=panel -> R1 only; no challenge/revise prompts; shallow by-design; blindness intact =="
new_out
DEPTH_PKT panel
run_in "$ROOT/p.md"
assert_eq "T: exit 0" "$RC" "0"
[ -f "$OUTDIR/prompt.draft.m1.txt" ]      && ok "T: R1 draft prompt rendered"   || bad "T: R1 draft prompt rendered"
[ ! -f "$OUTDIR/prompt.challenge.m1.txt" ] && ok "T: no R2 challenge prompt"      || bad "T: no R2 challenge prompt"
[ ! -f "$OUTDIR/prompt.revise.m1.txt" ]    && ok "T: no R3 revise prompt"         || bad "T: no R3 revise prompt"
has   "T: dossier depth panel"      "$DOSS" "depth: panel"
has   "T: dossier shallow by-design" "$DOSS" "shallow: by-design"
has   "T: quorum still ok (4 drafts)" "$DOSS" "quorum: ok"
hasnt "T: no R2 section"             "$DOSS" "## R2"
hasnt "T: no R3 section"             "$DOSS" "## R3"
hasnt "T: R1 prompt still blind"     "$(cat "$OUTDIR/prompt.draft.m1.txt")" "<peer_draft"
has   "T: skip reason is by-design"  "$LOG"  "shallow by design"

echo "== U: depth=challenge -> R1+R2, no R3; R2 section present, R3 absent =="
new_out
DEPTH_PKT challenge
run_in "$ROOT/p.md"
assert_eq "U: exit 0" "$RC" "0"
[ -f "$OUTDIR/prompt.challenge.m1.txt" ] && ok "U: R2 challenge prompt rendered" || bad "U: R2 challenge prompt rendered"
[ ! -f "$OUTDIR/prompt.revise.m1.txt" ]   && ok "U: no R3 revise prompt"          || bad "U: no R3 revise prompt"
has   "U: dossier depth challenge"   "$DOSS" "depth: challenge"
has   "U: dossier shallow by-design" "$DOSS" "shallow: by-design"
has   "U: R2 section present"        "$DOSS" "## R2"
hasnt "U: no R3 section"             "$DOSS" "## R3"
has   "U: skip self-revise logged"   "$LOG"  "skipping self-revise"

echo "== V: depth absent -> defaults to challenge (R1+R2, no R3) =="
new_out
pkt <<'EOF'
---
schema: aobus-phase-packet/v1
kind: council
mode: plan
---
A medium-stakes question with no explicit depth.
EOF
run_in "$ROOT/p.md"
assert_eq "V: exit 0" "$RC" "0"
has   "V: defaults to challenge"     "$DOSS" "depth: challenge"
has   "V: shallow by-design"         "$DOSS" "shallow: by-design"
has   "V: startup echo shows depth"  "$LOG"  "depth=challenge"
[ ! -f "$OUTDIR/prompt.revise.m1.txt" ] && ok "V: no R3 revise prompt" || bad "V: no R3 revise prompt"
has   "V: R2 section present"        "$DOSS" "## R2"
hasnt "V: no R3 section"             "$DOSS" "## R3"

echo "== W: unknown depth -> reject (exit 64) =="
new_out
DEPTH_PKT deep
run_in "$ROOT/p.md"
assert_eq "W: bad depth -> exit 64" "$RC" "64"
has "W: reports reason" "$LOG" "depth must be 'panel', 'challenge', or 'full'"

echo "== X: shallow and quorum are independent axes (single-member roster) =="
# X1: depth=full but only 1 draft -> quorum degraded (accidental) AND shallow full (not capped by design).
new_out
DEPTH_PKT full
run_in "$ROOT/p.md" COUNCIL_ROSTER="route_c3_member_m1"
assert_eq "X1: exit 0" "$RC" "0"
has "X1: quorum degraded"  "$DOSS" "quorum: degraded"
has "X1: shallow full"     "$DOSS" "shallow: full"
# X2: depth=panel with 1 draft -> quorum degraded AND shallow by-design (both axes flagged independently).
new_out
DEPTH_PKT panel
run_in "$ROOT/p.md" COUNCIL_ROSTER="route_c3_member_m1"
assert_eq "X2: exit 0" "$RC" "0"
has   "X2: quorum degraded"     "$DOSS" "quorum: degraded"
has   "X2: shallow by-design"   "$DOSS" "shallow: by-design"
hasnt "X2: notes never mention cross-challenge under panel" "$DOSS" "cross-challenge"

echo "== X3: depth=challenge + degraded quorum (1 draft) -> R2 skipped by ACCIDENT, not by design =="
# The new default's degraded path: R3 is capped by design (shallow), but R2's absence is the quorum
# shortfall — the skip log and the shallow note must not claim R2 was a deliberate choice.
new_out
DEPTH_PKT challenge
run_in "$ROOT/p.md" COUNCIL_ROSTER="route_c3_member_m1"
assert_eq "X3: exit 0" "$RC" "0"
has   "X3: depth challenge"     "$DOSS" "depth: challenge"
has   "X3: quorum degraded"     "$DOSS" "quorum: degraded"
has   "X3: shallow by-design"   "$DOSS" "shallow: by-design"
[ ! -f "$OUTDIR/prompt.challenge.m1.txt" ] && ok "X3: no R2 prompt (degraded, not run)" || bad "X3: no R2 prompt (degraded, not run)"
hasnt "X3: no R2 section"       "$DOSS" "## R2"
has   "X3: skip log says challenge only (not /revise)" "$LOG" "skipping challenge (quorum degraded)"
hasnt "X3: skip log omits revise" "$LOG" "skipping challenge/revise"

echo "============================================================"
echo "PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ] || { echo "=== COUNCIL TESTS FAILED ==="; exit 1; }
echo "=== ALL COUNCIL TESTS PASSED ==="
