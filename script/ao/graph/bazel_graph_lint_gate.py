"""Gate for the whole-graph lint test: authored-main coverage and findings.

This module is the gate action's entrypoint, kept separate from the replay
driver so gate-only changes never invalidate cached replay actions. It
imports the shared validation helpers and replay policy from the replay
driver instead of duplicating parsers, and holds findings and coverage to
authored mains exactly like the portal's ``gate_reports`` selection: a
generated translation unit's report still validates and still supplies
consumer/include evidence, but only authored mains gate findings.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import Any

# Supports both the portal package and a declared Python script action. The
# lexical path keeps imports inside the action's declared module closure.
if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).absolute().parents[2]))

from ao.core.paths import PROJECT_ROOT
from ao.graph.bazel_graph_lint import SCHEMA, _array, _object, _paths, _text, _write_json, replay_mode
from ao.graph.bazel_lint_reports import Identity, canonical_owner, completed_outcome, exec_path, load_json
from ao.policy import dedup
from ao.policy.tidy_policy import platform_incompatible

REPLAY_STATUSES = ("analyzed", "no_consumer")
ITEM_STATUSES = ("covered", "deferred", "uncovered")


def load_inventory(path: Path) -> dict[str, Any]:
    """Load and validate the gate's declared graph inventory."""
    data = load_json(path)
    if set(data) != {"schema", "platform", "owners", "replays"}:
        raise ValueError("inventory must contain exactly the graph gate fields")
    owners = []
    for owner in _array(data["owners"], "inventory owners"):
        owner = _object(owner, "inventory owner")
        if set(owner) != {"owner", "configuration", "index"}:
            raise ValueError("inventory owner must contain exactly the owner fields")
        owners.append(
            {
                "owner": canonical_owner(_text(owner["owner"], "inventory owner")),
                "configuration": exec_path(owner["configuration"]),
                "index": exec_path(owner["index"]),
            }
        )
    replays = []
    for replay_entry in _array(data["replays"], "inventory replays"):
        replay_entry = _object(replay_entry, "inventory replay")
        if set(replay_entry) != {"item", "replay", "report", "log"}:
            raise ValueError("inventory replay must contain exactly the replay fields")
        replays.append(
            {
                "item": exec_path(replay_entry["item"]),
                "replay": exec_path(replay_entry["replay"]),
                "report": exec_path(replay_entry["report"]),
                "log": exec_path(replay_entry["log"]),
            }
        )
    if len({(owner["owner"], owner["configuration"]) for owner in owners}) != len(owners):
        raise ValueError("duplicate configured lint graph owner")
    if len({replay_entry["item"] for replay_entry in replays}) != len(replays):
        raise ValueError("duplicate lint graph replay item")
    return {
        "schema": SCHEMA,
        "platform": _text(data["platform"], "inventory platform"),
        "owners": owners,
        "replays": replays,
    }


