"""Performance artifact freshness, validation and output ownership."""

import contextlib
import io
import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from workflow_support import executable_artifact

from ao.__main__ import make_parser
from ao.command import perf
from ao.core import builddir
from ao.core.bazel_native import BazelWorkspaceError


class PerformanceReportTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.artifact = executable_artifact(
            self.root, target=perf.TARGET_LABEL, config="release", compiler="test-compiler"
        )
        self.enterContext(mock.patch.object(builddir, "platform_profile", return_value=builddir.LINUX_PROFILE))
        self.output = self.root / "review.json"
        self.baseline = self.root / "报告-baseline.json"
        self.args = make_parser().parse_args(["perf", "--no-build", "-p", str(self.root), "--output", str(self.output)])
        environment = mock.patch.dict(os.environ, {"AOBUS_PERF_BASELINE_JSON": str(self.baseline)})
        environment.start()
        self.addCleanup(environment.stop)
        self.enterContext(
            mock.patch.object(
                perf.bazel_workflows,
                "prepare_artifacts",
                side_effect=lambda *args, **kwargs: contextlib.nullcontext((self.artifact,)),
            )
        )

    @staticmethod
    def review():
        return {
            "schema": "aobus-performance-review/v2",
            "metadata": {
                "platform": "linux",
                "build_mode": "release",
                "compiler": "test-compiler",
                "icu_version": "78.3",
                "revision": "unverified",
            },
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

    def test_fresh_report_cannot_claim_another_configured_launch_identity(self):
        for field in ("revision", "compiler", "build_mode", "platform"):
            with self.subTest(field=field):

                def child(_command, *, env, field=field):
                    report = self.review()
                    report["metadata"][field] = "foreign identity"
                    self.output.write_text(json.dumps(report), encoding="utf-8")
                    self.baseline.write_text(json.dumps(self.legacy()), encoding="utf-8")
                    return 0

                with self.assertRaises(SystemExit):
                    self.run_with(child)
                self.assertNotIn("bazel", json.loads(self.output.read_text(encoding="utf-8"))["metadata"])

    def test_build_identity_failure_preserves_both_reports(self):
        self.output.write_bytes(b"v2 sentinel")
        self.baseline.write_bytes(b"v1 sentinel")
        with mock.patch.object(
            perf.bazel_workflows, "prepare_artifacts", side_effect=BazelWorkspaceError("SDK admission failed")
        ):
            with mock.patch.object(perf, "run") as child, self.assertRaises(SystemExit):
                perf.run_command(self.args)
        child.assert_not_called()
        self.assertEqual(self.output.read_bytes(), b"v2 sentinel")
        self.assertEqual(self.baseline.read_bytes(), b"v1 sentinel")


if __name__ == "__main__":
    unittest.main()
