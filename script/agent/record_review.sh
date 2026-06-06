#!/usr/bin/env bash
# Record the C3 review verdict for a kept C1/C2 phase. This is C0 bookkeeping only: it never edits the
# repo, never stages, and never changes routing decisions by itself.
set -u

. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

phase_id="${1:-}"
verdict="${2:-}"
reason="${*:3}"

[ -n "$phase_id" ] || { echo "record_review: need phase id" >&2; exit 64; }
agent_id_ok "$phase_id" || { echo "record_review: unsafe or reserved phase id" >&2; exit 64; }
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

# Circuit breaker: a "silent-wrong" is a phase that PASSED validation (audit result keep /
# proposal-validated) yet C3 rejected on semantics. By the §Step A rule the first such case pauses that
# worker's route. A `modify` is a soft signal (mostly right, needed tweaks) and does NOT trip.
if [ "$verdict" = "reject" ]; then
  result="$(agent_audit_field_for "$phase_id" result)"
  case "$result" in
    keep | proposal-validated)
      worker="$(agent_audit_field_for "$phase_id" worker)"
      worker="${worker:-unknown}"
      if agent_breaker_trip "$worker" "$phase_id" "silent-wrong: $result then C3 reject"; then
        echo "record_review: SILENT-WRONG -> breaker TRIPPED for worker '$worker' (validated then rejected)." >&2
        echo "record_review: that route is now paused; run a postmortem, then 'review_stats.sh --reset \"$worker\"'." >&2
      else
        echo "record_review: SILENT-WRONG for worker '$worker'; breaker was already tripped." >&2
      fi
      ;;
  esac
fi
