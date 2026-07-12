"""Tests for ao.core.builddir — flavor/preset/build-tree mapping."""

import os
import tempfile
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
            os.environ.pop("AOBUS_BUILD_ROOT", None)
            self.assertEqual(builddir.build_dir("debug", os_name="posix"), Path("/tmp/build/debug"))
            self.assertEqual(builddir.build_dir("release", os_name="posix"), Path("/tmp/build/release"))
            self.assertEqual(builddir.build_dir("profile", os_name="posix"), Path("/tmp/build/profile"))

    def test_linux_build_root_honors_aobus_build_root(self):
        self.assertEqual(builddir.linux_build_root(environ={}), Path("/tmp/build"))
        self.assertEqual(
            builddir.linux_build_root(environ={"AOBUS_BUILD_ROOT": "/var/cache/aobus"}),
            Path("/var/cache/aobus"),
        )
        environment = {"AOBUS_BUILD_ROOT": "/var/cache/aobus"}
        with mock.patch.dict("os.environ", environment, clear=False):
            os.environ.pop("BUILD_DIR", None)
            self.assertEqual(builddir.platform_profile("posix").build_root, Path("/var/cache/aobus"))
            self.assertEqual(builddir.build_dir("debug", os_name="posix"), Path("/var/cache/aobus/debug"))

    def test_native_profile_rejects_unsupported_platforms(self):
        with (
            mock.patch("ao.core.builddir.os.name", "posix"),
            mock.patch("ao.core.builddir.sys.platform", "darwin"),
        ):
            with self.assertRaisesRegex(RuntimeError, "darwin"):
                builddir.platform_profile()
            # Explicit os_name values keep working for cross-profile queries.
            self.assertEqual(builddir.platform_profile("posix").name, "linux")

    def test_windows_builds_and_tests_share_one_flavor_tree(self):
        environment = {
            "AOBUS_STATE_ROOT": "C:/Users/Alice/AppData/Local/Aobus",
            "AOBUS_CHECKOUT_ID": "checkout-a",
        }
        with mock.patch.dict("os.environ", environment, clear=True):
            root = builddir.windows_build_root()
            self.assertEqual(
                builddir.build_dir("debug", os_name="nt"),
                root / "windows-debug",
            )
            self.assertEqual(
                builddir.build_dir("release", os_name="nt"),
                root / "windows-release",
            )

    def test_windows_profile_exposes_only_built_apps_and_suites(self):
        profile = builddir.platform_profile("nt")
        self.assertEqual(profile.apps, ("cli", "tui"))
        self.assertEqual(profile.default_suites, ("core", "tui"))
        self.assertEqual(profile.all_suites, ("core", "tui", "cli", "integration", "tooling"))
        self.assertEqual(profile.tsan_suites, ())
        self.assertEqual(builddir.flavors("nt"), ("debug", "release"))

    def test_linux_profile_exposes_only_baselined_tsan_suites(self):
        profile = builddir.platform_profile("posix")

        self.assertEqual(profile.tsan_suites, ("core",))

    def test_tidy_uses_a_dedicated_native_preset_and_tree(self):
        with mock.patch.dict("os.environ", {}, clear=False):
            os.environ.pop("BUILD_DIR", None)
            os.environ.pop("AOBUS_BUILD_ROOT", None)
            self.assertEqual(builddir.tidy_preset("posix"), "linux-debug")
            self.assertEqual(builddir.tidy_dir("posix"), Path("/tmp/build/debug-clang-tidy"))
        self.assertEqual(builddir.tidy_preset("nt"), "windows-tidy")
        self.assertEqual(builddir.tidy_dir("nt"), builddir.windows_build_root() / "windows-tidy")

    def test_windows_defaults_to_checkout_isolated_local_app_data(self):
        environment = {
            "LOCALAPPDATA": "C:/Users/Alice/AppData/Local",
            "AOBUS_CHECKOUT_ID": "checkout-a",
        }

        root = builddir.windows_build_root(environ=environment, project_root=Path("Y:/"))

        self.assertEqual(root.parent, Path("C:/Users/Alice/AppData/Local/Aobus/build"))
        self.assertRegex(root.name, r"^[A-Za-z0-9._-]+-[0-9a-f]{12}$")
        self.assertNotIn("out", root.parts)

    def test_windows_build_root_overrides_state_root_but_keeps_checkout_isolation(self):
        environment = {
            "AOBUS_STATE_ROOT": "C:/ignored-state",
            "AOBUS_BUILD_ROOT": "D:/fast-builds",
            "AOBUS_CHECKOUT_ID": "checkout-a",
        }

        root = builddir.windows_build_root(environ=environment, project_root=Path("Y:/"))

        self.assertEqual(root.parent, Path("D:/fast-builds"))

    def test_checkout_key_is_stable_and_path_sensitive(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            first = Path(temp_dir) / "first" / "Aobus"
            second = Path(temp_dir) / "second" / "Aobus"
            (first / ".git").mkdir(parents=True)
            (second / ".git").mkdir(parents=True)

            first_key = builddir.windows_checkout_key(first, environ={}, create_id=True)
            repeated_key = builddir.windows_checkout_key(first, environ={}, create_id=True)
            second_key = builddir.windows_checkout_key(second, environ={}, create_id=True)

            self.assertEqual(first_key, repeated_key)
            self.assertNotEqual(first_key, second_key)
            self.assertTrue((first / ".git" / "aobus-checkout-id").is_file())

    def test_checkout_id_override_avoids_git_metadata_writes(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir) / "Aobus"
            (root / ".git").mkdir(parents=True)

            key = builddir.windows_checkout_key(
                root,
                environ={"AOBUS_CHECKOUT_ID": "ssh-mapping"},
                create_id=True,
            )

            self.assertTrue(key.startswith("Aobus-"))
            self.assertFalse((root / ".git" / "aobus-checkout-id").exists())

    def test_missing_git_directory_requires_an_explicit_checkout_id(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir) / "Aobus"
            root.mkdir()

            with self.assertRaisesRegex(RuntimeError, "AOBUS_CHECKOUT_ID"):
                builddir.windows_checkout_key(root, environ={}, create_id=True)

    def test_native_executable_suffix_is_profile_specific(self):
        path = Path("build/test/ao_core_test")
        self.assertEqual(builddir.executable(path, "posix"), path)
        self.assertEqual(builddir.executable(path, "nt"), Path("build/test/ao_core_test.exe"))

    def test_suffixes_combine_in_fixed_order(self):
        with mock.patch.dict("os.environ", {}, clear=False):
            os.environ.pop("BUILD_DIR", None)
            os.environ.pop("AOBUS_BUILD_ROOT", None)
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
            self.assertEqual(builddir.tidy_dir(), Path("/host/custom-build"))
            self.assertEqual(builddir.analyze_dir(), Path("/host/custom-build"))


if __name__ == "__main__":
    unittest.main()
