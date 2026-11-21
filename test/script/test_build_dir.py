"""Tests for ao.portal.build_dir — state/checkout roots and platform profiles."""

import os
import runpy
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao.portal import build_dir


class LinuxBuildRootTest(unittest.TestCase):
    def test_linux_build_root_is_project_specific_and_host_overridable(self):
        project_root = Path("/home/alice/src/renamed-aobus")
        self.assertEqual(
            build_dir.linux_build_root(environ={}, project_root=project_root),
            Path("/tmp/build/renamed-aobus"),
        )
        self.assertEqual(
            build_dir.linux_build_root(environ={"AOBUS_BUILD_ROOT": "/var/cache/aobus"}, project_root=project_root),
            Path("/var/cache/aobus/renamed-aobus"),
        )
        self.assertEqual(
            build_dir.linux_build_root(environ={"AOBUS_BUILD_ROOT": ""}, project_root=project_root),
            Path("/tmp/build/renamed-aobus"),
        )

    def test_linux_profile_build_root_follows_the_environment_override(self):
        environment = {"AOBUS_BUILD_ROOT": "/var/cache/aobus"}
        with mock.patch.dict("os.environ", environment, clear=False):
            root = Path("/var/cache/aobus") / build_dir.PROJECT_ROOT.name
            self.assertEqual(build_dir.platform_profile("posix").build_root, root)


class StateRootTest(unittest.TestCase):
    @mock.patch.object(Path, "home", side_effect=RuntimeError("selected roots require no host profile"))
    def test_state_root_honors_override_and_platform_defaults(self, _home):
        self.assertEqual(
            build_dir.state_root(environ={"AOBUS_STATE_ROOT": "/chosen"}, system="Linux"),
            Path("/chosen"),
        )
        self.assertEqual(
            build_dir.state_root(environ={"HOME": "/users/me"}, system="Linux"),
            Path("/users/me/.cache/Aobus"),
        )
        self.assertEqual(
            build_dir.state_root(environ={"XDG_CACHE_HOME": "/xdg"}, system="Linux"),
            Path("/xdg/Aobus"),
        )
        self.assertEqual(
            build_dir.state_root(environ={"HOME": "/Users/me"}, system="Darwin"),
            Path("/Users/me/Library/Caches/Aobus"),
        )
        self.assertEqual(
            build_dir.state_root(environ={"LOCALAPPDATA": "C:/Users/me/AppData/Local"}, system="Windows"),
            Path("C:/Users/me/AppData/Local/Aobus"),
        )

    def test_windows_state_root_honors_override_localappdata_and_home_fallback(self):
        self.assertEqual(
            build_dir.windows_state_root(environ={"AOBUS_STATE_ROOT": "C:/chosen"}),
            Path("C:/chosen"),
        )
        self.assertEqual(
            build_dir.windows_state_root(environ={"LOCALAPPDATA": "C:/Users/Alice/AppData/Local"}),
            Path("C:/Users/Alice/AppData/Local/Aobus"),
        )
        with mock.patch("pathlib.Path.home", return_value=Path("C:/Users/Alice")):
            self.assertEqual(build_dir.windows_state_root(environ={}), Path("C:/Users/Alice/AppData/Local/Aobus"))

    def test_state_root_keeps_a_present_but_empty_selection(self):
        # Present-but-empty stays a selection, not a fallback to the host.
        with mock.patch("pathlib.Path.home", side_effect=RuntimeError("host home must stay unused")):
            self.assertEqual(build_dir.state_root(environ={"HOME": ""}, system="Linux"), Path(".cache/Aobus"))
            self.assertEqual(build_dir.state_root(environ={"LOCALAPPDATA": ""}, system="Windows"), Path("Aobus"))

    def test_state_root_falls_back_to_the_host_only_when_the_key_is_absent(self):
        with mock.patch("pathlib.Path.home", return_value=Path("/host/home")):
            self.assertEqual(build_dir.state_root(environ={}, system="Linux"), Path("/host/home/.cache/Aobus"))
            self.assertEqual(build_dir.state_root(environ={}, system="Darwin"), Path("/host/home/Library/Caches/Aobus"))


