"""Tests for Windows App SDK host contracts and launch safeguards."""

import hashlib
import json
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
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

    def test_enterprise_workload_can_satisfy_build_tools_requirement(self):
        build_tools = "Microsoft.VisualStudio.Workload.VCTools"
        enterprise = "Microsoft.VisualStudio.Workload.NativeDesktop"

        with mock.patch.object(
            winui,
            "visual_studio_installation",
            side_effect=lambda *, component, environ=None: Path("C:/VS") if component == enterprise else None,
        ):
            self.assertEqual(winui.installed_component(build_tools), enterprise)

    def test_canonical_build_tools_component_is_preferred(self):
        component = "Microsoft.VisualStudio.Workload.VCTools"

        with mock.patch.object(winui, "visual_studio_installation", return_value=Path("C:/VS")) as locate:
            self.assertEqual(winui.installed_component(component), component)

        locate.assert_called_once_with(component=component, environ=None)

    def test_visual_studio_query_uses_the_selected_environment(self):
        queried = subprocess.CompletedProcess(args=[], returncode=0, stdout="C:/VS\n", stderr="")
        environment = {"AOBUS_TEST_ENV": "present"}

        with tempfile.TemporaryDirectory() as temp_dir:
            locator = Path(temp_dir) / "vswhere.exe"
            locator.touch()
            with mock.patch.object(winui, "vswhere_path", return_value=locator):
                with mock.patch.object(winui, "_run_text", return_value=queried) as run:
                    installation = winui.visual_studio_installation(environ=environment)

        self.assertEqual(installation, Path("C:/VS"))
        self.assertEqual(run.call_args.kwargs["environ"], environment)

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

    def test_runtime_query_uses_selected_environment_without_powershell_7_module_path(self):
        runtime = winui.RuntimeContract(
            package_name="Microsoft.WindowsAppRuntime.2",
            version="2.4.0.0",
            architecture="x64",
            installer_url="https://example.invalid/runtime.exe",
            sha256="0" * 64,
        )
        queried = subprocess.CompletedProcess(args=[], returncode=0, stdout="", stderr="")
        inherited = {
            "AOBUS_TEST_ENV": "present",
            "PSModulePath": "C:/Program Files/PowerShell/7/Modules",
        }

        with mock.patch.object(winui, "_run_text", return_value=queried) as run:
            self.assertEqual(winui.installed_runtime_packages(runtime, environ=inherited), ())

        environment = run.call_args.kwargs["environ"]
        self.assertEqual(environment["AOBUS_TEST_ENV"], "present")
        self.assertNotIn("PSModulePath", environment)

    def test_host_inspection_propagates_the_selected_environment(self):
        environment = {
            "AOBUS_TEST_ENV": "present",
            "ProgramFiles(x86)": "C:/Program Files (x86)",
        }
        runtime = winui.RuntimeContract(
            package_name="Microsoft.WindowsAppRuntime.2",
            version="2.4.0.0",
            architecture="x64",
            installer_url="https://example.invalid/runtime.exe",
            sha256="0" * 64,
        )
        installed_runtime = winui.RuntimePackage(
            version=runtime.version,
            architecture=runtime.architecture,
            package_full_name="Microsoft.WindowsAppRuntime.2_2.4.0.0_x64__8wekyb3d8bbwe",
        )
        cmake_query = subprocess.CompletedProcess(
            args=[],
            returncode=0,
            stdout=f"Generators\n  {winui.VISUAL_STUDIO_GENERATOR}\n",
            stderr="",
        )

        with (
            mock.patch.object(winui, "os", SimpleNamespace(name="nt", environ={})),
            mock.patch.object(Path, "is_file", return_value=True),
            mock.patch.object(Path, "is_dir", return_value=True),
            mock.patch.object(winui, "required_components", return_value=("canonical",)),
            mock.patch.object(winui, "visual_studio_installation", return_value=Path("C:/VS")) as locate,
            mock.patch.object(winui, "installed_component", return_value="alternative") as component,
            mock.patch.object(winui, "_runtime_contract", return_value=runtime),
            mock.patch.object(winui, "matching_runtime", return_value=installed_runtime) as match_runtime,
            mock.patch.object(winui, "_registry_dword", side_effect=(1, 0)),
            mock.patch.object(winui, "_run_text", return_value=cmake_query) as run,
        ):
            checks = winui.inspect_host(environ=environment)

        self.assertTrue(all(check.ok for check in checks if check.required))
        locate.assert_called_once_with(environ=environment)
        component.assert_called_once_with("canonical", environ=environment)
        self.assertEqual(run.call_args.kwargs["environ"], environment)
        match_runtime.assert_called_once_with(runtime, environ=environment)

    def test_build_host_requirement_reports_required_failures_only(self):
        checks = (
            winui.HostCheck("Visual Studio Build Tools", False, "missing"),
            winui.HostCheck("Developer Mode", False, "optional", required=False),
        )

        with mock.patch.object(winui, "inspect_host", return_value=checks) as inspect:
            with self.assertRaisesRegex(RuntimeError, "Visual Studio Build Tools: missing"):
                winui.require_build_host()

        inspect.assert_called_once_with(include_runtime=False)

    def test_file_hash_is_sha256(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            path = Path(temp_dir) / "payload"
            path.write_bytes(b"aobus")

            self.assertEqual(winui.file_sha256(path), hashlib.sha256(b"aobus").hexdigest())

    def test_authenticode_path_is_passed_through_the_environment(self):
        installer = Path("C:/aobus state/WindowsAppRuntimeInstall-x64.exe")
        verified = subprocess.CompletedProcess(
            args=[],
            returncode=0,
            stdout=json.dumps({"Status": "Valid", "Subject": "CN=Microsoft Corporation"}),
            stderr="",
        )

        inherited = {
            "AOBUS_TEST_ENV": "present",
            "PSModulePath": "C:/Program Files/PowerShell/7/Modules",
        }
        with mock.patch.object(winui, "_run_text", return_value=verified) as run:
            winui._verify_authenticode(installer, environ=inherited)

        command = run.call_args.args[0]
        environment = run.call_args.kwargs["environ"]
        self.assertEqual(environment["AOBUS_TEST_ENV"], "present")
        self.assertEqual(environment["AOBUS_AUTHENTICODE_PATH"], str(installer))
        self.assertNotIn("PSModulePath", environment)
        self.assertNotIn(str(installer), command)
        self.assertIn("$env:AOBUS_AUTHENTICODE_PATH", command[-1])

    def test_windows_powershell_environment_removes_case_insensitive_module_path(self):
        environment = winui._windows_powershell_environment(
            {
                "Path": "C:/Windows/System32",
                "psmodulepath": "C:/Program Files/PowerShell/7/Modules",
            }
        )

        self.assertEqual(environment, {"Path": "C:/Windows/System32"})

    def test_authenticode_query_failure_reports_powershell_diagnostic(self):
        failed = subprocess.CompletedProcess(
            args=[],
            returncode=1,
            stdout="",
            stderr="Get-AuthenticodeSignature could not inspect the file",
        )

        with mock.patch.object(winui, "_run_text", return_value=failed):
            with self.assertRaisesRegex(RuntimeError, "could not inspect"):
                winui._verify_authenticode(Path("C:/runtime.exe"), environ={})

    def test_authenticode_query_rejects_valid_json_with_the_wrong_shape(self):
        invalid = subprocess.CompletedProcess(args=[], returncode=0, stdout="[]", stderr="")

        with mock.patch.object(winui, "_run_text", return_value=invalid):
            with self.assertRaisesRegex(RuntimeError, "returned invalid data"):
                winui._verify_authenticode(Path("C:/runtime.exe"), environ={})

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
