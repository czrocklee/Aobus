"""Tests for the C++ test registration guard."""

import tempfile
import unittest
from pathlib import Path

from ao.core import testregistry


class TestRegistryTest(unittest.TestCase):
    def test_real_test_sources_exclude_lint_fixtures(self):
        with tempfile.TemporaryDirectory(dir="/tmp") as temp_dir:
            root = Path(temp_dir)
            registered = root / "test" / "unit" / "utility" / "RegisteredTest.cpp"
            fixture = root / "test" / "integration" / "lint" / "fixture" / "RuleFixtureTest.cpp"
            registered.parent.mkdir(parents=True)
            fixture.parent.mkdir(parents=True)
            registered.touch()
            fixture.touch()

            self.assertEqual(testregistry.real_test_sources(root), ["unit/utility/RegisteredTest.cpp"])

    def test_registered_test_sources_ignore_comments_and_normalize_paths(self):
        with tempfile.TemporaryDirectory(dir="/tmp") as temp_dir:
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
        with tempfile.TemporaryDirectory(dir="/tmp") as temp_dir:
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
