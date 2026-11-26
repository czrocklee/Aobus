"""Parsed preflight admission and frozen source/provenance handoff."""

import contextlib
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao.__main__ import make_parser, parse_arguments
from ao.command import check, options
from ao.portal import build_dir, build_env, source_scope
from ao.portal.source_scope import SourceScope


class BuildEnvSelectionTest(unittest.TestCase):
    def setUp(self):
        self.enterContext(mock.patch.dict(os.environ, {"AOBUS_PREFLIGHT_SCOPE": ""}))

    def test_native_subjects_require_admission_while_sdk_free_subjects_do_not(self):
        for argv, expected in (
            (["check"], True),
            (["check", "--native-only", "--extend"], True),
            (["check", "contracts"], True),
            (["check", "lint", "lib/utility/Path.cpp"], True),
            (["check", "lint", "script/ao/core/proc.py"], False),
            (["check", "analyzer", "lib/utility/Path.cpp"], True),
            (["check", "docs"], False),
            (["setup", "bazel"], False),
            (["measure", "coverage"], True),
        ):
            with self.subTest(argv=argv), mock.patch("ao.core.bazel_native.native_platform", return_value="linux"):
                self.assertEqual(build_env.requires_build_env(argv[0], argv[1:]), expected)

    def test_unknown_commands_and_help_need_neither_environment(self):
        for command, arguments in (("unknown", ["--bogus"]), ("check", ["--help"])):
            with self.subTest(command=command):
                self.assertFalse(build_env.requires_build_env(command, arguments))
                self.assertFalse(build_env.requires_python_tools(command, arguments))

    def test_only_macos_cpp_formatting_requires_ready_native_tools(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for name in ("example.cpp", "example.py"):
                (root / name).touch()
            with mock.patch.object(source_scope, "PROJECT_ROOT", root):
                for profile, name, expected in (
                    (build_dir.MACOS_PROFILE, "example.cpp", True),
                    (build_dir.MACOS_PROFILE, "example.py", False),
                    (build_dir.WINDOWS_PROFILE, "example.cpp", False),
                    (build_dir.LINUX_PROFILE, "example.cpp", False),
                ):
                    with (
                        self.subTest(profile=profile.name, name=name),
                        mock.patch.object(build_dir, "platform_profile", return_value=profile),
                    ):
                        self.assertEqual(build_env.requires_build_env("format", [name]), expected)
                        self.assertEqual(build_env.requires_build_env("check", ["format", name]), expected)

    def test_macos_docs_format_preflight_without_site_packages(self):
        root = Path(__file__).resolve().parents[2]
        code = (
            "from unittest.mock import patch\n"
            "from ao.portal import build_dir, build_env\n"
            "with patch.object(build_dir, 'platform_profile', return_value=build_dir.MACOS_PROFILE):\n"
            "    print(int(build_env.requires_build_env('check', ['format', '--docs'])))\n"
            "    print(int(build_env.requires_python_tools('check', ['format', '--docs'])))\n"
        )
        result = subprocess.run(
            [sys.executable, "-S", "-c", code],
            cwd=root,
            env={**os.environ, "PYTHONPATH": str(root / "script"), "AOBUS_PREFLIGHT_SCOPE": ""},
            capture_output=True,
            text=True,
            timeout=15,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "0\n0\n")

    def test_invalid_requests_fail_before_environment_selection(self):
        for argv in (["setup"], ["check", "docs", "--fresh"]):
            with (
                self.subTest(argv=argv),
                contextlib.redirect_stderr(io.StringIO()),
                self.assertRaises(SystemExit) as error,
            ):
                build_env.requires_build_env(argv[0], argv[1:])
            self.assertEqual(error.exception.code, 2)

    def test_docs_format_rejects_other_selectors_in_both_preflight_modes(self):
        for selector in (["--all"], ["--commit", "HEAD"], ["README.md"]):
            for policy in (build_env.requires_build_env, build_env.requires_python_tools):
                with (
                    self.subTest(selector=selector, policy=policy.__name__),
                    contextlib.redirect_stderr(io.StringIO()),
                    self.assertRaises(SystemExit) as error,
                ):
                    policy("check", ["format", "--docs", *selector])
                self.assertEqual(error.exception.code, 1)

    def test_python_tools_follow_actual_python_sources_and_documentation_semantics(self):
        for argv, expected in (
            (["check"], False),
            (["check", "--native-only", "--extend"], False),
            (["check", "docs"], True),
            (["check", "format", "--docs"], False),
            (["check", "format", "README.md"], False),
            (["check", "format", "script/ao/core/proc.py"], True),
            (["check", "deps"], False),
            (["format", "script/ao/core/proc.py"], True),
            (["format", "README.md"], False),
            (["check", "lint", "script/ao/core/proc.py"], True),
            (["check", "lint", "lib/utility/Path.cpp"], False),
            (["check", "lint", "--tidy-check", "aobus-no-c-assert", "script/ao/core/proc.py"], False),
        ):
            with self.subTest(argv=argv):
                self.assertEqual(build_env.requires_python_tools(argv[0], argv[1:]), expected)

    def test_provider_tools_are_requested_only_by_explicit_dependency_setup(self):
        self.assertTrue(build_env.requires_provider_tools("setup", ["deps"]))
        self.assertFalse(build_env.requires_provider_tools("check", ["deps"]))
        self.assertFalse(build_env.requires_provider_tools("setup", ["bazel"]))

    def test_batch_output_and_exit_code_modes_share_the_same_decision(self):
        for arguments, expected in (
            (["check", "docs"], False),
            (["--python-tools", "check", "docs"], True),
            (["--python-tools", "check", "format", "--docs"], False),
            (["--provider-tools", "setup", "deps"], True),
        ):
            with self.subTest(arguments=arguments), contextlib.redirect_stdout(io.StringIO()) as output:
                self.assertEqual(build_env.main(arguments), 0)
                self.assertEqual(
                    build_env.main(["--exit-code", *arguments]),
                    build_env.BUILD_ENV_REQUIRED_EXIT_CODE if expected else 0,
                )
            self.assertEqual(output.getvalue(), f"{int(expected)}\n")


class SourceScopeHandoffTest(unittest.TestCase):
    def parse(self, *arguments):
        return parse_arguments(make_parser(), list(arguments))

    def test_json_roundtrip_freezes_files_and_individual_file_provenance_without_rediscovery(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            for name in ("lib/Foo.cpp", "test/lint/fixture/Explicit.cpp"):
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.touch()
            destination = str(root / "scope.json")
            argv = ["check", "lint", "lib", "test/lint/fixture/Explicit.cpp"]
            scope = SourceScope(("lib/Foo.cpp", "test/lint/fixture/Explicit.cpp"), ("test/lint/fixture/Explicit.cpp",))
            with (
                mock.patch.object(source_scope, "PROJECT_ROOT", root),
                mock.patch.object(source_scope.git_files, "PROJECT_ROOT", root),
                mock.patch.dict(os.environ, {"AOBUS_PREFLIGHT_SCOPE": destination}),
            ):
                self.assertTrue(build_env.requires_build_env(argv[0], argv[1:]))
                consumed = self.parse(*argv)
                with mock.patch.object(source_scope, "resolve", side_effect=AssertionError("must not rescan")):
                    self.assertEqual(options.resolved_scope(consumed), scope)
                    self.assertFalse(check.requires_python_tools(consumed))

    def test_docs_format_defers_discovery_and_preserves_prepared_scope(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "README.md").touch()
            destination = str(root / "scope.json")
            args = self.parse("check", "format", "--docs")
            scope = SourceScope(("README.md",))
            with (
                mock.patch.object(source_scope, "PROJECT_ROOT", root),
                mock.patch.dict(os.environ, {"AOBUS_PREFLIGHT_SCOPE": destination}),
                mock.patch.object(check.documentation_checks, "markdown_scope", return_value=scope) as discover,
            ):
                self.assertFalse(check.requires_build_environment(args))
                self.assertFalse(check.requires_python_tools(args))
                discover.assert_not_called()
                self.assertEqual(options.resolved_scope(args), scope)
                self.assertEqual(options.resolved_scope(self.parse("check", "format", "--docs")), scope)
                discover.assert_called_once_with()

    def test_handoff_rejects_another_subject_or_source_selector(self):
        with tempfile.TemporaryDirectory() as temporary:
            destination = str(Path(temporary) / "scope.json")
            with mock.patch.dict(os.environ, {"AOBUS_PREFLIGHT_SCOPE": destination}):
                build_env.record_source_scope(self.parse("check", "lint"), SourceScope(("lib/Foo.cpp",)))
            for argv in (["check", "analyzer"], ["check", "lint", "--all"]):
                with self.subTest(argv=argv), contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                    build_env.load_source_scope(self.parse(*argv), destination)

    def test_handoff_rejects_missing_or_foreign_explicit_provenance(self):
        with tempfile.TemporaryDirectory() as temporary:
            destination = Path(temporary) / "scope.json"
            with mock.patch.dict(os.environ, {"AOBUS_PREFLIGHT_SCOPE": str(destination)}):
                build_env.record_source_scope(self.parse("check", "lint"), SourceScope(("lib/Foo.cpp",)))
            valid = json.loads(destination.read_text(encoding="utf-8"))
            for explicit in (None, ["lib/Foreign.cpp"], [1]):
                record = {**valid, "explicit_files": explicit}
                destination.write_text(json.dumps(record), encoding="utf-8")
                with (
                    self.subTest(explicit=explicit),
                    contextlib.redirect_stderr(io.StringIO()),
                    self.assertRaises(SystemExit),
                ):
                    build_env.load_source_scope(self.parse("check", "lint"), str(destination))

    def test_missing_and_empty_handoff_files_do_not_create_a_prepared_scope(self):
        args = self.parse("check", "lint")
        with tempfile.TemporaryDirectory() as temporary:
            empty = Path(temporary) / "empty.json"
            empty.touch()
            for path in (None, str(empty), str(empty.parent / "missing.json")):
                build_env.load_source_scope(args, path)
        self.assertFalse(hasattr(args, "_resolved_sources"))
        self.assertFalse(hasattr(args, "_explicit_sources"))


if __name__ == "__main__":
    unittest.main()
