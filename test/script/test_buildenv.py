"""Tests for ao.core.buildenv — per-command native build environment needs."""

import argparse
import contextlib
import io
import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao.__main__ import make_parser
from ao.command import COMMAND_MODULES
from ao.core import builddir, buildenv

ROOT = Path(__file__).resolve().parents[2]


class CommandRegistryTest(unittest.TestCase):
    def test_registered_subparsers_match_declared_command_metadata(self):
        names = [module.NAME for module in COMMAND_MODULES]
        self.assertEqual(sorted(names), sorted(set(names)))
        for module in COMMAND_MODULES:
            self.assertIsInstance(module.NAME, str, module)
            self.assertIsInstance(module.REQUIRES_BUILD_ENV, bool, module)
        parser = make_parser()
        subparsers = next(action for action in parser._actions if isinstance(action, argparse._SubParsersAction))
        self.assertEqual(set(subparsers.choices), set(names))


class BuildEnvSelectionTest(unittest.TestCase):
    def test_native_toolchain_commands_require_the_build_environment(self):
        for command in ("analyze", "build", "check", "deps", "hygiene", "perf", "run", "test", "tidy"):
            with self.subTest(command=command):
                arguments = _choice_backed_arguments(command)
                self.assertTrue(buildenv.requires_build_env(command, arguments), command)

    def test_sdk_free_commands_do_not_require_the_build_environment(self):
        for command in ("doctor", "docs", "name-audit", "setup", "test-audit"):
            with self.subTest(command=command):
                arguments = _choice_backed_arguments(command)
                self.assertFalse(buildenv.requires_build_env(command, arguments), command)

    def test_requires_build_env_answers_by_command_name(self):
        self.assertTrue(buildenv.requires_build_env("build"))
        self.assertFalse(buildenv.requires_build_env("no-such-command"))
        self.assertFalse(buildenv.requires_build_env(""))
        self.assertFalse(buildenv.requires_build_env("no-such-command", ["--tidy-arg=-fix"]))
        self.assertFalse(buildenv.requires_build_env("", ["--tidy-arg=-fix"]))

    def test_help_flags_need_neither_environment(self):
        # One command that otherwise requires the environment proves the
        # generic _help_requested early return for both policies.
        self.assertFalse(buildenv.requires_build_env("check", ["--help"]))
        self.assertFalse(buildenv.requires_python_tools("check", ["--help"]))
        # Help inside application argv still requires the environment.
        self.assertTrue(buildenv.requires_build_env("run", ["cli", "--", "--help"]))

    def test_run_no_build_still_admits_the_native_host_and_sdk(self):
        self.assertTrue(buildenv.requires_build_env("run", ["cli", "-n"]))
        self.assertTrue(buildenv.requires_build_env("run", ["--no-build", "cli"]))
        self.assertTrue(buildenv.requires_build_env("run", ["cli"]))
        self.assertTrue(buildenv.requires_build_env("run", ["cli", "--", "-n"]))

    def test_linux_coverage_requires_native_admission_even_for_analysis_only(self):
        with mock.patch("ao.core.bazel_native.native_platform", return_value="linux"):
            self.assertTrue(buildenv.requires_build_env("coverage"))
            self.assertTrue(buildenv.requires_build_env("coverage", ["--analysis-only"]))

    def test_only_macos_cpp_formatting_prepares_ready_native_tools(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "example.cpp").touch()
            (root / "example.py").touch()
            with (
                mock.patch("ao.command.format.PROJECT_ROOT", root),
                mock.patch.object(buildenv, "PROJECT_ROOT", root),
                mock.patch.dict(os.environ, {"AOBUS_PREFLIGHT_SCOPE": ""}),
            ):
                for profile in (builddir.MACOS_PROFILE, builddir.WINDOWS_PROFILE, builddir.LINUX_PROFILE):
                    for name in ("example.cpp", "example.py"):
                        with (
                            self.subTest(platform=profile.name, source=name),
                            mock.patch.object(builddir, "platform_profile", return_value=profile),
                        ):
                            arguments = ["--check", name]
                            self.assertEqual(
                                buildenv.requires_build_env("format", arguments),
                                profile.name == "macos" and name.endswith(".cpp"),
                            )
                            self.assertFalse(buildenv.requires_provider_tools("format", arguments))

    def test_tooling_selection_does_not_need_the_cpp_toolchain(self):
        for arguments in (["--tooling"], ["--suite", "tooling"], ["--suite=tooling"]):
            self.assertFalse(buildenv.requires_build_env("test", arguments), arguments)
        self.assertTrue(buildenv.requires_build_env("test", ["--all"]))

    def test_known_commands_parse_before_the_static_or_dynamic_answer(self):
        # Missing required arguments are parser errors for static commands too.
        # Valid arguments come from each parser's own choices, not a second command list.
        for command in ("deps", "docs", "doctor", "run", "setup"):
            with self.subTest(command=command, arguments=()):
                with mock.patch("sys.stderr"), self.assertRaises(SystemExit) as failure:
                    buildenv.requires_build_env(command)
                self.assertEqual(failure.exception.code, 2)

        with (
            mock.patch.dict(os.environ, {"AOBUS_PREFLIGHT_SCOPE": ""}),
            contextlib.redirect_stderr(io.StringIO()),
        ):
            for module in COMMAND_MODULES:
                arguments = _choice_backed_arguments(module.NAME)
                with self.subTest(command=module.NAME, arguments=arguments), contextlib.ExitStack() as stack:
                    # Coverage admits only Linux; keep its expected and actual policy
                    # evaluations inside the same supported-host boundary.
                    if module.NAME == "coverage":
                        stack.enter_context(mock.patch("ao.core.bazel_native.native_platform", return_value="linux"))
                    parsed = buildenv.parse_command_arguments(module.NAME, arguments)
                    policy = getattr(module, "requires_build_environment", None)
                    expected = bool(policy(parsed) if policy else module.REQUIRES_BUILD_ENV)
                    self.assertEqual(buildenv.requires_build_env(module.NAME, arguments), expected)


