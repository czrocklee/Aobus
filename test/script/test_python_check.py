"""Tests for the Python hygiene check runner."""

import contextlib
import io
import sys
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

from ao.portal import python_check


class PythonCheckRunnerTest(unittest.TestCase):
    def setUp(self):
        stack = contextlib.ExitStack()
        self.addCleanup(stack.close)
        # Replace module references, not process-wide os/sys attributes used by other code.
        stack.enter_context(mock.patch.object(python_check, "os", SimpleNamespace(name="posix")))
        stack.enter_context(
            mock.patch.object(
                python_check, "sys", SimpleNamespace(platform="linux", executable="managed-python", prefix=sys.prefix)
            )
        )
        # The governed tool manifest, not the ambient interpreter, owns the tool targets.
        stack.enter_context(
            mock.patch.object(
                python_check,
                "tool_versions",
                lambda config_file=None: {"python": "3.14.10", "ruff": "0.15.0", "mypy": "1.19.0"},
            )
        )
        stack.enter_context(mock.patch.object(python_check, "_require_tool"))

    def test_explicit_paths_are_passed_to_both_tools(self):
        self.assertEqual(
            python_check.checks(["script/ao/portal/python_check.py"]),
            (
                python_check.Check(
                    "Ruff",
                    (
                        "managed-python",
                        "-m",
                        "ruff",
                        "check",
                        "--target-version",
                        "py314",
                        "script/ao/portal/python_check.py",
                    ),
                ),
                python_check.Check(
                    "mypy",
                    ("managed-python", "-m", "mypy", "--python-version", "3.14", "script/ao/portal/python_check.py"),
                ),
            ),
        )

    def test_mypy_skips_tooling_tests_that_are_excluded_by_project_config(self):
        self.assertEqual(
            python_check.checks(["test/script/test_python_check.py"]),
            (
                python_check.Check(
                    "Ruff",
                    (
                        "managed-python",
                        "-m",
                        "ruff",
                        "check",
                        "--target-version",
                        "py314",
                        "test/script/test_python_check.py",
                    ),
                ),
            ),
        )

    def test_runs_every_check_and_retains_findings_vs_tool_failure_severity(self):
        for exits, expected in (([1, 0], 1), ([2, 1], 2), ([1, -9], 2)):
            with self.subTest(exits=exits), mock.patch.object(python_check, "run", side_effect=exits) as run:
                with contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(python_check.run_paths(["script/ao"]), expected)

                self.assertEqual(run.call_count, 2)

    def test_explicit_managed_interpreter_runs_the_tools_and_owns_their_caches(self):
        environment = Path("managed tool env").absolute()
        for platform, python in (
            ("win32", environment / "Scripts" / "python.exe"),
            ("darwin", environment / "bin" / "python"),
        ):
            with (
                self.subTest(platform=platform),
                mock.patch.object(python_check.sys, "platform", platform),
                mock.patch.object(python_check, "run", return_value=0) as run,
            ):
                with contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(python_check.run_paths(["script/a.py"], python=python), 0)
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
                            "--target-version",
                            "py314",
                            "script/a.py",
                        ],
                        [
                            str(python),
                            "-I",
                            "-m",
                            "mypy",
                            "--cache-dir",
                            str(environment / ".mypy_cache"),
                            "--python-version",
                            "3.14",
                            "script/a.py",
                        ],
                    ],
                )
                self.assertEqual(
                    python_check.ruff_command("format", "--check", "script/a.py", python=python),
                    (
                        str(python),
                        "-I",
                        "-m",
                        "ruff",
                        "format",
                        "--cache-dir",
                        str(environment / ".ruff_cache"),
                        "--target-version",
                        "py314",
                        "--check",
                        "script/a.py",
                    ),
                )

    def test_sink_captures_tool_diagnostics(self):
        completed = python_check.subprocess.CompletedProcess([], 1, stdout="foo.py:1:1: F401 unused import\n")
        sink = io.StringIO()

        with mock.patch.object(python_check.subprocess, "run", return_value=completed):
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(python_check.run_paths(["script/ao/__main__.py"], sink=sink), 1)

        self.assertIn("F401 unused import", sink.getvalue())


class PythonToolAdmissionTest(unittest.TestCase):
    def test_missing_or_broken_module_exit_one_is_a_tool_error_not_an_advisory_finding(self):
        for code, stdout, stderr in (
            (1, "", "No module named ruff"),
            (0, "ruff 0.14.0\n", ""),
            (0, "ruff 0.15.0\n", "unexpected startup diagnostic"),
        ):
            with (
                self.subTest(code=code, stdout=stdout, stderr=stderr),
                mock.patch.object(python_check, "tool_versions", return_value={"python": "3.14.10", "ruff": "0.15.0"}),
                mock.patch.object(
                    python_check.subprocess,
                    "run",
                    return_value=python_check.subprocess.CompletedProcess([], code, stdout, stderr),
                ),
                mock.patch.object(python_check, "run", side_effect=AssertionError("no checker before ready admission")),
                contextlib.redirect_stdout(io.StringIO()),
                contextlib.redirect_stderr(io.StringIO()),
            ):
                self.assertEqual(python_check.run_paths(["test/script/test_fixture.py"]), 2)

    def test_version_probe_and_check_use_the_same_selected_interpreter(self):
        python = Path("selected environment/bin/python")
        completed = python_check.subprocess.CompletedProcess
        with (
            mock.patch.object(python_check, "tool_versions", return_value={"python": "3.14.10", "ruff": "0.15.0"}),
            mock.patch.object(
                python_check.subprocess,
                "run",
                side_effect=[
                    completed([], 0, "ruff 0.15.0\n", ""),
                    completed([], 1, "finding\n", ""),
                ],
            ) as invoke,
            contextlib.redirect_stdout(io.StringIO()),
        ):
            self.assertEqual(
                python_check.run_paths(["test/script/test_fixture.py"], python=python, sink=io.StringIO()), 1
            )
        probe, check = (call.args[0] for call in invoke.call_args_list)
        self.assertEqual((probe[0], check[0]), (str(python), str(python)))
        self.assertEqual(probe[-3:], ("-m", "ruff", "--version"))


if __name__ == "__main__":
    unittest.main()
