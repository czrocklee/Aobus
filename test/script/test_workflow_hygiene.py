"""Scoped workflow lint distinguishes advisory findings from tool failures."""

import contextlib
import io
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao.portal import bazel_check_tools, source_lint, source_scope


class WorkflowLintTest(unittest.TestCase):
    def test_non_linux_resolver_does_not_admit_actionlint(self):
        with (
            mock.patch.object(sys, "platform", "darwin"),
            mock.patch.object(source_lint, "actionlint_executable") as tool,
            self.assertRaisesRegex(ValueError, "only available on Linux"),
        ):
            bazel_check_tools.resolve("actionlint")
        tool.assert_not_called()

    @unittest.skipUnless(sys.platform == "linux", "Nix path admission runs only on Linux")
    def test_actionlint_requires_a_store_executable_before_invocation(self):
        store = "/nix/store/fixture/bin/actionlint"
        for selected, resolved, executable in (
            ("actionlint", store, True),
            ("/tmp/actionlint", store, True),
            (store, "/tmp/actionlint", True),
            (store, store, False),
        ):
            with (
                self.subTest(selected=selected, resolved=resolved, executable=executable),
                mock.patch.dict(os.environ, {"AO_NIX_ACTIONLINT": selected}),
                mock.patch.object(Path, "resolve", return_value=Path(resolved)),
                mock.patch.object(Path, "is_file", return_value=True),
                mock.patch.object(os, "access", return_value=executable),
                self.assertRaises(ValueError),
            ):
                source_lint.actionlint_executable()

    def test_report_only_accepts_findings_but_not_a_failed_actionlint_process(self):
        workflow = ".github/workflows/ci.yml"
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / workflow).parent.mkdir(parents=True)
            (root / workflow).touch()
            tool = str(root / "pinned-actionlint")
            with (
                mock.patch.object(source_scope, "PROJECT_ROOT", root),
                mock.patch.object(source_lint, "PROJECT_ROOT", root),
                mock.patch.object(source_lint.sys, "platform", "linux"),
                mock.patch.object(source_lint, "actionlint_executable", return_value=tool),
                mock.patch.object(source_lint.subprocess, "run") as process,
                contextlib.redirect_stdout(io.StringIO()),
                contextlib.redirect_stderr(io.StringIO()),
            ):
                for outcome, expected in ((subprocess.CompletedProcess([tool], 1), 0), (OSError("launch failed"), 2)):
                    with self.subTest(outcome=outcome):
                        process.side_effect = [outcome]
                        process.reset_mock()
                        self.assertEqual(source_lint.check_workflows((workflow,), report_only=True), expected)
                        process.assert_called_once_with(
                            [tool, "-no-color", workflow], cwd=root, stdout=None, stderr=None
                        )


if __name__ == "__main__":
    unittest.main()
