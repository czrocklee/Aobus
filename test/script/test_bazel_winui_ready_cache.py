"""WinUI selects an existing governed cache only when no explicit root is given."""

import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao.core import bazel_provision, bazel_winui, bazel_workspace


class SelectedPackages(Exception):
    pass


class ReadyWinUiCacheTest(unittest.TestCase):
    def test_default_ready_lookup_is_lazy_and_supplies_the_verified_root(self):
        with tempfile.TemporaryDirectory(prefix="aobus-ready-winui-") as temporary:
            root = Path(temporary).resolve()
            packages = root / "packages"
            packages.mkdir()
            environment = {"LOCALAPPDATA": str(root / "state")}
            with (
                mock.patch.object(bazel_provision, "restored_nuget_packages", return_value=packages) as ready,
                mock.patch.object(bazel_winui, "_locked_packages", side_effect=SelectedPackages) as selected,
            ):
                # Observe admission into the existing SDK verifier, not a fake
                # complete Windows provider or native MSBuild acceptance.
                with self.assertRaises(SelectedPackages):
                    bazel_winui.repository_plan(root / "output", {"host": "windows"}, environment)
                ready.assert_called_once_with(environment)
                selected.assert_called_once_with(packages)

    def test_invalid_explicit_root_never_falls_back_to_a_managed_cache(self):
        with tempfile.TemporaryDirectory(prefix="aobus-explicit-winui-") as temporary:
            root = Path(temporary)
            with mock.patch.object(bazel_provision, "restored_nuget_packages") as ready:
                with self.assertRaisesRegex(bazel_workspace.BazelWorkspaceError, "restored NuGet cache"):
                    bazel_winui.repository_plan(
                        root / "output", {"host": "windows"}, {"AOBUS_NUGET_PACKAGES_DIR": str(root / "missing")}
                    )
                ready.assert_not_called()

    def test_non_windows_consumers_do_not_look_for_a_managed_nuget_cache(self):
        with tempfile.TemporaryDirectory(prefix="aobus-no-winui-") as temporary:
            with mock.patch.object(bazel_provision, "restored_nuget_packages") as ready:
                for host in ("linux", "macos"):
                    bazel_winui.repository_plan(Path(temporary) / host, {"host": host}, {})
                ready.assert_not_called()
