"""Portal regressions for native feature selection and CMake forwarding."""

import contextlib
import io
import shutil
import subprocess
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
        # Either system-media value is rejected: the switch has no cache entry off Linux.
        for option in (("--gtk", "on"), ("--system-media", "on"), ("--system-media", "off")):
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

    @staticmethod
    def _write_feature_tree(tree, *, gtk="ON", system_media="OFF", asan="OFF", tsan="OFF"):
        """A reusable, compiler-valid tree for direct validate_build_tree checks."""
        lines = [
            "CMAKE_CACHE_MAJOR_VERSION:INTERNAL=4",
            "CMAKE_CACHE_MINOR_VERSION:INTERNAL=1",
            "CMAKE_CACHE_PATCH_VERSION:INTERNAL=2",
            f"AOBUS_ENABLE_ASAN:BOOL={asan}",
            f"AOBUS_ENABLE_TSAN:BOOL={tsan}",
        ]
        if gtk is not None:
            lines.append(f"AOBUS_BUILD_GTK:BOOL={gtk}")
        if system_media is not None:
            lines.append(f"AOBUS_BUILD_SYSTEM_MEDIA:BOOL={system_media}")
        (tree / "CMakeCache.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
        compiler_dir = tree / "CMakeFiles" / "4.1.2"
        compiler_dir.mkdir(parents=True)
        for language in ("C", "CXX"):
            (compiler_dir / f"CMake{language}Compiler.cmake").write_text(
                f'set(CMAKE_{language}_COMPILER_ID "GNU")\n', encoding="utf-8"
            )

    def test_feature_identity_normalizes_cmake_bool_spellings(self):
        # The configured BOOL is compared by CMake semantics, not raw text, so
        # equivalent truthy/falsy spellings match the requested on/off switch.
        cases = (
            # equivalent truthy spellings match a requested ON
            ("ON", "on", False),
            ("TRUE", "on", False),
            ("1", "on", False),
            ("YES", "on", False),
            ("Y", "on", False),
            # equivalent falsy spellings match a requested OFF
            ("OFF", "off", False),
            ("FALSE", "off", False),
            ("0", "off", False),
            ("NO", "off", False),
            ("N", "off", False),
            # genuine semantic mismatches fail closed
            ("ON", "off", True),
            ("OFF", "on", True),
            ("TRUE", "off", True),
            ("1", "off", True),
            ("FALSE", "on", True),
            ("0", "on", True),
        )
        for configured, requested, mismatch in cases:
            with self.subTest(configured=configured, requested=requested):
                with tempfile.TemporaryDirectory() as temporary:
                    tree = Path(temporary)
                    self._write_feature_tree(tree, gtk=configured)
                    args = self.parse("build", "--gtk", requested)
                    with mock.patch.object(builddir, "platform_profile", return_value=builddir.LINUX_PROFILE):
                        if mismatch:
                            with (
                                contextlib.redirect_stderr(io.StringIO()) as stderr,
                                self.assertRaises(SystemExit),
                            ):
                                build.validate_build_tree(args, tree)
                            self.assertIn("Feature mismatch", stderr.getvalue())
                        else:
                            self.assertEqual(build.validate_build_tree(args, tree), "gcc")

    def test_feature_identity_fails_closed_on_missing_cache_entry(self):
        with tempfile.TemporaryDirectory() as temporary:
            tree = Path(temporary)
            self._write_feature_tree(tree, gtk=None)
            args = self.parse("build", "--gtk", "on")
            with (
                mock.patch.object(builddir, "platform_profile", return_value=builddir.LINUX_PROFILE),
                contextlib.redirect_stderr(io.StringIO()) as stderr,
                self.assertRaises(SystemExit),
            ):
                build.validate_build_tree(args, tree)
            self.assertIn("Cannot determine AOBUS_BUILD_GTK", stderr.getvalue())

    def test_feature_identity_fails_closed_on_invalid_cache_value(self):
        with tempfile.TemporaryDirectory() as temporary:
            tree = Path(temporary)
            self._write_feature_tree(tree, gtk="MAYBE")
            args = self.parse("build", "--gtk", "on")
            with (
                mock.patch.object(builddir, "platform_profile", return_value=builddir.LINUX_PROFILE),
                contextlib.redirect_stderr(io.StringIO()) as stderr,
                self.assertRaises(SystemExit),
            ):
                build.validate_build_tree(args, tree)
            self.assertIn("Invalid AOBUS_BUILD_GTK", stderr.getvalue())

    def test_sanitizer_identity_normalizes_cmake_bool_spellings(self):
        # AOBUS_ENABLE_ASAN/TSAN are compared by CMake BOOL semantics, so
        # noncanonical truthy spellings match a requested flag and falsy ones a
        # plain build; genuine mismatches still fail closed.
        cases = (
            # requested --asan (expected ON) matches truthy spellings
            ("asan", "ON", True, False),
            ("asan", "TRUE", True, False),
            ("asan", "1", True, False),
            ("asan", "YES", True, False),
            ("asan", "Y", True, False),
            # plain build (expected OFF) matches falsy spellings
            (None, "OFF", False, False),
            (None, "FALSE", False, False),
            (None, "0", False, False),
            (None, "NO", False, False),
            (None, "N", False, False),
            # genuine semantic mismatches fail closed
            ("asan", "OFF", True, True),
            ("asan", "FALSE", True, True),
            ("asan", "0", True, True),
            (None, "ON", False, True),
            (None, "TRUE", False, True),
            (None, "1", False, True),
        )
        for flag, configured, requested_flag, mismatch in cases:
            with self.subTest(flag=flag, configured=configured, requested_flag=requested_flag):
                with tempfile.TemporaryDirectory() as temporary:
                    tree = Path(temporary)
                    self._write_feature_tree(tree, asan=configured, gtk="ON", system_media="OFF")
                    build_args = ("build", "--gtk", "on")
                    if flag == "asan":
                        build_args = ("build", "--asan", "--gtk", "on")
                    args = self.parse(*build_args)
                    self.assertEqual(getattr(args, "asan", False), requested_flag)
                    with mock.patch.object(builddir, "platform_profile", return_value=builddir.LINUX_PROFILE):
                        if mismatch:
                            with (
                                contextlib.redirect_stderr(io.StringIO()) as stderr,
                                self.assertRaises(SystemExit),
                            ):
                                build.validate_build_tree(args, tree)
                            self.assertIn("Sanitizer mismatch", stderr.getvalue())
                        else:
                            self.assertEqual(build.validate_build_tree(args, tree), "gcc")

    def test_sanitizer_identity_fails_closed_on_missing_cache_entry(self):
        with tempfile.TemporaryDirectory() as temporary:
            tree = Path(temporary)
            # No AOBUS_ENABLE_ASAN line; a plain build requests OFF.
            cache_lines = [
                "CMAKE_CACHE_MAJOR_VERSION:INTERNAL=4",
                "CMAKE_CACHE_MINOR_VERSION:INTERNAL=1",
                "CMAKE_CACHE_PATCH_VERSION:INTERNAL=2",
                "AOBUS_ENABLE_TSAN:BOOL=OFF",
                "AOBUS_BUILD_GTK:BOOL=ON",
                "AOBUS_BUILD_SYSTEM_MEDIA:BOOL=OFF",
            ]
            (tree / "CMakeCache.txt").write_text("\n".join(cache_lines) + "\n", encoding="utf-8")
            compiler_dir = tree / "CMakeFiles" / "4.1.2"
            compiler_dir.mkdir(parents=True)
            for language in ("C", "CXX"):
                (compiler_dir / f"CMake{language}Compiler.cmake").write_text(
                    f'set(CMAKE_{language}_COMPILER_ID "GNU")\n', encoding="utf-8"
                )
            args = self.parse("build", "--gtk", "on")
            with (
                mock.patch.object(builddir, "platform_profile", return_value=builddir.LINUX_PROFILE),
                contextlib.redirect_stderr(io.StringIO()) as stderr,
                self.assertRaises(SystemExit),
            ):
                build.validate_build_tree(args, tree)
            self.assertIn("Cannot determine AOBUS_ENABLE_ASAN", stderr.getvalue())

    def test_sanitizer_identity_fails_closed_on_invalid_cache_value(self):
        with tempfile.TemporaryDirectory() as temporary:
            tree = Path(temporary)
            self._write_feature_tree(tree, asan="MAYBE", gtk="ON", system_media="OFF")
            args = self.parse("build", "--asan", "--gtk", "on")
            with (
                mock.patch.object(builddir, "platform_profile", return_value=builddir.LINUX_PROFILE),
                contextlib.redirect_stderr(io.StringIO()) as stderr,
                self.assertRaises(SystemExit),
            ):
                build.validate_build_tree(args, tree)
            self.assertIn("Invalid AOBUS_ENABLE_ASAN", stderr.getvalue())


@unittest.skipIf(shutil.which("cmake") is None, "cmake is unavailable")
class SystemMediaCacheFixtureTest(unittest.TestCase):
    """Cache semantics of the real AOBUS_BUILD_SYSTEM_MEDIA check.

    The fixture project includes the production cmake/SystemMedia.cmake and pins
    the LINUX variable, so both platform branches of the actual check run
    through a real configure on any host. The assertions read the resulting
    CMakeCache.txt back instead of duplicating the branch in Python.
    """

    @staticmethod
    def _tool(name: str) -> str:
        tool = shutil.which(name)
        if tool is None:
            raise unittest.SkipTest(f"{name} is unavailable")
        return str(Path(tool).resolve())

    def _write_fixture_source(self, root: Path) -> Path:
        source = root / "source"
        (source / "cmake").mkdir(parents=True)
        shutil.copy2(
            PROJECT_ROOT / "cmake" / "SystemMedia.cmake",
            source / "cmake" / "SystemMedia.cmake",
        )
        (source / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.30)\n"
            "project(AobusSystemMediaCacheFixture LANGUAGES NONE)\n"
            # The production check reads LINUX at include time; the fixture pins
            # it so both branches run on any host.
            "if(FIXTURE_ON_LINUX)\n"
            "  set(LINUX TRUE)\n"
            "else()\n"
            "  set(LINUX FALSE)\n"
            "endif()\n"
            "include(cmake/SystemMedia.cmake)\n",
            encoding="utf-8",
        )
        return source

    def _configure(
        self, source: Path, build: Path, *, on_linux: bool, definitions: tuple[str, ...] = ()
    ) -> subprocess.CompletedProcess[str]:
        argv = [
            self._tool("cmake"),
            "-S",
            str(source),
            "-B",
            str(build),
            f"-DFIXTURE_ON_LINUX={'ON' if on_linux else 'OFF'}",
            *definitions,
        ]
        return subprocess.run(argv, capture_output=True, text=True, timeout=90, check=False)

    @staticmethod
    def _cache_values(build: Path) -> dict[str, str]:
        values: dict[str, str] = {}
        for line in (build / "CMakeCache.txt").read_text(encoding="utf-8").splitlines():
            if line.startswith(("#", "//")) or "=" not in line:
                continue
            name, _, value = line.partition("=")
            values[name.partition(":")[0]] = value
        return values

    def assert_system_media_entry_absent(self, build: Path) -> None:
        cache = (build / "CMakeCache.txt").read_text(encoding="utf-8")
        self.assertNotIn("AOBUS_BUILD_SYSTEM_MEDIA", cache)

    def test_linux_defaults_on_and_preserves_an_explicit_off(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = self._write_fixture_source(Path(temporary))
            build = Path(temporary) / "build"

            fresh = self._configure(source, build, on_linux=True)
            self.assertEqual(fresh.returncode, 0, fresh.stdout + fresh.stderr)
            self.assertEqual(self._cache_values(build)["AOBUS_BUILD_SYSTEM_MEDIA"], "ON")

            explicit = self._configure(source, build, on_linux=True, definitions=("-DAOBUS_BUILD_SYSTEM_MEDIA=OFF",))
            self.assertEqual(explicit.returncode, 0, explicit.stdout + explicit.stderr)
            self.assertEqual(self._cache_values(build)["AOBUS_BUILD_SYSTEM_MEDIA"], "OFF")

            reused = self._configure(source, build, on_linux=True)
            self.assertEqual(reused.returncode, 0, reused.stdout + reused.stderr)
            # A configure without -D keeps the existing explicit entry.
            self.assertEqual(self._cache_values(build)["AOBUS_BUILD_SYSTEM_MEDIA"], "OFF")

    def test_non_linux_keeps_the_cache_entry_absent(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = self._write_fixture_source(Path(temporary))
            build = Path(temporary) / "build"

            fresh = self._configure(source, build, on_linux=False)
            self.assertEqual(fresh.returncode, 0, fresh.stdout + fresh.stderr)
            self.assertNotIn("Removing cached", fresh.stdout)
            self.assert_system_media_entry_absent(build)

            reused = self._configure(source, build, on_linux=False)
            self.assertEqual(reused.returncode, 0, reused.stdout + reused.stderr)
            self.assertNotIn("Removing cached", reused.stdout)
            self.assert_system_media_entry_absent(build)

    def test_non_linux_rejects_explicit_truthy_requests(self):
        for spelling in ("ON", "TRUE", "1", "YES"):
            with self.subTest(spelling=spelling):
                with tempfile.TemporaryDirectory() as temporary:
                    source = self._write_fixture_source(Path(temporary))
                    build = Path(temporary) / "build"

                    rejected = self._configure(
                        source,
                        build,
                        on_linux=False,
                        definitions=(f"-DAOBUS_BUILD_SYSTEM_MEDIA={spelling}",),
                    )
                    self.assertNotEqual(rejected.returncode, 0, rejected.stdout + rejected.stderr)
                    self.assertIn(
                        "AOBUS_BUILD_SYSTEM_MEDIA is currently supported only on Linux",
                        rejected.stderr,
                    )

    def test_non_linux_removes_stale_false_spellings_and_keeps_unrelated_cache(self):
        unrelated = {"AOBUS_BUILD_TUI": "ON", "AOBUS_UNRELATED_STRING": "keep"}
        for spelling in ("OFF", "FALSE", "NO", "0", "MEDIA-NOTFOUND"):
            with self.subTest(spelling=spelling):
                with tempfile.TemporaryDirectory() as temporary:
                    root = Path(temporary)
                    source = self._write_fixture_source(root)
                    build = root / "build"
                    build.mkdir()
                    # A cache left by a configure from before the platform check,
                    # next to entries the cleanup must not touch.
                    (build / "CMakeCache.txt").write_text(
                        f"AOBUS_BUILD_SYSTEM_MEDIA:BOOL={spelling}\n"
                        "AOBUS_BUILD_TUI:BOOL=ON\n"
                        "AOBUS_UNRELATED_STRING:STRING=keep\n",
                        encoding="utf-8",
                    )

                    cleaned = self._configure(source, build, on_linux=False)
                    self.assertEqual(cleaned.returncode, 0, cleaned.stdout + cleaned.stderr)
                    # The status line reports the stored spelling, not a hardcoded OFF.
                    self.assertIn(f"Removing cached AOBUS_BUILD_SYSTEM_MEDIA={spelling}:", cleaned.stdout)
                    self.assert_system_media_entry_absent(build)
                    values = self._cache_values(build)
                    for name, expected in unrelated.items():
                        self.assertEqual(values[name], expected)

                    reused = self._configure(source, build, on_linux=False)
                    self.assertEqual(reused.returncode, 0, reused.stdout + reused.stderr)
                    self.assertNotIn("Removing cached", reused.stdout)
                    self.assert_system_media_entry_absent(build)
                    values = self._cache_values(build)
                    for name, expected in unrelated.items():
                        self.assertEqual(values[name], expected)

    def test_removed_entry_gives_a_later_linux_configure_its_own_default(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = self._write_fixture_source(root)
            build = root / "build"
            build.mkdir()
            (build / "CMakeCache.txt").write_text(
                "AOBUS_BUILD_SYSTEM_MEDIA:BOOL=OFF\n",
                encoding="utf-8",
            )

            cleaned = self._configure(source, build, on_linux=False)
            self.assertEqual(cleaned.returncode, 0, cleaned.stdout + cleaned.stderr)
            self.assert_system_media_entry_absent(build)

            switched = self._configure(source, build, on_linux=True)
            self.assertEqual(switched.returncode, 0, switched.stdout + switched.stderr)
            # Without the cleanup this configure would inherit the stale OFF.
            self.assertEqual(self._cache_values(build)["AOBUS_BUILD_SYSTEM_MEDIA"], "ON")


if __name__ == "__main__":
    unittest.main()
