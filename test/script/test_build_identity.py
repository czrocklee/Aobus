"""Build-tree identity regressions for configure and incremental test entry points."""

import contextlib
import io
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao.__main__ import make_parser
from ao.command import build, perf, test
from ao.core import builddir


class BuildIdentityTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.stack = contextlib.ExitStack()
        self.addCleanup(self.stack.close)
        self.stack.enter_context(mock.patch.object(builddir, "platform_profile", return_value=builddir.LINUX_PROFILE))
        self.stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
        self.stderr = self.stack.enter_context(contextlib.redirect_stderr(io.StringIO()))

    def write_configuration(self, compiler="GNU", *, asan=False, tsan=False, c_compiler=None, build_type="Release"):
        cache = self.directory / "CMakeCache.txt"
        cache.write_text(
            "CMAKE_CACHE_MAJOR_VERSION:INTERNAL=4\n"
            "CMAKE_CACHE_MINOR_VERSION:INTERNAL=1\n"
            "CMAKE_CACHE_PATCH_VERSION:INTERNAL=2\n"
            f"AOBUS_ENABLE_ASAN:BOOL={'ON' if asan else 'OFF'}\n"
            f"AOBUS_ENABLE_TSAN:BOOL={'ON' if tsan else 'OFF'}\n"
            f"CMAKE_BUILD_TYPE:STRING={build_type}\n",
            encoding="utf-8",
        )
        compiler_dir = self.directory / "CMakeFiles" / "4.1.2"
        compiler_dir.mkdir(parents=True, exist_ok=True)
        for language, identity in (("C", c_compiler or compiler), ("CXX", compiler)):
            (compiler_dir / f"CMake{language}Compiler.cmake").write_text(
                f'set(CMAKE_{language}_COMPILER_ID "{identity}")\n', encoding="utf-8"
            )

    def arguments(self, command, *options):
        return make_parser().parse_args([command, "-p", str(self.directory), *options])

    def test_every_configure_explicitly_sets_both_sanitizers(self):
        for options, asan, tsan in ((["--asan"], True, False), ([], False, False), (["--tsan"], False, True)):
            with self.subTest(options=options):
                self.write_configuration(asan=not asan, tsan=not tsan)
                args = self.arguments("build", *options)

                def configure(command, *, asan=asan, tsan=tsan, **_kwargs):
                    if "--preset" in command:
                        self.assertIn(f"-DAOBUS_ENABLE_ASAN={'ON' if asan else 'OFF'}", command)
                        self.assertIn(f"-DAOBUS_ENABLE_TSAN={'ON' if tsan else 'OFF'}", command)
                        self.write_configuration(asan=asan, tsan=tsan)
                    return 0

                with mock.patch.object(build, "run", side_effect=configure):
                    result = build.do_build(args, ["ao_core_test"])
                self.assertEqual(result.compiler, "gcc")

    def test_incompatible_compiler_is_rejected_before_configure_preserving_cache(self):
        cases = (
            (builddir.LINUX_PROFILE, "Clang", []),
            (builddir.LINUX_PROFILE, "GNU", ["--clang"]),
            (builddir.MACOS_PROFILE, "AppleClang", []),
            (builddir.WINDOWS_PROFILE, "Clang", []),
        )
        for profile, compiler, options in cases:
            with self.subTest(profile=profile.name, compiler=compiler, options=options):
                self.write_configuration(compiler)
                cache = (self.directory / "CMakeCache.txt").read_bytes()
                args = self.arguments("build", *options)
                with mock.patch.object(builddir, "platform_profile", return_value=profile):
                    with mock.patch.object(build, "run") as run, self.assertRaises(SystemExit):
                        build.do_build(args, [])
                run.assert_not_called()
                self.assertEqual((self.directory / "CMakeCache.txt").read_bytes(), cache)
                self.assertIn(compiler, self.stderr.getvalue())

    def test_c_compiler_mismatch_is_not_hidden_by_matching_cxx(self):
        self.write_configuration(c_compiler="Clang")
        with mock.patch.object(build, "run") as run, self.assertRaises(SystemExit):
            build.do_build(self.arguments("build"), [])
        run.assert_not_called()
        self.assertIn("Clang", self.stderr.getvalue())

    def test_configure_success_cannot_build_or_report_an_unexpected_identity(self):
        def configure(_command, **_kwargs):
            self.write_configuration("Clang")
            return 0

        with mock.patch.object(build, "run", side_effect=configure) as run, self.assertRaises(SystemExit):
            build.do_build(self.arguments("build"), [])
        self.assertEqual(run.call_count, 1)
        self.assertIn("Clang", self.stderr.getvalue())

    def test_incremental_and_no_build_tests_reject_incompatible_trees(self):
        for no_build in ([], ["--no-build"]):
            for options, compiler, asan, tsan in (
                (["--tsan"], "GNU", False, False),
                ([], "GNU", True, False),
                (["--asan"], "GNU", False, True),
                (["--clang"], "GNU", False, False),
            ):
                with self.subTest(no_build=no_build, options=options, compiler=compiler, asan=asan, tsan=tsan):
                    self.write_configuration(compiler, asan=asan, tsan=tsan)
                    args = self.arguments("test", "--core", *options, *no_build)
                    with mock.patch.object(test, "run") as run, mock.patch.object(test, "run_suites") as suites:
                        with self.assertRaises(SystemExit):
                            test.run_command(args)
                    run.assert_not_called()
                    suites.assert_not_called()

    def test_matching_configuration_runs_tests(self):
        for profile, compiler in (
            (builddir.LINUX_PROFILE, "GNU"),
            (builddir.MACOS_PROFILE, "Clang"),
            (builddir.WINDOWS_PROFILE, "MSVC"),
        ):
            with self.subTest(profile=profile.name):
                self.write_configuration(compiler)
                args = self.arguments("test", "--core", "--no-build")
                with mock.patch.object(builddir, "platform_profile", return_value=profile):
                    with mock.patch.object(test, "run_suites", return_value=0) as suites:
                        self.assertEqual(test.run_command(args), 0)
                suites.assert_called_once()

    def test_missing_compiler_metadata_cannot_be_treated_as_a_matching_tree(self):
        self.write_configuration()
        (self.directory / "CMakeFiles" / "4.1.2" / "CMakeCXXCompiler.cmake").unlink()
        with mock.patch.object(build, "run") as run, self.assertRaises(SystemExit):
            build.do_build(self.arguments("build"), [])
        run.assert_not_called()

    def test_no_build_perf_rejects_incompatible_identity_before_running_or_removing_report(self):
        executable = builddir.executable(self.directory / "test" / perf.TARGET)
        executable.parent.mkdir()
        executable.touch()
        output = self.directory / "review.json"
        for options, compiler, asan, tsan, build_type, diagnostic in (
            ([], "GNU", False, False, "Debug", "Build mode mismatch"),
            (["--clang"], "GNU", False, False, "Release", "Compiler mismatch"),
            ([], "GNU", True, False, "Release", "Sanitizer mismatch"),
            (["debug", "--asan"], "GNU", False, True, "Debug", "Sanitizer mismatch"),
            ([], "GNU", False, False, "", "Build mode mismatch"),
        ):
            with self.subTest(options=options, build_type=build_type, asan=asan, tsan=tsan):
                self.write_configuration(compiler, asan=asan, tsan=tsan, build_type=build_type)
                output.write_text("previous evidence", encoding="utf-8")
                args = self.arguments("perf", "--no-build", "--output", str(output), *options)
                self.stderr.seek(0)
                self.stderr.truncate()
                with mock.patch.object(perf, "run") as run, self.assertRaises(SystemExit):
                    perf.run_command(args)
                run.assert_not_called()
                self.assertEqual(output.read_text(encoding="utf-8"), "previous evidence")
                self.assertIn(diagnostic, self.stderr.getvalue())

    def test_performance_build_type_matches_each_supported_flavor(self):
        for flavor, build_type in (("debug", "Debug"), ("release", "Release"), ("profile", "RelWithDebInfo")):
            with self.subTest(flavor=flavor):
                self.write_configuration(build_type=build_type)
                self.assertEqual(
                    build.validate_build_tree(
                        self.arguments("perf", flavor), self.directory, expected_build_type=build_type
                    ),
                    "gcc",
                )

    def test_missing_test_tree_has_a_first_build_diagnostic(self):
        for no_build in ([], ["--no-build"]):
            with self.subTest(no_build=no_build):
                args = self.arguments("test", "--core", *no_build)
                args.path = str(self.directory / "missing")
                with mock.patch.object(test, "run_suites") as suites, self.assertRaises(SystemExit):
                    test.run_command(args)
                suites.assert_not_called()
                self.assertIn("does not exist. Run ./ao build first", self.stderr.getvalue())
                self.assertNotIn("--clean", self.stderr.getvalue())

    def test_vendor_cmake_version_suffix_retains_the_configured_compiler_identity(self):
        self.write_configuration("MSVC")
        metadata = self.directory / "CMakeFiles" / "4.1.2"
        metadata.rename(metadata.with_name("4.1.2-msvc1"))
        with mock.patch.object(builddir, "platform_profile", return_value=builddir.WINDOWS_PROFILE):
            self.assertEqual(build.validate_build_tree(self.arguments("build"), self.directory), "msvc")

    def test_ambiguous_cmake_metadata_is_rejected(self):
        self.write_configuration()
        (self.directory / "CMakeFiles" / "4.1.2-vendor").mkdir()
        with self.assertRaises(SystemExit):
            build.validate_build_tree(self.arguments("build"), self.directory)


if __name__ == "__main__":
    unittest.main()