class PythonToolsSelectionTest(unittest.TestCase):
    def test_python_tool_commands_need_the_managed_environment(self):
        for command in ("docs", "format", "hygiene", "tidy"):
            with self.subTest(command=command):
                arguments = _choice_backed_arguments(command)
                self.assertTrue(buildenv.requires_python_tools(command, arguments), command)

    def test_test_tooling_is_a_parsed_capability_not_a_first_argv_spelling(self):
        # REQUIRES_PYTHON_TOOLS is callable: only a tooling suite selection
        # needs the managed Python test dependencies.
        self.assertTrue(buildenv.requires_python_tools("test", ["--tooling"]))
        self.assertTrue(buildenv.requires_python_tools("test", ["--suite", "tooling"]))
        self.assertFalse(buildenv.requires_python_tools("test", ["--core"]))
        for profile in (builddir.LINUX_PROFILE, builddir.WINDOWS_PROFILE, builddir.MACOS_PROFILE):
            with self.subTest(platform=profile.name):
                with mock.patch.object(builddir, "platform_profile", return_value=profile):
                    self.assertEqual(buildenv.requires_python_tools("check"), profile.name != "macos")

    def test_windows_bootstrap_follows_the_parsed_module_capability(self):
        for arguments, expected in (
            ([], False),
            (["help"], False),
            (["--help"], False),
            (["build"], False),
            (["deps", "provision"], False),  # provider tools, not Python tools
            (["check"], True),
            (["docs", "check"], True),
            (["test", "--tooling"], True),
            (["test", "--core"], False),
            (["hygiene"], True),
        ):
            with self.subTest(arguments=arguments):
                self.assertEqual(buildenv.windows_bootstrap_requires_python_tools(arguments), expected)

    def test_windows_bootstrap_invalid_arguments_fail_before_environment_selection(self):
        for arguments in (["tidy", "--bogus"], ["test", "--bogus"], ["no-such-command", "--tooling"]):
            with self.subTest(arguments=arguments):
                with mock.patch("sys.stderr"), self.assertRaises(SystemExit) as failure:
                    buildenv.windows_bootstrap_requires_python_tools(arguments)
                self.assertEqual(failure.exception.code, 2)

    def test_provider_tools_intent_is_explicit_dependency_provisioning_only(self):
        self.assertTrue(buildenv.requires_provider_tools("deps", ["provision"]))
        self.assertFalse(buildenv.requires_provider_tools("deps", ["report"]))
        self.assertFalse(buildenv.requires_provider_tools("build", []))
        with mock.patch("sys.stderr"), self.assertRaises(SystemExit) as failure:
            buildenv.requires_provider_tools("deps", [])
        self.assertEqual(failure.exception.code, 2)
        self.assertFalse(buildenv.requires_provider_tools("docs", ["check"]))
        self.assertFalse(buildenv.requires_provider_tools("no-such-command", ["provision"]))


