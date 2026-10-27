"""Tests for the Python hygiene check runner."""

import contextlib
import io
import os
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

    def test_native_command_fixtures_do_not_change_process_python_state(self):
        with (
            mock.patch.object(pythoncheck.sys, "platform", "darwin"),
            mock.patch.object(pythoncheck.sys, "prefix", "managed prefix"),
        ):
            self.assertEqual(pythoncheck.mypy_command("script/ao")[0], "managed-python")
            self.assertEqual((sys.platform, sys.executable, sys.prefix), self.process_python_state)

    def test_windows_modules_ignore_ambient_python_packages(self):
        self.assertEqual(
            pythoncheck.module_command("ruff", "--version", os_name="nt"),
            (pythoncheck.sys.executable, "-I", "-m", "ruff", "--version"),
        )

    def test_macos_modules_ignore_ambient_python_packages(self):
        self.assertEqual(
            pythoncheck.module_command("ruff", "--version", os_name="posix", platform_name="darwin"),
            (pythoncheck.sys.executable, "-I", "-m", "ruff", "--version"),
        )

    def test_linux_modules_keep_the_nix_python_package_environment(self):
        self.assertEqual(
            pythoncheck.module_command("ruff", "--version", os_name="posix", platform_name="linux"),
            (pythoncheck.sys.executable, "-m", "ruff", "--version"),
        )

    def test_native_mypy_cache_stays_in_the_managed_environment(self):
        # Host-path argv coverage only; native filesystem behavior needs native process probes.
        prefix = str(Path("managed tool env").absolute())
        for platform in ("win32", "darwin"):
            with self.subTest(platform=platform), mock.patch.object(pythoncheck.sys, "prefix", prefix):
                self.assertEqual(
                    pythoncheck.mypy_command("script/a file.py", platform_name=platform),
                    (
                        "managed-python",
                        "-I",
                        "-m",
                        "mypy",
                        "--cache-dir",
                        prefix + os.sep + ".mypy_cache",
                        "script/a file.py",
                    ),
                )

    def test_native_ruff_check_and_format_share_the_managed_cache(self):
        prefix = str(Path("managed tool env").absolute())
        for platform in ("win32", "darwin"):
            for subcommand, mode in (("check", ()), ("format", ()), ("format", ("--check",))):
                with (
                    self.subTest(platform=platform, subcommand=subcommand, mode=mode),
                    mock.patch.object(pythoncheck.sys, "prefix", prefix),
                ):
                    self.assertEqual(
                        pythoncheck.ruff_command(subcommand, *mode, "script/a file.py", platform_name=platform),
                        (
                            "managed-python",
                            "-I",
                            "-m",
                            "ruff",
                            subcommand,
                            "--cache-dir",
                            prefix + os.sep + ".ruff_cache",
                            *mode,
                            "script/a file.py",
                        ),
                    )

    def test_linux_tool_commands_keep_nix_module_and_cache_defaults(self):
        self.assertEqual(
            pythoncheck.mypy_command("script/ao"),
            ("managed-python", "-m", "mypy", "script/ao"),
        )
        for subcommand, mode in (("check", ()), ("format", ()), ("format", ("--check",))):
            with self.subTest(subcommand=subcommand, mode=mode):
                self.assertEqual(
                    pythoncheck.ruff_command(subcommand, *mode, "script/ao"),
                    ("managed-python", "-m", "ruff", subcommand, *mode, "script/ao"),
                )

    def test_explicit_linux_profile_keeps_defaults_under_a_native_host_fixture(self):
        for platform in ("win32", "darwin"):
            with self.subTest(platform=platform), mock.patch.object(pythoncheck.sys, "platform", platform):
                self.assertEqual(
                    pythoncheck.mypy_command("script/ao", platform_name="linux"),
                    ("managed-python", "-m", "mypy", "script/ao"),
                )
                self.assertEqual(
                    pythoncheck.ruff_command("check", "script/ao", platform_name="linux"),
                    ("managed-python", "-m", "ruff", "check", "script/ao"),
                )

    def test_building_native_commands_does_not_mutate_inherited_cache_settings(self):
        inherited = {"RUFF_CACHE_DIR": "inherited-ruff", "MYPY_CACHE_DIR": "inherited-mypy"}
        with mock.patch.dict(os.environ, inherited):
            before = dict(os.environ)
            for platform in ("win32", "darwin"):
                pythoncheck.mypy_command("script/ao", platform_name=platform)
                pythoncheck.ruff_command("check", "script/ao", platform_name=platform)
                pythoncheck.ruff_command("format", "script/ao", platform_name=platform)
            self.assertEqual(dict(os.environ), before)

    def test_native_checks_use_both_managed_cache_commands(self):
        prefix = str(Path("managed tool env").absolute())
        for platform in ("win32", "darwin"):
            with (
                self.subTest(platform=platform),
                mock.patch.object(pythoncheck.sys, "platform", platform),
                mock.patch.object(pythoncheck.sys, "prefix", prefix),
            ):
                self.assertEqual(
                    pythoncheck.checks(["script/a file.py"]),
                    (
                        pythoncheck.Check(
                            "Ruff",
                            (
                                "managed-python",
                                "-I",
                                "-m",
                                "ruff",
                                "check",
                                "--cache-dir",
                                prefix + os.sep + ".ruff_cache",
                                "script/a file.py",
                            ),
                        ),
                        pythoncheck.Check(
                            "mypy",
                            (
                                "managed-python",
                                "-I",
                                "-m",
                                "mypy",
                                "--cache-dir",
                                prefix + os.sep + ".mypy_cache",
                                "script/a file.py",
                            ),
                        ),
                    ),
                )

    def test_default_checks_cover_python_tooling_targets(self):
        self.assertEqual(
            pythoncheck.checks([]),
            (
                pythoncheck.Check(
                    "Ruff",
                    (
                        "managed-python",
                        "-m",
                        "ruff",
                        "check",
                        "script/ao",
                        "app/cli/generate_test_library.py",
                        "test/script",
                    ),
                ),
                pythoncheck.Check(
                    "mypy",
                    ("managed-python", "-m", "mypy", "script/ao", "app/cli/generate_test_library.py"),
                ),
            ),
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
