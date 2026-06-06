#!/usr/bin/env bash
# script/agent/review_stats.sh — rolling C3-review statistics + circuit-breaker control.
#
# Read-only C0 bookkeeping: it NEVER edits the repo, stages, or runs a model. It joins the two logs the
# harness already writes — $AGENT_WORK/audit.log (per-phase result + worker, written by
# agent_audit_entry) and $AGENT_WORK/review-outcomes.log (per-phase C3 verdict, written by
# record_review.sh) — by phase_id, and reports per worker x capability how often a kept/validated phase
# was later accepted / modified / rejected by C3. A "silent-wrong" is a phase that PASSED validation
# (result keep | proposal-validated) yet C3 rejected: the failure mode the whole tiering model is built
# to keep at zero (§10.3 — without these stats "C2 quality is systematically overestimated").
#
# Usage:
#   review_stats.sh                 # print the lifetime report + tripped breakers
#   review_stats.sh --window N      # also show a rolling silent-wrong rate over each worker's last N
#                                   #   validated+reviewed evals (trend vs the lifetime average)
#   review_stats.sh --reset <label> # clear one worker's tripped breaker (after a postmortem)
#   review_stats.sh --reset-all     # clear every tripped breaker
# Exit: 0 ok ; 2 nothing to reset ; 64 usage.
set -u

. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

AUDIT="$AGENT_WORK/audit.log"
OUTCOMES="$AGENT_WORK/review-outcomes.log"

# Extract a JSON string field's value from a single log line (best-effort; labels carry no quotes).
_field() { printf '%s' "$1" | grep -oE "\"$2\":\"[^\"]*\"" | tail -n 1 | sed -E "s/^\"$2\":\"(.*)\"$/\1/"; }