def load_owner_inventory(owner: dict[str, Any]) -> dict[str, Any]:
    """Load one owner's index and validate its declared reports."""
    index = load_json(Path(owner["index"]))
    if set(index) != {"schema", "owner", "configuration", "sources", "authored", "uncovered", "reports"}:
        raise ValueError("owner index must contain exactly the index fields")
    if canonical_owner(index["owner"]) != owner["owner"] or index["configuration"] != owner["configuration"]:
        raise ValueError("index owner/configuration mismatch")
    sources = _paths(index["sources"], "eligible owner sources")
    authored = set(_paths(index["authored"], "authored owner mains"))
    if not authored <= set(sources):
        raise ValueError("authored mains outside the owner inventory")
    uncovered = []
    for item in _array(index["uncovered"], "uncovered"):
        item = _object(item, "uncovered entry")
        path = exec_path(item.get("path"))
        if path not in sources or not isinstance(item.get("reason"), str) or not item["reason"]:
            raise ValueError("uncovered entry must name a declared source with a reason")
        uncovered.append({"path": path, "reason": item["reason"]})
    if len({item["path"] for item in uncovered}) != len(uncovered):
        raise ValueError("duplicate uncovered entry")
    if any(item["path"] not in authored for item in uncovered):
        raise ValueError("uncovered entry outside the authored mains")
    reports: list[dict[str, Any]] = []
    for entry in _array(index["reports"], "indexed reports"):
        entry = _object(entry, "report index entry")
        if set(entry) != {"identity", "report", "log"}:
            raise ValueError("report index entry must contain exactly the entry fields")
        identity = Identity.from_json(entry["identity"])
        if identity.owner != owner["owner"] or identity.configuration != owner["configuration"]:
            raise ValueError("foreign report context")
        if identity.source not in sources or identity.main not in sources:
            raise ValueError("report main/source outside the owner inventory")
        report_path = exec_path(entry["report"])
        log_path = exec_path(entry["log"])
        if report_path == log_path:
            raise ValueError("report and log share one path")
        report = load_json(Path(report_path))
        if Identity.from_json(report["identity"]) != identity:
            raise ValueError("report identity differs from its index")
        outcome = completed_outcome(Path(log_path), report["tool_exit"])
        if report["outcome"] != outcome:
            raise ValueError("report outcome differs from its completed log")
        reports.append(
            {
                "identity": identity,
                "outcome": outcome,
                "report": report_path,
                "log": log_path,
                "includes": frozenset(_paths(report["includes"], "report includes")),
            }
        )
    if len({report["identity"] for report in reports}) != len(reports):
        raise ValueError("duplicate report identity")
    covered = {report["identity"].main for report in reports}
    if covered & {item["path"] for item in uncovered}:
        raise ValueError("simultaneously covered and uncovered source")
    if set(sources) != covered | {item["path"] for item in uncovered}:
        raise ValueError("eligible owner inventory has unaccounted sources")
    inventory: dict[str, Any] = {
        "owner": owner["owner"],
        "configuration": owner["configuration"],
        "sources": sources,
        "authored": authored,
        "uncovered": uncovered,
        "reports": reports,
    }
    return inventory


def load_replay_result(entry: dict[str, Any], platform: str) -> dict[str, Any]:
    """Load one replay's wrapper and, when analyzed, its underlying report."""
    wrapper = load_json(Path(entry["replay"]))
    if set(wrapper) != {"schema", "item", "status", "consumer", "report", "log"}:
        raise ValueError("replay result must contain exactly the replay fields")
    if wrapper["item"] != entry["item"] or wrapper["status"] not in REPLAY_STATUSES:
        raise ValueError("replay result item/status mismatch")
    if wrapper["status"] == "no_consumer":
        if wrapper["consumer"] is not None or wrapper["report"] is not None or wrapper["log"] is not None:
            raise ValueError("consumer-less replay must not name artifacts")
        return {"item": entry["item"], "status": "no_consumer", "consumer": None, "report": None}
    consumer = _object(wrapper["consumer"], "replay consumer")
    if set(consumer) != {"owner", "source"} or wrapper["report"] != entry["report"] or wrapper["log"] != entry["log"]:
        raise ValueError(
            "analyzed replay must name its consumer and artifacts: item="
            + entry["item"]
            + " report="
            + repr(wrapper["report"])
            + " log="
            + repr(wrapper["log"])
        )
    report = load_json(Path(entry["report"]))
    identity = Identity.from_json(report["identity"])
    if (
        identity.owner != consumer["owner"]
        or identity.source != consumer["source"]
        or identity.main != entry["item"]
        or identity.mode != replay_mode(entry["item"])
        or identity.platform != platform
    ):
        raise ValueError("replayed report identity differs from its replay result")
    outcome = completed_outcome(Path(entry["log"]), report["tool_exit"])
    if report["outcome"] != outcome:
        raise ValueError("replayed report outcome differs from its completed log")
    return {
        "item": entry["item"],
        "status": "analyzed",
        "consumer": consumer,
        "report": entry["report"],
        "log": entry["log"],
        "outcome": outcome,
    }


