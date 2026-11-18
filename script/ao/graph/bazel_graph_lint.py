"""Graph-native lint replay driver for declared Bazel actions.

This module runs inside the replay actions, never as a Bazel or ao client:
``replay`` selects the real consumer of one uncovered header or include
fragment from the baseline reports' include evidence and re-runs the
existing analyze helper in the consumer's native environment. The gate
entrypoint lives in ``bazel_graph_lint_gate.py`` so gate-only changes never
invalidate cached replay actions. The consumer ranking and platform policy
are the shared ownership policy in ``tidy_policy.py``, so the replay action
and the portal's scoped second pass cannot disagree; replay identities follow
the shared report contracts (``bazel_lint_reports.py``) and
``_identity_mode``/``_component`` in ``bazel/lint/lint.bzl``.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path, PurePath
from typing import Any

# Supports both the portal package and a declared Python script action. The
# lexical path keeps imports inside the action's declared module closure.
if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).absolute().parents[2]))

from ao.graph import bazel_lint_action
from ao.graph.bazel_lint_reports import Identity, canonical_owner, exec_path, load_json
from ao.policy.tidy_policy import consumer_rank

SCHEMA = 1
REPLAY_STATUSES = ("analyzed", "no_consumer")
ITEM_STATUSES = ("covered", "deferred", "uncovered")


def _object(value: object, label: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ValueError(f"{label}: expected an object")
    return value


def _array(value: object, label: str) -> list[Any]:
    if not isinstance(value, list):
        raise ValueError(f"{label}: expected an array")
    return value


def _text(value: object, label: str) -> str:
    if not isinstance(value, str) or not value or "\x00" in value:
        raise ValueError(f"{label}: expected a nonempty string")
    return value


def _paths(value: object, label: str) -> list[str]:
    items = _array(value, label)
    result = [exec_path(item) for item in items]
    if len(set(result)) != len(result):
        raise ValueError(f"{label}: duplicate paths")
    return result


def replay_mode(path: str) -> str:
    """Return the lint mode of one replayed main.

    Keep in sync with _identity_mode in bazel/lint/lint.bzl: test sources stay
    RELAXED except the deliberately invalid lint fixtures.
    """
    return "RELAXED" if path.startswith("test/") and not path.startswith("test/lint/fixture/") else "STRICT"


def load_candidates(path: Path) -> dict[str, Any]:
    """Load and validate one replay action's candidate selection."""
    data = load_json(path)
    if set(data) != {"schema", "item", "platform", "candidates"}:
        raise ValueError("candidates must contain exactly the replay selection fields")
    item = exec_path(data["item"])
    candidates = []
    for candidate in _array(data["candidates"], "candidates"):
        candidate = _object(candidate, "candidate")
        if set(candidate) != {"owner", "configuration", "source", "context", "report", "env"}:
            raise ValueError("candidate must contain exactly the consumer fields")
        environment = _object(candidate["env"], "candidate environment")
        if any(not isinstance(key, str) or not isinstance(value, str) for key, value in environment.items()):
            raise ValueError("candidate environment must map strings to strings")
        configuration = exec_path(candidate["configuration"])
        candidates.append(
            {
                "owner": _text(candidate["owner"], "candidate owner"),
                "configuration": configuration,
                "source": exec_path(candidate["source"]),
                "context": exec_path(candidate["context"]),
                "report": exec_path(candidate["report"]),
                "env": environment,
            }
        )
    return {"schema": SCHEMA, "item": item, "platform": _text(data["platform"], "platform"), "candidates": candidates}


def load_candidate_report(path: Path) -> dict[str, Any]:
    """Load one baseline report and keep only consumer-relevant evidence."""
    report = load_json(path)
    if set(report) != {"schema", "identity", "outcome", "tool_exit", "includes"}:
        raise ValueError("baseline report must contain exactly the report fields")
    identity = Identity.from_json(report["identity"])
    return {"identity": identity, "includes": frozenset(_paths(report["includes"], "report includes"))}


