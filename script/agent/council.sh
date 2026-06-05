#!/usr/bin/env bash
# script/agent/council.sh — Aobus agent-fleet C3 "council" orchestrator (§11 of the design doc).
#
# C3 work (plan / review) has NO deterministic oracle — a plan or a review is prose, and "correct" is a
# judgment. So instead of the C1/C2 model (cheap worker + deterministic gate), C3 convenes a COUNCIL: a
# panel of cross-vendor frontier models that draft independently, then CHALLENGE each other, then revise,
# and finally the in-loop chair (the frontier agent that ran this script) SYNTHESIZES a single answer.
#
# This script is itself C0 plumbing: it fans members out, collects their prose, runs the canary, and
# assembles a dossier. It makes NO judgments and it deliberately does NOT do the final synthesis (round
# 4) — that is the one irreducibly-frontier act and stays with the chair, in-loop, which reads the
# dossier this script prints.
#
# Protocol (members run R1-R3; the chair is only the R4 verifier/synthesizer):
#   R1 BLIND DRAFT  each member drafts independently, with NO peer context (diversity-preserving).
#   R2 CHALLENGE    each member is shown the OTHERS' drafts and critiques them.
#   R3 SELF-REVISE  each member revises its OWN draft having seen the critiques of it.
#   R4 SYNTHESIS    (NOT here) the chair verifies dossier claims, then writes the final plan/review.
#
# Safety: a council member is READ-ONLY — it produces an OPINION, never a patch, and must not touch the
# tree. Each member runs in its own disposable COPY of the repo (cwd), and council.sh content-hashes that
# copy before/after the call; any member that mutated its copy has its output DISCARDED and flagged (the
# opposite of C1, where a diff is the deliverable). This is process isolation for a TRUSTED roster, not a
# hard sandbox (an agentic CLI could still escape the cwd — see §10.3); members are additionally invoked
# in each vendor's read-only headless mode.
#
# Packet (YAML frontmatter + markdown body; schema aobus-phase-packet/v1):
#   kind: council            (required)
#   mode: plan | review      (required — selects the prompt templates)
#   inputs: [...]            (optional repo-relative paths the chair wants emphasized; safety-checked)
#   <body>                   the QUESTION: the task to plan, or the change to review (+ context)
# A council has NO `validation:` — there is no deterministic gate (that is the whole point of C3), so
# council packets do NOT go through dispatch.sh's allowlist path; this is a separate entry.
#
# Usage: script/agent/council.sh <packet.md>
# Exit:  0 = dossier emitted (path on stdout; possibly quorum-degraded — flagged inside) ; 2 = no usable
#        draft ; 3 = configuration / system error ; 5 = routing table missing ; 64 = bad packet.
set -u

. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"
agent_load_routing || exit 5

PACKET="${1:?council: need a council packet path}"
[ -r "$PACKET" ] || { echo "council: cannot read packet '$PACKET'" >&2; exit 64; }

KIND="$(agent_packet_scalar "$PACKET" kind 2>/dev/null)"
MODE="$(agent_packet_scalar "$PACKET" mode 2>/dev/null)"
mapfile -t INPUTS < <(agent_packet_list "$PACKET" inputs 2>/dev/null)
QUESTION="$(agent_packet_body "$PACKET")"

# --- contract gates: reject early ---
[ "$KIND" = "council" ] || { echo "council: packet kind must be 'council' (got '${KIND:-}')" >&2; exit 64; }
case "$MODE" in
  plan | review) ;;
  *) echo "council: mode must be 'plan' or 'review' (got '${MODE:-}')" >&2; exit 64 ;;
esac
[ -n "$QUESTION" ] || { echo "council: packet has an empty body (the question)" >&2; exit 64; }
for f in "${INPUTS[@]}"; do
  agent_arg_safe "$f" || { echo "council: unsafe input '$f'" >&2; exit 3; }
done

