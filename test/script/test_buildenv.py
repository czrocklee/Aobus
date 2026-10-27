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
from ao.core import builddir, buildenv, compiler_cache, gitfiles, tidyengine, workspace_cache


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
        self.assertFalse(buildenv.requires_build_env("no-such-command", ["--tidy-arg=-fix"]))
        self.assertFalse(buildenv.requires_build_env("", ["--tidy-arg=-fix"]))

    def test_run_no_build_skips_the_native_build_environment(self):
        self.assertFalse(buildenv.requires_build_env("run", ["cli", "-n"]))
        self.assertFalse(buildenv.requires_build_env("run", ["--no-build", "cli"]))
        self.assertTrue(buildenv.requires_build_env("run", ["cli"]))
        self.assertTrue(buildenv.requires_build_env("run", ["cli", "--", "-n"]))

    def test_test_no_build_skips_the_native_build_environment(self):
        self.assertFalse(buildenv.requires_build_env("test", ["--core", "-n"]))
        with mock.patch.object(builddir, "platform_profile", return_value=builddir.platform_profile("linux")):
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
        env = {**os.environ, "PYTHONPATH": str(root / "script"), "PYTHONDONTWRITEBYTECODE": "1"}
        for arguments in (
            ["test", "--bogus"],
            ["tidy", "--jobs", "invalid"],
            ["hygiene", "--folder"],
            ["analyze", "--tidy-arg=-fix"],
            ["tidy", "--tidy-arg=-export-fixes=fixes.yaml"],
            ["deps"],
            ["docs"],
            ["doctor"],
            ["run"],
            ["setup"],
        ):
            expected = subprocess.run(
                [sys.executable, "-m", "ao", *arguments],
                env=env,
                capture_output=True,
                text=True,
                timeout=30,
            )
            for mode in ([], ["--python-tools"], ["--exit-code"]):
                actual = subprocess.run(
                    [sys.executable, "-m", "ao.core.buildenv", *mode, *arguments],
                    env=env,
                    capture_output=True,
                    text=True,
                    timeout=30,
                )
                self.assertEqual(actual.returncode, 2, actual.stderr)
                self.assertEqual(actual.stderr, expected.stderr)

    def test_tidy_probe_includes_sources_outside_format_folders(self):
        with (
            mock.patch.object(gitfiles, "changed_files", return_value=["extras/probe.cpp"]),
            mock.patch.object(gitfiles, "diff_base", return_value="HEAD"),
        ):
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
                with (
                    mock.patch.object(gitfiles, "changed_files", return_value=selected),
                    mock.patch.object(gitfiles, "diff_base", return_value="HEAD"),
                ):
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

    def test_known_commands_parse_before_the_static_or_dynamic_answer(self):
        # Missing required arguments are parser errors for static commands too.
        # Valid arguments come from each parser's own choices, not a second command list.
        for command in ("deps", "docs", "doctor", "run", "setup"):
            with self.subTest(command=command, arguments=()):
                with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as failure:
                    buildenv.requires_build_env(command)
                self.assertEqual(failure.exception.code, 2)

        with (
            mock.patch.dict(os.environ, {"AOBUS_PREFLIGHT_SCOPE": ""}),
            mock.patch.object(gitfiles, "changed_files", return_value=[]),
            mock.patch.object(gitfiles, "diff_base", return_value="HEAD"),
            mock.patch.object(gitfiles, "find_sources", return_value=[]),
            contextlib.redirect_stderr(io.StringIO()),
        ):
            for module in COMMAND_MODULES:
                arguments = _choice_backed_arguments(module.NAME)
                parsed = buildenv.parse_command_arguments(module.NAME, arguments)
                policy = getattr(module, "requires_build_environment", None)
                expected = bool(policy(parsed) if policy else module.REQUIRES_BUILD_ENV)
                with self.subTest(command=module.NAME, arguments=arguments):
                    self.assertEqual(buildenv.requires_build_env(module.NAME, arguments), expected)

    def test_invalid_tidy_arg_preflight_exits_before_scope_or_native_env_selection(self):
        # python -m ao.core.buildenv rejects before scope write or native env selection.
        # Nix or the Windows Python bootstrap may already have run; this is the parser boundary.
        calls: list[str] = []

        def record(name: str, real):
            def wrapped(*args, **kwargs):
                calls.append(name)
                return real(*args, **kwargs)

            return wrapped

        with tempfile.TemporaryDirectory() as temporary:
            scope = Path(temporary) / "scope.json"
            source = Path(temporary) / "Selected.cpp"
            source.write_text("int original;\n", encoding="utf-8")
            rejected = (
                "--tidy-arg=-fix",
                "--tidy-arg=--export-fixes=/tmp/secret-fixes.yaml",
                "--tidy-arg=@/tmp/secret-responses.rsp",
                "--tidy-arg=---fix",
            )
            resolve_scope = tidyengine.resolve_scope
            activate_local = compiler_cache.activate_local
            command_context = workspace_cache.command_context
            source_build_env = buildenv.requires_source_build_env
            with mock.patch.dict(os.environ, {"AOBUS_PREFLIGHT_SCOPE": str(scope)}):
                for command in ("tidy", "analyze"):
                    for option in rejected:
                        with self.subTest(command=command, option=option, boundary="in-process"):
                            calls.clear()
                            with (
                                mock.patch.object(
                                    tidyengine,
                                    "resolve_scope",
                                    side_effect=record("resolve_scope", resolve_scope),
                                ),
                                mock.patch.object(
                                    compiler_cache,
                                    "activate_local",
                                    side_effect=record("activate_local", activate_local),
                                ),
                                mock.patch.object(
                                    workspace_cache,
                                    "command_context",
                                    side_effect=record("command_context", command_context),
                                ),
                                mock.patch.object(
                                    buildenv,
                                    "requires_source_build_env",
                                    side_effect=record("requires_source_build_env", source_build_env),
                                ),
                                contextlib.redirect_stderr(io.StringIO()),
                            ):
                                with self.assertRaises(SystemExit) as failure:
                                    buildenv.requires_build_env(command, [option, str(source)])
                            self.assertEqual(failure.exception.code, 2)
                            self.assertEqual(calls, [])
                            self.assertFalse(scope.exists())

            root = Path(__file__).resolve().parents[2]
            env = {
                **os.environ,
                "PYTHONPATH": str(root / "script"),
                "PYTHONDONTWRITEBYTECODE": "1",
                "AOBUS_PREFLIGHT_SCOPE": str(scope),
                "AOBUS_PHASE_LOG_DIR": "",
            }
            for command in ("tidy", "analyze"):
                portal = subprocess.run(
                    [sys.executable, "-m", "ao", command, "--tidy-arg=-fix", str(source)],
                    env=env,
                    cwd=temporary,
                    capture_output=True,
                    text=True,
                    timeout=30,
                )
                self.assertEqual(portal.returncode, 2, portal.stderr)
                self.assertNotIn("No files specified", portal.stderr)
                for mode in ([], ["--exit-code"], ["--python-tools"]):
                    actual = subprocess.run(
                        [
                            sys.executable,
                            "-m",
                            "ao.core.buildenv",
                            *mode,
                            command,
                            "--tidy-arg=-fix",
                            str(source),
                        ],
                        env=env,
                        cwd=temporary,
                        capture_output=True,
                        text=True,
                        timeout=30,
                    )
                    self.assertEqual(actual.returncode, 2, actual.stderr)
                    self.assertNotEqual(actual.returncode, buildenv.BUILD_ENV_REQUIRED_EXIT_CODE)
                    self.assertEqual(actual.stderr, portal.stderr)
                    self.assertEqual(actual.stdout, "")
                    self.assertFalse(scope.exists())
                    self.assertNotIn("No files specified", actual.stderr)
                extra_options = (
                    "--tidy-arg=@/tmp/secret-responses.rsp",
                    "--tidy-arg=-export-fixes=/tmp/secret-fixes.yaml",
                )
                for option in extra_options:
                    actual = subprocess.run(
                        [sys.executable, "-m", "ao.core.buildenv", "--exit-code", command, option, str(source)],
                        env=env,
                        cwd=temporary,
                        capture_output=True,
                        text=True,
                        timeout=30,
                    )
                    self.assertEqual(actual.returncode, 2, actual.stderr)
                    self.assertFalse(scope.exists())

    def test_accepted_compiler_response_keeps_native_build_env_policy(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "Selected.cpp"
            source.write_text("int value;\n", encoding="utf-8")
            scope = Path(temporary) / "scope.json"
            accepted = ["--tidy-arg=--extra-arg=@compiler.rsp", str(source)]
            with mock.patch.dict(os.environ, {"AOBUS_PREFLIGHT_SCOPE": str(scope)}):
                self.assertTrue(buildenv.requires_build_env("tidy", accepted))
                self.assertTrue(scope.is_file())
                self.assertGreater(scope.stat().st_size, 0)
                scope.unlink()
                self.assertFalse(buildenv.requires_build_env("tidy", ["--no-build", *accepted]))
                self.assertFalse(scope.exists())
                self.assertTrue(buildenv.requires_build_env("analyze", accepted))
                self.assertFalse(scope.exists())
            for profile in (builddir.LINUX_PROFILE, builddir.WINDOWS_PROFILE):
                with self.subTest(profile=profile.name):
                    with mock.patch.object(builddir, "platform_profile", return_value=profile):
                        self.assertFalse(
                            buildenv.requires_build_env(
                                "tidy", ["--no-build", "--tidy-arg=--extra-arg=@compiler.rsp", str(source)]
                            )
                        )
                        self.assertTrue(
                            buildenv.requires_build_env(
                                "analyze", ["--tidy-arg=--extra-arg=@compiler.rsp", str(source)]
                            )
                        )

            root = Path(__file__).resolve().parents[2]
            env = {
                **os.environ,
                "PYTHONPATH": str(root / "script"),
                "PYTHONDONTWRITEBYTECODE": "1",
                "AOBUS_PREFLIGHT_SCOPE": str(scope),
                "AOBUS_PHASE_LOG_DIR": "",
            }
            required = subprocess.run(
                [sys.executable, "-m", "ao.core.buildenv", "--exit-code", "tidy", *accepted],
                env=env,
                cwd=temporary,
                capture_output=True,
                text=True,
                timeout=30,
            )
            self.assertEqual(required.returncode, buildenv.BUILD_ENV_REQUIRED_EXIT_CODE, required.stderr)
            self.assertTrue(scope.is_file())
            scope.unlink()
            skipped = subprocess.run(
                [sys.executable, "-m", "ao.core.buildenv", "--exit-code", "tidy", "--no-build", *accepted],
                env=env,
                cwd=temporary,
                capture_output=True,
                text=True,
                timeout=30,
            )
            self.assertEqual(skipped.returncode, 0, skipped.stderr)
            self.assertFalse(scope.exists())
            analyzed = subprocess.run(
                [sys.executable, "-m", "ao.core.buildenv", "--exit-code", "analyze", *accepted],
                env=env,
                cwd=temporary,
                capture_output=True,
                text=True,
                timeout=30,
            )
            self.assertEqual(analyzed.returncode, buildenv.BUILD_ENV_REQUIRED_EXIT_CODE, analyzed.stderr)
            self.assertFalse(scope.exists())


def _choice_backed_arguments(command: str) -> list[str]:
    """Minimal required arguments taken from the command parser's own choices."""
    parser = make_parser()
    subparsers = next(action for action in parser._actions if isinstance(action, argparse._SubParsersAction))
    command_parser = subparsers.choices[command]
    arguments: list[str] = []
    for action in command_parser._actions:
        if action.option_strings or action.required is not True:
            continue
        if not action.choices:
            raise AssertionError(f"{command} required argument {action.dest} has no choices")
        arguments.append(next(iter(action.choices)))
    return arguments


if __name__ == "__main__":
    unittest.main()