def select_consumer(
    item: str, candidates: list[dict[str, Any]], reports: dict[str, dict[str, Any]]
) -> dict[str, Any] | None:
    """Select the nearest source report that included the item.

    A same-component translation unit comes first, then the longest shared
    directory prefix; owner and source labels break remaining ties. Only the
    baseline main==source reports carry include evidence.
    """
    options = []
    for candidate in candidates:
        report = reports[candidate["report"]]
        identity = report["identity"]
        if identity.main == identity.source and item in report["includes"]:
            options.append(candidate)
    if not options:
        return None
    return min(options, key=lambda option: consumer_rank(item, option["owner"], option["source"]))


def _write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def replay_wrapper(item: str, consumer: dict[str, Any], report: PurePath, log: PurePath) -> dict[str, Any]:
    """Build an analyzed replay record with portable artifact spellings.

    Bazel declares outputs with forward-slash exec paths, but a Windows
    ``Path`` stringifies with backslashes; the gate compares against the
    inventory's strict portable spellings, so the producer normalizes here
    instead of the gate loosening its validation.
    """
    return {
        "schema": SCHEMA,
        "item": item,
        "status": "analyzed",
        "consumer": consumer,
        "report": report.as_posix(),
        "log": log.as_posix(),
    }


def replay(args: argparse.Namespace) -> int:
    """Select the real consumer at execution, then analyze under its environment."""
    selection = load_candidates(args.candidates)
    item = selection["item"]
    reports = {
        candidate["report"]: load_candidate_report(Path(candidate["report"])) for candidate in selection["candidates"]
    }
    consumer = select_consumer(item, selection["candidates"], reports)
    if consumer is None:
        # Coverage of this item is the gate's decision: a platform-owned path
        # defers there, a compatible path without evidence fails there.
        args.log.write_text("", encoding="utf-8")
        _write_json(args.report, {"schema": SCHEMA, "item": item, "outcome": "no_consumer"})
        _write_json(
            args.replay,
            {
                "schema": SCHEMA,
                "item": item,
                "status": "no_consumer",
                "consumer": None,
                "report": None,
                "log": None,
            },
        )
        return 0
    with tempfile.TemporaryDirectory(prefix=".replay-", dir=args.report.parent) as temporary:
        work = Path(temporary)
        chosen = work / "consumer.json"
        _write_json(
            chosen,
            {
                "schema": SCHEMA,
                "item": item,
                "owner": consumer["owner"],
                "source": consumer["source"],
                "context": consumer["context"],
            },
        )
        command = [
            sys.executable,
            "-I",
            "-S",
            "-B",
            "-X",
            "utf8",
            os.fspath(Path(__file__).absolute()),
            "replay-analyze",
            "--consumer",
            os.fspath(chosen),
            "--policy",
            os.fspath(args.policy),
            "--sdk",
            os.fspath(args.sdk),
            "--verification",
            os.fspath(args.verification),
            "--tidy",
            os.fspath(args.tidy),
            "--helper",
            os.fspath(args.helper),
            "--report",
            os.fspath(args.report),
            "--log",
            os.fspath(args.log),
        ]
        if args.plugin is not None:
            command[command.index("--tidy") + 2 : command.index("--tidy") + 2] = ["--plugin", os.fspath(args.plugin)]
        result = subprocess.run(
            command,
            env=consumer["env"],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
        )
        if result.returncode != 0:
            sys.stderr.write(result.stdout)
            print(f"lint graph replay analysis failed with exit {result.returncode}", file=sys.stderr)
            return 2
    report = load_json(args.report)
    identity = Identity.from_json(report["identity"])
    expected = {
        "owner": canonical_owner(consumer["owner"]),
        "source": consumer["source"],
        "main": item,
        "mode": replay_mode(item),
        "platform": selection["platform"],
    }
    for key, value in expected.items():
        if getattr(identity, key) != value:
            raise ValueError(f"replayed report {key} differs from the selected consumer: {getattr(identity, key)}")
    _write_json(
        args.replay,
        replay_wrapper(
            item,
            {"owner": consumer["owner"], "source": consumer["source"]},
            args.report,
            args.log,
        ),
    )
    return 0


