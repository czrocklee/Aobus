"""Windows prerequisite helper tests: native PS 5.1 decision tests.

The native tests AST-extract the REAL production helpers and main block via
the fixtures/windows-prerequisites.ps1 driver and assert actual decision
outcomes and the machine-readable JSON report. Every installer spawn is
overridden and recorded; an unexpected spawn fails the test, so these are
mocked decision tests, NOT proof of real UAC/installer/AppX behavior.

A small structural group runs on any host (Linux included) but is weak
source-text checking only: it is not a PowerShell parser or native proof.

Missing, empty, or exhausted mock plans record fixture-violation and fail
this driver even when production catches the throw and returns 1. Nested
production report values are not normalized in Python.
"""

import hashlib
import json
import shutil
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

REPOSITORY = Path(__file__).resolve().parents[2]
HELPER = REPOSITORY / "app" / "windows-winui" / "deployment" / "Install-Prerequisites.ps1"
STAGER = REPOSITORY / "script" / "windows" / "stage-prerequisites.ps1"
CONTRACT = REPOSITORY / "config/dependency-contract.json"
LAUNCHER = REPOSITORY / "app" / "windows-winui" / "deployment" / "Prerequisites.cmd"
DRIVER = Path(__file__).resolve().parent / "fixtures" / "windows-prerequisites.ps1"
DEPLOYMENT_README = HELPER.parent / "README.md"

RUNTIME_NAME = "Microsoft.WindowsAppRuntime.2"
RUNTIME_FAMILY = "Microsoft.WindowsAppRuntime.2_8wekyb3d8bbwe"
VC_INSTALLER = "vc_redist.x64.exe"
RUNTIME_INSTALLER = "WindowsAppRuntimeInstall-x64.exe"


def default_manifest():
    return {
        "schemaVersion": 1,
        "vcRuntime": {"version": "14.51.36247.0", "sha256": "a" * 64, "installer": VC_INSTALLER},
        "windowsAppRuntime": {
            "packageName": RUNTIME_NAME,
            "packageFamilyName": RUNTIME_FAMILY,
            "version": "2.4.0.0",
            "architecture": "X64",
            "sha256": "b" * 64,
            "installer": RUNTIME_INSTALLER,
        },
    }


def vc_state(ready):
    return {"ready": ready, "versions": {"msvcp140.dll": "14.51.36247.0"}, "missing": [] if ready else ["msvcp140.dll"]}


def runtime_state(ready, selected=None):
    return {"ready": ready, "selectedVersion": selected}


def healthy_package(version, status="Ok", architecture="X64"):
    return {
        "Name": RUNTIME_NAME,
        "PackageFamilyName": RUNTIME_FAMILY,
        "Version": version,
        "Status": status,
        "Architecture": architecture,
        "IsFramework": True,
    }