# Enforcement: C3 council is prose-only; it never runs a deterministic validation gate.
if agent_packet_scalar "$PACKET" validation 2>/dev/null | grep -q "."; then
  echo "council: packet must not have a 'validation:' key (C3 has no deterministic gate)" >&2
  exit 64
fi

# --- roster (cross-vendor by default; configurable/opt-in via ROUTE_C3_MEMBERS, like C1 candidates) ---
if declare -p ROUTE_C3_MEMBERS >/dev/null 2>&1 && [ "${#ROUTE_C3_MEMBERS[@]}" -gt 0 ]; then
  MEMBERS=("${ROUTE_C3_MEMBERS[@]}")
else
  MEMBERS=(route_c3_member_claude_opus route_c3_member_codex route_c3_member_gemini route_c3_member_dspro)
fi
COUNCIL_MIN="${COUNCIL_MIN:-2}"   # member drafts needed for a real debate; below = degraded

OUT="${AGENT_COUNCIL_OUT:-$AGENT_WORK/council/$(date +%Y%m%d-%H%M%S)-$$}"
export AGENT_COUNCIL_OUT="$OUT"

# Guard: the out dir MUST be outside the repo. Each member's cwd is a COPY of $AGENT_REPO; if $OUT lived
# under the repo, staging would copy the out dir (incl. in-flight artifacts) into every member's cwd —
# breaking R1 blindness and polluting the canary. Reject rather than silently corrupt.
_real_repo="$(cd "$AGENT_REPO" 2>/dev/null && pwd -P)"
[ -n "$_real_repo" ] || { echo "council: AGENT_REPO ('$AGENT_REPO') not found or not a directory" >&2; exit 3; }