class CheckoutKeyTest(unittest.TestCase):
    def test_checkout_key_is_stable_and_path_sensitive(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            first = Path(temp_dir) / "first" / "Aobus"
            second = Path(temp_dir) / "second" / "Aobus"
            (first / ".git").mkdir(parents=True)
            (second / ".git").mkdir(parents=True)

            first_key = build_dir.windows_checkout_key(first, environ={}, create_id=True)
            repeated_key = build_dir.windows_checkout_key(first, environ={}, create_id=True)
            second_key = build_dir.windows_checkout_key(second, environ={}, create_id=True)

            self.assertEqual(first_key, repeated_key)
            self.assertNotEqual(first_key, second_key)
            self.assertTrue((first / ".git" / "aobus-checkout-id").is_file())

    def test_checkout_key_reuses_one_tree_for_mapped_and_unc_spellings(self):
        environment = {"AOBUS_CHECKOUT_ID": "checkout-a"}
        mapped_root = Path("Y:/work/Aobus")
        unc_root = Path("//server/share/work/Aobus")

        with mock.patch.object(
            build_dir,
            "_windows_canonical_path",
            side_effect=lambda path: r"\\server\share\work\Aobus" if path.lower().startswith("y:") else None,
        ):
            mapped_key = build_dir.windows_checkout_key(mapped_root, environ=environment)
            unc_key = build_dir.windows_checkout_key(unc_root, environ=environment)

        self.assertEqual(mapped_key, unc_key)

    def test_checkout_key_reuses_one_tree_for_a_local_junction_and_target(self):
        environment = {"AOBUS_CHECKOUT_ID": "checkout-a"}
        junction_root = Path("C:/junction/Aobus")
        target_root = Path("D:/source/Aobus")

        with mock.patch.object(
            build_dir,
            "_windows_canonical_path",
            side_effect=lambda path: r"D:\source\Aobus" if path.lower().startswith("c:") else path,
        ):
            junction_key = build_dir.windows_checkout_key(junction_root, environ=environment)
            target_key = build_dir.windows_checkout_key(target_root, environ=environment)

        self.assertEqual(junction_key, target_key)

    def test_windows_canonical_path_normalizes_dos_and_unc_namespaces(self):
        cases = (
            (r"\\?\UNC\server\share\repo", r"\\server\share\repo"),
            (r"\\?\unc\SERVER\Share\repo", r"\\SERVER\Share\repo"),
            (r"\\server\share\repo", r"\\server\share\repo"),
            (r"C:\local\repo", r"C:\local\repo"),
            (r"\\?\C:\local\repo", r"C:\local\repo"),
            (r"\\.\C:\local\repo", None),
        )

        for resolved, expected in cases:
            with self.subTest(resolved=resolved):
                with (
                    mock.patch.object(build_dir.os, "name", "nt"),
                    mock.patch.object(build_dir.os.path, "realpath", return_value=resolved),
                ):
                    self.assertEqual(build_dir._windows_canonical_path(r"Z:\repo"), expected)

    def test_windows_canonical_path_handles_resolution_failures(self):
        for failure in (OSError(53, "network path unavailable"), ValueError("invalid path")):
            with self.subTest(failure=failure):
                with (
                    mock.patch.object(build_dir.os, "name", "nt"),
                    mock.patch.object(build_dir.os.path, "realpath", side_effect=failure),
                ):
                    self.assertIsNone(build_dir._windows_canonical_path(r"Z:\repo"))

    def test_checkout_id_override_avoids_git_metadata_writes(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir) / "Aobus"
            (root / ".git").mkdir(parents=True)

            key = build_dir.windows_checkout_key(
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
                build_dir.windows_checkout_key(root, environ={}, create_id=True)


class WindowsBuildRootTest(unittest.TestCase):
    def test_windows_defaults_to_checkout_isolated_local_app_data(self):
        environment = {
            "LOCALAPPDATA": "C:/Users/Alice/AppData/Local",
            "AOBUS_CHECKOUT_ID": "checkout-a",
        }

        root = build_dir.windows_build_root(environ=environment, project_root=Path("Y:/"))

        self.assertEqual(root.parent, Path("C:/Users/Alice/AppData/Local/Aobus/build"))
        self.assertRegex(root.name, r"^[A-Za-z0-9._-]+-[0-9a-f]{12}$")
        self.assertNotIn("out", root.parts)

    def test_windows_build_root_overrides_state_root_but_keeps_checkout_isolation(self):
        environment = {
            "AOBUS_STATE_ROOT": "C:/ignored-state",
            "AOBUS_BUILD_ROOT": "D:/fast-builds",
            "AOBUS_CHECKOUT_ID": "checkout-a",
        }

        root = build_dir.windows_build_root(environ=environment, project_root=Path("Y:/"))

        self.assertEqual(root.parent, Path("D:/fast-builds"))


class PlatformProfileTest(unittest.TestCase):
    def test_import_and_windows_profile_name_do_not_access_host_storage(self):
        with (
            mock.patch.dict(os.environ, clear=True),
            mock.patch.object(Path, "home", side_effect=RuntimeError("host home unavailable")),
            mock.patch.object(Path, "stat", side_effect=AssertionError("unexpected checkout stat")),
            mock.patch.object(Path, "open", side_effect=AssertionError("unexpected checkout open")),
        ):
            # Execute the module afresh instead of using the test suite's cached import.
            isolated = runpy.run_path(build_dir.__file__, run_name="ao.portal._build_dir_import_test")
            profile = isolated["platform_profile"]("nt")
            self.assertEqual(profile.name, "windows")
            with self.assertRaisesRegex(RuntimeError, "host home unavailable"):
                _ = profile.build_root

    def test_linux_profile_selects_the_current_host_storage_override(self):
        with mock.patch.dict(os.environ, {"AOBUS_BUILD_ROOT": "/tmp/earlier"}, clear=True):
            profile = build_dir.platform_profile("posix")
        self.assertEqual(profile.name, "linux")
        with mock.patch.dict(os.environ, {"AOBUS_BUILD_ROOT": "/tmp/native-checks"}, clear=True):
            self.assertEqual(profile.build_root, Path("/tmp/native-checks") / build_dir.PROJECT_ROOT.name)

    def test_macos_is_a_native_posix_profile_distinct_from_linux(self):
        # os.name is "posix" on macOS too, so only sys.platform separates them.
        host_os = mock.Mock(environ=os.environ)
        host_os.name = "posix"
        with (
            mock.patch.dict(os.environ, {"AOBUS_BUILD_ROOT": "/tmp/earlier"}, clear=True),
            mock.patch("ao.portal.build_dir.os", host_os),
            mock.patch("ao.portal.build_dir.sys.platform", "darwin"),
        ):
            profile = build_dir.platform_profile()
            self.assertEqual(profile.name, "macos")
            # An explicit os_name still selects the Linux profile.
            self.assertEqual(build_dir.platform_profile("posix").name, "linux")
            with mock.patch.dict(os.environ, {"AOBUS_BUILD_ROOT": "/tmp/mac-builds"}, clear=True):
                self.assertEqual(profile.build_root, Path("/tmp/mac-builds") / build_dir.PROJECT_ROOT.name)

    def test_windows_profile_selects_checkout_isolated_host_storage(self):
        environment = {"AOBUS_CHECKOUT_ID": "checkout-a", "LOCALAPPDATA": "C:/Users/Alice/AppData/Local"}
        with mock.patch.dict(os.environ, environment, clear=True):
            profile = build_dir.platform_profile("nt")
            default = profile.build_root
            with mock.patch.dict(os.environ, {"AOBUS_BUILD_ROOT": "D:/native/checkouts"}):
                overridden = profile.build_root

        self.assertEqual(profile.name, "windows")
        self.assertEqual(default.parent, Path("C:/Users/Alice/AppData/Local/Aobus/build"))
        self.assertRegex(default.name, r"^[A-Za-z0-9._-]+-[0-9a-f]{12}$")
        self.assertEqual(overridden, Path("D:/native/checkouts") / default.name)


if __name__ == "__main__":
    unittest.main()
