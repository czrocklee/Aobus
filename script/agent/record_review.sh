#!/usr/bin/env bash
# Record the C3 review verdict for a kept C1/C2 phase. This is C0 bookkeeping only: it never edits the
# repo, never stages, and never changes routing decisions by itself.
set -u

. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

phase_id="${1:-}"
verdict="${2:-}"
reason="${*:3}"

[ -n "$phase_id" ] || { echo "record_review: need phase id" >&2; exit 64; }
case "$phase_id" in
  *[!A-Za-z0-9._:-]*) echo "record_review: unsafe phase id" >&2; exit 64 ;;
esac
case "$verdict" in
  accept | reject | modify) ;;
  *) echo "record_review: verdict must be accept, modify, or reject" >&2; exit 64 ;;
esac

mkdir -p "$AGENT_WORK"
if ! grep -F "\"phase_id\":\"$(agent_json_escape "$phase_id")\"" "$AGENT_WORK/audit.log" 2>/dev/null |
    grep -E -q '"result":"(keep|proposal-validated|proposal-diagnostic|proposal-rejected)"'; then
  echo "record_review: phase id '$phase_id' has no kept audit entry" >&2
  exit 2
fi
if grep -F "\"phase_id\":\"$(agent_json_escape "$phase_id")\"" "$AGENT_WORK/review-outcomes.log" 2>/dev/null |
    grep -Fq '"verdict":"'; then
  if grep -F "\"phase_id\":\"$(agent_json_escape "$phase_id")\"" "$AGENT_WORK/review-outcomes.log" |
      grep -Fq "\"verdict\":\"$verdict\""; then
    echo "record_review: $verdict already recorded for $phase_id"
    exit 0
  fi
  echo "record_review: conflicting verdict already recorded for $phase_id" >&2
  exit 2
fi
printf '{"ts":"%s","phase_id":"%s","verdict":"%s","reason":"%s"}\n' \
  "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$(agent_json_escape "$phase_id")" \
  "$verdict" "$(agent_json_escape "$reason")" >> "$AGENT_WORK/review-outcomes.log"
echo "record_review: recorded $verdict for $phase_id"
