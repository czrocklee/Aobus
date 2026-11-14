"""Public deps consumer holds SDK verification and measurement in one tree lock."""

import argparse
import io
import json
import tempfile
import unittest
from contextlib import contextmanager, redirect_stderr, redirect_stdout
from pathlib import Path
from unittest.mock import patch

from ao.command import deps
from ao.core import bazel_concepts, concept_report


class DepsConceptConsumerTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-deps-concepts-")
        self.addCleanup(temporary.cleanup)
        self.tree = Path(temporary.name)
        self.destination = self.tree / "copied-report.json"
        self.canonical = self.tree / "concept-report.json"
        self.args = argparse.Namespace(
            deps_action="report",
            config="release",
            path=str(self.tree),
            vcpkg_prefix=None,
            jobs=3,
            concepts=True,
            json=self.destination,
        )
        self.events = []
        self.locked = False

    @contextmanager
    def lock(self, tree):
        self.assertEqual(tree, self.tree)
        self.events.append("enter")
        self.locked = True
        try:
            yield
        finally:
            self.locked = False
            self.events.append("exit")

    def test_verified_sdk_and_measurement_share_lock_and_json_is_identical(self):
        sdk = {"schema": 1, "fixture": "verified SDK facts"}
        report = {"schemaVersion": 4, "measurement": {"nativeMethod": "fresh includes"}}
        expected = b'{\n  "measurement": {\n    "nativeMethod": "fresh includes"\n  },\n  "schemaVersion": 4\n}\n'

        def verify(args):
            self.assertTrue(self.locked)
            self.assertIs(args, self.args)
            self.events.append("verify")
            return sdk

        def measure(args, *, sdk_facts):
            self.assertTrue(self.locked)
            self.assertIs(args, self.args)
            self.assertIs(sdk_facts, sdk)
            self.events.append("measure")
            self.canonical.write_bytes(expected)
            return report

        output = io.StringIO()
        with (
            patch.object(deps.bazel_workspace, "workspace_path", return_value=self.tree),
            patch.object(deps, "build_tree_lock", side_effect=self.lock),
            patch.object(deps, "_verified", side_effect=verify),
            patch.object(bazel_concepts, "write_bazel_report", side_effect=measure),
            patch.object(concept_report, "format_summary", return_value="Fresh compiler includes"),
            redirect_stdout(output),
        ):
            self.assertEqual(deps.run_command(self.args), 0)

        self.assertEqual(self.events, ["enter", "verify", "measure", "exit"])
        self.assertEqual(self.destination.read_bytes(), expected)
        self.assertEqual(self.destination.read_bytes(), self.canonical.read_bytes())
        self.assertEqual(json.loads(self.destination.read_text()), report)
        self.assertEqual(output.getvalue(), "Fresh compiler includes\n")

    def test_sdk_failure_does_not_measure_or_replace_existing_reports(self):
        previous = b'{"previous": true}\n'
        self.canonical.write_bytes(previous)
        self.destination.write_bytes(previous)
        with (
            patch.object(deps.bazel_workspace, "workspace_path", return_value=self.tree),
            patch.object(deps, "build_tree_lock", side_effect=self.lock),
            patch.object(deps, "_verified", side_effect=ValueError("changed native SDK input")),
            patch.object(bazel_concepts, "write_bazel_report") as measure,
            redirect_stderr(io.StringIO()),
            self.assertRaises(SystemExit),
        ):
            deps.run_command(self.args)
        measure.assert_not_called()
        self.assertEqual(self.events, ["enter", "exit"])
        self.assertEqual(self.canonical.read_bytes(), previous)
        self.assertEqual(self.destination.read_bytes(), previous)


if __name__ == "__main__":
    unittest.main()
