"""Windows selection of the official rules_cc auto-config and its repo_env ingress."""

import json
import os
import tempfile
import unittest
from pathlib import Path, PureWindowsPath
from unittest.mock import patch

from ao.core import bazel_native as native
from ao.core import bazel_providers as providers
from ao.core import bazel_workspace as workspace

ROOT = Path(__file__).resolve().parents[2]


# The request channel keeps the original selected strings for dependency and
# WinUI admission; the upstream selectors are derived, never captured verbatim.
_CAPTURED_ENVIRONMENT = (
    "INCLUDE",
    "LIB",
    "PATH",
    "TMP",
    "VCToolsInstallDir",
    "WindowsSdkDir",
    "WindowsSDKVersion",
    "VCToolsVersion",
    "UCRTVersion",
    "NETFXSDKDir",
)


class WindowsToolchainPreparationTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-msvc-ingress-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.build = self.root / "build"
        self.selected = {
            "INCLUDE": (
                "C:\\VS\\2022\\Community\\VC\\Tools\\MSVC\\14.44.35207\\include;C:\\SDK\\Include\\10.0.26100.0\\um;"
            ),
            "LIB": (
                "C:\\VS\\2022\\Community\\VC\\Tools\\MSVC\\14.44.35207\\lib\\x64;C:\\SDK\\Lib\\10.0.26100.0\\um\\x64;"
            ),
            "PATH": "C:\\VS\\2022\\Community\\VC\\Tools\\MSVC\\14.44.35207\\bin\\Hostx64\\x64;C:\\Windows\\System32",
            "TMP": "C:\\Users\\fixture\\AppData\\Local\\Temp",
            "VCToolsInstallDir": "C:\\VS\\2022\\Community\\VC\\Tools\\MSVC\\14.44.35207\\",
            "WindowsSdkDir": "C:\\SDK\\",
            "WindowsSDKVersion": "10.0.26100.0\\",
            "VCToolsVersion": "14.44.35207",
            "UCRTVersion": "10.0.26100.0",
            "NETFXSDKDir": "C:\\Windows Kits\\NETFXSDK\\selected-netfx\\",
            "VSINSTALLDIR": "C:\\VS\\2022\\Community\\",
        }

    def prepare(self, environment):
        with (
            patch.dict(os.environ, environment, clear=True),
            patch.object(native, "native_platform", return_value="windows"),
            patch.object(providers, "vcpkg_prefix", return_value=self.root / "provider"),
            patch.object(workspace.builddir, "windows_state_root", return_value=self.root / "state"),
        ):
            return workspace.prepare_workspace(self.build, "debug")

    def repo_env(self, prepared):
        values = {}
        for line in (prepared / "aobus.bazelrc").read_text(encoding="utf-8").splitlines():
            # Only repository-environment options are JSON-quoted; plain options
            # such as --experimental_install_base_gc_max_age are not.
            if not line.startswith('common "--repo_env='):
                continue
            key, _, value = json.loads(line[len("common ") :])[len("--repo_env=") :].partition("=")
            values[key] = value
        return values

    def test_case_insensitive_windows_capture_stays_request_only_and_keeps_original_strings(self):
        prepared = self.prepare({name.swapcase(): value for name, value in self.selected.items()})
        request = json.loads((prepared / "request.json").read_text(encoding="utf-8"))
        self.assertEqual(
            {name: request["compiler_environment"][name] for name in _CAPTURED_ENVIRONMENT},
            {name: self.selected[name] for name in _CAPTURED_ENVIRONMENT},
        )
        # The request is the only channel for the selected ABI; the launcher rc
        # carries the derived upstream selectors instead.
        rc = (prepared / "aobus.bazelrc").read_text(encoding="utf-8")
        self.assertNotIn("--repo_env=INCLUDE=", rc)
        self.assertNotIn("--repo_env=LIB=", rc)
        self.assertNotIn(self.selected["TMP"], rc)

    def test_absent_compiler_capture_fields_remain_empty_in_the_watched_request(self):
        environment = dict(self.selected)
        for name in ("INCLUDE", "LIB", "TMP", "UCRTVersion", "NETFXSDKDir"):
            del environment[name]
        prepared = self.prepare(environment)
        request = json.loads((prepared / "request.json").read_text(encoding="utf-8"))
        self.assertEqual(
            request["compiler_environment"],
            {name: environment.get(name, "") for name in _CAPTURED_ENVIRONMENT},
        )

    def test_selected_vs_and_sdk_versions_feed_the_upstream_auto_config_selectors(self):
        prepared = self.prepare(dict(self.selected))
        repo_env = self.repo_env(prepared)
        self.assertEqual(repo_env["BAZEL_VC"], (PureWindowsPath(self.selected["VSINSTALLDIR"]) / "VC").as_posix())
        self.assertEqual(repo_env["BAZEL_VC_FULL_VERSION"], self.selected["VCToolsVersion"])
        self.assertEqual(repo_env["BAZEL_WINSDK_FULL_VERSION"], "10.0.26100.0")
        # The dependency and WinUI admission keeps the original separator-bearing
        # spelling; only the upstream selector is normalized.
        request = json.loads((prepared / "request.json").read_text(encoding="utf-8"))
        self.assertEqual(request["compiler_environment"]["WindowsSDKVersion"], self.selected["WindowsSDKVersion"])

    def test_missing_required_selected_vs_metadata_fails_closed_without_fallback(self):
        for name in ("VSINSTALLDIR", "VCToolsVersion", "WindowsSDKVersion"):
            for absent in ("deleted", "empty", "blank"):
                with self.subTest(name=name, absent=absent):
                    environment = dict(self.selected)
                    if absent == "deleted":
                        del environment[name]
                    elif absent == "empty":
                        environment[name] = ""
                    else:
                        environment[name] = " "
                    with (
                        patch.object(workspace.subprocess, "run", side_effect=AssertionError("no VS reselection")),
                        self.assertRaisesRegex(native.BazelWorkspaceError, name),
                    ):
                        self.prepare(environment)
        # A separator-only SDK version normalizes to nothing and is not a selection.
        with self.assertRaisesRegex(native.BazelWorkspaceError, "WindowsSDKVersion"):
            self.prepare(dict(self.selected, WindowsSDKVersion="\\"))

    def test_conflicting_explicit_bazel_selectors_fail_closed_with_case_and_slash_insensitive_comparison(self):
        for name, conflict in (
            ("BAZEL_VC", "D:\\Other\\VC"),
            ("BAZEL_VC_FULL_VERSION", "14.43-other"),
            ("BAZEL_WINSDK_FULL_VERSION", "10.0.other"),
        ):
            with self.subTest(name=name, conflict=conflict):
                with self.assertRaisesRegex(native.BazelWorkspaceError, name):
                    self.prepare(dict(self.selected, **{name: conflict}))
        # Equivalent spellings of the selected values are admitted, and the
        # derived selection is emitted, never the ambient spelling.
        equivalent = {
            "BAZEL_VC": (PureWindowsPath(self.selected["VSINSTALLDIR"]) / "VC").as_posix().upper(),
            "BAZEL_VC_FULL_VERSION": self.selected["VCToolsVersion"].upper(),
            "BAZEL_WINSDK_FULL_VERSION": self.selected["WindowsSDKVersion"].rstrip("\\/").upper(),
        }
        repo_env = self.repo_env(self.prepare(dict(self.selected, **equivalent)))
        self.assertEqual(repo_env["BAZEL_VC"], (PureWindowsPath(self.selected["VSINSTALLDIR"]) / "VC").as_posix())
        self.assertEqual(repo_env["BAZEL_VC_FULL_VERSION"], self.selected["VCToolsVersion"])
        self.assertEqual(repo_env["BAZEL_WINSDK_FULL_VERSION"], "10.0.26100.0")

    def test_prepare_selects_git_for_windows_bash_unless_the_caller_names_a_shell(self):
        git = self.root / "Git/cmd/git.exe"
        bash = self.root / "Git/usr/bin/bash.exe"
        for path in (git, bash):
            path.parent.mkdir(parents=True)
            path.write_bytes(b"")

        def shell(extra, *, found=str(git)):
            with patch.object(workspace.shutil, "which", return_value=found) as which:
                prepared = self.prepare({**self.selected, **extra})
            rc = (prepared / "aobus.bazelrc").read_text(encoding="utf-8")
            selected = [line for line in rc.splitlines() if "--repo_env=BAZEL_SH=" in line]
            return [json.loads(line.split(" ", 1)[1]).partition("=")[2].partition("=")[2] for line in selected], which

        selected, which = shell({})
        self.assertEqual(selected, [str(bash.resolve())])
        which.assert_called_once_with("git")
        explicit = str(self.root / "msys64/usr/bin/bash.exe")
        selected, which = shell({"BAZEL_SH": explicit})
        self.assertEqual(selected, [explicit])
        which.assert_not_called()
        bash.unlink()
        self.assertEqual(shell({})[0], [])
        self.assertEqual(shell({}, found=None)[0], [])


if __name__ == "__main__":
    unittest.main()
