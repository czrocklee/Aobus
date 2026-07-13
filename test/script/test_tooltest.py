"""Tests for the concise tooling test runner."""

import contextlib
import io
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao.core import tooltest


class ToolTestRunnerTest(unittest.TestCase):
    def _run(self, completed, *, static_status=0, log=None):
        with mock.patch.object(tooltest.pythoncheck, "run_paths", return_value=static_status) as static:
            with mock.patch.object(tooltest.doccheck, "check_tree", return_value=[]) as docs:
                with mock.patch.object(tooltest.subprocess, "run", return_value=completed):
                    output = io.StringIO()
                    with contextlib.redirect_stdout(output):
                        status = tooltest.run(log=log)
        static.assert_called_once_with([], log=log)
        docs.assert_called_once_with()
        return status, output.getvalue()

    def test_success_prints_only_a_concise_summary(self):
        completed = tooltest.subprocess.CompletedProcess(
            [],
            0,
            stdout="expected argparse noise\nRan 61 tests in 0.032s\n\nOK\n",
        )

        status, output = self._run(completed)

        self.assertEqual(status, 0)
        self.assertEqual(output, "Tooling tests passed (61 tests).\n")

    def test_failure_prints_the_captured_test_output(self):
        completed = tooltest.subprocess.CompletedProcess([], 1, stdout="FAILED: test_example\n")

        status, output = self._run(completed)

        self.assertEqual(status, 1)
        self.assertEqual(output, "Tooling tests failed.\nFAILED: test_example\n")

    def test_static_check_failure_fails_the_suite(self):
        completed = tooltest.subprocess.CompletedProcess([], 0, stdout="Ran 1 test in 0.001s\n\nOK\n")

        status, _ = self._run(completed, static_status=1)

        self.assertEqual(status, 1)

    def test_unit_test_failure_takes_precedence_over_static_status(self):
        completed = tooltest.subprocess.CompletedProcess([], 2, stdout="FAILED: test_example\n")

        status, _ = self._run(completed, static_status=1)

        self.assertEqual(status, 2)

    def test_documentation_failure_fails_the_suite_and_prints_issues(self):
        completed = tooltest.subprocess.CompletedProcess([], 0, stdout="Ran 1 test in 0.001s\n\nOK\n")
        issue = tooltest.doccheck.Issue(Path("doc/spec/example.md"), 7, "broken-link", "missing target")

        with mock.patch.object(tooltest.pythoncheck, "run_paths", return_value=0):
            with mock.patch.object(tooltest.doccheck, "check_tree", return_value=[issue]):
                with mock.patch.object(tooltest.subprocess, "run", return_value=completed):
                    output = io.StringIO()
                    with contextlib.redirect_stdout(output):
                        status = tooltest.run()

        self.assertEqual(status, 1)
        self.assertIn("Documentation checks failed.", output.getvalue())
        self.assertIn("broken-link: missing target", output.getvalue())

    def test_captured_output_is_appended_to_the_gate_log(self):
        completed = tooltest.subprocess.CompletedProcess([], 0, stdout="Ran 2 tests in 0.001s\n\nOK\n")

        with tempfile.TemporaryDirectory() as temp_dir:
            log = Path(temp_dir) / "build.log"
            status, _ = self._run(completed, log=log)

            self.assertEqual(status, 0)
            self.assertEqual(log.read_text(encoding="utf-8"), completed.stdout)


if __name__ == "__main__":
    unittest.main()