def gate(args: argparse.Namespace) -> int:
    """Gate the whole graph: coverage failures are infrastructure, findings fail."""
    inventory = load_inventory(args.inventory)
    platform = inventory["platform"]
    owners = [load_owner_inventory(owner) for owner in inventory["owners"]]
    replays = {entry["item"]: load_replay_result(entry, platform) for entry in inventory["replays"]}

    covered = {report["identity"].main for owner in owners for report in owner["reports"]}
    failures: list[str] = []
    deferred: set[str] = set()
    item_status: dict[str, str] = {}
    findings: list[dict[str, Any]] = []
    for owner in owners:
        # The portal's authored-main selection: generated translation units
        # still validate above and still supply include evidence, but only
        # authored mains gate findings.
        for report in owner["reports"]:
            if report["outcome"] != "clean" and report["identity"].main in owner["authored"]:
                findings.append(report)
    for owner in owners:
        for item in owner["uncovered"]:
            path = item["path"]
            if path in covered:
                item_status.setdefault(path, "covered")
                continue
            replay = replays.get(path)
            if replay is None:
                failures.append(f"Uncovered lint source has no replay: {path}")
                item_status.setdefault(path, "uncovered")
                continue
            if replay["status"] == "analyzed":
                item_status.setdefault(path, "covered")
                covered.add(path)
                if replay["outcome"] != "clean":
                    findings.append({"identity": None, "log": replay["log"], "item": path})
                continue
            if platform_incompatible(path, platform):
                deferred.add(path)
                item_status.setdefault(path, "deferred")
            else:
                failures.append(f"Uncovered lint source: {path}: {item['reason']}")
                item_status.setdefault(path, "uncovered")
    for path in sorted(set(replays) - {item["path"] for owner in owners for item in owner["uncovered"]}):
        failures.append(f"Replay without an uncovered lint source: {path}")

    messages: list[str] = []
    messages.extend(f"Deferred platform-owned lint source: {path}" for path in sorted(deferred))
    messages.extend(failures)
    with args.log.open("w", encoding="utf-8") as sink:
        count = dedup.deduplicate(
            [Path(report["log"]) for report in findings],
            sink,
            PROJECT_ROOT,
        )
        sink.writelines(message + "\n" for message in messages)
    status = 2 if failures else (1 if findings or count else 0)
    _write_json(
        args.result,
        {
            "schema": SCHEMA,
            "platform": platform,
            "status": status,
            "counts": {
                "owners": len(owners),
                "reports": sum(len(owner["reports"]) for owner in owners),
                "replays": len(replays),
                "deferred": len(deferred),
                "findings": count,
            },
            "deferred": sorted(deferred),
            "failures": failures,
        },
    )
    _write_json(
        args.coverage,
        {
            "schema": SCHEMA,
            "platform": platform,
            "status": status,
            "owners": [
                {
                    "owner": owner["owner"],
                    "configuration": owner["configuration"],
                    "sources": owner["sources"],
                    "authored": sorted(owner["authored"]),
                    "covered": sorted(path for path in owner["sources"] if path in covered),
                    "uncovered": [
                        {**item, "status": item_status.get(item["path"], "uncovered")} for item in owner["uncovered"]
                    ],
                    "reports": len(owner["reports"]),
                }
                for owner in owners
            ],
            "replays": [
                {
                    "item": entry["item"],
                    "status": replays[entry["item"]]["status"],
                    "consumer": replays[entry["item"]]["consumer"],
                }
                for entry in inventory["replays"]
            ],
            "totals": {
                "owners": len(owners),
                "sources": sum(len(owner["sources"]) for owner in owners),
                "covered": len(covered),
                "deferred": len(deferred),
                "findings": count,
            },
        },
    )
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    gate_command = parser.add_subparsers(dest="command", required=True).add_parser("gate")
    for name in ("inventory", "result", "log", "coverage"):
        gate_command.add_argument(f"--{name}", type=Path, required=True)
    gate_command.set_defaults(run=gate)
    args = parser.parse_args(argv)
    try:
        return int(args.run(args))
    except (OSError, ValueError, RuntimeError, KeyError, TypeError) as error:
        print(f"Bazel lint graph {args.command} failed: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