class BuildEnvCliTest(unittest.TestCase):
    def test_main_prints_a_batch_consumable_flag(self):
        import io

        for arguments, expected in (
            (["check"], "1"),
            (["name-audit"], "0"),
            (["run", "cli", "-n"], "1"),
            (["--python-tools", "hygiene"], "1"),
            (["--python-tools", "check"], "1"),
            (["--provider-tools", "deps", "report"], "0"),
            (["--provider-tools", "deps", "provision"], "1"),
            ([], "0"),
        ):
            with self.subTest(arguments=arguments), contextlib.redirect_stdout(io.StringIO()) as stdout:
                status = buildenv.main(arguments)
            self.assertEqual(status, 0)
            self.assertEqual(stdout.getvalue().strip(), expected)

    def test_exit_code_mode_avoids_command_substitution_output(self):
        import io

        with contextlib.redirect_stdout(io.StringIO()) as stdout:
            required_status = buildenv.main(["--exit-code", "check"])
            skipped_status = buildenv.main(["--exit-code", "test", "--tooling"])

        self.assertEqual(required_status, buildenv.BUILD_ENV_REQUIRED_EXIT_CODE)
        self.assertEqual(skipped_status, 0)
        self.assertEqual(stdout.getvalue(), "")


class SourceScopeHandoffTest(unittest.TestCase):
    def test_scope_handoff_writes_one_scan_and_consumes_it_without_rescanning(self):
        with tempfile.TemporaryDirectory() as temporary:
            destination = str(Path(temporary) / "scope.json")
            selected = ["lib/Foo.cpp"]
            args = buildenv.parse_command_arguments("tidy", [])
            with mock.patch.dict(os.environ, {"AOBUS_PREFLIGHT_SCOPE": destination}):
                with mock.patch.object(Path, "is_file", return_value=True):
                    self.assertTrue(buildenv.requires_source_build_env(args, selected))
            consumed = buildenv.parse_command_arguments("tidy", [])
            buildenv.load_source_scope(consumed, destination)
            self.assertEqual(consumed._resolved_sources, selected)
            # A different invocation cannot consume the prepared scope.
            different = buildenv.parse_command_arguments("tidy", ["--all"])
            with mock.patch("sys.stderr"), self.assertRaises(SystemExit):
                buildenv.load_source_scope(different, destination)

    def test_load_scope_ignores_missing_or_empty_scope_files(self):
        args = buildenv.parse_command_arguments("tidy", [])
        buildenv.load_source_scope(args, None)
        buildenv.load_source_scope(args, str(Path("/nonexistent/scope.json")))
        self.assertFalse(hasattr(args, "_resolved_sources"))

    def test_requires_source_build_env_selects_only_real_cpp_files(self):
        args = buildenv.parse_command_arguments("tidy", [])
        with mock.patch.dict(os.environ, {}, clear=False):
            os.environ.pop("AOBUS_PREFLIGHT_SCOPE", None)
            with mock.patch.object(Path, "is_file", return_value=False):
                self.assertFalse(buildenv.requires_source_build_env(args, ["lib/Absent.cpp"]))
            # A real tracked source is a native scope; a Python file is not.
            self.assertTrue(buildenv.requires_source_build_env(args, ["lib/utility/Path.cpp"]))
            self.assertFalse(buildenv.requires_source_build_env(args, ["script/ao/core/proc.py"]))


def _choice_backed_arguments(command: str) -> list[str]:
    """Minimal required arguments taken from the command parser's own choices."""
    # `ao run` admits any first-party executable label, so its positional has no
    # closed choices; one explicit valid app fixture drives the same parse.
    if command == "run":
        return ["cli"]
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
    # Scope-owning commands must not scan this checkout's actual Git metadata.
    if command in ("analyze", "format", "hygiene", "tidy"):
        arguments.append("lib/utility/Path.cpp")
    return arguments


if __name__ == "__main__":
    unittest.main()
