"""Tests for the Python hygiene check runner."""

import contextlib
import io
import sys
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

from ao.core import pythoncheck


class PythonCheckRunnerTest(unittest.TestCase):
    def setUp(self):
        self.process_python_state = (sys.platform, sys.executable, sys.prefix)
        stack = contextlib.ExitStack()
        self.addCleanup(stack.close)
        # Replace module references, not process-wide os/sys attributes used by other code.
        stack.enter_context(mock.patch.object(pythoncheck, "os", SimpleNamespace(name="posix")))
        stack.enter_context(
            mock.patch.object(
                pythoncheck, "sys", SimpleNamespace(platform="linux", executable="managed-python", prefix=sys.prefix)
            )
        )

    def test_explicit_paths_are_passed_to_both_tools(self):
        self.assertEqual(
            pythoncheck.checks(["script/ao/core/pythoncheck.py"]),
            (
                pythoncheck.Check(
                    "Ruff",
                    ("managed-python", "-m", "ruff", "check", "script/ao/core/pythoncheck.py"),
                ),
                pythoncheck.Check("mypy", ("managed-python", "-m", "mypy", "script/ao/core/pythoncheck.py")),
            ),
        )

    def test_mypy_skips_tooling_tests_that_are_excluded_by_project_config(self):
        self.assertEqual(
            pythoncheck.checks(["test/script/test_pythoncheck.py"]),
            (
                pythoncheck.Check(
                    "Ruff",
                    ("managed-python", "-m", "ruff", "check", "test/script/test_pythoncheck.py"),
                ),
            ),
        )

    def test_runs_every_check_and_returns_failure_when_any_tool_fails(self):
        for exits in ([2, 0], [0, 2]):
            with self.subTest(exits=exits), mock.patch.object(pythoncheck, "run", side_effect=exits) as run:
                with contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(pythoncheck.run_paths(["script/ao"]), 1)

                self.assertEqual(
                    [call.args[0] for call in run.call_args_list],
                    [
                        ["managed-python", "-m", "ruff", "check", "script/ao"],
                        ["managed-python", "-m", "mypy", "script/ao"],
                    ],
                )

    def test_explicit_managed_interpreter_runs_the_tools_and_owns_their_caches(self):
        environment = Path("managed tool env").absolute()
        for platform, python in (
            ("win32", environment / "Scripts" / "python.exe"),
            ("darwin", environment / "bin" / "python"),
        ):
            with (
                self.subTest(platform=platform),
                mock.patch.object(pythoncheck.sys, "platform", platform),
                mock.patch.object(pythoncheck, "run", return_value=0) as run,
            ):
                with contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(pythoncheck.run_paths(["script/a.py"], python=python), 0)
                self.assertEqual(
                    [call.args[0] for call in run.call_args_list],
                    [
                        [
                            str(python),
                            "-I",
                            "-m",
                            "ruff",
                            "check",
                            "--cache-dir",
                            str(environment / ".ruff_cache"),
                            "script/a.py",
                        ],
                        [
                            str(python),
                            "-I",
                            "-m",
                            "mypy",
                            "--cache-dir",
                            str(environment / ".mypy_cache"),
                            "script/a.py",
                        ],
                    ],
                )
                self.assertEqual(
                    pythoncheck.ruff_command("format", "--check", "script/a.py", python=python),
                    (
                        str(python),
                        "-I",
                        "-m",
                        "ruff",
                        "format",
                        "--cache-dir",
                        str(environment / ".ruff_cache"),
                        "--check",
                        "script/a.py",
                    ),
                )

    def test_sink_captures_tool_diagnostics(self):
        completed = pythoncheck.subprocess.CompletedProcess([], 1, stdout="foo.py:1:1: F401 unused import\n")
        sink = io.StringIO()

        with mock.patch.object(pythoncheck.subprocess, "run", return_value=completed):
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(pythoncheck.run_paths(["script/ao/__main__.py"], sink=sink), 1)

        self.assertIn("F401 unused import", sink.getvalue())


if __name__ == "__main__":
    unittest.main()
