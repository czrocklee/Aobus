"""CLI smoke tests: every command registers and parses its documented invocations."""

import contextlib
import io
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao.__main__ import main, make_parser
from ao.command import check as check_command
from ao.command import test as test_command
from ao.command import tidy as tidy_command
from ao.command.build import BuildResult


class CliParseTest(unittest.TestCase):
    def parse(self, argv):
        return make_parser().parse_args(argv)

    def test_all_commands_are_registered(self):
        buffer = io.StringIO()
        with contextlib.redirect_stdout(buffer):
            self.assertEqual(main(["help"]), 0)
        for command in ("build", "check", "test", "coverage", "tidy", "analyze", "format", "hygiene"):
            self.assertIn(command, buffer.getvalue())
        self.assertNotIn("selftest", buffer.getvalue())
        self.assertNotIn("pycheck", buffer.getvalue())

    def test_build_arguments(self):
        args = self.parse(["build", "release", "--clang", "--clean", "--target", "aobus-gtk"])
        self.assertEqual(args.flavor, "release")
        self.assertTrue(args.clang)
        self.assertEqual(args.target, ["aobus-gtk"])

    def test_build_rejects_unknown_flavor(self):
        with self.assertRaises(SystemExit):
            self.parse(["build", "fastest"])

    def test_sanitizers_are_mutually_exclusive(self):
        with self.assertRaises(SystemExit):
            self.parse(["build", "--asan", "--tsan"])

    def test_check_defaults_to_debug(self):
        args = self.parse(["check"])
        self.assertEqual(args.flavor, "debug")
        self.assertFalse(args.asan)

    def test_test_suite_shortcuts(self):
        args = self.parse(["test", "--gtk", "[layout],[model]", "-n", "--clang", "--asan"])
        self.assertEqual(args.suite, "gtk")
        self.assertEqual(args.filter, "[layout],[model]")
        self.assertTrue(args.no_build)
        self.assertTrue(args.clang)
        self.assertTrue(args.asan)

    def test_test_defaults_to_default_suite_group(self):
        args = self.parse(["test"])
        self.assertEqual(args.suite, "default")

    def test_test_suite_targets_use_suite_name_suffixes(self):
        self.assertEqual(
            test_command.SUITE_TARGETS,
            {
                "core": ["ao_core_test"],
                "gtk": ["ao_gtk_test"],
                "integration": ["ao_integration_test"],
                "fleet": ["ao_fleet_test"],
            },
        )

    def test_test_all_runs_every_suite(self):
        args = self.parse(["test", "--all", "-n", "-p", "/tmp/aobus-test-build"])

        with mock.patch.object(test_command, "run_suites", return_value=0) as run_suites:
            self.assertEqual(test_command.run_command(args), 0)

        run_suites.assert_called_once_with(
            ("core", "gtk", "integration", "fleet", "tooling", "lint"),
            Path("/tmp/aobus-test-build"),
            test_filter="",
            list_only=False,
        )

    def test_suite_group_dispatches_registered_runner_kinds_in_order(self):
        build_dir = Path("/tmp/aobus-test-build")

        with mock.patch.object(test_command, "run_suite", return_value=0) as run_suite:
            with mock.patch.object(test_command, "run_non_catch2_suite", return_value=0) as run_non_catch2:
                self.assertEqual(test_command.run_suites(test_command.SUITE_GROUPS["all"], build_dir), 0)

        self.assertEqual([call.args[0] for call in run_suite.call_args_list], ["core", "gtk", "integration", "fleet"])
        self.assertEqual([call.args[0] for call in run_non_catch2.call_args_list], ["tooling", "lint"])

    def test_test_no_build_lint_uses_selected_tree_without_building(self):
        build_dir = Path("/tmp/aobus-test-build")

        with mock.patch.object(test_command.linttest, "run", return_value=0) as run:
            self.assertEqual(test_command.run_non_catch2_suite("lint", build_dir), 0)

        run.assert_called_once_with(build_dir, log=None)

    def test_tooling_suite_does_not_require_a_cmake_build_tree(self):
        args = self.parse(["test", "--tooling", "-p", "/tmp/nonexistent-aobus-build"])

        with mock.patch.object(test_command, "run_suites", return_value=0) as run_suites:
            with mock.patch.object(test_command, "run") as run:
                self.assertEqual(test_command.run_command(args), 0)

        run.assert_not_called()
        run_suites.assert_called_once_with(
            ("tooling",),
            Path("/tmp/nonexistent-aobus-build"),
            test_filter="",
            list_only=False,
        )

    def test_test_list_describes_non_catch2_suite_without_running_it(self):
        with mock.patch.object(test_command.linttest, "run") as run:
            self.assertEqual(
                test_command.run_non_catch2_suite("lint", Path("/tmp/aobus-test-build"), list_only=True),
                0,
            )

        run.assert_not_called()

    def test_check_runs_the_same_all_suite_group(self):
        args = self.parse(["check"])
        result = BuildResult(
            build_dir=Path("/tmp/aobus-test-build"),
            log=Path("/tmp/aobus-test-build/build.log"),
            compiler="gcc",
        )

        with mock.patch.object(check_command.build, "do_build", return_value=result):
            with mock.patch.object(check_command.test, "run_suites", return_value=0) as run_suites:
                with mock.patch.object(check_command.build, "print_summary"):
                    self.assertEqual(check_command.run_command(args), 0)

        run_suites.assert_called_once_with(
            test_command.SUITE_GROUPS["all"],
            result.build_dir,
            log=result.log,
        )

    def test_test_sanitizers_are_mutually_exclusive(self):
        with self.assertRaises(SystemExit):
            self.parse(["test", "--asan", "--tsan"])

    def test_test_suite_shortcuts_are_mutually_exclusive(self):
        with self.assertRaises(SystemExit):
            self.parse(["test", "--core", "--gtk"])

    def test_coverage_defaults_to_core_suite(self):
        args = self.parse(["coverage", "rt::SmartListEvaluator"])
        self.assertEqual(args.suite, "core")
        self.assertEqual(args.filter, "rt::SmartListEvaluator")

    def test_tidy_scope_and_passthrough_arguments(self):
        args = self.parse(
            [
                "tidy",
                "--folder",
                "lib",
                "--folder",
                "app",
                "--check",
                "modernize-use-nullptr",
                "--tidy-arg=--extra-arg=-std=c++26",
                "-o",
                "/tmp/report.txt",
                "-j",
                "4",
            ]
        )
        self.assertEqual(args.folder, ["lib", "app"])
        self.assertEqual(args.check, "modernize-use-nullptr")
        self.assertEqual(args.tidy_arg, ["--extra-arg=-std=c++26"])
        self.assertEqual(args.output, "/tmp/report.txt")
        self.assertEqual(args.jobs, 4)

    def test_tidy_no_build_uses_existing_plugin_and_compile_database(self):
        with tempfile.TemporaryDirectory(dir="/tmp") as temp_dir:
            build_dir = Path(temp_dir)
            (build_dir / "compile_commands.json").touch()
            plugin = build_dir / "tool" / "lint" / "libAobusLintPlugin.so"
            plugin.parent.mkdir(parents=True)
            plugin.touch()

            with mock.patch.object(tidy_command.tidyengine, "ensure_compile_db") as ensure_compile_db:
                with mock.patch.object(tidy_command.subprocess, "run") as subprocess_run:
                    self.assertEqual(tidy_command.prepare_plugin(build_dir, no_build=True), plugin)

        ensure_compile_db.assert_not_called()
        subprocess_run.assert_not_called()

    def test_tidy_explicit_files(self):
        args = self.parse(["tidy", "lib/audio/Foo.cpp", "include/aobus/Foo.h"])
        self.assertEqual(args.files, ["lib/audio/Foo.cpp", "include/aobus/Foo.h"])

    def test_analyze_flags(self):
        args = self.parse(["analyze", "--all", "--alpha", "--fail-on-diagnostics"])
        self.assertTrue(args.all)
        self.assertTrue(args.alpha)
        self.assertTrue(args.fail_on_diagnostics)

    def test_format_check_mode(self):
        args = self.parse(["format", "--check", "--folder", "script"])
        self.assertTrue(args.check)
        self.assertEqual(args.files, [])
        self.assertEqual(args.folder, ["script"])

    def test_hygiene_scope_arguments(self):
        args = self.parse(["hygiene", "--folder", "script", "--commit", "HEAD~3", "-j", "4"])
        self.assertEqual(args.folder, ["script"])
        self.assertEqual(args.commit, "HEAD~3")
        self.assertEqual(args.jobs, 4)

    def test_help_exits_zero(self):
        for argv in (["--help"], ["build", "--help"], ["tidy", "--help"]):
            buffer = io.StringIO()
            with contextlib.redirect_stdout(buffer), self.assertRaises(SystemExit) as caught:
                make_parser().parse_args(argv)
            self.assertEqual(caught.exception.code, 0)
            self.assertTrue(buffer.getvalue())


if __name__ == "__main__":
    unittest.main()
