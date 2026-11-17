"""Concept CLI export and verified SDK admission."""

import hashlib
import io
import json
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest.mock import patch

from ao.__main__ import make_parser
from ao.command import measure
from ao.portal import bazel_concepts, concept_report
from ao.portal import dependency_checks as deps


class DepsConceptConsumerTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-deps-concepts-")
        self.addCleanup(temporary.cleanup)
        self.tree = Path(temporary.name).resolve()
        self.destination = self.tree / "copied-report.json"
        self.canonical = self.tree / "concept-report.json"
        self.args = make_parser().parse_args(
            [
                "measure",
                "concepts",
                "--config",
                "debug",
                "--path",
                str(self.tree),
                "--jobs",
                "3",
                "--json",
                str(self.destination),
            ]
        )

    def test_json_option_exports_the_measured_report(self):
        sdk = {"schema": 1}
        report = {"schemaVersion": 4, "measurement": {"nativeMethod": "fresh includes"}}
        with (
            patch.object(deps, "verified_report", return_value=sdk),
            patch.object(bazel_concepts, "write_bazel_report", return_value=report) as collect,
            patch.object(concept_report, "format_summary", return_value=""),
            redirect_stdout(io.StringIO()),
        ):
            self.assertEqual(measure.run_command(self.args), 0)
        collect.assert_called_once_with(deps.Request(config="debug", path=str(self.tree), jobs=3), sdk_facts=sdk)
        self.assertEqual(json.loads(self.destination.read_text(encoding="utf-8")), report)

    def test_invalid_sdk_does_not_replace_an_existing_export_or_run_measurement(self):
        previous = b'{"previous": true}\n'
        self.canonical.write_bytes(previous)
        self.destination.write_bytes(previous)
        with (
            patch.object(deps, "verified_report", side_effect=ValueError("stale SDK manifest")),
            patch.object(bazel_concepts, "write_bazel_report") as collect,
            redirect_stderr(io.StringIO()),
            self.assertRaises(SystemExit) as failure,
        ):
            measure.run_command(self.args)
        self.assertEqual(failure.exception.code, 1)
        collect.assert_not_called()
        self.assertEqual(self.canonical.read_bytes(), previous)
        self.assertEqual(self.destination.read_bytes(), previous)


class FastbuildManifestAdmissionTest(unittest.TestCase):
    def test_fastbuild_consumer_admits_the_existing_sdk_manifest(self):
        with tempfile.TemporaryDirectory(prefix="aobus-deps-fastbuild-") as temporary:
            header = Path(temporary) / "input.h"
            payload = b"native input payload"
            header.write_bytes(payload)
            manifest = {
                "generator": "aobus-bazel-artifact-adapter-v1",
                "configuration": {"platform": "linux", "config": "Release"},
                "packages": [{"name": "fixture"}],
                "inputs": [{"path": str(header), "sha256": hashlib.sha256(payload).hexdigest(), "roles": ["header"]}],
            }
            deps._verify_manifest(manifest, host="linux", config="fastbuild", workers=1)


if __name__ == "__main__":
    unittest.main()
