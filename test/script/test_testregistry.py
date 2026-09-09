"""Tests for the C++ test registration guard."""

import contextlib
import io
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao.__main__ import make_parser
from ao.command import check
from ao.core import builddir, testregistry


class TestRegistryTest(unittest.TestCase):
    def test_real_test_sources_exclude_lint_fixtures(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            registered = root / "test" / "unit" / "utility" / "RegisteredTest.cpp"
            fixture = root / "test" / "integration" / "lint" / "fixture" / "RuleFixtureTest.cpp"
            registered.parent.mkdir(parents=True)
            fixture.parent.mkdir(parents=True)
            registered.touch()
            fixture.touch()

            self.assertEqual(testregistry.real_test_sources(root), ["unit/utility/RegisteredTest.cpp"])

    def test_registered_test_sources_ignore_comments_and_normalize_paths(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            cmake_file = root / "test" / "CMakeLists.txt"
            cmake_file.parent.mkdir()
            cmake_file.write_text(
                """\
add_executable(ao_core_test
    unit/utility/RegisteredTest.cpp
    ${CMAKE_SOURCE_DIR}/test/unit/core/AbsoluteTest.cpp
    # unit/utility/CommentedTest.cpp
)
""",
                encoding="utf-8",
            )

            self.assertEqual(
                testregistry.registered_test_sources(root),
                ["unit/core/AbsoluteTest.cpp", "unit/utility/RegisteredTest.cpp"],
            )

    def test_unregistered_test_sources_report_repo_relative_paths(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            cmake_file = root / "test" / "CMakeLists.txt"
            registered = root / "test" / "unit" / "utility" / "RegisteredTest.cpp"
            missing = root / "test" / "unit" / "utility" / "MissingTest.cpp"
            registered.parent.mkdir(parents=True)
            registered.touch()
            missing.touch()
            cmake_file.write_text(
                "add_executable(ao_core_test unit/utility/RegisteredTest.cpp)\n",
                encoding="utf-8",
            )

            self.assertEqual(
                testregistry.unregistered_test_sources(root),
                ["test/unit/utility/MissingTest.cpp"],
            )


class NativeCheckRegistryTest(unittest.TestCase):
    def test_unreadable_registry_reports_an_actionable_error_before_building(self):
        args = make_parser().parse_args(["check"])
        with mock.patch.object(
            testregistry, "unregistered_test_sources", side_effect=FileNotFoundError("test/CMakeLists.txt")
        ):
            with (
                mock.patch.object(check.build, "do_build") as build,
                contextlib.redirect_stderr(io.StringIO()) as errors,
            ):
                with self.assertRaises(SystemExit):
                    check.run_command(args)
        build.assert_not_called()
        self.assertIn("Cannot read the C++ test registry", errors.getvalue())
        self.assertIn("test/CMakeLists.txt", errors.getvalue())

    def test_every_profile_rejects_unregistered_sources_before_building(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "test").mkdir()
            (root / "test" / "UnregisteredTest.cpp").touch()
            (root / "test" / "CMakeLists.txt").write_text("", encoding="utf-8")
            missing = testregistry.unregistered_test_sources(root)
            for profile in (builddir.LINUX_PROFILE, builddir.WINDOWS_PROFILE, builddir.MACOS_PROFILE):
                with self.subTest(profile=profile.name):
                    with mock.patch.object(builddir, "platform_profile", return_value=profile):
                        args = make_parser().parse_args(["check"])
                        with mock.patch.object(testregistry, "unregistered_test_sources", return_value=missing):
                            with mock.patch.object(check.build, "do_build") as build:
                                with contextlib.redirect_stderr(io.StringIO()) as errors:
                                    with self.assertRaises(SystemExit):
                                        check.run_command(args)
                                self.assertIn("test/UnregisteredTest.cpp", errors.getvalue())
                                build.assert_not_called()


class RepositoryTestRegistryGuardTest(unittest.TestCase):
    def test_all_real_cpp_tests_are_registered_in_cmake(self):
        missing = testregistry.unregistered_test_sources()

        self.assertEqual(
            missing,
            [],
            "C++ test files must be registered in test/CMakeLists.txt:\n" + "\n".join(missing),
        )


if __name__ == "__main__":
    unittest.main()
