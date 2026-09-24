"""Performance artifact freshness, validation and output ownership."""

import contextlib
import io
import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao.__main__ import make_parser
from ao.command import perf
from ao.core import builddir


class PerformanceReportTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        executable = builddir.executable(self.root / "test" / perf.TARGET)
        executable.parent.mkdir()
        executable.touch()
        self.output = self.root / "review.json"
        self.baseline = self.root / "报告-baseline.json"
        self.args = make_parser().parse_args(["perf", "--no-build", "-p", str(self.root), "--output", str(self.output)])
        environment = mock.patch.dict(os.environ, {"AOBUS_PERF_BASELINE_JSON": str(self.baseline)})
        environment.start()
        self.addCleanup(environment.stop)
        identity = mock.patch.object(perf.build, "validate_build_tree", return_value="test-compiler")
        identity.start()
        self.addCleanup(identity.stop)

    @staticmethod
    def review():
        return {
            "schema": "aobus-performance-review/v2",
            "metadata": {"platform": "test", "build_mode": "release", "compiler": "test", "icu_version": "78.3"},
            "measurements": [
                {
                    "capability": "ordering",
                    "scenario": "construction",
                    "dataset": "none",
                    "input_count": 0,
                    "median_ns": 1,
                    "p95_ns": 2,
                }
            ],
        }

    @staticmethod
    def legacy():
        return {
            "schema": "aobus-performance-baseline/v1",
            "records": [{"benchmark": "move", "metrics": [{"name": "count", "value": 0, "unit": "item"}]}],
        }

    def run_with(self, child):
        with mock.patch.object(perf, "run", side_effect=child), contextlib.redirect_stdout(io.StringIO()) as output:
            status = perf.run_command(self.args)
        return status, output.getvalue()

    def test_success_refreshes_and_validates_both_requested_reports(self):
        self.output.write_text("stale v2", encoding="utf-8")
        self.baseline.write_text("stale v1", encoding="utf-8")

        def child(_command, *, env):
            self.assertEqual(Path(env["AOBUS_PERF_REPORT_JSON"]), self.output.absolute())
            self.assertEqual(Path(env["AOBUS_PERF_BASELINE_JSON"]), self.baseline.absolute())
            self.assertFalse(self.output.exists())
            self.assertFalse(self.baseline.exists())
            self.output.write_text(json.dumps(self.review()), encoding="utf-8")
            self.baseline.write_text(json.dumps(self.legacy()), encoding="utf-8")
            return 0

        status, output = self.run_with(child)
        self.assertEqual(status, 0)
        self.assertIn("Performance review:", output)
        self.assertEqual(json.loads(self.baseline.read_text(encoding="utf-8")), self.legacy())

    def test_missing_malformed_or_empty_v1_cannot_hide_behind_valid_v2(self):
        invalid = [
            None,
            b"not JSON",
            b"\xff",
            b"[]",
            json.dumps({"schema": "wrong", "records": []}).encode(),
            json.dumps({"schema": "aobus-performance-baseline/v1", "records": []}).encode(),
            json.dumps({"schema": "aobus-performance-baseline/v1", "records": [None]}).encode(),
        ]
        for field, value in (("benchmark", ""), ("metrics", [])):
            report = self.legacy()
            report["records"][0][field] = value
            invalid.append(json.dumps(report).encode())
        for field, value in (("name", ""), ("unit", ""), ("value", True), ("value", "1")):
            report = self.legacy()
            report["records"][0]["metrics"][0][field] = value
            invalid.append(json.dumps(report).encode())

        for payload in invalid:
            with self.subTest(payload=payload):
                self.baseline.write_text(json.dumps(self.legacy()), encoding="utf-8")

                def child(_command, *, env, payload=payload):
                    self.assertFalse(self.baseline.exists(), "the previous v1 report must not survive launch")
                    self.output.write_text(json.dumps(self.review()), encoding="utf-8")
                    if payload is not None:
                        self.baseline.write_bytes(payload)
                    return 0

                with self.assertRaises(SystemExit):
                    self.run_with(child)

    def test_empty_v2_and_failed_children_cannot_claim_success(self):
        for empty, status in ((True, 0), (False, 42)):
            with self.subTest(empty=empty, status=status):

                def child(_command, *, env, empty=empty, status=status):
                    report = self.review()
                    if empty:
                        report["measurements"] = []
                    self.output.write_text(json.dumps(report), encoding="utf-8")
                    self.baseline.write_text(json.dumps(self.legacy()), encoding="utf-8")
                    return status

                with self.assertRaises(SystemExit):
                    self.run_with(child)

    def test_no_v1_request_requires_only_the_review_report(self):
        def child(_command, *, env):
            self.assertEqual(env["AOBUS_PERF_BASELINE_JSON"], "")
            self.output.write_text(json.dumps(self.review()), encoding="utf-8")
            return 0

        with mock.patch.dict(os.environ, {"AOBUS_PERF_BASELINE_JSON": ""}):
            self.assertEqual(self.run_with(child)[0], 0)
        self.assertFalse(self.baseline.exists())

    def test_colliding_paths_preserve_existing_reports_before_launch(self):
        self.output.write_bytes(b"existing evidence")
        for alias in (self.output, self.root / "." / self.output.name):
            with self.subTest(alias=alias), mock.patch.dict(os.environ, {"AOBUS_PERF_BASELINE_JSON": str(alias)}):
                with mock.patch.object(perf, "run") as child, self.assertRaises(SystemExit):
                    perf.run_command(self.args)
                child.assert_not_called()
                self.assertEqual(self.output.read_bytes(), b"existing evidence")
        os.link(self.output, self.baseline)
        with mock.patch.object(perf, "run") as child, self.assertRaises(SystemExit):
            perf.run_command(self.args)
        child.assert_not_called()
        self.assertEqual(self.output.read_bytes(), b"existing evidence")
        self.assertEqual(self.baseline.read_bytes(), b"existing evidence")

    def test_directory_destinations_preserve_the_other_existing_report(self):
        for directory, sibling in ((self.baseline, self.output), (self.output, self.baseline)):
            with self.subTest(directory=directory):
                directory.mkdir()
                sibling.write_bytes(b"keep")
                with mock.patch.object(perf, "run") as child, self.assertRaises(SystemExit):
                    perf.run_command(self.args)
                child.assert_not_called()
                self.assertTrue(directory.is_dir())
                self.assertEqual(sibling.read_bytes(), b"keep")
                directory.rmdir()
                sibling.unlink()

    @unittest.skipUnless(os.name == "posix", "POSIX symlink and special-device destinations")
    def test_symlinks_and_special_files_are_rejected_without_deleting_evidence(self):
        self.output.write_bytes(b"keep")
        self.baseline.symlink_to(self.output)
        for destination in (self.baseline, Path("/dev/null")):
            with self.subTest(destination=destination):
                with mock.patch.dict(os.environ, {"AOBUS_PERF_BASELINE_JSON": str(destination)}):
                    with mock.patch.object(perf, "run") as child, self.assertRaises(SystemExit):
                        perf.run_command(self.args)
                child.assert_not_called()
                self.assertEqual(self.output.read_bytes(), b"keep")
                self.assertTrue(self.baseline.is_symlink())

    def test_build_identity_failure_preserves_both_reports(self):
        self.output.write_bytes(b"v2 sentinel")
        self.baseline.write_bytes(b"v1 sentinel")
        with mock.patch.object(perf.build, "validate_build_tree", side_effect=SystemExit(1)):
            with mock.patch.object(perf, "run") as child, self.assertRaises(SystemExit):
                perf.run_command(self.args)
        child.assert_not_called()
        self.assertEqual(self.output.read_bytes(), b"v2 sentinel")
        self.assertEqual(self.baseline.read_bytes(), b"v1 sentinel")


if __name__ == "__main__":
    unittest.main()
