"""Render CI evidence (completion receipts, Bazel events, R2 cache accounting) as Markdown.

Read-only observability for evidence the workflow already produced. A missing or
truncated input is displayed as unavailable, incomplete or unknown, never as a
passed result or a zero count, so an evidence gap can never mask the product
result. The renderer gates nothing and trusts the pinned tools' output.
"""

from __future__ import annotations

import argparse
import json
import sys
from collections.abc import Sequence
from pathlib import Path
from typing import Any

UNKNOWN = "unknown"
# Bounded evidence globs only; output bases and state directories are never scanned.
_BEP_GLOBS = ("*.bep.json", "checks/*/check-*/*.bep.json", "lint/run-*/*.bep.json", "coverage/run-*/*.bep.json")
_RECEIPT_GLOB = "checks/*/check-*/receipt.json"


def _cell(value: Any) -> str:
    """Keep cell text single-line without Markdown table separators."""
    return " ".join(str(value).split()).replace("|", "\\|")


def _count(value: Any) -> int | None:
    """Accept protobuf JSON's number-or-string counts; None when absent or malformed."""
    if isinstance(value, bool):
        return None
    if isinstance(value, int):
        return value
    if isinstance(value, float) and value.is_integer():
        return int(value)
    if isinstance(value, str) and value.strip().isdigit():
        return int(value)
    return None


def _known(value: Any) -> str:
    number = _count(value)
    return str(number) if number is not None else UNKNOWN


def _load(path: Path | None) -> dict[str, Any] | None:
    if path is None:
        return None
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except OSError, ValueError:
        return None
    return value if isinstance(value, dict) else None


def _bep_events(path: Path) -> tuple[dict[str, Any], str]:
    """Collect finished/buildMetrics from a JSON-lines stream without trusting its tail."""
    if not path.is_file():
        return {}, "missing"
    events: dict[str, Any] = {}
    try:
        with path.open("r", encoding="utf-8") as stream:
            for line in stream:
                if not line.strip():
                    continue
                try:
                    event = json.loads(line)
                except ValueError:
                    return events, "truncated"
                if isinstance(event, dict):
                    for key in ("finished", "buildMetrics"):
                        if key in event and key not in events:
                            events[key] = event[key]
    except OSError, UnicodeDecodeError:
        return events, "unreadable"
    return events, "ok" if "finished" in events else "incomplete"


def _outcome(finished: Any, state: str) -> str:
    if not isinstance(finished, dict):
        return "incomplete (no finished event)"
    if finished.get("overallSuccess") is True:
        label = "passed"
    elif finished.get("overallSuccess") is False:
        label = "failed"
    else:
        label = UNKNOWN
    detail = ""
    code = finished.get("exitCode")
    if isinstance(code, dict) and "name" in code:
        detail = f" ({_cell(code['name'])})"
    elif isinstance(code, dict) and _count(code.get("code")) is not None:
        detail = f" (code {code['code']})"
    if state == "truncated":
        # A truncated stream is incomplete evidence even when a finished event survived.
        recorded = detail.removeprefix(" (").removesuffix(")") or label
        return f"incomplete ({recorded}; stream truncated)"
    return label + detail


def _wall_seconds(metrics: dict[str, Any]) -> str:
    timing = metrics.get("timingMetrics")
    if not isinstance(timing, dict):
        return UNKNOWN
    milliseconds = _count(timing.get("wallTimeInMs"))
    return f"{milliseconds / 1000:.1f}" if milliseconds is not None else UNKNOWN


def _action_cache(metrics: dict[str, Any]) -> str:
    """Local incremental action-cache hits/misses; a missing statistics object is unknown."""
    summary = metrics.get("actionSummary")
    statistics = summary.get("actionCacheStatistics") if isinstance(summary, dict) else None
    if not isinstance(statistics, dict):
        return UNKNOWN
    # Protobuf omits zero scalars inside a present statistics object; a malformed value stays unknown.
    return f"{_known(statistics.get('hits', 0))} hit(s), {_known(statistics.get('misses', 0))} miss(es)"


