"""Tests for ao.core.buildenv — per-command native build environment needs."""

import argparse
import contextlib
import io
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao.__main__ import main, make_parser
from ao.command import COMMAND_MODULES
from ao.core import builddir, buildenv, compiler_cache, gitfiles, tidyengine


class BuildEnvTest(unittest.TestCase):
    def test_every_command_module_declares_its_metadata(self):
        names = [module.NAME for module in COMMAND_MODULES]
        self.assertEqual(sorted(names), sorted(set(names)))
        for module in COMMAND_MODULES:
            self.assertIsInstance(module.NAME, str, module)
            self.assertIsInstance(module.REQUIRES_BUILD_ENV, bool, module)

    def test_registered_subparsers_match_declared_names(self):
        parser = make_parser()
        subparsers = next(action for action in parser._actions if isinstance(action, argparse._SubParsersAction))
        self.assertEqual(set(subparsers.choices), {module.NAME for module in COMMAND_MODULES})

    def test_native_toolchain_commands_require_the_build_environment(self):
        self.assertEqual(
            {module.NAME for module in COMMAND_MODULES if module.REQUIRES_BUILD_ENV},
            {"analyze", "build", "check", "deps", "format", "hygiene", "perf", "run", "test", "tidy"},
        )

    def test_requires_build_env_answers_by_command_name(self):
        self.assertTrue(buildenv.requires_build_env("build"))
        self.assertFalse(buildenv.requires_build_env("no-such-command"))
        self.assertFalse(buildenv.requires_build_env(""))

    def test_run_no_build_skips_the_native_build_environment(self):
        self.assertFalse(buildenv.requires_build_env("run", ["cli", "-n"]))
        self.assertFalse(buildenv.requires_build_env("run", ["--no-build", "cli"]))
        self.assertTrue(buildenv.requires_build_env("run", ["cli"]))
        self.assertTrue(buildenv.requires_build_env("run", ["cli", "--", "-n"]))

    def test_test_no_build_skips_the_native_build_environment(self):
        self.assertFalse(buildenv.requires_build_env("test", ["--core", "-n"]))
        self.assertFalse(buildenv.requires_build_env("test", ["--no-build", "--tooling"]))
        self.assertTrue(buildenv.requires_build_env("test", ["--core"]))
        self.assertTrue(buildenv.requires_build_env("test", ["--", "-n"]))

    def test_tooling_selection_does_not_need_the_cpp_toolchain(self):
        with mock.patch.object(builddir, "platform_profile", return_value=builddir.WINDOWS_PROFILE):
            for arguments in (["--tooling"], ["--suite", "tooling"], ["--suite=tooling"]):
                self.assertFalse(buildenv.requires_build_env("test", arguments))
            self.assertTrue(buildenv.requires_build_env("test", ["--all"]))

    def test_help_needs_neither_cpp_nor_managed_python_tools(self):
        for module in COMMAND_MODULES:
            for flag in ("-h", "--help"):
                self.assertFalse(buildenv.requires_build_env(module.NAME, [flag]), module.NAME)
                self.assertFalse(buildenv.requires_python_tools(module.NAME, [flag]), module.NAME)
        self.assertTrue(buildenv.requires_build_env("run", ["cli", "--", "--help"]))

    def test_python_and_empty_source_scopes_skip_native_cpp_preparation(self):
        from ao.command import format as format_command

        for command in ("format", "hygiene", "tidy"):
            for files, expected in (([], False), (["script/ao/core/proc.py"], False), (["lib/Foo.cpp"], True)):
                with mock.patch.object(format_command, "resolve_files", return_value=files):
                    with mock.patch.object(tidyengine, "resolve_scope", return_value=(files, False)):
                        with mock.patch.object(Path, "is_file", return_value=True):
                            self.assertEqual(buildenv.requires_build_env(command), expected, (command, files))

    def test_invalid_arguments_have_the_real_portal_diagnostic(self):
        root = Path(__file__).resolve().parents[2]
        env = {**os.environ, "PYTHONPATH": str(root / "script")}
        for arguments in (["test", "--bogus"], ["tidy", "--jobs", "invalid"], ["hygiene", "--folder"]):
            expected = subprocess.run([sys.executable, "-m", "ao", *arguments], env=env, capture_output=True, text=True)
            for mode in ([], ["--python-tools"]):
                actual = subprocess.run(
                    [sys.executable, "-m", "ao.core.buildenv", *mode, *arguments],
                    env=env,
                    capture_output=True,
                    text=True,
                )
                self.assertEqual(actual.returncode, 2, actual.stderr)
                self.assertEqual(actual.stderr, expected.stderr)

    def test_tidy_probe_includes_sources_outside_format_folders(self):
        with mock.patch.object(gitfiles, "changed_files", return_value=["extras/probe.cpp"]):
            with mock.patch.object(Path, "is_file", return_value=True):
                self.assertTrue(buildenv.requires_build_env("tidy"))

    def test_native_scope_handoff_avoids_rescanning_and_rejects_a_different_scope(self):
        from ao.command import format as format_command
        from ao.command import tidy

        for command in ("format", "hygiene", "tidy"):
            with self.subTest(command=command), tempfile.TemporaryDirectory() as temporary:
                destination = str(Path(temporary) / "scope.json")
                arguments = []
                selected = ["script/ao/core/proc.py"]
                with mock.patch.object(gitfiles, "changed_files", return_value=selected):
                    with mock.patch.dict(os.environ, {"AOBUS_PREFLIGHT_SCOPE": destination}):
                        self.assertFalse(buildenv.requires_build_env(command, arguments))
                args = buildenv.parse_command_arguments(command, arguments)
                buildenv.load_source_scope(args, destination)
                with mock.patch.object(gitfiles, "changed_files", side_effect=AssertionError("scope scanned again")):
                    if command == "tidy":
                        self.assertEqual(
                            tidyengine.resolve_scope(args, tidy.ALL_FOLDERS, "Checking"), (selected, False)
                        )
                    else:
                        self.assertEqual(format_command.resolve_files(args), selected)
                different = buildenv.parse_command_arguments(command, ["--all"])
                with self.assertRaises(SystemExit):
                    buildenv.load_source_scope(different, destination)

    def test_cache_activation_and_dispatch_share_one_source_scan(self):
        from ao.command import format as format_command
        from ao.command import hygiene, tidy

        scopes = (([], False), (["script/ao/core/proc.py"], False), (["include/ao/async/LoopExecutor.h"], True))
        for command in (format_command, hygiene, tidy):
            for prepared in (False, True):
                for selected, needs_cache in scopes:
                    with self.subTest(command=command.NAME, prepared=prepared, selected=selected):
                        with tempfile.TemporaryDirectory() as temporary, contextlib.ExitStack() as stack:
                            destination = str(Path(temporary) / "scope.json")
                            environment = {**os.environ, "AOBUS_PREFLIGHT_SCOPE": destination}
                            if not prepared:
                                environment.pop("AOBUS_PREFLIGHT_SCOPE")
                            stack.enter_context(mock.patch.dict(os.environ, environment, clear=True))
                            stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
                            stack.enter_context(contextlib.redirect_stderr(io.StringIO()))
                            scan = stack.enter_context(
                                mock.patch.object(gitfiles, "changed_files", side_effect=[selected])
                            )
                            stack.enter_context(mock.patch.object(gitfiles, "diff_base", return_value="HEAD"))
                            activate = stack.enter_context(mock.patch.object(compiler_cache, "activate_local"))
                            observed = []

                            def consume_scope(args, command=command, observed=observed):
                                if command is tidy:
                                    files, _ = tidyengine.resolve_scope(args, tidy.ALL_FOLDERS, "Checking")
                                else:
                                    files = format_command.resolve_files(args)
                                observed.append(files)
                                return 0

                            stack.enter_context(mock.patch.object(command, "run_command", side_effect=consume_scope))
                            if prepared:
                                self.assertEqual(buildenv.requires_build_env(command.NAME), needs_cache)
                            self.assertEqual(main([command.NAME]), 0)

                            scan.assert_called_once()
                            self.assertEqual(observed, [selected])
                            self.assertEqual(activate.call_count, int(needs_cache))

    def test_main_prints_a_batch_consumable_flag(self):
        for arguments, expected in (
            (["check"], "1"),
            (["name-audit"], "0"),
            (["run", "cli", "-n"], "0"),
            (["--python-tools", "hygiene"], "1"),
            (["--python-tools", "check"], "0"),
            ([], "0"),
        ):
            stdout = io.StringIO()
            with contextlib.redirect_stdout(stdout):
                status = buildenv.main(arguments)
            self.assertEqual(status, 0)
            self.assertEqual(stdout.getvalue().strip(), expected)

    def test_exit_code_mode_avoids_command_substitution_output(self):
        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout):
            required_status = buildenv.main(["--exit-code", "check"])
            skipped_status = buildenv.main(["--exit-code", "run", "cli", "-n"])

        self.assertEqual(required_status, buildenv.BUILD_ENV_REQUIRED_EXIT_CODE)
        self.assertEqual(skipped_status, 0)
        self.assertEqual(stdout.getvalue(), "")


if __name__ == "__main__":
    unittest.main()
