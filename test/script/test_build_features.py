"""Portal regressions for configured native test-suite selection."""

import contextlib
import io
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao.__main__ import make_parser
from ao.command import build, check, test
from ao.core import builddir


class BuildFeatureTest(unittest.TestCase):
    def parse(self, *arguments):
        return make_parser().parse_args(list(arguments))

    def test_check_rejects_tests_disabled_even_with_old_binaries(self):
        with tempfile.TemporaryDirectory() as temporary:
            tree = Path(temporary)
            self.write_suite_configuration(tree, tests="OFF")
            stale = tree / "test" / "ao_gtk_test"
            stale.parent.mkdir()
            stale.touch()
            result = build.BuildResult(tree, tree / "build.log", "gcc")
            args = self.parse("check", "-p", temporary)
            with (
                mock.patch.object(builddir, "platform_profile", return_value=builddir.LINUX_PROFILE),
                mock.patch.object(check.build, "do_build", return_value=result),
                mock.patch.object(check.dependency_policy, "verified_report"),
                mock.patch.object(check.test, "run_suites") as run_suites,
                contextlib.redirect_stdout(io.StringIO()),
                contextlib.redirect_stderr(io.StringIO()) as stderr,
                self.assertRaises(SystemExit),
            ):
                check.run_command(args)
            self.assertIn("AOBUS_BUILD_TESTS=ON is required", stderr.getvalue())
            run_suites.assert_not_called()

    def test_test_groups_skip_disabled_targets_and_explicit_gtk_fails(self):
        with tempfile.TemporaryDirectory() as temporary:
            tree = Path(temporary)
            self.write_suite_configuration(tree, gtk="OFF", tui="OFF", cli="OFF", lint="OFF")
            stale = tree / "test" / "ao_gtk_test"
            stale.parent.mkdir()
            stale.touch()
            with (
                mock.patch.object(builddir, "platform_profile", return_value=builddir.LINUX_PROFILE),
                mock.patch.object(test.build, "validate_build_tree"),
                mock.patch.object(test, "run_suites", return_value=0) as run_suites,
                contextlib.redirect_stdout(io.StringIO()),
            ):
                self.assertEqual(test.run_command(self.parse("test", "--all", "-n", "-p", temporary)), 0)
                run_suites.assert_called_once()
                self.assertEqual(run_suites.call_args.args[0], ("core", "integration", "tooling"))
                for no_build in ((), ("-n",)):
                    with self.subTest(no_build=no_build):
                        with (
                            mock.patch.object(test, "run") as run,
                            contextlib.redirect_stderr(io.StringIO()) as stderr,
                            self.assertRaises(SystemExit),
                        ):
                            test.run_command(self.parse("test", "--gtk", "-p", temporary, *no_build))
                        self.assertIn("suite 'gtk' is disabled", stderr.getvalue())
                        run.assert_not_called()
                self.assertEqual(run_suites.call_count, 1)

    def test_cmake_false_spellings_disable_gtk_and_missing_feature_is_an_error(self):
        with tempfile.TemporaryDirectory() as temporary:
            tree = Path(temporary)
            for disabled in ("OFF", "FALSE", "NO", "0", "GTK-NOTFOUND"):
                with self.subTest(disabled=disabled):
                    self.write_suite_configuration(tree, gtk=disabled)
                    with mock.patch.object(builddir, "platform_profile", return_value=builddir.LINUX_PROFILE):
                        self.assertNotIn("gtk", test.configured_suites_for("all", tree))
            self.write_suite_configuration(tree)
            (tree / "CMakeCache.txt").write_text(
                "AOBUS_BUILD_TESTS:BOOL=ON\n"
                "AOBUS_BUILD_TUI:BOOL=ON\n"
                "AOBUS_BUILD_CLI:BOOL=ON\n"
                "AOBUS_BUILD_LINT_PLUGIN:BOOL=ON\n",
                encoding="utf-8",
            )
            with (
                mock.patch.object(builddir, "platform_profile", return_value=builddir.LINUX_PROFILE),
                contextlib.redirect_stderr(io.StringIO()) as stderr,
                self.assertRaises(SystemExit),
            ):
                test.configured_suites_for("all", tree)
            self.assertIn("Cannot determine AOBUS_BUILD_GTK", stderr.getvalue())

    def test_enabled_suite_with_no_binary_fails_instead_of_being_skipped(self):
        with tempfile.TemporaryDirectory() as temporary:
            tree = Path(temporary)
            self.write_suite_configuration(tree, gtk="ON")
            with (
                mock.patch.object(builddir, "platform_profile", return_value=builddir.LINUX_PROFILE),
                mock.patch.object(test.build, "validate_build_tree"),
                contextlib.redirect_stdout(io.StringIO()),
                contextlib.redirect_stderr(io.StringIO()) as stderr,
                self.assertRaises(SystemExit),
            ):
                test.run_command(self.parse("test", "--gtk", "-n", "-p", temporary))
            self.assertIn("gtk test binary not found", stderr.getvalue())

    @staticmethod
    def write_suite_configuration(tree, *, gtk="ON", tui="ON", cli="ON", lint="ON", tests="ON"):
        (tree / "CMakeCache.txt").write_text(
            f"AOBUS_BUILD_TESTS:BOOL={tests}\n"
            f"AOBUS_BUILD_GTK:BOOL={gtk}\n"
            f"AOBUS_BUILD_TUI:BOOL={tui}\n"
            f"AOBUS_BUILD_CLI:BOOL={cli}\n"
            f"AOBUS_BUILD_LINT_PLUGIN:BOOL={lint}\n",
            encoding="utf-8",
        )


if __name__ == "__main__":
    unittest.main()