# --- option parsing: breaker-control modes exit here; report modes set WINDOW and fall through ------
WINDOW=0   # 0 = lifetime only; >0 also prints a rolling "last N validated+reviewed evals" column
case "${1:-}" in
  --reset)
    label="${2:-}"
    [ -n "$label" ] || { echo "review_stats: --reset needs a worker label" >&2; exit 64; }
    f="$(agent_breaker_dir)/$(agent_breaker_slug "$label").tripped"
    if [ -f "$f" ]; then rm -f "$f"; echo "review_stats: breaker reset for '$label'"; exit 0; fi
    echo "review_stats: no tripped breaker for '$label'" >&2; exit 2 ;;
  --reset-all)
    dir="$(agent_breaker_dir)"; any=0
    for f in "$dir"/*.tripped; do [ -e "$f" ] || continue; rm -f "$f"; any=1; done
    if [ "$any" -eq 1 ]; then echo "review_stats: all breakers reset"; exit 0; fi
    echo "review_stats: no tripped breakers" >&2; exit 2 ;;
  --window)
    WINDOW="${2:-}"
    case "$WINDOW" in '' | *[!0-9]*) echo "review_stats: --window needs a positive integer" >&2; exit 64 ;; esac
    [ "$WINDOW" -gt 0 ] || { echo "review_stats: --window needs a positive integer" >&2; exit 64; } ;;
  "") ;;
  *) echo "review_stats: unknown option '$1'" >&2; exit 64 ;;
esac

# --- join: last verdict per phase, last audit row per phase ----------------------------------------
declare -A verdict_by_phase result_by_phase worker_by_phase cap_by_phase
declare -a phase_seq=()   # phase_ids in audit (chronological) order; powers the rolling window
if [ -r "$OUTCOMES" ]; then
  while IFS= read -r line; do
    [ -n "$line" ] || continue
    pid="$(_field "$line" phase_id)"; [ -n "$pid" ] || continue
    verdict_by_phase["$pid"]="$(_field "$line" verdict)"
  done < "$OUTCOMES"
fi
if [ -r "$AUDIT" ]; then
  while IFS= read -r line; do
    [ -n "$line" ] || continue
    pid="$(_field "$line" phase_id)"; [ -n "$pid" ] || continue
    result_by_phase["$pid"]="$(_field "$line" result)"   # later rows win (last-wins, like agent_audit_field_for)
    worker_by_phase["$pid"]="$(_field "$line" worker)"
    cap_by_phase["$pid"]="$(_field "$line" capability)"
    phase_seq+=("$pid")
  done < "$AUDIT"
fi

# --- tally per worker x capability (keys may contain spaces, so iterate an assoc array, not a list) -
declare -A n_validated n_accept n_modify n_reject n_silent seen_key
# Guard: under `set -u`, expanding an EMPTY associative array (`${!arr[@]}`/`${#arr[@]}`) errors on
# bash 5.3. `${arr[*]+x}` is a set-u-safe "is it non-empty" probe; only iterate keys when it is.
if [ -n "${result_by_phase[*]+x}" ]; then
for pid in "${!result_by_phase[@]}"; do
  res="${result_by_phase[$pid]}"
  worker="${worker_by_phase[$pid]:-unknown}"
  cap="${cap_by_phase[$pid]:-?}"
  key="$worker|$cap"
  seen_key["$key"]=1
  case "$res" in keep | proposal-validated) n_validated["$key"]=$(( ${n_validated["$key"]:-0} + 1 )) ;; esac
  case "${verdict_by_phase[$pid]:-}" in
    accept) n_accept["$key"]=$(( ${n_accept["$key"]:-0} + 1 )) ;;
    modify) n_modify["$key"]=$(( ${n_modify["$key"]:-0} + 1 )) ;;
    reject)
      n_reject["$key"]=$(( ${n_reject["$key"]:-0} + 1 ))
      case "$res" in keep | proposal-validated) n_silent["$key"]=$(( ${n_silent["$key"]:-0} + 1 )) ;; esac ;;
  esac
done
fi

# --- rolling window: per worker x capability, the last N validated+reviewed evals ------------------
# The lifetime silent-wrong rate above never recovers — one early miss stays in the denominator
# forever. A rolling rate over the most recent N evals lets the operator see whether a worker route is
# trending better or worse (the Step G "rolling statistics across many evals" follow-up). The window
# unit is one validated-and-reviewed proposal (the only thing that can be a silent-wrong), taken in
# audit (production) order; if a phase_id recurs (a re-run) only its last occurrence is counted.
declare -A roll_flags   # key -> space-separated 0/1 in chronological order (1 = silent-wrong)
if [ "$WINDOW" -gt 0 ] && [ -n "${result_by_phase[*]+x}" ]; then
  declare -A _lastidx
  n=${#phase_seq[@]}
  for (( i = 0; i < n; i++ )); do _lastidx["${phase_seq[$i]}"]=$i; done
  for (( i = 0; i < n; i++ )); do
    p="${phase_seq[$i]}"
    [ "${_lastidx[$p]}" -eq "$i" ] || continue                              # keep only the last occurrence
    case "${result_by_phase[$p]}" in keep | proposal-validated) ;; *) continue ;; esac
    case "${verdict_by_phase[$p]:-}" in accept | modify | reject) ;; *) continue ;; esac
    flag=0; [ "${verdict_by_phase[$p]}" = reject ] && flag=1
    key="${worker_by_phase[$p]:-unknown}|${cap_by_phase[$p]:-?}"
    roll_flags["$key"]="${roll_flags["$key"]:-} $flag"
  done
fi

# --- report ----------------------------------------------------------------------------------------
echo "=== Aobus review stats ($AGENT_WORK) ==="
if [ -z "${seen_key[*]+x}" ]; then
  echo "(no audited phases yet)"
else
  if [ "$WINDOW" -gt 0 ]; then
    printf '%-34s %-4s %4s %4s %4s %4s %4s  %-17s  %s\n' \
      "WORKER" "CAP" "VALD" "ACPT" "MOD" "REJ" "SLNT" "SILENT-WRONG-RATE" "ROLL(last $WINDOW)"
  else
    printf '%-34s %-4s %4s %4s %4s %4s %4s  %s\n' "WORKER" "CAP" "VALD" "ACPT" "MOD" "REJ" "SLNT" "SILENT-WRONG-RATE"
  fi
  for key in "${!seen_key[@]}"; do
    worker="${key%|*}"; cap="${key##*|}"
    v="${n_validated[$key]:-0}"; a="${n_accept[$key]:-0}"; m="${n_modify[$key]:-0}"
    r="${n_reject[$key]:-0}"; s="${n_silent[$key]:-0}"
    if [ "$v" -gt 0 ]; then rate="$(awk -v s="$s" -v v="$v" 'BEGIN{printf "%.1f%%", 100*s/v}')"; else rate="n/a"; fi
    if [ "$WINDOW" -gt 0 ]; then
      _f=(); read -r -a _f <<< "${roll_flags[$key]:-}"
      rc=${#_f[@]}
      if [ "$rc" -eq 0 ]; then
        roll="n/a"
      else
        st=0; [ "$rc" -gt "$WINDOW" ] && st=$(( rc - WINDOW ))
        rs=0; rv=0
        for (( j = st; j < rc; j++ )); do rv=$(( rv + 1 )); rs=$(( rs + _f[j] )); done
        roll="$(awk -v s="$rs" -v v="$rv" 'BEGIN{printf "%d/%d=%.1f%%", s, v, 100*s/v}')"
      fi
      printf '%-34.34s %-4.4s %4s %4s %4s %4s %4s  %-17s  %s\n' "$worker" "$cap" "$v" "$a" "$m" "$r" "$s" "$rate" "$roll"
    else
      printf '%-34.34s %-4.4s %4s %4s %4s %4s %4s  %s\n' "$worker" "$cap" "$v" "$a" "$m" "$r" "$s" "$rate"
    fi
  done
fi

echo
echo "=== tripped breakers ==="
bdir="$(agent_breaker_dir)"
found=0
for f in "$bdir"/*.tripped; do
  [ -e "$f" ] || continue
  found=1
  printf '  %s  %s\n' "$(basename "$f" .tripped)" "$(cat "$f")"
done
if [ "$found" -eq 1 ]; then
  echo "  (clear with: review_stats.sh --reset <worker-label>)"
else
  echo "  (none)"
fi
