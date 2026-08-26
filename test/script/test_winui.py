"""Tests for Windows App SDK host contracts and launch safeguards."""

import hashlib
import json
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao.core import winui


class WinUiTest(unittest.TestCase):
    def test_generated_msbuild_import_writer_preserves_unchanged_mtime(self):
        cmake = shutil.which("cmake")
        if cmake is None:
            self.skipTest("cmake is unavailable")

        module = Path(__file__).resolve().parents[2] / "cmake" / "WindowsAppSdk.cmake"
        with tempfile.TemporaryDirectory() as temp_dir:
            temp = Path(temp_dir)
            output = temp / "generated.props"
            script = temp / "write-import.cmake"
            script.write_text(
                f"include([=[{module.as_posix()}]=])\n"
                f"_aobus_write_if_different([=[{output.as_posix()}]=] [=[alpha;beta\n]=])\n",
                encoding="utf-8",
            )
            first = subprocess.run([cmake, "-P", str(script)], capture_output=True, text=True)
            self.assertEqual(first.returncode, 0, first.stderr)
            self.assertEqual(output.read_text(encoding="utf-8"), "alpha;beta\n")

            os.utime(output, ns=(1_000_000_000, 1_000_000_000))
            preserved_mtime = output.stat().st_mtime_ns
            repeated = subprocess.run([cmake, "-P", str(script)], capture_output=True, text=True)
            repeated_mtime = output.stat().st_mtime_ns

            update_script = temp / "write-import-update.cmake"
            update_script.write_text(
                f"include([=[{module.as_posix()}]=])\n"
                f"_aobus_write_if_different([=[{output.as_posix()}]=] [=[updated;content\n]=])\n",
                encoding="utf-8",
            )
            updated = subprocess.run([cmake, "-P", str(update_script)], capture_output=True, text=True)
            updated_content = output.read_text(encoding="utf-8")
            updated_mtime = output.stat().st_mtime_ns

        self.assertEqual(repeated.returncode, 0, repeated.stderr)
        self.assertEqual(repeated_mtime, preserved_mtime)
        self.assertEqual(updated.returncode, 0, updated.stderr)
        self.assertEqual(updated_content, "updated;content\n")
        self.assertNotEqual(updated_mtime, preserved_mtime)

    def test_component_manifest_is_the_build_tools_source_of_truth(self):
        components = winui.required_components()

        self.assertIn("Microsoft.VisualStudio.Component.NuGet.BuildTools", components)
        self.assertIn("Microsoft.VisualStudio.ComponentGroup.WindowsAppDevelopment.VC.BuildTools", components)
        self.assertIn("Microsoft.VisualStudio.Component.Windows11SDK.26100", components)

    def test_runtime_contract_is_governed_with_an_exact_hash(self):
        runtime = winui._runtime_contract()

        self.assertEqual(runtime.package_name, "Microsoft.WindowsAppRuntime.2")
        self.assertEqual(runtime.version, "2.4.0.0")
        self.assertEqual(runtime.architecture, "x64")
        self.assertRegex(runtime.sha256, r"^[0-9a-f]{64}$")

    def test_runtime_json_filters_other_architectures(self):
        payload = json.dumps(
            [
                {
                    "Version": "2.4.0.0",
                    "PackageFullName": "Microsoft.WindowsAppRuntime.2_2.4.0.0_x64__8wekyb3d8bbwe",
                },
                {
                    "Version": "2.4.0.0",
                    "PackageFullName": "Microsoft.WindowsAppRuntime.2_2.4.0.0_x86__8wekyb3d8bbwe",
                },
            ]
        )

        packages = winui._runtime_packages_from_json(payload, "x64")

        self.assertEqual(len(packages), 1)
        self.assertEqual(packages[0].architecture, "x64")
        self.assertIn("_x64__", packages[0].package_full_name)

    def test_file_hash_is_sha256(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            path = Path(temp_dir) / "payload"
            path.write_bytes(b"aobus")

            self.assertEqual(winui.file_sha256(path), hashlib.sha256(b"aobus").hexdigest())

    def test_runtime_setup_is_idempotent_when_the_exact_runtime_exists(self):
        installed = winui.RuntimePackage(
            version="2.4.0.0",
            architecture="x64",
            package_full_name="Microsoft.WindowsAppRuntime.2_2.4.0.0_x64__8wekyb3d8bbwe",
        )
        with mock.patch.object(winui.os, "name", "nt"):
            with mock.patch.object(winui, "matching_runtime", return_value=installed):
                with mock.patch.object(winui, "_download_verified_installer") as download:
                    self.assertEqual(winui.setup_runtime(), installed)

        download.assert_not_called()

    def test_service_session_is_rejected_before_launch(self):
        with mock.patch.object(winui, "current_session_id", return_value=0):
            with self.assertRaisesRegex(RuntimeError, "session 0"):
                winui.require_interactive_session()

    def test_interactive_session_is_returned(self):
        with mock.patch.object(winui, "current_session_id", return_value=3):
            self.assertEqual(winui.require_interactive_session(), 3)


if __name__ == "__main__":
    unittest.main()
