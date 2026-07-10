"""Tests for the Python hygiene check runner."""

import contextlib
import io
import unittest
from unittest import mock

from ao.core import pythoncheck


class PythonCheckRunnerTest(unittest.TestCase):
    def test_windows_modules_ignore_ambient_python_packages(self):
        self.assertEqual(
            pythoncheck.module_command("ruff", "--version", os_name="nt"),
            (pythoncheck.sys.executable, "-I", "-m", "ruff", "--version"),
        )

    def test_default_checks_cover_python_tooling_targets(self):
        self.assertEqual(
            pythoncheck.checks([]),
            (
                pythoncheck.Check(
                    "Ruff",
                    pythoncheck.module_command(
                        "ruff", "check", "script/ao", "app/cli/generate_test_library.py", "test/script"
                    ),
                ),
                pythoncheck.Check(
                    "mypy",
                    pythoncheck.module_command("mypy", "script/ao", "app/cli/generate_test_library.py"),
                ),
            ),
        )

    def test_explicit_paths_are_passed_to_both_tools(self):
        self.assertEqual(
            pythoncheck.checks(["script/ao/core/pythoncheck.py"]),
            (
                pythoncheck.Check(
                    "Ruff",
                    pythoncheck.module_command("ruff", "check", "script/ao/core/pythoncheck.py"),
                ),
                pythoncheck.Check("mypy", pythoncheck.module_command("mypy", "script/ao/core/pythoncheck.py")),
            ),
        )

    def test_mypy_skips_tooling_tests_that_are_excluded_by_project_config(self):
        self.assertEqual(
            pythoncheck.checks(["test/script/test_pythoncheck.py"]),
            (
                pythoncheck.Check(
                    "Ruff",
                    pythoncheck.module_command("ruff", "check", "test/script/test_pythoncheck.py"),
                ),
            ),
        )

    def test_runs_every_check_and_returns_failure_when_any_tool_fails(self):
        with mock.patch.object(pythoncheck, "run", side_effect=[1, 0]) as run:
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(pythoncheck.run_paths([]), 1)

        self.assertEqual(
            [call.args[0] for call in run.call_args_list],
            [list(check.argv) for check in pythoncheck.checks([])],
        )

    def test_log_is_forwarded_to_the_process_runner(self):
        with mock.patch.object(pythoncheck, "run", return_value=0) as run:
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(pythoncheck.run_paths([], log=pythoncheck.PROJECT_ROOT / "build.log"), 0)

        for call in run.call_args_list:
            self.assertEqual(call.kwargs["log"], pythoncheck.PROJECT_ROOT / "build.log")
            self.assertTrue(call.kwargs["append"])

    def test_sink_captures_tool_diagnostics(self):
        completed = pythoncheck.subprocess.CompletedProcess([], 1, stdout="foo.py:1:1: F401 unused import\n")
        sink = io.StringIO()

        with mock.patch.object(pythoncheck.subprocess, "run", return_value=completed):
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(pythoncheck.run_paths(["script/ao/__main__.py"], sink=sink), 1)

        self.assertIn("F401 unused import", sink.getvalue())


if __name__ == "__main__":
    unittest.main()
