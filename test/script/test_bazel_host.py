"""Bounded host ISA and OS-family selection for the Bazel portal."""

import ctypes
import os
import unittest
from pathlib import Path
from unittest.mock import Mock, patch

from ao.core import bazel_native as native
from ao.core import bazel_workspace as workspace


class BazelHostTests(unittest.TestCase):
    def test_windows_machine_reads_only_native_processor_architecture_without_platform_inventory(self):
        for architecture, machine in ((0, "x86"), (9, "AMD64"), (12, "ARM64")):
            kernel32 = Mock(spec=["GetNativeSystemInfo"])

            def get_info(buffer, value=architecture):
                buffer._obj.processor_architecture = value

            kernel32.GetNativeSystemInfo.side_effect = get_info
            with (
                self.subTest(architecture=architecture),
                patch.object(native.os, "name", "nt"),
                patch.object(native.ctypes, "WinDLL", create=True, return_value=kernel32) as load,
                patch.object(native.platform, "machine", side_effect=AssertionError("no uname/WMI query")),
                patch.object(native.platform, "system", side_effect=AssertionError("no OS inventory")),
                patch.object(native.platform, "uname", side_effect=AssertionError("no OS inventory")),
                patch.dict(os.environ, {"PROCESSOR_ARCHITECTURE": "wrong", "PROCESSOR_ARCHITEW6432": "wrong"}),
            ):
                self.assertEqual(native.host_machine(), machine)
            load.assert_called_once_with("kernel32")
            kernel32.GetNativeSystemInfo.assert_called_once()
            self.assertIsNone(kernel32.GetNativeSystemInfo.restype)
            info = kernel32.GetNativeSystemInfo.call_args.args[0]._obj
            # Windows writes the full SYSTEM_INFO even though we read only its ISA.
            self.assertEqual(ctypes.sizeof(info), 48 if ctypes.sizeof(ctypes.c_void_p) == 8 else 36)
            self.assertEqual(kernel32.GetNativeSystemInfo.argtypes, (ctypes.POINTER(type(info)),))
            self.assertEqual([call[0] for call in kernel32.mock_calls], ["GetNativeSystemInfo"])

    def test_posix_machine_preserves_platform_machine_without_loading_windows_api(self):
        for value in ("x86_64", "aarch64", "arm64"):
            with (
                self.subTest(machine=value),
                patch.object(native.os, "name", "posix"),
                patch.object(native.platform, "machine", return_value=value) as machine,
                patch.object(native.ctypes, "WinDLL", create=True, side_effect=AssertionError("not Windows")) as load,
            ):
                self.assertEqual(native.host_machine(), value)
            machine.assert_called_once_with()
            load.assert_not_called()

    def test_artifact_os_family_comes_from_native_profile_not_platform_inventory(self):
        for host, machine, expected in (
            ("linux", "x86_64", "linux-x86_64"),
            ("linux", "aarch64", "linux-arm64"),
            ("windows", "AMD64", "windows-x86_64"),
            ("windows", "ARM64", "windows-arm64"),
            ("windows", "x86", "windows-x86"),
            ("macos", "arm64", "darwin-arm64"),
            ("macos", "x86_64", "darwin-x86_64"),
        ):
            with (
                self.subTest(host=host, machine=machine),
                patch.object(native, "native_platform", return_value=host),
                patch.object(native, "host_machine", return_value=machine),
                patch.object(native.platform, "system", side_effect=AssertionError("no OS inventory")),
                patch.object(native.platform, "machine", side_effect=AssertionError("use bounded ISA")),
                patch.object(native.platform, "uname", side_effect=AssertionError("no OS inventory")),
            ):
                self.assertEqual(workspace._artifact_key(), expected)

    def test_unpinned_windows_isa_never_downloads_or_uses_x64_artifact(self):
        executable = Path("unavailable-managed-bazel.exe")
        for machine, key in (("ARM64", "windows-arm64"), ("x86", "windows-x86")):
            with (
                self.subTest(machine=machine),
                patch.dict(os.environ, {}, clear=True),
                patch.object(native, "native_platform", return_value="windows"),
                patch.object(native, "host_machine", return_value=machine),
                patch.object(workspace, "_managed_tool", return_value=executable),
                patch.object(workspace.urllib.request, "urlretrieve") as download,
                patch.object(workspace, "_tool_version") as version,
                self.assertRaisesRegex(native.BazelWorkspaceError, f"no pinned Bazel artifact for {key}"),
            ):
                workspace.bazel_executable(setup=True)
            download.assert_not_called()
            version.assert_not_called()


if __name__ == "__main__":
    unittest.main()
