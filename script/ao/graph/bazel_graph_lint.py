"""Graph-native lint replay driver for declared Bazel actions.

This module runs inside the graph actions, never as a Bazel or ao client:
``select`` routes an uncovered header or include fragment to a consumer using
baseline include evidence. ``replay`` re-runs the existing analyze helper
with only that consumer's native inputs and environment. The gate
entrypoint lives in ``bazel_graph_lint_gate.py`` so gate-only changes never
invalidate cached replay actions. The selector and the scoped portal share
consumer ranking in ``lint_ownership.py``. Replay identities follow
the shared report contracts (``bazel_lint_schema.py``) and
``_identity_mode``/``_component`` in ``bazel/lint/lint.bzl``.
"""

from __future__ import annotations

import argparse
import json
import os
import re
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
from ao.graph.bazel_lint_schema import Identity, canonical_owner, exec_path, load_json

SCHEMA = 1
REPLAY_STATUSES = ("analyzed", "no_consumer")
ITEM_STATUSES = ("covered", "deferred", "uncovered")
_CONSUMER_FIELDS = {"owner", "configuration", "source", "context", "env"}
_ANALYSIS_TOOLS = ("policy", "sdk", "verification", "tidy")


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


def _consumer(data: dict[str, Any]) -> dict[str, Any]:
    environment = _object(data["env"], "consumer environment")
    if any(not isinstance(key, str) or not isinstance(value, str) for key, value in environment.items()):
        raise ValueError("consumer environment must map strings to strings")
    owner = _text(data["owner"], "consumer owner")
    canonical_owner(owner)
    return {
        "owner": owner,
        "configuration": exec_path(data["configuration"]),
        "source": exec_path(data["source"]),
        "context": exec_path(data["context"]),
        "env": environment,
    }


def _route_id(value: object) -> str:
    # The analysis rule assigns these IDs so both the manifest and the
    # template map name the same native context without duplicating a hash.
    if not isinstance(value, str) or re.fullmatch(r"[0-9a-f]{8}", value) is None:
        raise ValueError("consumer route must be an eight-digit lowercase hexadecimal ID")
    return value


def load_candidates(path: Path) -> dict[str, Any]:
    """Load and validate one item's candidate manifest."""
    data = load_json(path)
    if set(data) != {"schema", "item", "platform", "candidates"}:
        raise ValueError("candidates must contain exactly the replay selection fields")
    candidates = []
    routes = set()
    for candidate in _array(data["candidates"], "candidates"):
        candidate = _object(candidate, "candidate")
        if set(candidate) != _CONSUMER_FIELDS | {"report", "route"}:
            raise ValueError("candidate must contain exactly the consumer fields, report and route")
        route = _route_id(candidate["route"])
        if route in routes:
            raise ValueError(f"duplicate consumer route: {route}")
        routes.add(route)
        candidates.append({**_consumer(candidate), "report": exec_path(candidate["report"]), "route": route})
    return {
        "schema": SCHEMA,
        "item": exec_path(data["item"]),
        "platform": _text(data["platform"], "platform"),
        "candidates": candidates,
    }


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
    # Only the selector declares the routing policy. Replay consumes its marker.
    from ao.policy.lint_ownership import consumer_rank

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


def select(args: argparse.Namespace) -> int:
    """Write the sole marker for the selected native context (or none)."""
    manifest = load_candidates(args.candidates)
    candidates = manifest["candidates"]
    item = manifest["item"]
    reports = {candidate["report"]: load_candidate_report(Path(candidate["report"])) for candidate in candidates}
    consumer = select_consumer(item, candidates, reports)
    route = "none" if consumer is None else consumer["route"]
    _write_json(
        args.routes / route,
        {
            "schema": SCHEMA,
            "item": item,
            "platform": manifest["platform"],
            "route": route,
            "consumer": None if consumer is None else _consumer(consumer),
        },
    )
    return 0


def _load_selection(path: Path, context: Path | None) -> tuple[str, str, dict[str, Any] | None]:
    marker = load_json(path)
    if set(marker) != {"schema", "item", "platform", "consumer", "route"}:
        raise ValueError("selection marker must contain exactly the replay fields")
    if marker["route"] != path.name:
        raise ValueError("selection marker name differs from its route")
    item = exec_path(marker["item"])
    platform = _text(marker["platform"], "platform")
    if path.name == "none":
        if marker["consumer"] is not None or context is not None:
            raise ValueError("no-consumer route must not carry a consumer or context")
        return item, platform, None
    _route_id(path.name)
    consumer = _object(marker["consumer"], "selected consumer")
    if set(consumer) != _CONSUMER_FIELDS:
        raise ValueError("selection marker must contain exactly the consumer fields")
    consumer = _consumer(consumer)
    if context is None or context.as_posix() != consumer["context"]:
        raise ValueError("routed context path differs from the selected consumer")
    return item, platform, consumer