def replay_analyze(args: argparse.Namespace) -> int:
    """Analyze one replayed main inside its consumer's native environment."""
    chosen = load_json(args.consumer)
    if set(chosen) != {"schema", "item", "owner", "source", "context"}:
        raise ValueError("consumer selection must contain exactly the replay fields")
    item = exec_path(chosen["item"])
    source = exec_path(chosen["source"])
    context = load_json(Path(chosen["context"]))
    # The analyze helper's context is flat: the identity fields sit beside
    # schema/argv/virtual_headers, exactly as the aspect writes them.
    if "argv" not in context or "virtual_headers" not in context:
        raise ValueError("consumer context must carry argv and virtual_headers")
    identity = Identity.from_json(
        {key: value for key, value in context.items() if key not in ("schema", "argv", "virtual_headers")}
    )
    if identity.owner != canonical_owner(chosen["owner"]) or identity.source != source:
        raise ValueError("consumer context differs from the selected consumer")
    sdk = bazel_lint_action.load_verified_tools(args)[1]
    replayed = {
        "owner": identity.owner,
        "configuration": identity.configuration,
        "source": source,
        "main": item,
        "outputs": list(identity.outputs),
        "mode": replay_mode(item),
        "platform": sdk["platform"],
    }
    if identity.platform != sdk["platform"]:
        raise ValueError("consumer context platform differs from the verified SDK")
    with tempfile.TemporaryDirectory(prefix=".context-", dir=args.report.parent) as temporary:
        replayed_context = Path(temporary) / "context.json"
        _write_json(
            replayed_context,
            {
                **replayed,
                "schema": SCHEMA,
                "argv": context["argv"],
                "virtual_headers": context["virtual_headers"],
            },
        )
        command = [
            "analyze",
            "--context",
            os.fspath(replayed_context),
            "--policy",
            os.fspath(args.policy),
            "--sdk",
            os.fspath(args.sdk),
            "--verification",
            os.fspath(args.verification),
            "--tidy",
            os.fspath(args.tidy),
            "--report",
            os.fspath(args.report),
            "--log",
            os.fspath(args.log),
        ]
        if args.plugin is not None:
            command[command.index("--tidy") + 2 : command.index("--tidy") + 2] = ["--plugin", os.fspath(args.plugin)]
        return bazel_lint_action.main(command)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    replay_command = commands.add_parser("replay")
    analyze_command = commands.add_parser("replay-analyze")
    for command in (replay_command, analyze_command):
        for name in ("policy", "sdk", "verification", "tidy"):
            command.add_argument(f"--{name}", type=Path, required=True)
        command.add_argument("--plugin", type=Path)
    for name in ("report", "log"):
        replay_command.add_argument(f"--{name}", type=Path, required=True)
        analyze_command.add_argument(f"--{name}", type=Path, required=True)
    replay_command.add_argument("--candidates", type=Path, required=True)
    replay_command.add_argument("--helper", type=Path, required=True)
    replay_command.add_argument("--replay", type=Path, required=True)
    analyze_command.add_argument("--consumer", type=Path, required=True)
    analyze_command.add_argument("--helper", type=Path, required=True)
    replay_command.set_defaults(run=replay)
    analyze_command.set_defaults(run=replay_analyze)
    args = parser.parse_args(argv)
    try:
        return int(args.run(args))
    except (OSError, ValueError, RuntimeError, KeyError, TypeError) as error:
        print(f"Bazel lint graph {args.command} failed: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