def _runners(metrics: dict[str, Any]) -> str:
    """Exactly the reported spawn runner counts; absent runner names are not invented."""
    summary = metrics.get("actionSummary")
    counts = summary.get("runnerCount") if isinstance(summary, dict) else None
    if not isinstance(counts, list):
        return "not reported"
    rendered = []
    for entry in counts:
        count = _count(entry.get("count")) if isinstance(entry, dict) else None
        if count is None or not isinstance(entry, dict) or "name" not in entry:
            continue
        text = f"{_cell(entry['name'])}: {count}"
        if "execKind" in entry:
            text += f" ({_cell(entry['execKind'])})"
        rendered.append(text)
    return "; ".join(rendered) if rendered else "none reported"


def _receipt_lines(tree: Path, path: Path, receipt: dict[str, Any]) -> list[str]:
    lines = [f"#### `{_cell(path.relative_to(tree).as_posix())}`"]
    details = [f"status **{_cell(receipt.get('status', UNKNOWN))}**"]
    if isinstance(receipt.get("exit"), int):
        details.append(f"exit {receipt['exit']}")
    if "test_cache_policy" in receipt:
        details.append(f"test cache policy {_cell(receipt['test_cache_policy'])}")
    lines.append("- " + "; ".join(details))
    summary = receipt.get("test_summary")
    if isinstance(summary, dict):
        lines.append(
            f"- Admitted tests: {_known(summary.get('passed'))} passed target(s), "
            f"{_known(summary.get('executed_runs'))} executed run(s), "
            f"{_known(summary.get('cached_runs'))} cached run(s)"
        )
    else:
        lines.append(f"- Admitted tests: {UNKNOWN} (no test summary in the receipt)")
    phases = receipt.get("phases")
    if isinstance(phases, list) and phases:
        lines += ["", "| Phase | Status | Elapsed (s) |", "| --- | --- | --- |"]
        for phase in phases:
            if not isinstance(phase, dict):
                continue
            elapsed = phase.get("elapsed_seconds")
            seconds = f"{elapsed:.1f}" if isinstance(elapsed, (int, float)) else UNKNOWN
            lines.append(f"| {_cell(phase.get('name', UNKNOWN))} | {_cell(phase.get('status', UNKNOWN))} | {seconds} |")
    return lines


def _completion(tree: Path) -> list[str]:
    lines = [f"### Bazel completion — `{_cell(tree)}`", ""]
    receipts = sorted(tree.glob(_RECEIPT_GLOB))
    if not receipts:
        lines.append(f"- No completion receipt under `{_cell(tree)}`; completion status is {UNKNOWN}.")
        return [*lines, ""]
    for path in receipts:
        receipt = _load(path)
        if receipt is None or receipt.get("schema") != 1:
            lines.append(f"- `{_cell(path.relative_to(tree).as_posix())}`: unavailable (no readable schema-1 receipt)")
            continue
        lines += _receipt_lines(tree, path, receipt)
    return [*lines, ""]


def _invocations(tree: Path) -> list[str]:
    paths = sorted({path for pattern in _BEP_GLOBS for path in tree.glob(pattern)})
    lines = [f"### Bazel invocations — `{_cell(tree)}`", ""]
    if not paths:
        lines.append(f"- No Bazel event files under `{_cell(tree)}`; invocation outcomes are {UNKNOWN}.")
        return [*lines, ""]
    lines += [
        "| Invocation | Outcome | Wall time (s) | Local incremental action cache | Spawn runners |",
        "| --- | --- | --- | --- | --- |",
    ]
    for path in paths:
        relative = _cell(path.relative_to(tree).as_posix())
        events, state = _bep_events(path)
        if state in ("missing", "unreadable"):
            lines.append(f"| {relative} | unavailable | {UNKNOWN} | {UNKNOWN} | not reported |")
            continue
        metrics = events.get("buildMetrics")
        if not isinstance(metrics, dict):
            metrics = {}
        lines.append(
            f"| {relative} | {_outcome(events.get('finished'), state)} | {_wall_seconds(metrics)}"
            f" | {_action_cache(metrics)} | {_runners(metrics)} |"
        )
    return [*lines, ""]