def _analysis_options(args: argparse.Namespace) -> list[str]:
    missing = [f"--{name}" for name in _ANALYSIS_TOOLS if getattr(args, name) is None]
    if missing:
        raise ValueError("selected replay requires " + ", ".join(missing))
    options: list[str] = []
    for name in (*_ANALYSIS_TOOLS, "plugin", "report", "log"):
        value = getattr(args, name)
        if value is not None:
            options.extend((f"--{name}", os.fspath(value)))
    return options


def _validate_identity(identity: Identity, expected: dict[str, str], artifact: str) -> None:
    for key, value in expected.items():
        if getattr(identity, key) != value:
            raise ValueError(f"{artifact} {key} differs from the selected consumer: {getattr(identity, key)}")


def replay(args: argparse.Namespace) -> int:
    """Replay the selected route inside its native producer environment."""
    item, platform, consumer = _load_selection(args.selection, args.context)
    if consumer is None:
        # Coverage of this item is the gate's decision: a platform-owned path
        # defers there, a compatible path without evidence fails there.
        args.log.parent.mkdir(parents=True, exist_ok=True)
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
    if args.driver is None:
        raise ValueError("selected replay requires the declared driver source")
    # Expanded actions share a template environment. Spawn the helper under
    # the exact producer environment, including Python's startup settings.
    command = [
        sys.executable,
        "-I",
        "-S",
        "-B",
        "-X",
        "utf8",
        os.fspath(args.driver.absolute()),
        "replay-analyze",
        "--consumer",
        os.fspath(args.selection),
        "--context",
        os.fspath(args.context),
        *_analysis_options(args),
    ]
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
    _validate_identity(
        Identity.from_json(load_json(args.report)["identity"]),
        {
            "owner": canonical_owner(consumer["owner"]),
            "configuration": consumer["configuration"],
            "source": consumer["source"],
            "main": item,
            "mode": replay_mode(item),
            "platform": platform,
        },
        "replayed report",
    )
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
    item, platform, consumer = _load_selection(args.consumer, args.context)
    if consumer is None:
        raise ValueError("analysis requires a selected consumer")
    context = load_json(args.context)
    # The analyze helper's context is flat: the identity fields sit beside
    # schema/argv/virtual_headers, exactly as the aspect writes them.
    if "argv" not in context or "virtual_headers" not in context:
        raise ValueError("consumer context must carry argv and virtual_headers")
    identity = Identity.from_json(
        {key: value for key, value in context.items() if key not in ("schema", "argv", "virtual_headers")}
    )
    _validate_identity(
        identity,
        {
            "owner": canonical_owner(consumer["owner"]),
            "configuration": consumer["configuration"],
            "source": consumer["source"],
            "main": consumer["source"],
            "platform": platform,
        },
        "consumer context",
    )
    # The analyze helper still verifies that the platform matches the SDK.
    args.report.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".context-", dir=args.report.parent) as temporary:
        replayed_context = Path(temporary) / "context.json"
        _write_json(replayed_context, {**context, "main": item, "mode": replay_mode(item)})
        return bazel_lint_action.main(["analyze", "--context", os.fspath(replayed_context), *_analysis_options(args)])


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    select_command = commands.add_parser("select")
    select_command.add_argument("--candidates", type=Path, required=True)
    select_command.add_argument("--routes", type=Path, required=True)
    select_command.set_defaults(run=select)
    replay_command = commands.add_parser("replay")
    analyze_command = commands.add_parser("replay-analyze")
    for command in (replay_command, analyze_command):
        for name in _ANALYSIS_TOOLS:
            command.add_argument(f"--{name}", type=Path, required=command is analyze_command)
        for name in ("report", "log"):
            command.add_argument(f"--{name}", type=Path, required=True)
        command.add_argument("--plugin", type=Path)
    replay_command.add_argument("--selection", type=Path, required=True)
    replay_command.add_argument("--driver", type=Path)
    replay_command.add_argument("--context", type=Path)
    replay_command.add_argument("--replay", type=Path, required=True)
    replay_command.set_defaults(run=replay)
    analyze_command.add_argument("--consumer", type=Path, required=True)
    analyze_command.add_argument("--context", type=Path, required=True)
    analyze_command.set_defaults(run=replay_analyze)
    args = parser.parse_args(argv)
    try:
        return int(args.run(args))
    except (OSError, ValueError, RuntimeError, KeyError, TypeError) as error:
        print(f"Bazel lint graph {args.command} failed: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
