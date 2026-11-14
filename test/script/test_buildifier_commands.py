"""Buildifier command scope and diagnostic-only integration boundaries."""

import argparse
import contextlib
import io
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao.command import format, hygiene, setup, tidy
from ao.core import bazel_native as native
from ao.core import builddir, buildifier, gitfiles, tidyengine


def arguments(command, *values):
    parser = argparse.ArgumentParser()
    command.register(parser.add_subparsers(required=True))
    return parser.parse_args([command.NAME, *values])


class BuildifierCommandTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="buildifier commands ")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        stack = self.enterContext(contextlib.ExitStack())
        for module in (format, tidy, gitfiles):
            stack.enter_context(mock.patch.object(module, "PROJECT_ROOT", self.root))
        stack.enter_context(mock.patch("ao.core.paths.PROJECT_ROOT", self.root))
        self.stdout = self.enterContext(contextlib.redirect_stdout(io.StringIO()))
        self.stderr = self.enterContext(contextlib.redirect_stderr(io.StringIO()))

    def source(self, name):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("# Test source.\n", encoding="utf-8")
        return path

    def test_all_format_scope_covers_root_and_asset_graph_without_scanning_foreign_trees(self):
        expected = [
            "BUILD",
            "MODULE.bazel",
            "REPO.bazel",
            "asset/BUILD.bazel",
            "bazel/policy.bzl",
            "root.bzl",
            "test/lint/BUILD.bazel",
            "test/lint/analysis/BUILD.bazel",
            "test/lint/analysis/generated_header.bzl",
        ]
        for name in (*expected, "MODULE.bazel.lock", ".bazelrc", "fooBUILD", "cache/BUILD.bazel"):
            self.source(name)
        self.assertEqual(format.resolve_files(arguments(format, "--all")), expected)

    def test_changed_format_scope_includes_root_and_asset_graph(self):
        selected = ["BUILD", "MODULE.bazel", "asset/BUILD.bazel", "bazel/policy.bzl"]
        for name in (*selected, "cache/BUILD.bazel"):
            self.source(name)
        with mock.patch.object(gitfiles, "changed_files", return_value=[*selected, "cache/BUILD.bazel"]):
            self.assertEqual(format.resolve_files(arguments(format)), selected)

    def test_all_tidy_scope_includes_root_graph_only_for_the_full_source_query(self):
        self.source("BUILD")
        self.source("MODULE.bazel")
        self.source("asset/BUILD.bazel")
        self.source("test/lint/BUILD.bazel")
        args = arguments(tidy, "--all")
        files, explicit = tidyengine.resolve_scope(
            args, tidy.ALL_FOLDERS, "Checking", suffixes=gitfiles.SOURCE_SUFFIXES
        )
        self.assertEqual(files, ["BUILD", "MODULE.bazel", "asset/BUILD.bazel", "test/lint/BUILD.bazel"])
        self.assertFalse(explicit)
        with mock.patch.object(gitfiles, "root_bazel_files", side_effect=AssertionError("C++ query widened")):
            files, _ = tidyengine.resolve_scope(args, tidy.ALL_FOLDERS, "Checking")
        self.assertEqual(files, [])

    def test_format_selects_bazel_without_cpp_tools_or_ruff(self):
        self.source("BUILD.bazel")
        for check in (False, True):
            with self.subTest(check=check):
                args = arguments(format, "BUILD.bazel", *(("--check",) if check else ()))
                with (
                    mock.patch.object(
                        format.windows_llvm_sdk, "ready_root", side_effect=AssertionError("unexpected SDK")
                    ),
                    mock.patch.object(
                        format.subprocess, "run", side_effect=AssertionError("unexpected native formatter")
                    ),
                    mock.patch.object(format, "run_ruff_format", return_value=0) as ruff,
                    mock.patch.object(buildifier, "run", return_value=0) as runner,
                ):
                    self.assertEqual(format.run_command(args), 0)
                runner.assert_called_once_with(["BUILD.bazel"], check=check)
                self.assertEqual(ruff.call_args.args[0], [])

    def test_format_propagates_findings_and_tool_errors(self):
        self.source("BUILD.bazel")
        args = arguments(format, "BUILD.bazel", "--check")
        with mock.patch.object(buildifier, "run", return_value=1):
            self.assertEqual(format.run_command(args), 1)
        with mock.patch.object(buildifier, "run", side_effect=buildifier.BuildifierError("bad tool")):
            with self.assertRaises(SystemExit):
                format.run_command(args)

    def test_bazel_scope_needs_no_native_compiler_environment(self):
        self.source("BUILD.bazel")
        with mock.patch.object(builddir, "platform_profile", return_value=builddir.WINDOWS_PROFILE):
            for command in (format, tidy, hygiene):
                with self.subTest(command=command.NAME):
                    self.assertFalse(command.requires_build_environment(arguments(command, "BUILD.bazel")))
        with mock.patch.object(builddir, "platform_profile", side_effect=AssertionError("unexpected SDK")):
            self.assertFalse(setup.requires_build_environment(arguments(setup, "buildifier")))

    def test_setup_uses_only_governed_buildifier_admission(self):
        tool = self.root / "tools/buildifier"
        with mock.patch.object(buildifier, "executable", return_value=tool) as ensure:
            self.assertEqual(setup.run_command(arguments(setup, "buildifier")), 0)
        ensure.assert_called_once_with()
        self.assertIn(str(tool), self.stdout.getvalue())

    def test_tidy_checks_bazel_and_propagates_diagnostics_without_creating_build_output(self):
        self.source("root.bzl")
        output = self.root / "never-configured"
        args = arguments(tidy, "root.bzl", "--path", str(output))
        with (
            mock.patch.object(tidy.bazel_lint, "run", side_effect=AssertionError("unexpected native analysis")),
            mock.patch.object(buildifier, "run", return_value=1) as runner,
        ):
            self.assertEqual(tidy.run_command(args), 1)
        runner.assert_called_once_with(["root.bzl"], check=True, lint=True, sink=None)
        self.assertFalse(output.exists())

    def test_tidy_writes_bazel_diagnostics_to_its_requested_report(self):
        self.source("root.bzl")
        report = self.root / "report.txt"
        args = arguments(tidy, "root.bzl", "--path", str(self.root / "unused"), "-o", str(report))

        def diagnose(files, *, check, lint, sink):
            self.assertEqual(files, ["root.bzl"])
            self.assertTrue(check and lint)
            sink.write("root.bzl:1:1: unused-variable: diagnostic\n")
            return 1

        with mock.patch.object(buildifier, "run", side_effect=diagnose):
            self.assertEqual(tidy.run_command(args), 1)
        self.assertIn("unused-variable", report.read_text(encoding="utf-8"))

    def test_cpp_only_check_selector_keeps_other_language_checks_unselected(self):
        self.source("root.bzl")
        args = arguments(tidy, "root.bzl", "--path", str(self.root / "unused"), "--check", "aobus-no-c-assert")
        with mock.patch.object(buildifier, "run", side_effect=AssertionError("unexpected language check")):
            self.assertEqual(tidy.run_command(args), 0)
        self.assertIn("Skipping Bazel checks", self.stderr.getvalue())

    def test_hygiene_checks_format_before_lint_with_one_shared_scope(self):
        # The flat gate routes through its native-tools selection; a Bazel-only
        # scope needs no SDK or managed Python, so both hosts take the same path.
        original = self.source("MODULE.bazel").read_bytes()
        for host in ("linux", "windows"):
            with self.subTest(host=host):
                args = arguments(hygiene, "MODULE.bazel")
                with (
                    mock.patch.object(buildifier, "run", side_effect=[0, 1]) as runner,
                    mock.patch.object(native, "native_platform", return_value=host),
                    mock.patch.object(tidy.bazel_lint, "run", side_effect=AssertionError("unexpected native analysis")),
                ):
                    self.assertEqual(hygiene.run_command(args), 1)
                self.assertEqual(runner.call_args_list[0], mock.call(["MODULE.bazel"], check=True))
                second = runner.call_args_list[1]
                self.assertEqual(second.args, (["MODULE.bazel"],))
                self.assertTrue(second.kwargs["check"] and second.kwargs["lint"])
                self.assertEqual((self.root / "MODULE.bazel").read_bytes(), original)

    def test_hygiene_stops_after_bazel_formatting_failure(self):
        self.source("BUILD.bazel")
        with mock.patch.object(buildifier, "run", return_value=1) as runner:
            self.assertEqual(hygiene.run_command(arguments(hygiene, "BUILD.bazel")), 1)
        runner.assert_called_once_with(["BUILD.bazel"], check=True)


if __name__ == "__main__":
    unittest.main()