def _remote(args: argparse.Namespace) -> list[str]:
    lines = ["### Remote cache (R2)", ""]
    bound = {None: UNKNOWN, "true": "yes", "false": "no"}.get(args.remote_bound)
    lines.append(f"- Start step: {_cell(args.remote_start)}; cache URL published to later steps: {bound}")
    if args.remote_start == "skipped":
        lines.append("- The start step was skipped, so no remote cache was selected for this run.")
    lines.append(f"- Stop step: {_cell(args.remote_stop_outcome)}")
    report = _load(args.remote_report)
    if report is None or report.get("schemaVersion") != 1:
        note = "not provided" if args.remote_report is None else "unavailable"
        lines.append(f"- Stop report: {note} — accounting unavailable")
        lines.append(f"- Drain: accounting unavailable (stop evidence {UNKNOWN})")
        return [*lines, ""]
    started = report.get("started")
    upload = report.get("upload")
    started_text = "true" if started is True else "false" if started is False else UNKNOWN
    upload_text = "enabled" if upload is True else "disabled" if upload is False else UNKNOWN
    lines.append(f"- Stop report: started={started_text}, uploads {upload_text}")
    if started is True:
        activity = report.get("activity")
        if isinstance(activity, dict):
            lines.append(
                f"  - uploads: {_known(activity.get('queued'))} queued, {_known(activity.get('uploaded'))} uploaded, "
                f"{_known(activity.get('pending'))} pending, {_known(activity.get('uploadFailures'))} failed"
            )
            lines.append(
                f"  - downloads: {_known(activity.get('downloaded'))} served, "
                f"{_known(activity.get('downloadMisses'))} miss(es)"
            )
        lines.append(f"  - lost uploads: {_known(report.get('lostUploads'))}")
        activity = report.get("activity")
        failures = _count(activity.get("uploadFailures")) if isinstance(activity, dict) else None
        lost = _count(report.get("lostUploads"))
        if upload is False:
            # stop() drains nothing when the proxy ran without uploaders.
            lines.append("- Drain: not applicable (uploads disabled)")
        elif upload is not True:
            lines.append(f"- Drain: {UNKNOWN} (upload state not reported)")
        elif args.remote_stop_outcome == "success" and lost == 0 and failures == 0:
            lines.append("- Drain: accounted complete")
        else:
            lines.append("- Drain: not accounted complete")
    elif started is False:
        lines.append("- Drain: not applicable (no cache was started)")
    else:
        lines.append(f"- Drain: {UNKNOWN} (started state not reported)")
    return [*lines, ""]


def _notes() -> list[str]:
    return [
        "Notes:",
        "- Counter families are independent and are never summed: `actionCacheStatistics` is Bazel's persistent "
        "local incremental action cache — hits are actions skipped because a stored local result was reused, and "
        "misses may still be served by the disk or remote cache. `runnerCount` entries are separate reported "
        "spawn counts: `total` counts completed actions (local action-cache hits are not spawns), and other names "
        "such as `disk cache hit` and `remote cache hit` count spawns by runner or strategy. `total` is not a "
        "compiler denominator, and no compiler or backend hit rate is implied.",
        "- Completion `native-context` wraps the inner phases, so phase elapsed times overlap and are not summed. "
        "Admitted test counts cover the completion receipt's scope only; documentation, tooling and hygiene run as "
        "separate checks.",
        "- Remote-cache download counts are whole-job backend blob operations observed by the proxy, not Bazel "
        "action-cache or test-result hits; a ready proxy does not by itself prove backend access, and a published "
        "URL says only that the start step advertised it.",
        "",
    ]


def render(args: argparse.Namespace) -> str:
    lines = ["## CI evidence summary", ""]
    for tree in dict.fromkeys(args.tree):
        lines += _completion(tree)
        lines += _invocations(tree)
    lines += _remote(args)
    lines += _notes()
    return "\n".join(lines)


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Render CI evidence as a Markdown summary")
    parser.add_argument("--tree", type=Path, action="append", default=[], help="CI Bazel output root; repeatable")
    parser.add_argument("--remote-start", default=UNKNOWN, help="remote-cache start step outcome")
    parser.add_argument(
        "--remote-bound", choices=("true", "false"), help="whether the cache URL was published to later steps"
    )
    parser.add_argument("--remote-stop-outcome", default=UNKNOWN, help="remote-cache stop step outcome")
    parser.add_argument("--remote-report", type=Path, help="R2 stop report JSON")
    parser.add_argument("--github-summary", type=Path, help="file to append the rendered Markdown to")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    markdown = render(args)
    sys.stdout.write(markdown + "\n")
    if args.github_summary is not None:
        try:
            with args.github_summary.open("a", encoding="utf-8") as sink:
                sink.write(markdown + "\n")
        except OSError:
            print("Error: the CI evidence summary could not be appended.", file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
