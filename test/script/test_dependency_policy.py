"""Tests for governed dependency policy and resolution evidence."""

import copy
import hashlib
import json
import tempfile
import unittest
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
        path.write_text(json.dumps(contract or self.contract), encoding="utf-8")
        return path

    def _report(self, root, contract_path):
        for name in ("vcpkg-configuration.json", "vcpkg.json"):
            (root / name).write_text("{}\n", encoding="utf-8")
        dependencies = {}
        for name, definition in self.contract["dependencies"].items():
            version = definition["policy"]["version"]
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
            "schemaVersion": 1,
            "contract": {
                "schemaVersion": 1,
                "sha256": hashlib.sha256(contract_path.read_bytes()).hexdigest(),
            },
            "host": {
                "platform": "linux",
                "vcpkgConfigurationSha256": hashlib.sha256(
                    (root / "vcpkg-configuration.json").read_bytes()
                ).hexdigest(),
                "vcpkgManifestSha256": hashlib.sha256((root / "vcpkg.json").read_bytes()).hexdigest(),
            },
            "dependencies": dependencies,
        }

    def test_initial_contract_is_valid(self):
        self.assertEqual(set(self.contract["dependencies"]), {"boost", "ftxui", "spdlog"})

    def test_unknown_schema_is_rejected(self):
        contract = copy.deepcopy(self.contract)
        contract["schemaVersion"] = 2

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

    def _governed_port_versions(self):
        return {
            port: definition["policy"]["version"]
            for definition in self.contract["dependencies"].values()
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