# Resolve the absolute path of $OUT without creating it to avoid repository mutation on rejection.
_real_out="$(readlink -f "$OUT" 2>/dev/null || echo "$OUT")"
case "$_real_out/" in
  "$_real_repo"/*)
    echo "council: AGENT_COUNCIL_OUT ('$OUT') is inside the repo ('$AGENT_REPO') -> reject (would leak into member copies and break R1 blindness)" >&2
    exit 3 ;;
esac
mkdir -p "$OUT"

c3_label() { if declare -p ROUTE_C3_MEMBER_LABELS >/dev/null 2>&1; then printf '%s' "${ROUTE_C3_MEMBER_LABELS[$1]:-$1}"; else printf '%s' "$1"; fi; }
mid_of()   { printf '%s' "${1#route_c3_member_}"; }

# Mode-specific nouns woven into the prompts.
case "$MODE" in
  plan)
    ARTIFACT="implementation PLAN"
    STRUCT="Approach / Files to change (real paths) / Risks & edge cases / Alternatives considered / Open questions" ;;
  review)
    ARTIFACT="CODE REVIEW"
    STRUCT="Findings (each: severity [blocker|major|minor|nit], location file:line, problem, suggested fix) / Correctness & regressions / Overall verdict [approve|approve-with-nits|request-changes]" ;;
esac
PREAMBLE="You are one member of a cross-vendor advisory council for Aobus (a C++26 music app). You are running in a READ-ONLY, NON-GIT temporary copy: you may READ files but MUST NOT use git commands or modify any file. Put your ENTIRE response on stdout as markdown."

# The packet's validated `inputs:` (emphasis paths) are surfaced to every member; empty string if none.
INPUTS_NOTE=""
[ "${#INPUTS[@]}" -gt 0 ] && INPUTS_NOTE="$(printf 'Pay particular attention to these files: %s' "${INPUTS[*]}")"

# render_*: write the round prompt for one member to a file (also the audit record). R1 carries NO peer
# text — blindness is what keeps the drafts diverse; the offline test asserts this.
render_draft() { # <mid> <promptfile>
  { printf '%s\n\n' "$PREAMBLE"
    printf 'Independently produce an %s for the task below. Work alone — do not assume any other input.\n' "$ARTIFACT"
    printf 'Structure your answer as: %s.\n' "$STRUCT"
    printf 'Be concrete and cite real files where relevant.\n\n'
    [ -n "$INPUTS_NOTE" ] && printf '%s\n\n' "$INPUTS_NOTE"
    printf '=== TASK ===\n%s\n' "$QUESTION"
  } > "$2"
}
render_challenge() { # <mid> <promptfile> ; peers = every OTHER member draft
  { printf '%s\n\n' "$PREAMBLE"
    printf 'Below are the OTHER council members %s drafts for the same task. CHALLENGE them: find flaws,\n' "$ARTIFACT"
    printf 'missed cases, hidden risks, wrong assumptions, and points of disagreement. Be specific and\n'
    printf 'refer to peer drafts by their member label.\n\n'
    [ -n "$INPUTS_NOTE" ] && printf '%s\n\n' "$INPUTS_NOTE"
    printf '=== TASK ===\n%s\n\n' "$QUESTION"
    local mid="$1" peer pmid
    for peer in "${SEATED[@]}"; do
      pmid="$(mid_of "$peer")"; [ "$pmid" = "$mid" ] && continue
      printf '<peer_draft member="%s">\n' "$(c3_label "$peer")"
      cat "$OUT/draft.$pmid.md"
      printf '\n</peer_draft>\n\n'
    done
  } > "$2"
}
render_revise() { # <mid> <promptfile> ; own draft + the full challenge log (member self-selects critiques of it)
  { printf '%s\n\n' "$PREAMBLE"
    printf 'Below is YOUR earlier draft %s and the full challenge log from the other members. Produce a REVISED\n' "$ARTIFACT"
    printf '%s that addresses the valid critiques of YOUR draft and explicitly defends any point where you\n' "$ARTIFACT"
    printf 'disagree. Identify the critiques aimed at YOUR draft within the log.\n\n'
    printf 'Keep the same structure: %s.\n\n' "$STRUCT"
    [ -n "$INPUTS_NOTE" ] && printf '%s\n\n' "$INPUTS_NOTE"
    printf '=== TASK ===\n%s\n\n' "$QUESTION"
    printf '=== YOUR DRAFT ===\n'; cat "$OUT/draft.$1.md"; printf '\n\n'
    printf '=== FULL CHALLENGE LOG (Identify the critiques aimed at your draft) ===\n'
    local peer pmid
    for peer in "${SEATED[@]}"; do
      pmid="$(mid_of "$peer")"
      [ "$pmid" = "$1" ] && continue                       # skip the member's OWN challenge (it critiques others)
      [ -s "$OUT/challenge.$pmid.md" ] || continue
      printf '<peer_challenge member="%s">\n' "$(c3_label "$peer")"
      cat "$OUT/challenge.$pmid.md"
      printf '\n</peer_challenge>\n\n'
    done
  } > "$2"
}

# run_one <fn> <mid> <phase> <promptfile> : run one member for one round in the background. Each member
# works in its OWN repo copy so the read-only canary is attributable under parallel fan-out.
run_one() {
  local fn="$1" mid="$2" phase="$3" pf="$4"
  local slot="$phase.$mid.md" cwd="$OUT/copy.$mid"
  local pre post rc
  pre="$(agent_tree_hash "$cwd")"
  # The member reads its prompt from AGENT_COUNCIL_PROMPT_FILE (on stdin, no argv -> no ARG_MAX limit).
  ( AGENT_COUNCIL_CWD="$cwd"; AGENT_COUNCIL_SLOT="$slot"; AGENT_COUNCIL_PROMPT_FILE="$pf"; "$fn" ); rc=$?
  post="$(agent_tree_hash "$cwd")"
  : > "$OUT/$slot.note"
  if [ "$pre" != "$post" ]; then                         # mutation is the most severe outcome (checked first)
    rm -f "$OUT/$slot"                                   # a violator's opinion is not trusted
    printf 'violation' > "$OUT/$slot.status"
    printf 'VIOLATION  %-8s %-9s [%s]: member mutated its working copy (read-only contract broken)\n' \
      "$phase" "$mid" "$(c3_label "$fn")" > "$OUT/$slot.note"
  elif [ "$rc" -ne 0 ]; then                             # non-zero exit: a partial answer from a crash/timeout is not trusted
    rm -f "$OUT/$slot"
    printf 'failed' > "$OUT/$slot.status"
    printf 'FAILED     %-8s %-9s [%s]: exited non-zero (rc=%s; timeout/crash) -> output discarded\n' \
      "$phase" "$mid" "$(c3_label "$fn")" "$rc" > "$OUT/$slot.note"
  elif [ -s "$OUT/$slot" ]; then
    printf 'ok' > "$OUT/$slot.status"
  else
    printf 'empty' > "$OUT/$slot.status"
    printf 'ABSENT     %-8s %-9s [%s]: produced no output (empty)\n' \
      "$phase" "$mid" "$(c3_label "$fn")" > "$OUT/$slot.note"
  fi
}

status_of() { cat "$OUT/$1.md.status" 2>/dev/null || echo missing; }   # $1 = "<phase>.<mid>"

# quarantine <phase> : a member that committed a read-only VIOLATION in any phase is untrusted from then
# on — drop it from SEATED so it neither taints later rounds nor appears (incl. its earlier drafts) in the
# dossier. Only a violation quarantines; a merely absent/failed challenge leaves the member seated.
quarantine() {
  local phase="$1" fn mid; local -a keep=()
  for fn in "${SEATED[@]}"; do
    mid="$(mid_of "$fn")"
    if [ "$(status_of "$phase.$mid")" = "violation" ]; then
      echo "  quarantined: $mid [$(c3_label "$fn")] — violation in $phase -> excluded from the dossier"
    else
      keep+=("$fn")
    fi
  done
  SEATED=("${keep[@]}")
}

echo "council: mode=$MODE members=[${MEMBERS[*]}] out=$OUT"

# Stage one disposable repo copy per member (cwd for every round). .git is excluded by the copy.
for fn in "${MEMBERS[@]}"; do
  mid="$(mid_of "$fn")"; dest="$OUT/copy.$mid"; mkdir -p "$dest"
  ( cd "$AGENT_REPO" && find . -mindepth 1 -maxdepth 1 -not -name '.git' -exec cp -a {} "$dest/" \; ) 2>/dev/null
  [ "$(ls -A "$dest" 2>/dev/null)" ] || { echo "council: failed to stage repo copy for $mid" >&2; exit 3; }
done

# ---- R1: blind draft ----
echo "==================== R1: blind draft ===================="
for fn in "${MEMBERS[@]}"; do
  mid="$(mid_of "$fn")"; pf="$OUT/prompt.draft.$mid.txt"; render_draft "$mid" "$pf"
  run_one "$fn" "$mid" "draft" "$pf" &
done
wait

# Seat the members that returned a usable draft; they alone proceed to challenge/revise.
declare -a SEATED=()
for fn in "${MEMBERS[@]}"; do
  mid="$(mid_of "$fn")"
  case "$(status_of "draft.$mid")" in
    ok) SEATED+=("$fn"); echo "  seated: $mid [$(c3_label "$fn")]" ;;
    *)  echo "  not seated: $mid [$(c3_label "$fn")] ($(status_of "draft.$mid"))" ;;
  esac
done

drafts=${#SEATED[@]}
QUORUM="ok"; [ "$drafts" -lt "$COUNCIL_MIN" ] && QUORUM="degraded"
if [ "$drafts" -eq 0 ]; then
  echo "council: no member produced a draft -> nothing to deliberate" >&2
  exit 2
fi

# A challenge round needs at least two drafts to compare; with one it is skipped (degraded).
if [ "${#SEATED[@]}" -ge 1 ] && [ "$drafts" -ge 2 ]; then
  echo "==================== R2: cross-challenge ===================="
  for fn in "${SEATED[@]}"; do
    mid="$(mid_of "$fn")"; pf="$OUT/prompt.challenge.$mid.txt"; render_challenge "$mid" "$pf"
    run_one "$fn" "$mid" "challenge" "$pf" &
  done
  wait
  quarantine challenge    # drop R2 violators before they can taint R3 or the dossier

  echo "==================== R3: self-revise ===================="
  for fn in "${SEATED[@]}"; do
    mid="$(mid_of "$fn")"; pf="$OUT/prompt.revise.$mid.txt"; render_revise "$mid" "$pf"
    run_one "$fn" "$mid" "revised" "$pf" &
  done
  wait
  quarantine revised      # drop R3 violators from the dossier
else
  echo "council: only $drafts draft(s) -> skipping challenge/revise (quorum degraded)"
fi

# ---- assemble the dossier the chair synthesizes from (this script never synthesizes) ----
# Recompute trusted drafts count and QUORUM status for accurate dossier metadata.
drafts=${#SEATED[@]}
QUORUM="ok"; [ "$drafts" -lt "$COUNCIL_MIN" ] && QUORUM="degraded"

if [ "$drafts" -eq 0 ]; then
  echo "council: all members quarantined — no trusted drafts remain" >&2
  exit 2
fi

DOSSIER="$OUT/dossier.md"
{
  echo "---"
  echo "schema: aobus-phase-packet/v1"
  echo "kind: council-dossier"
  echo "mode: $MODE"
  echo "quorum: $QUORUM"
  echo "drafts: $drafts"
  echo "---"
  echo "# Council dossier — $MODE"
  echo
  echo "- mode: \`$MODE\`  |  drafts: $drafts  |  quorum: **$QUORUM**"
  echo "- members: $(for fn in "${MEMBERS[@]}"; do printf '%s; ' "$(c3_label "$fn")"; done)"
  [ "${#INPUTS[@]}" -gt 0 ] && echo "- emphasized inputs: ${INPUTS[*]}"
  echo "- NEXT (chair, R4): independently verify the dossier's key claims, then write the FINAL $ARTIFACT, resolving consensus vs dissent."
  if [ "$QUORUM" = "degraded" ]; then
    echo
    echo "> **quorum: degraded** — fewer than $COUNCIL_MIN member drafts; the cross-challenge did not run in full."
    echo "> The chair should treat this as close to a solo draft and decide whether to proceed or re-convene."
  fi
  echo
  echo "## The question"
  echo '```'
  printf '%s\n' "$QUESTION"
  echo '```'

  echo; echo "## R1 — blind drafts"
  for fn in "${SEATED[@]}"; do
    mid="$(mid_of "$fn")"; echo; echo "### $(c3_label "$fn")"; echo; cat "$OUT/draft.$mid.md"
  done
  if [ "$drafts" -ge 2 ] && [ "${#SEATED[@]}" -ge 1 ]; then
    echo; echo "## R2 — challenges"
    for fn in "${SEATED[@]}"; do
      mid="$(mid_of "$fn")"; [ "$(status_of "challenge.$mid")" = ok ] || continue
      echo; echo "### challenge by $(c3_label "$fn")"; echo; cat "$OUT/challenge.$mid.md"
    done
    echo; echo "## R3 — revised drafts"
    for fn in "${SEATED[@]}"; do
      mid="$(mid_of "$fn")"; [ "$(status_of "revised.$mid")" = ok ] || continue
      echo; echo "### $(c3_label "$fn") (revised)"; echo; cat "$OUT/revised.$mid.md"
    done
  fi

  # Roll up every absence/violation note for an audit trail.
  notes="$(cat "$OUT"/*.note 2>/dev/null | grep -v '^$' || true)"
  echo; echo "## Protocol log (absences & violations)"
  if [ -n "$notes" ]; then echo '```'; printf '%s\n' "$notes"; echo '```'; else echo "(none — all seated members behaved read-only)"; fi
} > "$DOSSIER"

echo "==================== summary ===================="
echo "council: quorum=$QUORUM drafts=$drafts seated=${#SEATED[@]}"
echo "council: dossier -> $DOSSIER"
echo "$DOSSIER"
exit 0
