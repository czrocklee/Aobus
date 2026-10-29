"""Tests for the governed CMake selection used by tooling fixtures."""

import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

import cmake_support
from cmake_support import cmake_executable

GOVERNED_COMPONENT = cmake_support._WINDOWS_CMAKE_COMPONENT


class CmakeExecutableTest(unittest.TestCase):
    def setUp(self):
        # A mocked discovery must never leak into later tests or real lookups.
        cmake_executable.cache_clear()
        self.addCleanup(cmake_executable.cache_clear)

    def temporary_directory(self) -> Path:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        return Path(temporary.name)

    @staticmethod
    def windows_os() -> SimpleNamespace:
        # Replace the module-level ``os`` reference instead of patching the
        # global os module so pathlib and other stdlib users stay untouched.
        return SimpleNamespace(name="nt")

    def test_posix_selects_cmake_without_windows_discovery(self):
        with mock.patch.object(cmake_support, "os", SimpleNamespace(name="posix")):
            with mock.patch.object(cmake_support.winui, "visual_studio_installation") as discover:
                self.assertEqual(cmake_executable(), "cmake")
        discover.assert_not_called()

    def test_windows_discovers_governed_component_and_caches_success(self):
        root = self.temporary_directory()
        executable = root / "cmake.exe"
        executable.write_bytes(b"")
        calls = []

        def discover(*, component):
            calls.append(component)
            return SimpleNamespace()

        with mock.patch.object(cmake_support, "os", self.windows_os()):
            with mock.patch.object(cmake_support.winui, "visual_studio_installation", side_effect=discover):
                with mock.patch.object(cmake_support.winui, "bundled_cmake", return_value=executable):
                    self.assertEqual(cmake_executable(), str(executable))
                    self.assertEqual(cmake_executable(), str(executable))
        self.assertEqual(calls, [GOVERNED_COMPONENT])

    def test_windows_without_component_fails_and_is_not_cached(self):
        root = self.temporary_directory()
        executable = root / "cmake.exe"
        executable.write_bytes(b"")

        with mock.patch.object(cmake_support, "os", self.windows_os()):
            with mock.patch.object(cmake_support.winui, "visual_studio_installation", return_value=None):
                with self.assertRaises(RuntimeError) as missing_component:
                    cmake_executable()
            self.assertIn(GOVERNED_COMPONENT, str(missing_component.exception))
            with mock.patch.object(cmake_support.winui, "visual_studio_installation", return_value=SimpleNamespace()):
                with mock.patch.object(cmake_support.winui, "bundled_cmake", return_value=executable):
                    self.assertEqual(cmake_executable(), str(executable))

    def test_windows_missing_bundled_executable_names_the_path(self):
        missing = self.temporary_directory() / "absent" / "cmake.exe"

        with mock.patch.object(cmake_support, "os", self.windows_os()):
            with mock.patch.object(cmake_support.winui, "visual_studio_installation", return_value=SimpleNamespace()):
                with mock.patch.object(cmake_support.winui, "bundled_cmake", return_value=missing):
                    with self.assertRaises(RuntimeError) as missing_executable:
                        cmake_executable()
        self.assertIn(str(missing), str(missing_executable.exception))


if __name__ == "__main__":
    unittest.main()
