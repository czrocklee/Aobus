"""Exercise the test-source guard against actual CMake executable targets."""

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

from ao.core import winui
from ao.core.paths import PROJECT_ROOT

CMAKE_HELPER = PROJECT_ROOT / "cmake" / "AobusTestSources.cmake"


class TestRegistryTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cmake = "cmake"
        if os.name == "nt":
            # Tooling-only commands intentionally do not enter an MSVC shell.
            # Use the governed VS installation, not an unrelated PATH CMake.
            installation = winui.visual_studio_installation(
                component="Microsoft.VisualStudio.Component.VC.CMake.Project"
            )
            if installation is None:
                raise RuntimeError("CMake registration fixtures require the configured Visual Studio CMake component")
            cls.cmake = str(winui.bundled_cmake(installation))

    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.build = self.root / "build"
        (self.root / "test").mkdir()
        (self.root / "main.cpp").write_text("int main() { return 0; }\n", encoding="utf-8")

    def write_test(self, relative: str) -> None:
        path = self.root / "test" / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("// Registration fixture.\n", encoding="utf-8")

    def configure(self, body: str, targets: str = "suite") -> subprocess.CompletedProcess[str]:
        (self.root / "CMakeLists.txt").write_text(
            f'''\
cmake_minimum_required(VERSION 3.30)
project(test_source_fixture LANGUAGES CXX)
include("{CMAKE_HELPER.as_posix()}")
{body}
aobus_finalize_test_sources("${{CMAKE_BINARY_DIR}}/aobus-test-sources.tsv" {targets})
''',
            encoding="utf-8",
        )
        return subprocess.run(
            [self.cmake, "-S", str(self.root), "-B", str(self.build)],
            capture_output=True,
            text=True,
            check=False,
            timeout=120,
        )

    def assert_configure_failed(self, body: str, diagnostic: str, targets: str = "suite") -> None:
        result = self.configure(body, targets)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn(diagnostic, " ".join((result.stdout + result.stderr).split()))

    def test_records_executable_membership_and_disabled_sources(self):
        self.write_test("core/RegisteredTest.cpp")
        self.write_test("core/GtkTest.mm")
        self.write_test("lint/fixture/NotARealTest.cpp")
        result = self.configure("""\
add_executable(suite main.cpp)
aobus_target_test_sources(suite TRUE "unused" test/core/RegisteredTest.cpp)
aobus_target_test_sources(gtk_suite FALSE "AOBUS_BUILD_GTK=OFF" test/core/GtkTest.mm)
""")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(
            (self.build / "aobus-test-sources.tsv").read_text(encoding="utf-8").splitlines(),
            [
                "source\tstate\towner-or-disabled-reason",
                "test/core/GtkTest.mm\tdisabled\tAOBUS_BUILD_GTK=OFF",
                "test/core/RegisteredTest.cpp\tconfigured\tsuite",
            ],
        )

    def test_support_sources_use_ordinary_target_sources(self):
        self.write_test("core/RegisteredTest.cpp")
        self.write_test("core/FirstTestSupport.cpp")
        self.write_test("core/SecondTestSupport.cpp")
        result = self.configure("""\
add_executable(suite main.cpp test/core/FirstTestSupport.cpp)
target_sources(suite PRIVATE test/core/SecondTestSupport.cpp)
aobus_target_test_sources(suite TRUE "unused" test/core/RegisteredTest.cpp)
""")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(
            (self.build / "aobus-test-sources.tsv").read_text(encoding="utf-8").splitlines(),
            [
                "source\tstate\towner-or-disabled-reason",
                "test/core/RegisteredTest.cpp\tconfigured\tsuite",
            ],
        )

    def test_helper_validates_sources_in_both_branches(self):
        self.write_test("core/RegisteredTest.cpp")
        self.write_test("core/FixtureTestSupport.cpp")
        self.write_test("core/FixtureTestSupport.mm")
        self.write_test("core/FixtureTest.h")
        (self.root / "OutsideTest.cpp").write_text("// Outside test/.\n", encoding="utf-8")
        invalid_path = "Test source must be a *Test.cpp or *Test.mm file below test/"
        for enabled in ("TRUE", "FALSE"):
            for source, diagnostic in (
                ("test/core/FixtureTestSupport.cpp", invalid_path),
                ("test/core/FixtureTestSupport.mm", invalid_path),
                ("test/core/FixtureTest.h", invalid_path),
                ("OutsideTest.cpp", invalid_path),
                ("test/../OutsideTest.cpp", invalid_path),
                ("test/core/MissingTest.cpp", "Declared test source does not exist: test/core/MissingTest.cpp"),
                ("$<$<BOOL:1>:test/core/RegisteredTest.cpp>", "Test source generator expressions are unsupported"),
            ):
                with self.subTest(enabled=enabled, source=source):
                    self.assert_configure_failed(
                        "add_executable(suite main.cpp)\n"
                        f'aobus_target_test_sources(suite {enabled} "other platform" '
                        f'test/core/RegisteredTest.cpp "{source}")',
                        diagnostic,
                    )

    def test_source_property_does_not_count_as_registration(self):
        self.write_test("core/PseudoRegisteredTest.cpp")
        self.assert_configure_failed(
            "set_property(SOURCE test/core/PseudoRegisteredTest.cpp PROPERTY COMPILE_DEFINITIONS FAKE_REGISTRATION)",
            "Missing test target sources",
        )

    def test_missing_source_fails_even_after_a_previous_successful_configure(self):
        self.write_test("core/RegisteredTest.cpp")
        body = "add_executable(suite main.cpp test/core/RegisteredTest.cpp)"
        result = self.configure(body)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.write_test("core/NewTest.mm")
        self.assert_configure_failed(body, "test/core/NewTest.mm")

    def test_unselected_targets_cannot_hide_unregistered_tests(self):
        self.write_test("core/OrphanTest.cpp")
        for declaration in (
            "add_custom_target(orphan SOURCES test/core/OrphanTest.cpp)",
            "add_library(orphan STATIC test/core/OrphanTest.cpp)",
            "add_library(orphan OBJECT test/core/OrphanTest.cpp)",
            "add_executable(orphan main.cpp test/core/OrphanTest.cpp)",
        ):
            with self.subTest(declaration=declaration):
                self.assert_configure_failed(
                    "add_executable(suite main.cpp)\n" + declaration, "Missing test target sources"
                )

    def test_source_owners_must_be_nonimported_executables(self):
        self.write_test("core/OrphanTest.cpp")
        for declaration in (
            "add_custom_target(suite SOURCES test/core/OrphanTest.cpp)",
            "add_library(suite STATIC test/core/OrphanTest.cpp)",
            "add_library(suite OBJECT test/core/OrphanTest.cpp)",
            "add_executable(suite IMPORTED)",
        ):
            with self.subTest(declaration=declaration):
                self.assert_configure_failed(declaration, "must be a non-imported executable")

    def test_noncompiled_source_properties_are_rejected(self):
        self.write_test("core/HeaderOnlyTest.cpp")
        for property_name in ("HEADER_FILE_ONLY", "EXTERNAL_OBJECT"):
            with self.subTest(property_name=property_name):
                self.assert_configure_failed(
                    "add_executable(suite main.cpp test/core/HeaderOnlyTest.cpp)\n"
                    f"set_property(SOURCE test/core/HeaderOnlyTest.cpp PROPERTY {property_name} TRUE)",
                    "Test source must be compiled",
                )

    def test_source_on_multiple_executables_fails(self):
        self.write_test("core/DuplicateTest.cpp")
        self.assert_configure_failed(
            "add_executable(first main.cpp test/core/DuplicateTest.cpp)\n"
            "add_executable(second main.cpp test/core/DuplicateTest.cpp)",
            "belongs to more than one executable",
            "first second",
        )

    def test_duplicate_source_on_one_executable_fails(self):
        self.write_test("core/DuplicateTest.cpp")
        self.assert_configure_failed(
            "add_executable(suite main.cpp test/core/DuplicateTest.cpp test/core/DuplicateTest.cpp)",
            "listed more than once",
        )

    def test_disabled_source_cannot_also_be_configured(self):
        self.write_test("core/ContradictoryTest.cpp")
        self.assert_configure_failed(
            "add_executable(suite main.cpp test/core/ContradictoryTest.cpp)\n"
            'aobus_target_test_sources(unavailable FALSE "other platform" test/core/ContradictoryTest.cpp)',
            "both configured and disabled",
        )

    def test_disabled_reason_cannot_break_manifest_records(self):
        self.write_test("core/DisabledTest.cpp")
        for reason in ("", "other;platform", "other\tplatform"):
            with self.subTest(reason=reason):
                self.assert_configure_failed(
                    f'aobus_target_test_sources(unavailable FALSE "{reason}" test/core/DisabledTest.cpp)',
                    "single-line reason without semicolons",
                )

    def test_source_generator_expression_is_rejected(self):
        self.write_test("core/GeneratedTest.cpp")
        self.assert_configure_failed(
            'add_executable(suite main.cpp "$<$<BOOL:1>:test/core/GeneratedTest.cpp>")',
            "unsupported source generator expression",
        )


if __name__ == "__main__":
    unittest.main()
