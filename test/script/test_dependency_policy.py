"""Tests for governed dependency policy and resolution evidence."""

import copy
import hashlib
import json
import tempfile
import unittest
import zipfile
from datetime import date
from pathlib import Path

from ao.core import dependency_policy


class DependencyPolicyTest(unittest.TestCase):
    def setUp(self):
        self.contract = dependency_policy.load_contract(today=date(2026, 7, 10))

    def _exception(self, **updates):
        exception = {
            "id": "DEP-2026-001",
            "dependency": "ftxui",
            "platform": "windows",
            "allowedVersion": "7.0.0",
            "reason": "Registry availability is delayed.",
            "owner": "windows-maintainers",
            "issue": "https://example.invalid/issues/1",
            "created": "2026-07-10",
            "expires": "2026-08-09",
            "exitCondition": "Remove after registry alignment.",
        }
        exception.update(updates)
        return exception

    def _write_contract(self, root, contract=None):
        path = root / "dependency-contract.json"
        path.write_text(json.dumps(self.contract if contract is None else contract), encoding="utf-8")
        return path

    def _report(self, root, contract_path, contract=None):
        active_contract = self.contract if contract is None else contract
        for name in ("vcpkg-configuration.json", "vcpkg.json"):
            (root / name).write_text("{}\n", encoding="utf-8")
        nuget_lock_relative = active_contract["dependencies"]["windows-app-sdk"]["nuget"]["lock"]
        nuget_lock = root / nuget_lock_relative
        nuget_lock.parent.mkdir(parents=True, exist_ok=True)
        source_lock = dependency_policy.PROJECT_ROOT / self.contract["dependencies"]["windows-app-sdk"]["nuget"]["lock"]
        nuget_lock.write_bytes(source_lock.read_bytes())
        nuget_config = nuget_lock.parent / "NuGet.Config"
        nuget_config.write_bytes(source_lock.with_name("NuGet.Config").read_bytes())
        dependencies = {}
        for name, definition in active_contract["dependencies"].items():
            version = definition["policy"]["version"]
            if "linux" not in definition["platforms"]:
                dependencies[name] = {
                    "status": "not-applicable",
                    "policyKind": "exact",
                    "requestedVersion": version,
                    "resolvedVersion": None,
                    "exceptionId": None,
                    "targets": {},
                    "capabilities": {},
                    "packages": {},
                    "condition": {"name": definition["msbuild"]["requiredWhen"], "value": False},
                }
                continue
            entry = {
                "status": "verified",
                "policyKind": "exact",
                "requestedVersion": version,
                "resolvedVersion": version,
                "exceptionId": None,
                "targets": {target: True for target in definition["cmake"]["targets"]},
                "capabilities": {capability: True for capability in definition.get("capabilities", [])},
            }
            if condition := definition["cmake"].get("requiredWhen"):
                entry["condition"] = {"name": condition, "value": True}
            dependencies[name] = entry
        return {
            "schemaVersion": 2,
            "contract": {
                "schemaVersion": 2,
                "sha256": hashlib.sha256(contract_path.read_bytes()).hexdigest(),
            },
            "host": {
                "platform": "linux",
                "vcpkgConfigurationSha256": hashlib.sha256(
                    (root / "vcpkg-configuration.json").read_bytes()
                ).hexdigest(),
                "vcpkgManifestSha256": hashlib.sha256((root / "vcpkg.json").read_bytes()).hexdigest(),
                "nugetLockSha256": hashlib.sha256(nuget_lock.read_bytes()).hexdigest(),
                "nugetConfigSha256": hashlib.sha256(nuget_config.read_bytes()).hexdigest(),
            },
            "dependencies": dependencies,
        }

    def _nuget_resolution_fixture(self, root, *, identity_ids=None, identity_versions=None):
        identity_ids = identity_ids or {}
        identity_versions = identity_versions or {}
        lock_relative = self.contract["dependencies"]["windows-app-sdk"]["nuget"]["lock"]
        lock_path = root / lock_relative
        lock_path.parent.mkdir(parents=True, exist_ok=True)
        governed = {
            definition["nuget"]["package"]: definition["policy"]["version"]
            for definition in self.contract["dependencies"].values()
            if "nuget" in definition
        }
        package_rows = "\n".join(
            f'  <package id="{package_id}" version="{version}" targetFramework="native" />'
            for package_id, version in governed.items()
        )
        lock_path.write_text(
            f'<?xml version="1.0" encoding="utf-8"?>\n<packages>\n{package_rows}\n</packages>\n',
            encoding="utf-8",
        )

        package_root = root / "restored"
        archives = {}
        for package_id, locked_version in governed.items():
            directory = package_root / f"{package_id}.{locked_version}"
            directory.mkdir(parents=True)
            archive_path = directory / f"{package_id}.{locked_version}.nupkg"
            nuspec_id = identity_ids.get(package_id, package_id)
            nuspec_version = identity_versions.get(package_id, locked_version)
            nuspec = (
                '<?xml version="1.0" encoding="utf-8"?>\n'
                '<package xmlns="http://schemas.microsoft.com/packaging/2013/05/nuspec.xsd">\n'
                "  <metadata>\n"
                f"    <id>{nuspec_id}</id>\n"
                f"    <version>{nuspec_version}</version>\n"
                "  </metadata>\n"
                "</package>\n"
            )
            with zipfile.ZipFile(archive_path, "w") as archive:
                archive.writestr(f"{package_id}.nuspec", nuspec)
            archives[package_id] = archive_path

        report = {
            "host": {"platform": "windows", "nugetPackagesDir": str(package_root)},
            "dependencies": {
                name: {"status": "verified" if "nuget" in definition else "not-applicable"}
                for name, definition in self.contract["dependencies"].items()
            },
        }
        return report, archives

    def test_initial_contract_is_valid(self):
        self.assertEqual(
            set(self.contract["dependencies"]),
            {"boost", "ftxui", "spdlog", "windows-app-sdk", "cppwinrt"},
        )
        self.assertEqual(self.contract["dependencies"]["windows-app-sdk"]["policy"]["version"], "2.3.1")
        self.assertEqual(self.contract["dependencies"]["cppwinrt"]["policy"]["version"], "3.0.260715.1")

    def test_unknown_schema_is_rejected(self):
        contract = copy.deepcopy(self.contract)
        contract["schemaVersion"] = 3

        with self.assertRaisesRegex(dependency_policy.DependencyPolicyError, "unsupported.*schema"):
            dependency_policy.validate_contract(contract)

    def test_exact_policy_accepts_only_the_contracted_version(self):
        policy = dependency_policy.effective_policy(self.contract, "boost", "linux")

        self.assertTrue(policy.accepts("1.89.0"))
        self.assertFalse(policy.accepts("1.89.1"))

    def test_range_policy_enforces_both_boundaries(self):
        contract = copy.deepcopy(self.contract)
        contract["dependencies"]["boost"]["policy"] = {
            "kind": "range",
            "minimum": "1.89.0",
            "exclusiveMaximum": "1.91.0",
        }
        policy = dependency_policy.effective_policy(contract, "boost", "linux")

        self.assertFalse(policy.accepts("1.88.9"))
        self.assertTrue(policy.accepts("1.89.0"))
        self.assertTrue(policy.accepts("1.90.9"))
        self.assertFalse(policy.accepts("1.91.0"))

    def test_unknown_dependency_exception_is_rejected(self):
        contract = copy.deepcopy(self.contract)
        contract["exceptions"] = [self._exception(dependency="unknown")]

        with self.assertRaisesRegex(dependency_policy.DependencyPolicyError, "unknown dependency"):
            dependency_policy.validate_contract(contract, today=date(2026, 7, 10))

    def test_exception_applies_only_to_its_platform(self):
        contract = copy.deepcopy(self.contract)
        contract["exceptions"] = [self._exception()]

        windows = dependency_policy.effective_policy(contract, "ftxui", "windows", today=date(2026, 7, 10))
        linux = dependency_policy.effective_policy(contract, "ftxui", "linux", today=date(2026, 7, 10))

        self.assertEqual(windows.requested, "7.0.0")
        self.assertEqual(windows.exception_id, "DEP-2026-001")
        self.assertEqual(linux.requested, "6.1.9")
        self.assertIsNone(linux.exception_id)

    def test_expired_exception_fails_closed(self):
        contract = copy.deepcopy(self.contract)
        contract["exceptions"] = [self._exception(created="2026-07-01", expires="2026-07-09")]

        with self.assertRaisesRegex(dependency_policy.DependencyPolicyError, "expired"):
            dependency_policy.validate_contract(contract, today=date(2026, 7, 10))

    def test_duplicate_dependency_platform_exception_is_rejected(self):
        contract = copy.deepcopy(self.contract)
        contract["exceptions"] = [self._exception(), self._exception(id="DEP-2026-002")]

        with self.assertRaisesRegex(dependency_policy.DependencyPolicyError, "multiple active exceptions"):
            dependency_policy.validate_contract(contract, today=date(2026, 7, 10))

    def test_stale_contract_hash_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            contract_path = self._write_contract(root)
            report = self._report(root, contract_path)
            report["contract"]["sha256"] = "0" * 64

            with self.assertRaisesRegex(dependency_policy.DependencyPolicyError, "stale"):
                dependency_policy.verify_build_report(
                    self.contract, report, contract_path=contract_path, project_root=root
                )

    def test_missing_dependency_is_rejected_and_disabled_condition_is_accepted(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            contract_path = self._write_contract(root)
            report = self._report(root, contract_path)
            del report["dependencies"]["boost"]
            with self.assertRaisesRegex(dependency_policy.DependencyPolicyError, "missing governed"):
                dependency_policy.verify_build_report(
                    self.contract, report, contract_path=contract_path, project_root=root
                )

            report = self._report(root, contract_path)
            report["dependencies"]["ftxui"] = {
                "status": "not-applicable",
                "policyKind": "exact",
                "requestedVersion": "6.1.9",
                "resolvedVersion": None,
                "exceptionId": None,
                "targets": {},
                "capabilities": {},
                "condition": {"name": "AOBUS_BUILD_TUI", "value": False},
            }
            dependency_policy.verify_build_report(self.contract, report, contract_path=contract_path, project_root=root)

    def test_nuget_config_follows_contract_lock_location(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            contract = copy.deepcopy(self.contract)
            relocated_lock = "config/windows/packages.config"
            for definition in contract["dependencies"].values():
                if "nuget" in definition:
                    definition["nuget"]["lock"] = relocated_lock
            contract_path = self._write_contract(root, contract)
            report = self._report(root, contract_path, contract)

            dependency_policy.verify_build_report(contract, report, contract_path=contract_path, project_root=root)

            (root / "config" / "windows" / "NuGet.Config").write_text("<configuration />", encoding="utf-8")

            with self.assertRaisesRegex(dependency_policy.DependencyPolicyError, "NuGet.Config changed"):
                dependency_policy.verify_build_report(contract, report, contract_path=contract_path, project_root=root)

    def test_governed_nuget_dependencies_must_share_one_lock(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            contract_path = self._write_contract(root)
            report = self._report(root, contract_path)
            contract = copy.deepcopy(self.contract)
            contract["dependencies"]["cppwinrt"]["nuget"]["lock"] = "config/cppwinrt/packages.config"
            contract_path.write_text(json.dumps(contract), encoding="utf-8")
            report["contract"]["sha256"] = hashlib.sha256(contract_path.read_bytes()).hexdigest()

            with self.assertRaisesRegex(dependency_policy.DependencyPolicyError, "share one lock file"):
                dependency_policy.verify_build_report(contract, report, contract_path=contract_path, project_root=root)

    def test_verified_nuget_dependency_requires_restored_package_root(self):
        report = {
            "host": {"platform": "windows", "nugetPackagesDir": None},
            "dependencies": {
                name: {"status": "verified" if "nuget" in definition else "not-applicable"}
                for name, definition in self.contract["dependencies"].items()
            },
        }

        with self.assertRaisesRegex(dependency_policy.DependencyPolicyError, "nugetPackagesDir"):
            dependency_policy._verify_nuget_resolution(self.contract, report)

    def test_verified_nuget_resolution_inspects_restored_archives(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            report, archives = self._nuget_resolution_fixture(root)

            resolved = dependency_policy._verify_nuget_resolution(self.contract, report, root)

        self.assertEqual(
            resolved["windows-app-sdk"],
            {
                "package": "Microsoft.WindowsAppSDK",
                "version": "2.3.1",
                "archive": str(archives["Microsoft.WindowsAppSDK"]),
            },
        )
        self.assertEqual(resolved["cppwinrt"]["version"], "3.0.260715.1")

    def test_restored_nuget_identity_accepts_normalized_zero_revision(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            report, _ = self._nuget_resolution_fixture(root, identity_versions={"Microsoft.WindowsAppSDK": "2.3.1.0"})

            resolved = dependency_policy._verify_nuget_resolution(self.contract, report, root)

        self.assertEqual(resolved["windows-app-sdk"]["version"], "2.3.1")

    def test_restored_nuget_identity_must_match_the_locked_package(self):
        cases = (
            ({"Microsoft.WindowsAppSDK": "Unexpected.Package"}, {}),
            ({}, {"Microsoft.WindowsAppSDK": "2.3.2"}),
        )
        for identity_ids, identity_versions in cases:
            with self.subTest(identity_ids=identity_ids, identity_versions=identity_versions):
                with tempfile.TemporaryDirectory() as temp_dir:
                    root = Path(temp_dir)
                    report, _ = self._nuget_resolution_fixture(
                        root,
                        identity_ids=identity_ids,
                        identity_versions=identity_versions,
                    )

                    with self.assertRaisesRegex(dependency_policy.DependencyPolicyError, "identity does not match"):
                        dependency_policy._verify_nuget_resolution(self.contract, report, root)

    def _governed_port_versions(self):
        return {
            port: definition["policy"]["version"]
            for definition in self.contract["dependencies"].values()
            if "vcpkg" in definition
            for port in definition["vcpkg"]["ports"]
        }

    @staticmethod
    def _write_vcpkg_status(status_file, versions):
        status_file.write_text(
            "\n\n".join(
                f"Package: {name}\nVersion: {version}\nArchitecture: x64-windows\n"
                "Status: install ok installed\nFeature: core"
                for name, version in versions.items()
            ),
            encoding="utf-8",
        )

    def test_vcpkg_port_revision_is_not_part_of_the_upstream_version(self):
        packages = dependency_policy.parse_vcpkg_status(
            "Package: spdlog\nVersion: 1.17.0#1\nArchitecture: x64-windows\n"
            "Status: install ok installed\nFeature: core\n"
        )

        self.assertEqual(packages["spdlog"].version, "1.17.0#1")
        self.assertEqual(packages["spdlog"].upstream_version, "1.17.0")

    def test_nix_resolution_schema_is_independent_from_contract_schema(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            revision = "test-nixpkgs-revision"
            (root / "nixpkgs.json").write_text(json.dumps({"rev": revision}), encoding="utf-8")
            nix_report_path = root / "nix-dependencies.json"
            nix_report = {
                "schemaVersion": dependency_policy.SUPPORTED_NIX_REPORT_SCHEMA,
                "nixpkgsRevision": revision,
                "dependencies": {
                    name: {
                        "version": definition["policy"]["version"],
                        "storePath": f"/nix/store/test-{name}",
                    }
                    for name, definition in self.contract["dependencies"].items()
                    if "linux" in definition["platforms"]
                },
            }
            nix_report_path.write_text(json.dumps(nix_report), encoding="utf-8")
            report = {"host": {"nixReportPath": str(nix_report_path)}}

            resolved = dependency_policy._verify_nix_resolution(self.contract, report, root)

        self.assertEqual(resolved["schemaVersion"], 1)

    def test_vcpkg_status_counts_only_installed_packages(self):
        packages = dependency_policy.parse_vcpkg_status(
            "Package: spdlog\nVersion: 1.17.0\nArchitecture: x64-windows\nStatus: install ok installed\n"
            "\n"
            "Package: boost-regex\nVersion: 1.90.0\nArchitecture: x64-windows\nStatus: deinstall ok not-installed\n"
            "\n"
            "Package: fdk-aac\nVersion: 2.0.3\nArchitecture: x64-windows\n"
        )

        self.assertEqual(set(packages), {"spdlog"})

    def test_boost_recipe_helpers_are_not_release_family_members(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            build_dir = Path(temp_dir)
            installed = build_dir / "vcpkg_installed"
            status_file = installed / "vcpkg" / "status"
            status_file.parent.mkdir(parents=True)
            versions = self._governed_port_versions()
            versions.update(
                {
                    "boost-regex": "1.89.0",
                    "vcpkg-boost": "2025-03-29",
                    "boost-vcpkg-helpers": "1.11.0",
                }
            )

            self._write_vcpkg_status(status_file, versions)
            report = {
                "host": {
                    "vcpkgInstalledDir": str(installed),
                    "vcpkgTriplet": "x64-windows",
                }
            }
            resolved = dependency_policy._verify_vcpkg_resolution(self.contract, report, build_dir)
            self.assertIn("boost-regex", resolved["boost"])
            self.assertEqual(set(resolved["recipeHelpers"]), {"vcpkg-boost", "boost-vcpkg-helpers"})

            versions["boost-regex"] = "1.90.0"
            self._write_vcpkg_status(status_file, versions)
            with self.assertRaisesRegex(dependency_policy.DependencyPolicyError, "family is mixed"):
                dependency_policy._verify_vcpkg_resolution(self.contract, report, build_dir)

    def test_injected_verification_date_governs_native_resolution(self):
        contract = copy.deepcopy(self.contract)
        contract["exceptions"] = [self._exception(created="2020-01-01", expires="2020-01-30")]
        with tempfile.TemporaryDirectory() as temp_dir:
            build_dir = Path(temp_dir)
            installed = build_dir / "vcpkg_installed"
            status_file = installed / "vcpkg" / "status"
            status_file.parent.mkdir(parents=True)
            versions = self._governed_port_versions()
            versions["ftxui"] = "7.0.0"
            self._write_vcpkg_status(status_file, versions)
            report = {
                "host": {
                    "vcpkgInstalledDir": str(installed),
                    "vcpkgTriplet": "x64-windows",
                }
            }
            resolved = dependency_policy._verify_vcpkg_resolution(contract, report, build_dir, today=date(2020, 1, 15))

        self.assertEqual(resolved["ftxui"]["ftxui"]["upstreamVersion"], "7.0.0")

    def test_human_summary_and_json_fields_are_stable(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            contract_path = self._write_contract(root)
            report = self._report(root, contract_path)
            report["nativeResolution"] = {"kind": "nix", "report": {}}

        summary = dependency_policy.format_summary(report)
        self.assertIn("DEPENDENCY  STATUS", summary)
        self.assertIn("boost       verified", summary)
        self.assertEqual(set(report), {"schemaVersion", "contract", "host", "dependencies", "nativeResolution"})


if __name__ == "__main__":
    unittest.main()
