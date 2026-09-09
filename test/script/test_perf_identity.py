"""Regression coverage for source identity in reused performance executables."""

import contextlib
import io
import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao.__main__ import make_parser
from ao.command import perf
from ao.core import builddir


class PerformanceIdentityTest(unittest.TestCase):
    def test_reused_executable_cannot_claim_the_current_checkout_revision(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            executable = builddir.executable(directory / "test" / perf.TARGET)
            executable.parent.mkdir()
            original = b"existing executable from a previous revision"
            executable.write_bytes(original)
            report_path = directory / "new-parent" / "report.json"
            args = make_parser().parse_args(["perf", "--no-build", "-p", str(directory), "--output", str(report_path)])

            def write_report(_command, *, env, **_kwargs):
                report = {
                    "schema": perf.REPORT_SCHEMA,
                    "metadata": {
                        "revision": env["AOBUS_PERF_REVISION"],
                        "compiler": env["AOBUS_PERF_COMPILER"],
                        "build_mode": env["AOBUS_PERF_BUILD_MODE"],
                        "platform": env["AOBUS_PERF_PLATFORM"],
                        "icu_version": "78.3",
                    },
                    "measurements": [],
                }
                Path(env["AOBUS_PERF_REPORT_JSON"]).write_text(json.dumps(report), encoding="utf-8")
                return 0

            output = io.StringIO()
            with (
                mock.patch.object(perf, "_revision", return_value="new-checkout+dirty") as revision,
                mock.patch.object(perf.build, "do_build") as build,
                mock.patch.object(perf.build, "validate_build_tree", return_value="gcc") as validate,
                mock.patch.object(perf, "run", side_effect=write_report),
                contextlib.redirect_stdout(output),
            ):
                self.assertEqual(perf.run_command(args), 0)

            report = json.loads(report_path.read_text(encoding="utf-8"))
            self.assertEqual(report["metadata"]["revision"], "unverified")
            self.assertIn("Source revision: unverified", output.getvalue())
            self.assertNotIn("new-checkout", output.getvalue())
            self.assertEqual(executable.read_bytes(), original)
            validate.assert_called_once_with(args, directory, expected_build_type="Release")
            self.assertEqual(report["metadata"]["compiler"], "gcc")
            self.assertEqual(report["metadata"]["build_mode"], "release")
            build.assert_not_called()
            revision.assert_not_called()


if __name__ == "__main__":
    unittest.main()
