"""Selection and verdicts owned by whole-graph lint."""

import io
import json
import os
import subprocess
import tempfile
import unittest
from contextlib import redirect_stderr
from pathlib import Path, PureWindowsPath
from unittest.mock import patch

from ao.graph import bazel_graph_lint
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


def _identity(source, *, main=None, owner=_OWNER, configuration=_CONFIGURATION, platform="linux"):
    return {
        "owner": owner,
        "configuration": configuration,
        "source": source,
        "main": main or source,
        "outputs": [f"{configuration}/{source}.o"],
        "mode": "STRICT",
        "platform": platform,
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


class GraphWorkspace(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="ao-graph-lint-")
        self.addCleanup(temporary.cleanup)
        self.addCleanup(os.chdir, os.getcwd())
        os.chdir(temporary.name)

    def write_json(self, name, document):
        path = Path(name)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(document), encoding="utf-8")
        return name

    def read_json(self, name):
        return json.loads(Path(name).read_text(encoding="utf-8"))


class ReplayDriverTest(GraphWorkspace):
    _TOOLS = (
        "--policy",
        "policy.json",
        "--sdk",
        "sdk.json",
        "--verification",
        "verified.json",
        "--tidy",
        "tidy",
        "--plugin",
        "plugin",
    )
    _OUTPUTS = ("--report", "out/report.json", "--log", "out/replay.log")

    def consumer(self, name, identity, *, includes=(_HEADER,)):
        self.write_report(f"{name}.report", identity, includes)
        context = self.write_context(f"{name}.context", identity)
        return {
            **{field: identity[field] for field in ("owner", "configuration", "source")},
            "context": context,
            "report": f"{name}.report",
            "env": {"PATH": f"native/{name}", "SDKROOT": f"sdk/{name}"},
        }

    def write_report(self, path, identity, includes=()):
        self.write_json(
            path, {"schema": 1, "identity": identity, "outcome": "clean", "tool_exit": 0, "includes": includes}
        )

    def write_context(self, path, identity):
        return self.write_json(
            path, {"schema": 1, **identity, "argv": ["compiler", identity["source"]], "virtual_headers": {}}
        )

    def select(self, *candidates, route, routes="routes", item=_HEADER):
        manifest = self.write_json(
            "candidates.json", {"schema": 1, "item": item, "platform": "linux", "candidates": candidates}
        )
        self.assertEqual(bazel_graph_lint.main(["select", "--candidates", manifest, "--routes", routes]), 0)
        marker = f"{routes}/{route}"
        self.assertEqual(list(Path(routes).iterdir()), [Path(marker)])
        consumer = None if route == "none" else {k: v for k, v in candidates[int(route)].items() if k != "report"}
        self.assertEqual(self.read_json(marker), {"schema": 1, "item": item, "platform": "linux", "consumer": consumer})
        return marker

    def replay(self, marker, context=None):
        options = (
            ["--context", context, "--driver", bazel_graph_lint.__file__, *self._TOOLS] if context is not None else []
        )
        return bazel_graph_lint.main(
            ["replay", "--selection", marker, *options, *self._OUTPUTS, "--replay", "out/replay.json"]
        )

    def test_route_uses_original_index_and_changes_when_include_evidence_changes(self):
        other = self.consumer("other", _identity("app/example/Main.cpp", owner="@@//app/example:example"))
        local = self.consumer("local", _identity(_SOURCE))
        self.select(other, local, route="1")
        self.write_report(local["report"], _identity(_SOURCE))
        self.select(other, local, route="0", routes="updated")

    def test_no_consumer_preserves_the_three_outputs_without_launching_analysis(self):
        candidate = self.consumer("local", _identity(_SOURCE))
        marker = self.select(candidate, route="none", item=_FOREIGN)
        with patch.object(bazel_graph_lint.subprocess, "run") as run:
            self.assertEqual(self.replay(marker), 0)
        run.assert_not_called()
        self.assertEqual(Path("out/replay.log").read_bytes(), b"")
        self.assertEqual(self.read_json("out/report.json"), {"schema": 1, "item": _FOREIGN, "outcome": "no_consumer"})
        self.assertEqual(
            self.read_json("out/replay.json"),
            {
                "schema": 1,
                "item": _FOREIGN,
                "status": "no_consumer",
                "consumer": None,
                "report": None,
                "log": None,
            },
        )

    def test_replay_rejects_a_marker_from_another_route_even_with_matching_identity(self):
        local = self.consumer("local", _identity(_SOURCE))
        foreign = self.consumer("foreign", _identity(_SOURCE))
        marker = self.select(foreign, route="0")
        with patch.object(bazel_graph_lint.subprocess, "run") as run, redirect_stderr(io.StringIO()) as errors:
            self.assertEqual(self.replay(marker, local["context"]), 2)
        run.assert_not_called()
        self.assertIn("routed context path differs", errors.getvalue())

    def test_analysis_rejects_context_identity_mismatches_before_invoking_analyzer(self):
        consumer = self.consumer("local", _identity(_SOURCE))
        marker = self.select(consumer, route="0")
        for field, identity in (
            ("owner", _identity(_SOURCE, owner="@@//lib/foreign:foreign")),
            ("configuration", _identity(_SOURCE, configuration="bazel-out/k8-opt/bin")),
            ("source", _identity("lib/sample/Other.cpp", main=_SOURCE)),
            ("main", _identity(_SOURCE, main=_HEADER)),
            ("platform", _identity(_SOURCE, platform="windows")),
        ):
            with self.subTest(field=field):
                self.write_context(consumer["context"], identity)
                with (
                    patch.object(bazel_graph_lint.bazel_lint_action, "main") as analyze,
                    redirect_stderr(io.StringIO()) as errors,
                ):
                    self.assertEqual(
                        bazel_graph_lint.main(
                            [
                                "replay-analyze",
                                "--consumer",
                                marker,
                                "--context",
                                consumer["context"],
                                *self._TOOLS,
                                *self._OUTPUTS,
                            ]
                        ),
                        2,
                    )
                analyze.assert_not_called()
                self.assertIn(f"consumer context {field} differs", errors.getvalue())

    def test_replay_uses_the_native_environment_and_rejects_a_foreign_report_configuration(self):
        consumer = self.consumer("local", _identity(_SOURCE))
        item = "test/sample/Detail.h"
        self.write_report(consumer["report"], _identity(_SOURCE), (item,))
        marker = self.select(consumer, route="0", item=item)

        def run(command, *, env, **_kwargs):
            self.assertEqual(env, consumer["env"])
            status = bazel_graph_lint.main(command[command.index("replay-analyze") :])
            return subprocess.CompletedProcess(command, status, "")

        def analyze(command):
            self.assertEqual(command[0], "analyze")
            context = self.read_json(command[2])
            self.assertEqual(context, {**self.read_json(consumer["context"]), "main": item, "mode": "RELAXED"})
            self.write_report(
                command[command.index("--report") + 1],
                {**_identity(_SOURCE, main=item, configuration=configuration), "mode": "RELAXED"},
            )
            Path(command[command.index("--log") + 1]).touch()
            return 0

        for configuration in ("bazel-out/k8-opt/bin", _CONFIGURATION):
            with (
                self.subTest(configuration=configuration),
                patch.object(bazel_graph_lint.subprocess, "run", side_effect=run),
                patch.object(bazel_graph_lint.bazel_lint_action, "main", side_effect=analyze),
                redirect_stderr(io.StringIO()) as errors,
            ):
                status = self.replay(marker, consumer["context"])
                if configuration != _CONFIGURATION:
                    self.assertEqual(status, 2)
                    self.assertIn("replayed report configuration differs", errors.getvalue())
                    self.assertFalse(Path("out/replay.json").exists())
                else:
                    self.assertEqual(status, 0)
                    self.assertEqual(
                        self.read_json("out/replay.json"),
                        {
                            "schema": 1,
                            "item": item,
                            "status": "analyzed",
                            "consumer": {"owner": _OWNER, "source": _SOURCE},
                            "report": "out/report.json",
                            "log": "out/replay.log",
                        },
                    )


class GraphGateTest(GraphWorkspace):
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
