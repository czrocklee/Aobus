"""Portal regressions for native feature selection and CMake forwarding."""

import contextlib
import io
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao.__main__ import make_parser
from ao.command import build, check, test
from ao.core import builddir
from ao.core.paths import PROJECT_ROOT


class BuildFeatureTest(unittest.TestCase):
    def parse(self, *arguments):
        return make_parser().parse_args(list(arguments))

    def test_build_owners_parse_narrow_feature_switches(self):
        for arguments in (
            ("build", "--gtk", "off", "--system-media", "on"),
            ("check", "--gtk", "off", "--system-media", "on"),
            ("run", "tui", "--gtk", "off", "--system-media", "on"),
        ):
            with self.subTest(arguments=arguments):
                args = self.parse(*arguments)
                self.assertEqual(args.gtk, "off")
                self.assertEqual(args.system_media, "on")

    def test_feature_switch_defaults_leave_cmake_defaults_in_control(self):
        args = self.parse("build")

        self.assertIsNone(args.gtk)
        self.assertIsNone(args.system_media)

    def test_explicit_feature_switches_are_forwarded_to_cmake(self):
        with tempfile.TemporaryDirectory() as temporary:
            args = self.parse(
                "build",
                "-p",
                temporary,
                "--gtk",
                "off",
                "--system-media",
                "on",
            )
            workspace = mock.Mock(
                source_dir=PROJECT_ROOT,
                compiler_build_dir=None,
                cmake_arguments=[],
            )
            with (
                mock.patch.object(builddir, "platform_profile", return_value=builddir.LINUX_PROFILE),
                mock.patch.object(build, "_workspace_cache", return_value=workspace),
                mock.patch.object(build, "validate_build_tree", return_value="gcc"),
                mock.patch.object(build, "run", return_value=0) as run,
                contextlib.redirect_stdout(io.StringIO()),
            ):
                build.do_build(args, ["aobus-tui"])

        configure = run.call_args_list[0].args[0]
        self.assertIn("-DAOBUS_BUILD_GTK=OFF", configure)
        self.assertIn("-DAOBUS_BUILD_SYSTEM_MEDIA=ON", configure)

    def test_omitted_feature_switches_are_not_forwarded(self):
        with tempfile.TemporaryDirectory() as temporary:
            args = self.parse("build", "-p", temporary)
            workspace = mock.Mock(
                source_dir=PROJECT_ROOT,
                compiler_build_dir=None,
                cmake_arguments=[],
            )
            with (
                mock.patch.object(builddir, "platform_profile", return_value=builddir.LINUX_PROFILE),
                mock.patch.object(build, "_workspace_cache", return_value=workspace),
                mock.patch.object(build, "validate_build_tree", return_value="gcc"),
                mock.patch.object(build, "run", return_value=0) as run,
                contextlib.redirect_stdout(io.StringIO()),
            ):
                build.do_build(args, [])

        configure = run.call_args_list[0].args[0]
        self.assertFalse(any(argument.startswith("-DAOBUS_BUILD_GTK=") for argument in configure))
        self.assertFalse(any(argument.startswith("-DAOBUS_BUILD_SYSTEM_MEDIA=") for argument in configure))

    def test_check_plans_gtk_from_the_configured_tree_after_build(self):
        for existing, arguments, configured, expected_gtk in (
            (None, ("--gtk", "off"), "OFF", False),
            ("OFF", (), "OFF", False),
            ("OFF", ("--gtk", "on"), "ON", True),
            ("ON", (), "ON", True),
            (None, (), "ON", True),
        ):
            with self.subTest(existing=existing, arguments=arguments, configured=configured):
                with tempfile.TemporaryDirectory() as temporary:
                    tree = Path(temporary)
                    args = self.parse("check", "-p", temporary, *arguments)
                    if existing:
                        self.write_suite_configuration(tree, gtk=existing)
                    # An old GTK executable must not make a disabled suite runnable.
                    stale = tree / "test" / "ao_gtk_test"
                    stale.parent.mkdir(exist_ok=True)
                    stale.touch()
                    result = build.BuildResult(tree, tree / "build.log", "gcc")

                    def configure(_args, *, targets, tree=tree, configured=configured, result=result):
                        self.write_suite_configuration(tree, gtk=configured)
                        return result

                    with (
                        mock.patch.object(builddir, "platform_profile", return_value=builddir.LINUX_PROFILE),
                        mock.patch.object(check.build, "do_build", side_effect=configure) as do_build,
                        mock.patch.object(check.dependency_policy, "verified_report"),
                        mock.patch.object(check.test, "run_suites", return_value=0) as run_suites,
                        mock.patch.object(check.build, "print_summary"),
                        contextlib.redirect_stdout(io.StringIO()),
                    ):
                        self.assertEqual(check.run_command(args), 0)

                do_build.assert_called_once_with(args, targets=["all", "aobus_guardrails", "ao_perf_baseline"])
                selected = run_suites.call_args.args[0]
                self.assertEqual("gtk" in selected, expected_gtk)
                self.assertEqual(
                    tuple(name for name in selected if name != "gtk"),
                    tuple(name for name in builddir.LINUX_PROFILE.all_suites if name != "gtk"),
                )

    def test_tsan_check_builds_only_configured_supported_targets(self):
        for existing, arguments, configured, expected_gtk in (
            ("OFF", (), "OFF", False),
            (None, ("--gtk", "off"), "OFF", False),
            ("OFF", ("--gtk", "on"), "ON", True),
            ("OFF", ("--clean",), "ON", True),
        ):
            with self.subTest(existing=existing, arguments=arguments):
                with tempfile.TemporaryDirectory() as temporary:
                    tree = Path(temporary)
                    args = self.parse("check", "-p", temporary, "--tsan", *arguments)
                    if existing:
                        self.write_suite_configuration(tree, gtk=existing)
                    result = build.BuildResult(tree, tree / "build.log", "gcc")

                    def configure(_args, *, targets, tree=tree, configured=configured, result=result):
                        self.write_suite_configuration(tree, gtk=configured)
                        return result

                    with (
                        mock.patch.object(builddir, "platform_profile", return_value=builddir.LINUX_PROFILE),
                        mock.patch.object(check.build, "do_build", side_effect=configure) as do_build,
                        mock.patch.object(check.dependency_policy, "verified_report"),
                        mock.patch.object(check.test, "run_suites", return_value=0) as run_suites,
                        mock.patch.object(check.build, "print_summary"),
                        contextlib.redirect_stdout(io.StringIO()),
                    ):
                        self.assertEqual(check.run_command(args), 0)

                expected_targets = ["ao_core_test"] + (["ao_gtk_test"] if expected_gtk else []) + ["aobus_guardrails"]
                do_build.assert_called_once_with(args, targets=expected_targets)
                self.assertEqual(run_suites.call_args.args[0], ("core", "gtk") if expected_gtk else ("core",))

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

    def test_enabling_linux_features_is_rejected_on_unsupported_platforms(self):
        for option in (("--gtk", "on"), ("--system-media", "on")):
            with self.subTest(option=option):
                args = self.parse("build", *option)
                with (
                    mock.patch.object(builddir, "platform_profile", return_value=builddir.MACOS_PROFILE),
                    contextlib.redirect_stderr(io.StringIO()),
                    self.assertRaises(SystemExit),
                ):
                    build.validate_build_options(args)

    def test_explicit_feature_identity_is_checked_after_configure(self):
        with tempfile.TemporaryDirectory() as temporary:
            tree = Path(temporary)
            (tree / "CMakeCache.txt").write_text(
                "CMAKE_CACHE_MAJOR_VERSION:INTERNAL=4\n"
                "CMAKE_CACHE_MINOR_VERSION:INTERNAL=1\n"
                "CMAKE_CACHE_PATCH_VERSION:INTERNAL=2\n"
                "AOBUS_ENABLE_ASAN:BOOL=OFF\n"
                "AOBUS_ENABLE_TSAN:BOOL=OFF\n"
                "AOBUS_BUILD_GTK:BOOL=ON\n"
                "AOBUS_BUILD_SYSTEM_MEDIA:BOOL=OFF\n",
                encoding="utf-8",
            )
            compiler_dir = tree / "CMakeFiles" / "4.1.2"
            compiler_dir.mkdir(parents=True)
            for language in ("C", "CXX"):
                (compiler_dir / f"CMake{language}Compiler.cmake").write_text(
                    f'set(CMAKE_{language}_COMPILER_ID "GNU")\n', encoding="utf-8"
                )
            args = self.parse("build", "--gtk", "off", "--system-media", "on")

            with (
                mock.patch.object(builddir, "platform_profile", return_value=builddir.LINUX_PROFILE),
                contextlib.redirect_stderr(io.StringIO()),
                self.assertRaises(SystemExit),
            ):
                build.validate_build_tree(args, tree)


if __name__ == "__main__":
    unittest.main()
