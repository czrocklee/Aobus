"""Windows selection of the official rules_cc auto-config and its repo_env ingress."""

import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path, PureWindowsPath
from unittest.mock import Mock, patch

from ao.core import bazel_native as native
from ao.core import bazel_providers as providers
from ao.core import bazel_workspace as workspace

ROOT = Path(__file__).resolve().parents[2]

# The pinned rules_cc auto-config splits BAZEL_COPTS/BAZEL_CXXOPTS on ':'
# unless it is %-escaped (lib_cc_configure.bzl: escape_string/split_escaped):
# a literal '%' is '%%' and a ':' inside one flag is '%:'.
WINDOWS_CXXOPTS = [
    "/std:c++latest",
    "/external:W0",
    "/utf-8",
    "/permissive-",
    "/Zc:__cplusplus",
    "/Zc:preprocessor",
    "/DWIN32_LEAN_AND_MEAN",
    "/DUNICODE",
    "/D_UNICODE",
    "/DWINVER=0x0A00",
]

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


def _escape(flags):
    return ":".join(flag.replace("%", "%%").replace(":", "%:") for flag in flags)


def _split_escaped(value):
    """Mirror upstream split_escaped: '%' escapes the following character."""
    flags, current, index = [], "", 0
    while index < len(value):
        if value[index] == "%":
            current += value[index + 1]
            index += 2
        elif value[index] == ":":
            flags.append(current)
            current = ""
            index += 1
        else:
            current += value[index]
            index += 1
    return [*flags, current]


class WindowsAutoConfigIngressTest(unittest.TestCase):
    """The static workspace rc selects and feeds the official auto-configuration."""

    def setUp(self):
        lines = (ROOT / ".bazelrc").read_text(encoding="utf-8").splitlines()
        self.active = [line for line in lines if line.strip() and not line.startswith("#")]

    def test_official_auto_config_is_opted_in_only_on_windows(self):
        self.assertIn("common --repo_env=BAZEL_DO_NOT_DETECT_CPP_TOOLCHAIN=1", self.active)
        self.assertIn("build:windows --repo_env=BAZEL_DO_NOT_DETECT_CPP_TOOLCHAIN=0", self.active)
        self.assertEqual(
            [line for line in self.active if "BAZEL_DO_NOT_DETECT_CPP_TOOLCHAIN" in line],
            [
                "common --repo_env=BAZEL_DO_NOT_DETECT_CPP_TOOLCHAIN=1",
                "build:windows --repo_env=BAZEL_DO_NOT_DETECT_CPP_TOOLCHAIN=0",
            ],
        )

    def test_windows_no_longer_registers_a_custom_toolchain(self):
        self.assertFalse(any("registered_msvc" in line for line in self.active))
        self.assertFalse(any(line.startswith("build:windows --extra_toolchains") for line in self.active))
        # The governed Unix toolchains keep their explicit registrations.
        self.assertIn("build:linux --extra_toolchains=@aobus_toolchain//:registered_nix_clang", self.active)
        self.assertIn("build:macos --extra_toolchains=@aobus_toolchain//:registered_llvm22", self.active)

    def test_compiler_policy_enters_only_through_the_upstream_repo_env_channel(self):
        self.assertIn("build:windows --repo_env=BAZEL_COPTS=/W4", self.active)
        self.assertIn("build:windows --repo_env=BAZEL_CXXOPTS=" + _escape(WINDOWS_CXXOPTS), self.active)
        cxxopts = next(entry for entry in self.active if "--repo_env=BAZEL_CXXOPTS=" in entry)
        self.assertEqual(_split_escaped(cxxopts.split("=", 2)[2]), WINDOWS_CXXOPTS)
        self.assertIn("build:windows --repo_env=BAZEL_WIN32_WINNT=/D_WIN32_WINNT=0x0A00", self.active)
        for channel in ("BAZEL_COPTS", "BAZEL_CXXOPTS", "BAZEL_WIN32_WINNT"):
            self.assertTrue(
                all(entry.startswith("build:windows") for entry in self.active if f"--repo_env={channel}=" in entry),
                channel,
            )

    def test_warning_promotion_stays_feature_scoped_without_source_path_regexes(self):
        # /WX comes from the repository-scoped standard feature, never a global
        # flag or a per-source exemption regex; third parties stay exempt.
        self.assertFalse(any("/WX" in line for line in self.active))
        self.assertFalse(any("-Werror" in line for line in self.active))
        self.assertFalse(any("--per_file_copt" in line for line in self.active))

    def test_windows_external_include_paths_cover_target_and_host_configurations(self):
        self.assertIn("build:windows --features=external_include_paths", self.active)
        self.assertIn("build:windows --host_features=external_include_paths", self.active)
        for line in self.active:
            if "external_include_paths" in line:
                self.assertTrue(line.startswith("build:windows"), line)

    def test_windows_selects_msvc_cl_and_keeps_ambient_response_channels_out(self):
        self.assertIn("build:windows --repo_env=USE_CLANG_CL=0", self.active)
        # The C-only inlet stays unset globally; the ambient MSVC response-file
        # and link channels are unset only for the Windows configuration.
        self.assertIn("common --repo_env==BAZEL_CONLYOPTS", self.active)
        for name in ("CL", "_CL_", "LINK", "_LINK_"):
            self.assertIn(f"build:windows --repo_env=={name}", self.active)
            self.assertFalse(any(line.startswith(f"common --repo_env=={name}") for line in self.active), name)


class WindowsToolchainPreparationTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-msvc-ingress-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.build = self.root / "build"
        self.selected = {
            "INCLUDE": "C:\\VS\\VC\\Tools\\MSVC\\selected\\include;C:\\SDK\\Include\\10.0.selected\\um;",
            "LIB": "C:\\VS\\VC\\Tools\\MSVC\\selected\\lib\\x64;C:\\SDK\\Lib\\10.0.selected\\um\\x64;",
            "PATH": "C:\\VS\\VC\\Tools\\MSVC\\selected\\bin\\Hostx64\\x64;C:\\Windows\\System32",
            "TMP": "C:\\Users\\fixture\\AppData\\Local\\Temp",
            "VCToolsInstallDir": "C:\\VS\\VC\\Tools\\MSVC\\selected\\",
            "WindowsSdkDir": "C:\\SDK\\",
            "WindowsSDKVersion": "10.0.selected\\",
            "VCToolsVersion": "14.44-selected",
            "UCRTVersion": "10.0.selected.0",
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

    def test_selected_vs_and_sdk_versions_feed_the_upstream_auto_config_selectors(self):
        prepared = self.prepare(dict(self.selected))
        repo_env = self.repo_env(prepared)
        self.assertEqual(repo_env["BAZEL_VC"], (PureWindowsPath(self.selected["VSINSTALLDIR"]) / "VC").as_posix())
        self.assertEqual(repo_env["BAZEL_VC_FULL_VERSION"], self.selected["VCToolsVersion"])
        self.assertEqual(repo_env["BAZEL_WINSDK_FULL_VERSION"], "10.0.selected")
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
        self.assertEqual(repo_env["BAZEL_WINSDK_FULL_VERSION"], "10.0.selected")

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


class WindowsLauncherTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-windows-launcher-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.generated = self.root / "configuration"

    def test_windows_deps_fetch_omits_the_unix_only_toolchain_repository(self):
        source = Mock(spec=Path, name="Y:\\checkout")
        source.resolve.return_value = PureWindowsPath(r"C:\\checkout")
        with (
            patch.object(native, "native_platform", return_value="windows"),
            patch.object(native, "PROJECT_ROOT", source),
            patch.object(workspace.subprocess, "run", return_value=subprocess.CompletedProcess([], 0)) as run,
        ):
            self.assertEqual(workspace.execute(Path("bazel.exe"), self.root, self.generated, "deps", "debug", []), 0)
        argv = run.call_args.args[0]
        self.assertEqual(
            [argument for argument in argv if argument.startswith("--repo=")],
            ["--repo=@aobus_native_deps", "--repo=@aobus_test_deps", "--repo=@aobus_native_tools"],
        )
        # The governed toolchain repository is a Unix-only fetch on Windows.
        self.assertFalse(any("aobus_toolchain" in argument for argument in argv))


if __name__ == "__main__":
    unittest.main()
