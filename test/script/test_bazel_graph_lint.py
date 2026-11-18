"""Selection and verdicts owned by whole-graph lint."""

import json
import os
import tempfile
import unittest
from pathlib import Path, PureWindowsPath

from ao.graph.bazel_graph_lint import replay_wrapper, select_consumer
from ao.graph.bazel_graph_lint_gate import load_replay_result
from ao.graph.bazel_graph_lint_gate import main as gate_main
from ao.graph.bazel_lint_reports import Identity

_OWNER = "@@//lib/sample:sample"
_CONFIGURATION = "bazel-out/k8-dbg/bin"
_SOURCE = "lib/sample/Sample.cpp"
_HEADER = "include/ao/sample/Sample.h"
_FOREIGN = "app/windows-winui/View.h"
_GENERATED = f"{_CONFIGURATION}/lib/sample/generated.cpp"


def _identity(source, *, main=None, owner=_OWNER):
    return {
        "owner": owner,
        "configuration": _CONFIGURATION,
        "source": source,
        "main": main or source,
        "outputs": [f"{_CONFIGURATION}/{source}.o"],
        "mode": "STRICT",
        "platform": "linux",
    }


class ReplaySelectionTest(unittest.TestCase):
    def test_only_a_translation_unit_that_included_the_header_is_a_consumer(self):
        alternate = "@@//app/example:example"
        entries = {
            "nearby.json": _identity(_SOURCE),
            "paired.json": _identity(_SOURCE, main="lib/sample/Other.h"),
            "consumer.json": _identity("app/example/Main.cpp", owner=alternate),
        }
        candidates = [
            {"owner": identity["owner"], "source": identity["source"], "report": path}
            for path, identity in entries.items()
        ]
        reports = {
            path: {
                "identity": Identity.from_json(identity),
                "includes": frozenset() if path == "nearby.json" else frozenset({_HEADER}),
            }
            for path, identity in entries.items()
        }
        self.assertIsNone(select_consumer(_HEADER, candidates[:2], reports))
        self.assertEqual(select_consumer(_HEADER, candidates, reports)["report"], "consumer.json")

    def test_graph_replay_uses_its_component_when_two_translation_units_include_the_header(self):
        foreign_owner = "@@//app/example:example"
        entries = [
            ("foreign.json", _identity("app/example/Main.cpp", owner=foreign_owner)),
            ("local.json", _identity(_SOURCE)),
        ]
        candidates = [
            {"owner": identity["owner"], "source": identity["source"], "report": path} for path, identity in entries
        ]
        reports = {
            path: {"identity": Identity.from_json(identity), "includes": frozenset({_HEADER})}
            for path, identity in entries
        }
        self.assertEqual(select_consumer(_HEADER, candidates, reports)["report"], "local.json")


class GraphGateTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="ao-graph-gate-")
        self.addCleanup(temporary.cleanup)
        self.addCleanup(os.chdir, os.getcwd())
        os.chdir(temporary.name)

    def write_json(self, name, document):
        path = Path(name)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(document), encoding="utf-8")
        return name

    def report(self, name, identity, *, finding=False):
        report = self.write_json(
            f"{name}.report.json",
            {
                "schema": 1,
                "identity": identity,
                "outcome": "warning" if finding else "clean",
                "tool_exit": 0,
                "includes": [],
            },
        )
        log = f"{name}.log"
        Path(log).write_text(
            f"{identity['main']}:10:3: warning: sample finding [aobus-license-header]\n" if finding else "",
            encoding="utf-8",
        )
        return {"identity": identity, "report": report, "log": log}

    def owner(self, *, findings=(), uncovered=()):
        index = self.write_json(
            "owner.index.json",
            {
                "schema": 1,
                "owner": _OWNER,
                "configuration": _CONFIGURATION,
                "sources": [_SOURCE, _GENERATED, *uncovered],
                "authored": [_SOURCE, *uncovered],
                "uncovered": [{"path": item, "reason": "no matching source"} for item in uncovered],
                "reports": [
                    self.report(name, _identity(source), finding=source in findings)
                    for name, source in (("source", _SOURCE), ("generated", _GENERATED))
                ],
            },
        )
        return {"owner": _OWNER, "configuration": _CONFIGURATION, "index": index}

    def replay(self, item, *, consumer=False, finding=False):
        name = Path(item).stem
        artifacts = self.report(name, _identity(_SOURCE, main=item), finding=finding) if consumer else None
        report = artifacts["report"] if artifacts else f"{name}.report.json"
        log = artifacts["log"] if artifacts else f"{name}.log"
        if not artifacts:
            Path(report).touch()
            Path(log).touch()
        payload = (
            replay_wrapper(item, {"owner": _OWNER, "source": _SOURCE}, Path(report), Path(log))
            if consumer
            else {"schema": 1, "item": item, "status": "no_consumer", "consumer": None, "report": None, "log": None}
        )
        wrapper = self.write_json(f"{name}.replay.json", payload)
        return {"item": item, "replay": wrapper, "report": report, "log": log}

    def gate(self, owner, *replays):
        inventory = self.write_json(
            "inventory.json", {"schema": 1, "platform": "linux", "owners": [owner], "replays": list(replays)}
        )
        action_status = gate_main(
            [
                "gate",
                "--inventory",
                inventory,
                "--result",
                "result.json",
                "--log",
                "gate.log",
                "--coverage",
                "coverage.json",
            ]
        )
        self.assertEqual(action_status, 0)
        result = json.loads(Path("result.json").read_text(encoding="utf-8"))
        return result["status"], Path("gate.log").read_text(encoding="utf-8")

    def test_generated_findings_do_not_gate_but_authored_findings_do(self):
        status, log = self.gate(self.owner(findings=(_GENERATED,)))
        self.assertEqual(status, 0)
        self.assertNotIn(_GENERATED, log)
        owner = self.owner(findings=(_GENERATED, _SOURCE), uncovered=(_HEADER,))
        status, log = self.gate(owner, self.replay(_HEADER, consumer=True, finding=True))
        self.assertEqual(status, 1)
        self.assertIn(_SOURCE, log)
        self.assertIn(_HEADER, log)
        self.assertNotIn(_GENERATED, log)

    def test_compatible_header_needs_a_consumer_even_when_a_foreign_header_can_defer(self):
        owner = self.owner(uncovered=(_HEADER, _FOREIGN))
        foreign = self.replay(_FOREIGN)
        status, log = self.gate(owner, self.replay(_HEADER), foreign)
        self.assertEqual(status, 2)
        self.assertIn(f"{_HEADER}: no matching source", log)
        status, log = self.gate(owner, self.replay(_HEADER, consumer=True), foreign)
        self.assertEqual(status, 0)
        self.assertNotIn(_HEADER, log)
        self.assertIn(_FOREIGN, log)

    def test_windows_replay_artifacts_can_be_loaded_by_the_portable_inventory(self):
        artifacts = self.report("graph/header", _identity(_SOURCE, main=_HEADER))
        consumer = {"owner": _OWNER, "source": _SOURCE}
        replay = self.write_json(
            "header.replay.json",
            replay_wrapper(_HEADER, consumer, PureWindowsPath(artifacts["report"]), PureWindowsPath(artifacts["log"])),
        )
        loaded = load_replay_result(
            {"item": _HEADER, "replay": replay, "report": artifacts["report"], "log": artifacts["log"]}, "linux"
        )
        self.assertEqual(loaded["consumer"], consumer)


if __name__ == "__main__":
    unittest.main()
