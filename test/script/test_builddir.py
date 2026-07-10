"""Tests for ao.core.builddir — flavor/preset/build-tree mapping."""

import os
import unittest
from pathlib import Path
from unittest import mock

from ao.core import builddir


class BuildDirTest(unittest.TestCase):
    def test_linux_profile_presets_cover_all_flavors(self):
        self.assertEqual(
            builddir.platform_profile("posix").presets,
            {
                "debug": "linux-debug",
                "release": "linux-release",
                "pgo1": "linux-pgo-profile",
                "pgo2": "linux-pgo-optimize",
                "profile": "profile",
            },
        )

    def test_linux_plain_flavors_map_to_their_own_tree(self):
        with mock.patch.dict("os.environ", {}, clear=False):
            os.environ.pop("BUILD_DIR", None)
            self.assertEqual(builddir.build_dir("debug", os_name="posix"), Path("/tmp/build/debug"))
            self.assertEqual(builddir.build_dir("release", os_name="posix"), Path("/tmp/build/release"))
            self.assertEqual(builddir.build_dir("profile", os_name="posix"), Path("/tmp/build/profile"))

    def test_windows_profile_separates_app_and_test_trees(self):
        with mock.patch.dict("os.environ", {}, clear=False):
            os.environ.pop("BUILD_DIR", None)
            root = builddir.WINDOWS_BUILD_ROOT
            self.assertEqual(
                builddir.build_dir("debug", os_name="nt"),
                root / "windows-tui-debug",
            )
            self.assertEqual(
                builddir.build_dir("debug", with_tests=True, os_name="nt"),
                root / "windows-tui-debug-tests",
            )
            self.assertEqual(
                builddir.build_dir("release", with_tests=True, os_name="nt"),
                root / "windows-tui-release-tests",
            )

    def test_windows_profile_exposes_only_built_apps_and_suites(self):
        profile = builddir.platform_profile("nt")
        self.assertEqual(profile.apps, ("tui",))
        self.assertEqual(profile.default_suites, ("core", "tui"))
        self.assertEqual(profile.all_suites, ("core", "tui", "integration"))
        self.assertEqual(builddir.flavors("nt"), ("debug", "release"))

    def test_tidy_uses_a_dedicated_native_preset_and_tree(self):
        self.assertEqual(builddir.tidy_preset("posix"), "linux-debug")
        self.assertEqual(builddir.tidy_dir("posix"), Path("/tmp/build/debug-clang-tidy"))
        self.assertEqual(builddir.tidy_preset("nt"), "windows-tidy")
        self.assertEqual(
            builddir.tidy_dir("nt"),
            builddir.WINDOWS_BUILD_ROOT / "windows-tidy",
        )

    def test_native_executable_suffix_is_profile_specific(self):
        path = Path("build/test/ao_core_test")
        self.assertEqual(builddir.executable(path, "posix"), path)
        self.assertEqual(builddir.executable(path, "nt"), Path("build/test/ao_core_test.exe"))

    def test_suffixes_combine_in_fixed_order(self):
        with mock.patch.dict("os.environ", {}, clear=False):
            os.environ.pop("BUILD_DIR", None)
            self.assertEqual(builddir.build_dir("debug", clang=True, os_name="posix"), Path("/tmp/build/debug-clang"))
            self.assertEqual(builddir.build_dir("debug", asan=True, os_name="posix"), Path("/tmp/build/debug-asan"))
            self.assertEqual(builddir.build_dir("debug", tsan=True, os_name="posix"), Path("/tmp/build/debug-tsan"))
            self.assertEqual(
                builddir.build_dir("debug", clang=True, asan=True, os_name="posix"),
                Path("/tmp/build/debug-clang-asan"),
            )

    def test_pgo_steps_share_one_tree(self):
        with mock.patch.dict("os.environ", {}, clear=False):
            os.environ.pop("BUILD_DIR", None)
            self.assertEqual(builddir.build_dir("pgo1", os_name="posix"), builddir.build_dir("pgo2", os_name="posix"))
            self.assertEqual(builddir.build_dir("pgo1", clang=True, os_name="posix"), Path("/tmp/build/pgo-clang"))

    def test_build_dir_env_always_wins(self):
        # External tooling can redirect builds via BUILD_DIR.
        with mock.patch.dict("os.environ", {"BUILD_DIR": "/host/custom-build"}):
            self.assertEqual(builddir.build_dir("debug", asan=True), Path("/host/custom-build"))


if __name__ == "__main__":
    unittest.main()