class NativePowerShellTest(unittest.TestCase):
    """Windows-only native tests; all production spawns are mocked and recorded."""

    @classmethod
    def setUpClass(cls):
        cls.powershell = shutil.which("powershell.exe")
        if cls.powershell is None:
            raise unittest.SkipTest("powershell.exe is unavailable; a native Windows host is required")

    def setUp(self):
        self.driver_invocations = 0

    def run_driver(self, scenario, source=HELPER):
        self.driver_invocations += 1
        label = f"{self.id()}: {scenario['kind']} #{self.driver_invocations} ({source.name})"
        with tempfile.TemporaryDirectory() as temp:
            scenario_file = Path(temp) / "scenario.json"
            scenario_file.write_text(json.dumps(scenario), encoding="utf-8")
            print(f"{label}: starting PowerShell", flush=True)
            started = time.monotonic()
            # Pure and scenario modes spawn a second PowerShell. Bazel owns the
            # tree deadline; killing only the driver could strand that child.
            completed = subprocess.run(
                [
                    self.powershell,
                    "-NoProfile",
                    "-NonInteractive",
                    "-ExecutionPolicy",
                    "Bypass",
                    "-File",
                    str(DRIVER),
                    "-ProductionSource",
                    str(source),
                    "-ScenarioFile",
                    str(scenario_file),
                    "-WorkDirectory",
                    str(Path(temp) / "Aobus [offline]"),
                ],
                capture_output=True,
                encoding="utf-8",
                errors="replace",
            )
            print(
                f"{label}: PowerShell exited {completed.returncode} after {time.monotonic() - started:.2f}s", flush=True
            )
            self.assertEqual(completed.returncode, 0, f"{label}\n{completed.stdout}\n{completed.stderr}")
            result = json.loads(completed.stdout)
        # Only the driver's own one-element arrays are wrapped. Production
        # report fields stay as Finish emitted them.
        for key in ("calls", "cases"):
            if key in result and isinstance(result[key], dict):
                result[key] = [result[key]]
        violations = [call for call in (result.get("calls") or []) if call.get("kind") == "fixture-violation"]
        self.assertEqual(violations, [], "fixture violation: " + json.dumps(violations))
        return result

    def run_pure(self, cases, mock=None, fixture=None):
        scenario = {"kind": "pure", "cases": cases}
        if mock:
            scenario["mock"] = mock
        if fixture:
            scenario["fixtureFile"] = fixture
        result = self.run_driver(scenario)
        return {case["name"]: json.loads(case["actual"]) for case in result["cases"]}, result["calls"]

    def run_scenario(self, mock=None, switches=(), manifest=None, create_installers=(), relative_manifest=False):
        scenario = {
            "kind": "scenario",
            "switches": list(switches),
            "manifest": default_manifest() if manifest is None else manifest,
            "relativeManifest": relative_manifest,
        }
        scenario["createInstallers"] = list(create_installers)
        if mock:
            scenario["mock"] = mock
        result = self.run_driver(scenario)
        # The child stdout must contain exactly one report with its actual exit code.
        result["report"] = json.loads(result["stdout"])
        self.assertEqual(result["report"]["exitCode"], result["exitCode"])
        return result

    @staticmethod
    def process_calls(result):
        return [call for call in result["calls"] if call["kind"] == "process"]

    def test_pure_version_tuple_is_strict_four_part_ascii(self):
        cases = [
            ("valid", "ConvertTo-VersionTuple '14.51.36247.0'", [14, 51, 36247, 0]),
            ("boundary", "ConvertTo-VersionTuple '65535.65535.65535.65535'", [65535] * 4),
            ("leading-zeros", "ConvertTo-VersionTuple '1.02.003.00004'", [1, 2, 3, 4]),
            ("overlong-leading-zeros", "ConvertTo-VersionTuple '000001.2.3.4'", None),
            ("major-too-large", "ConvertTo-VersionTuple '99999.1.1.1'", None),
            ("revision-too-large", "ConvertTo-VersionTuple '1.1.1.99999'", None),
            ("integer-overflow", "ConvertTo-VersionTuple '999999999999999999999.1.1.1'", None),
            ("trailing-newline", 'ConvertTo-VersionTuple "1.2.3.4`n"', None),
            ("negative", "ConvertTo-VersionTuple '-1.2.3.4'", None),
            ("three-parts", "ConvertTo-VersionTuple '1.2.3'", None),
            ("five-parts", "ConvertTo-VersionTuple '1.2.3.4.5'", None),
            ("prefix", "ConvertTo-VersionTuple 'v1.2.3.4'", None),
            ("suffix", "ConvertTo-VersionTuple '1.2.3.4 (RC)'", None),
            ("leading-space", "ConvertTo-VersionTuple ' 1.2.3.4'", None),
            ("unicode-digit", "ConvertTo-VersionTuple '\uff11.2.3.4'", None),
            ("empty", "ConvertTo-VersionTuple ''", None),
        ]
        values, _ = self.run_pure([{"name": name, "expr": expr} for name, expr, _ in cases])
        for name, _expr, expected in cases:
            with self.subTest(case=name):
                self.assertEqual(values[name], expected)

    def test_pure_version_comparison_is_numeric_not_lexicographic(self):
        cases = [
            ("2.10-vs-2.9", "Test-VersionAtLeast -Installed '2.10.0.0' -Minimum '2.9.0.0'", True),
            ("2.9-vs-2.10", "Test-VersionAtLeast -Installed '2.9.0.0' -Minimum '2.10.0.0'", False),
            ("equal", "Test-VersionAtLeast -Installed '2.4.0.0' -Minimum '2.4.0.0'", True),
            ("below", "Test-VersionAtLeast -Installed '2.3.9.9' -Minimum '2.4.0.0'", False),
            ("invalid-installed", "Test-VersionAtLeast -Installed '2.4' -Minimum '2.4.0.0'", False),
        ]
        values, _ = self.run_pure([{"name": name, "expr": expr} for name, expr, _ in cases])
        for name, _expr, expected in cases:
            with self.subTest(case=name):
                self.assertEqual(values[name], expected)

    def test_pure_runtime_detection_selects_highest_healthy_package(self):
        packages = [
            healthy_package("2.9.0.0"),
            healthy_package("2.10.0.0"),
            healthy_package("2.11.0.0", status="Modified"),
        ]
        values, calls = self.run_pure(
            [{"name": "state", "expr": "Get-AppRuntimeState -RequiredVersion '2.4.0.0'"}],
            mock={"appx": {"packages": packages}},
        )
        self.assertTrue(values["state"]["ready"])
        self.assertEqual(values["state"]["selectedVersion"], "2.10.0.0")
        queries = [call for call in calls if call["kind"] == "appx"]
        self.assertEqual(len(queries), 1)
        self.assertEqual(queries[0]["data"]["name"], RUNTIME_NAME)
        self.assertEqual(queries[0]["data"]["filter"], "Framework")

    def test_pure_runtime_detection_reports_missing_without_user_packages(self):
        for name, packages in (("empty", []), ("only-invalid", [healthy_package("2.8.0.0", architecture="X86")])):
            with self.subTest(packages=name):
                values, _ = self.run_pure(
                    [{"name": "state", "expr": "Get-AppRuntimeState -RequiredVersion '2.4.0.0'"}],
                    mock={"appx": {"packages": packages}},
                )
                self.assertFalse(values["state"]["ready"])
                self.assertIsNone(values["state"]["selectedVersion"])

    def test_pure_runtime_detection_applies_the_manifest_minimum(self):
        values, _ = self.run_pure(
            [{"name": "state", "expr": "Get-AppRuntimeState -RequiredVersion '2.5.0.0'"}],
            mock={"appx": {"packages": [healthy_package("2.4.0.0")]}},
        )
        self.assertFalse(values["state"]["ready"])

    def test_pure_installer_integrity_requires_hash_and_microsoft_signer(self):
        content = "integrity-fixture-content"
        digest = hashlib.sha256(content.encode()).hexdigest()
        fixture = {"name": "fixture [offline].bin", "content": content}
        good_signer = "CN=Microsoft Corporation, O=Microsoft Corporation, L=Redmond, S=Washington, C=US"

        def integrity(expr):
            return (
                "Test-InstallerIntegrity -Path (Join-Path $FixtureDirectory 'fixture [offline].bin') -ExpectedSha256 "
                + expr
            )

        cases = [
            {"name": "verified", "expr": integrity(f"'{digest}'")},
            {"name": "wrong-sha", "expr": integrity(f"'{'c' * 64}'")},
            {
                "name": "missing-file",
                "expr": integrity("'" + digest + "'").replace("fixture [offline].bin", "absent.bin"),
            },
        ]
        good = {"signature": {"status": "Valid", "subject": good_signer}}
        values, calls = self.run_pure(cases, mock=good, fixture=fixture)
        signatures = [call for call in calls if call["kind"] == "signature"]
        self.assertEqual(len(signatures), 1)
        self.assertTrue(signatures[0]["data"]["literalPath"].endswith("fixture [offline].bin"))
        self.assertTrue(values["verified"]["Ok"])
        self.assertFalse(values["wrong-sha"]["Ok"])
        self.assertFalse(values["missing-file"]["Ok"])

        signer_cases = [
            ("exact-cn", "CN=Microsoft Corporation", True),
            ("cn-overlap", "CN=Microsoft Corporationation", False),
            ("cn-with-space", "CN=Microsoft Corporation Evil", False),
            ("other-cn", "CN=Contoso", False),
            ("other-cn-with-microsoft-o", "CN=Contoso, O=Microsoft Corporation", False),
            ("o-contains-cn", "O=Microsoft Corporation, CN=Microsoft Corporation", False),
            ("bare-organization", "Microsoft Corporation", False),
            ("case-wrong", "CN=microsoft corporation", False),
            ("cn-prefix-case", "cn=Microsoft Corporation", False),
            ("cn-trailing-space", "CN=Microsoft Corporation ", False),
            ("cn-trailing-newline", "CN=Microsoft Corporation\n", False),
            ("cn-then-other-organization", "CN=Microsoft Corporation, O=Contoso", True),
            ("cn-trailing-comma", "CN=Microsoft Corporation,", True),
            ("invalid-status", None, False),
        ]
        # These stateless decisions share real fixture bytes, not scenario-main state.
        values, _ = self.run_pure(
            [{"name": name, "expr": integrity(f"'{digest}'")} for name, _, _ in signer_cases],
            mock={
                "signatures": [
                    {
                        "status": "NotSigned" if signature is None else "Valid",
                        "subject": good_signer if signature is None else signature,
                    }
                    for _, signature, _ in signer_cases
                ]
            },
            fixture=fixture,
        )
        for name, _, ok in signer_cases:
            with self.subTest(signer=name):
                self.assertEqual(values[name]["Ok"], ok)

    def test_scenario_check_mode_reports_missing_without_installer_activity(self):
        mock = {"admin": False, "vcStates": [vc_state(False)], "runtimeStates": [runtime_state(False)]}
        result = self.run_scenario(mock=mock)
        self.assertEqual(result["exitCode"], 3)
        self.assertEqual(result["report"]["mode"], "check")
        self.assertFalse(result["report"]["vcRuntime"]["ready"])
        self.assertFalse(result["report"]["windowsAppRuntime"]["ready"])
        self.assertEqual(self.process_calls(result), [])

    def test_scenario_relative_manifest_uses_powershell_location(self):
        mock = {"admin": False, "vcStates": [vc_state(True)], "runtimeStates": [runtime_state(True)]}
        result = self.run_scenario(mock=mock, relative_manifest=True)
        self.assertEqual(result["exitCode"], 0)
        self.assertTrue(result["report"]["vcRuntime"]["ready"])
        self.assertTrue(result["report"]["windowsAppRuntime"]["ready"])

    def test_scenario_install_refuses_an_elevated_admin_token(self):
        mock = {"admin": True, "vcStates": [vc_state(False)], "runtimeStates": [runtime_state(False)]}
        result = self.run_scenario(switches=["-Install"], mock=mock)
        self.assertEqual(result["exitCode"], 1)
        self.assertIn("original application user", result["report"]["error"])
        self.assertEqual(self.process_calls(result), [])

    def test_scenario_install_real_integrity_verifies_both_files_before_spawning(self):
        # PS 5.1 Set-Content -Encoding Ascii writes this exact CRLF-terminated payload.
        digest = hashlib.sha256(b"fixture-installer\r\n").hexdigest()
        manifest = default_manifest()
        manifest["vcRuntime"]["sha256"] = digest
        manifest["windowsAppRuntime"]["sha256"] = digest
        result = self.run_scenario(
            switches=["-Install"],
            manifest=manifest,
            mock={
                "admin": False,
                "vcStates": [vc_state(False), vc_state(True)],
                "runtimeStates": [runtime_state(False), runtime_state(True)],
                "signature": {"status": "Valid", "subject": "CN=Microsoft Corporation"},
                "processes": [{"exitCode": 0}, {"exitCode": 0}],
            },
            create_installers=[VC_INSTALLER, RUNTIME_INSTALLER],
        )
        self.assertEqual(result["exitCode"], 0)
        self.assertTrue(result["report"]["vcRuntime"]["ready"])
        self.assertTrue(result["report"]["windowsAppRuntime"]["ready"])
        # Both files must be verified before the first installer launch.
        boundaries = [call["kind"] for call in result["calls"] if call["kind"] in {"signature", "process"}]
        self.assertEqual(boundaries, ["signature", "signature", "process", "process"])
        signatures = [call for call in result["calls"] if call["kind"] == "signature"]
        processes = self.process_calls(result)
        for signature, process, name in zip(signatures, processes, (VC_INSTALLER, RUNTIME_INSTALLER), strict=True):
            self.assertTrue(signature["data"]["literalPath"].endswith(name))
            self.assertIn("Aobus [offline]", signature["data"]["literalPath"])
            self.assertEqual(signature["data"]["literalPath"], process["data"]["path"])
        self.assertEqual(processes[0]["data"]["verb"], "RunAs")
        self.assertEqual(processes[0]["data"]["args"], ["/install", "/quiet", "/norestart"])
        self.assertEqual(processes[1]["data"]["verb"], "")
        self.assertEqual(processes[1]["data"]["args"], ["--quiet"])

    def test_scenario_install_real_integrity_refuses_bad_runtime_hash_before_any_spawn(self):
        digest = hashlib.sha256(b"fixture-installer\r\n").hexdigest()
        manifest = default_manifest()
        manifest["vcRuntime"]["sha256"] = digest
        manifest["windowsAppRuntime"]["sha256"] = "0" * 64
        result = self.run_scenario(
            switches=["-Install"],
            manifest=manifest,
            mock={
                "admin": False,
                "vcStates": [vc_state(False)],
                "runtimeStates": [runtime_state(False)],
                "signature": {"status": "Valid", "subject": "CN=Microsoft Corporation"},
                "processes": [],
            },
            create_installers=[VC_INSTALLER, RUNTIME_INSTALLER],
        )
        self.assertEqual(result["exitCode"], 1)
        self.assertIn("SHA-256 mismatch: " + RUNTIME_INSTALLER, result["report"]["error"])
        self.assertEqual(self.process_calls(result), [])

    def test_scenario_vc_exit_codes_are_gated_before_postconditions(self):
        cases = [
            ("exit-0-post-false", 0, False, 1),
            ("exit-1603-even-ready", 1603, True, 1),
            ("exit-1638-ready", 1638, True, 0),
            ("exit-1638-post-false", 1638, False, 1),
        ]
        for name, exit_code, post_ready, expected in cases:
            with self.subTest(case=name):
                result = self.run_scenario(
                    switches=["-Install"],
                    mock={
                        "admin": False,
                        "vcStates": [vc_state(False), vc_state(post_ready)],
                        "runtimeStates": [runtime_state(True)],
                        "integrity": {VC_INSTALLER: True},
                        "processes": [{"exitCode": exit_code}],
                    },
                    create_installers=[VC_INSTALLER],
                )
                self.assertEqual(result["exitCode"], expected)
                self.assertEqual(len(self.process_calls(result)), 1)

    def test_scenario_vc_3010_reports_reboot_and_stops_before_runtime(self):
        result = self.run_scenario(
            switches=["-Install"],
            mock={
                "admin": False,
                "vcStates": [vc_state(False), vc_state(False)],
                "runtimeStates": [runtime_state(False)],
                "integrity": {VC_INSTALLER: True, RUNTIME_INSTALLER: True},
                "processes": [{"exitCode": 3010}],
            },
            create_installers=[VC_INSTALLER, RUNTIME_INSTALLER],
        )
        self.assertEqual(result["exitCode"], 3010)
        self.assertTrue(result["report"]["rebootRequired"])
        calls = self.process_calls(result)
        self.assertEqual(len(calls), 1)
        self.assertTrue(calls[0]["data"]["path"].endswith(VC_INSTALLER))

    def test_scenario_runtime_requires_exit_zero_and_healthy_postcondition(self):
        cases = [("exit-0-post-false", 0, False), ("exit-5-post-true", 5, True)]
        for name, exit_code, post_ready in cases:
            with self.subTest(case=name):
                result = self.run_scenario(
                    switches=["-Install"],
                    mock={
                        "admin": False,
                        "vcStates": [vc_state(True)],
                        "runtimeStates": [runtime_state(False), runtime_state(post_ready)],
                        "integrity": {RUNTIME_INSTALLER: True},
                        "processes": [{"exitCode": exit_code}],
                    },
                    create_installers=[RUNTIME_INSTALLER],
                )
                self.assertEqual(result["exitCode"], 1)
                self.assertEqual(result["report"]["windowsAppRuntime"]["ready"], post_ready)
                (call,) = self.process_calls(result)
                self.assertTrue(call["data"]["path"].endswith(RUNTIME_INSTALLER))

    def test_scenario_uac_decline_exits_1223_without_runtime_attempt(self):
        result = self.run_scenario(
            switches=["-Install"],
            mock={
                "admin": False,
                "vcStates": [vc_state(False)],
                "runtimeStates": [runtime_state(False)],
                "integrity": {VC_INSTALLER: True, RUNTIME_INSTALLER: True},
                "processes": [{"throwNativeError": 1223}],
            },
            create_installers=[VC_INSTALLER, RUNTIME_INSTALLER],
        )
        self.assertEqual(result["exitCode"], 1223)
        self.assertIn("declined", result["report"]["error"])
        calls = self.process_calls(result)
        self.assertEqual(len(calls), 1)
        self.assertTrue(calls[0]["data"]["path"].endswith(VC_INSTALLER))

    def test_scenario_machine_mode_requires_an_admin_token(self):
        result = self.run_scenario(switches=["-InstallVcRuntime"], mock={"admin": False, "vcStates": [vc_state(False)]})
        self.assertEqual(result["exitCode"], 1)
        self.assertIn("administrator token", result["report"]["error"])
        self.assertEqual(self.process_calls(result), [])

    def test_scenario_machine_mode_is_vc_only(self):
        result = self.run_scenario(
            switches=["-InstallVcRuntime"],
            mock={
                "admin": True,
                "vcStates": [vc_state(False), vc_state(True)],
                "appx": {"packages": []},
                "integrity": {VC_INSTALLER: True},
                "processes": [{"exitCode": 0}],
            },
            create_installers=[VC_INSTALLER],
        )
        self.assertEqual(result["exitCode"], 0)
        self.assertIsNone(result["report"]["windowsAppRuntime"])
        self.assertEqual([call for call in result["calls"] if call["kind"] == "appx"], [])
        self.assertEqual([call for call in result["calls"] if call["kind"] == "runtime-query"], [])
        calls = self.process_calls(result)
        self.assertEqual(len(calls), 1)
        self.assertTrue(calls[0]["data"]["path"].endswith(VC_INSTALLER))
        self.assertEqual(len([call for call in result["calls"] if call["kind"] == "vc-query"]), 2)

    def test_scenario_machine_mode_ready_vc_is_a_noop(self):
        result = self.run_scenario(switches=["-InstallVcRuntime"], mock={"admin": True, "vcStates": [vc_state(True)]})
        self.assertEqual(result["exitCode"], 0)
        self.assertEqual(self.process_calls(result), [])

    def test_scenario_machine_mode_exit_codes_require_the_postcondition(self):
        for name, code, ready, expected in (
            ("1638-ready", 1638, True, 0),
            ("1638-not-ready", 1638, False, 1),
            ("unexpected-even-ready", 1603, True, 1),
        ):
            with self.subTest(case=name):
                result = self.run_scenario(
                    switches=["-InstallVcRuntime"],
                    mock={
                        "admin": True,
                        "vcStates": [vc_state(False), vc_state(ready)],
                        "integrity": {VC_INSTALLER: True},
                        "processes": [{"exitCode": code}],
                    },
                    create_installers=[VC_INSTALLER],
                )
                self.assertEqual(result["exitCode"], expected)
                self.assertEqual(len(self.process_calls(result)), 1)
                self.assertIsNone(result["report"]["windowsAppRuntime"])

    def test_pure_crt_detector_requires_every_dll_at_the_floor(self):
        dlls = ["msvcp140.dll", "msvcp140_1.dll", "msvcp140_atomic_wait.dll", "vcruntime140.dll", "vcruntime140_1.dll"]
        versions = dict.fromkeys(dlls, "14.51.36247.0")
        cases = [
            ("ready", versions, []),
            ("one-old", {**versions, dlls[-1]: "14.50.0.0"}, [dlls[-1]]),
            ("one-missing", {k: v for k, v in versions.items() if k != dlls[-1]}, [dlls[-1]]),
        ]
        for name, observed, missing in cases:
            with self.subTest(case=name):
                values, _ = self.run_pure(
                    [{"name": "state", "expr": "Get-VcRuntimeState -RequiredVersion '14.51.36247.0'"}],
                    mock={"crtVersions": observed},
                )
                state = values["state"]
                self.assertEqual(state["ready"], not missing)
                self.assertEqual(state["missing"], missing)
                self.assertEqual(set(state["versions"]), set(dlls))

    def run_stage(self, **overrides):
        plan = {
            "toolset": "14.51.36231",
            "badRuntimeHash": False,
            "missingReadme": False,
            "destinationExists": False,
            "relativePaths": False,
            "status": "Valid",
            "subject": "CN=Microsoft Corporation, O=Microsoft Corporation",
            "versionInfo": {
                "FileMajorPart": 14,
                "FileMinorPart": 51,
                "FileBuildPart": 36247,
                "FilePrivatePart": 0,
                "OriginalFilename": VC_INSTALLER,
            },
        }
        plan.update(overrides)
        return self.run_driver({"kind": "stage", "stage": plan}, source=STAGER)

    def test_stager_writes_verified_payloads_and_contract_with_literal_paths(self):
        result = self.run_stage()
        self.assertTrue(result["succeeded"], result["error"])
        manifest = result["manifest"]
        runtime = json.loads(CONTRACT.read_text(encoding="utf-8"))["dependencies"]["windows-app-sdk"]["runtime"]
        self.assertEqual(manifest["vcToolsVersion"], "14.51.36231")
        self.assertEqual(manifest["vcRuntime"]["version"], "14.51.36247.0")
        self.assertEqual(manifest["vcRuntime"]["sha256"], result["hashes"][VC_INSTALLER])
        self.assertEqual(manifest["windowsAppRuntime"]["packageName"], runtime["packageName"])
        self.assertEqual(manifest["windowsAppRuntime"]["architecture"], runtime["architecture"].upper())
        self.assertEqual(manifest["windowsAppRuntime"]["version"], runtime["version"])
        self.assertEqual(manifest["windowsAppRuntime"]["sha256"], result["hashes"][RUNTIME_INSTALLER])
        for source in (HELPER, LAUNCHER, DEPLOYMENT_README):
            self.assertEqual(result["hashes"][source.name], hashlib.sha256(source.read_bytes()).hexdigest())

    def test_stager_resolves_relative_paths_against_powershell_location(self):
        result = self.run_stage(relativePaths=True)
        self.assertTrue(result["succeeded"], result["error"])
        self.assertFalse(result["otherDestinationExists"])
        for installer in (VC_INSTALLER, RUNTIME_INSTALLER):
            self.assertEqual(result["hashes"][installer], result["sourceHashes"][installer])

    def test_stager_refuses_inputs_before_creating_output(self):
        base_version = {
            "FileMajorPart": 14,
            "FileMinorPart": 51,
            "FileBuildPart": 36247,
            "FilePrivatePart": 0,
            "OriginalFilename": VC_INSTALLER,
        }
        for name, overrides, message in (
            ("wrong-architecture", {"versionInfo": {**base_version, "OriginalFilename": "vc_redist.x86.exe"}}, "x64"),
            ("older-redist", {"versionInfo": {**base_version, "FileBuildPart": 36200}}, "predates"),
            ("bad-runtime-hash", {"badRuntimeHash": True}, "SHA-256"),
            ("unsigned", {"status": "NotSigned"}, "Authenticode"),
            ("wrong-publisher", {"subject": "CN=Contoso, O=Microsoft Corporation"}, "Authenticode"),
            ("publisher-newline", {"subject": "CN=Microsoft Corporation\n"}, "Authenticode"),
            ("invalid-toolset", {"toolset": "19.51.36231"}, "toolset"),
            ("missing-readme", {"missingReadme": True}, "README.md"),
        ):
            with self.subTest(case=name):
                result = self.run_stage(**overrides)
                self.assertFalse(result["succeeded"])
                self.assertIn(message, result["error"])
                self.assertFalse(result["destinationExists"])
                self.assertIsNone(result["manifest"])

    def test_stager_preserves_existing_destination(self):
        result = self.run_stage(destinationExists=True)
        self.assertFalse(result["succeeded"])
        self.assertIn("Destination exists", result["error"])
        self.assertEqual(result["sentinel"], "preserve me")
        self.assertEqual(result["calls"], [])

    def test_scenario_machine_mode_vc_3010_reports_reboot(self):
        result = self.run_scenario(
            switches=["-InstallVcRuntime"],
            mock={
                "admin": True,
                "vcStates": [vc_state(False), vc_state(False)],
                "integrity": {VC_INSTALLER: True},
                "processes": [{"exitCode": 3010}],
            },
            create_installers=[VC_INSTALLER],
        )
        self.assertEqual(result["exitCode"], 3010)
        self.assertTrue(result["report"]["rebootRequired"])
        self.assertIsNone(result["report"]["windowsAppRuntime"])
        (call,) = self.process_calls(result)
        self.assertTrue(call["data"]["path"].endswith(VC_INSTALLER))
        self.assertEqual(call["data"]["verb"], "")
        self.assertEqual(call["data"]["args"], ["/install", "/quiet", "/norestart"])

    def test_scenario_report_actions_stay_json_arrays_for_zero_one_and_many(self):
        # Real Finish output. Python must not wrap nested production values.
        zero = self.run_scenario(manifest={})
        self.assertEqual(zero["exitCode"], 1)
        self.assertEqual(zero["report"]["actions"], [])

        one = self.run_scenario(mock={"vcStates": [{"throwMessage": "planned vc query failure"}]})
        self.assertEqual(one["exitCode"], 1)
        self.assertEqual(one["report"]["actions"], ["manifest-validated"])
        self.assertIn("planned vc query failure", one["report"]["error"])

        many = self.run_scenario(
            switches=["-Install"],
            mock={"admin": False, "vcStates": [vc_state(True)], "runtimeStates": [runtime_state(True)]},
        )
        self.assertEqual(many["exitCode"], 0)
        self.assertIsInstance(many["report"]["actions"], list)
        self.assertEqual(
            many["report"]["actions"],
            ["manifest-validated", "detected-current-user-state", "ready-noop"],
        )
        self.assertEqual(self.process_calls(many), [])

    def test_scenario_report_vc_missing_stays_json_array_for_zero_one_and_many(self):
        cases = (
            ("zero", [], 0),
            ("one", ["msvcp140.dll"], 3),
            ("many", ["msvcp140.dll", "vcruntime140_1.dll"], 3),
        )
        for name, missing, exit_code in cases:
            with self.subTest(case=name):
                result = self.run_scenario(
                    mock={
                        "admin": False,
                        "vcStates": [
                            {
                                "ready": not missing,
                                "versions": {"msvcp140.dll": "14.51.36247.0"},
                                "missing": missing,
                            }
                        ],
                        "runtimeStates": [runtime_state(True)],
                    }
                )
                self.assertEqual(result["exitCode"], exit_code)
                self.assertTrue(result["report"]["windowsAppRuntime"]["ready"])
                observed = result["report"]["vcRuntime"]["missing"]
                self.assertIsInstance(observed, list)
                self.assertEqual(observed, missing)
                self.assertEqual(self.process_calls(result), [])


class StructuralContractTest(unittest.TestCase):
    """Batch launcher's install-mode and exit-code wiring."""

    def test_launcher_install_mode_routes_cancellation_but_not_check_only_missing(self):
        code = "\n".join(
            line
            for line in LAUNCHER.read_text(encoding="utf-8").splitlines()
            if not line.strip().lower().startswith("rem")
        )
        self.assertIn('-File "%~dp0Install-Prerequisites.ps1" -Install', code)
        self.assertRegex(code, r'if\s+"%RESULT%"=="1223"')
        self.assertNotRegex(code, r'if\s+"%RESULT%"=="3"')
        self.assertRegex(code, r'if\s+"%RESULT%"=="3010"')
        self.assertRegex(code, r'if\s+"%RESULT%"=="0"')

    def test_launcher_forwards_the_real_exit_code_without_embedded_credentials(self):
        lines = LAUNCHER.read_text(encoding="utf-8").splitlines()
        # Comments may mention what is deliberately absent; scan code only.
        code = [line for line in lines if not line.strip().lower().startswith("rem")]
        self.assertIn("exit /b %RESULT%", "\n".join(code))
        self.assertIn("Sysnative", "\n".join(code))
        self.assertNotIn("runas", "\n".join(code).lower())
        self.assertNotIn("password", "\n".join(code).lower())


if __name__ == "__main__":
    unittest.main()
