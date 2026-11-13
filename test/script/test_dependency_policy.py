"""Tests for governed dependency policy and installed resolver identities."""

import copy
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

    def test_exact_policy_accepts_only_the_contracted_version(self):
        contract = copy.deepcopy(self.contract)
        contract["dependencies"]["boost"]["policy"] = {"kind": "exact", "version": "1.2.3"}
        policy = dependency_policy.effective_policy(contract, "boost", "linux")

        self.assertTrue(policy.accepts("1.2.3"))
        self.assertFalse(policy.accepts("1.2.4"))

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

    def test_exception_applies_only_to_its_platform(self):
        contract = copy.deepcopy(self.contract)
        contract["dependencies"]["ftxui"]["policy"] = {"kind": "exact", "version": "1.2.3"}
        contract["exceptions"] = [self._exception()]

        windows = dependency_policy.effective_policy(contract, "ftxui", "windows", today=date(2026, 7, 10))
        linux = dependency_policy.effective_policy(contract, "ftxui", "linux", today=date(2026, 7, 10))

        self.assertEqual(windows.requested, "7.0.0")
        self.assertEqual(windows.exception_id, "DEP-2026-001")
        self.assertEqual(linux.requested, "1.2.3")
        self.assertIsNone(linux.exception_id)

    def test_expired_exception_fails_closed(self):
        contract = copy.deepcopy(self.contract)
        contract["exceptions"] = [self._exception(created="2026-07-01", expires="2026-07-09")]

        with self.assertRaisesRegex(dependency_policy.DependencyPolicyError, "expired"):
            dependency_policy.validate_contract(contract, today=date(2026, 7, 10))

    def test_each_platform_declares_exactly_its_native_resolver(self):
        cases = (
            ("icu", lambda d: d.pop("vcpkg"), "vcpkg resolver for macOS"),
            ("icu", lambda d: d.pop("nix"), r"icu\.nix must be a JSON object"),
            ("icu", lambda d: d.update(nuget={"package": "ICU", "lock": "packages.config"}), "exactly one Windows"),
            ("cppwinrt", lambda d: d.pop("nuget"), "exactly one Windows"),
            ("cppwinrt", lambda d: d.update(nix={"attribute": "cppwinrt"}), "Nix resolver without Linux"),
            ("cppwinrt", lambda d: d["nuget"].pop("lock"), r"nuget\.lock must be a non-empty string"),
            (
                "cppwinrt",
                lambda d: d.update(policy={"kind": "range", "minimum": "3.0"}),
                "NuGet policy must be exact",
            ),
        )
        for name, change, message in cases:
            contract = copy.deepcopy(self.contract)
            change(contract["dependencies"][name])
            with self.subTest(name=name, message=message):
                with self.assertRaisesRegex(dependency_policy.DependencyPolicyError, message):
                    dependency_policy.validate_contract(contract, today=date(2026, 7, 10))

    @staticmethod
    def _restored_package(root, package_id, locked_version, *, nuspec_id=None, nuspec_version=None):
        directory = root / f"{package_id}.{locked_version}"
        directory.mkdir(parents=True)
        archive_path = directory / f"{package_id}.{locked_version}.nupkg"
        nuspec = (
            '<?xml version="1.0" encoding="utf-8"?>\n'
            '<package xmlns="http://schemas.microsoft.com/packaging/2013/05/nuspec.xsd">\n'
            "  <metadata>\n"
            f"    <id>{nuspec_id or package_id}</id>\n"
            f"    <version>{nuspec_version or locked_version}</version>\n"
            "  </metadata>\n"
            "</package>\n"
        )
        with zipfile.ZipFile(archive_path, "w") as archive:
            archive.writestr(f"{package_id}.nuspec", nuspec)
        return archive_path

    def test_restored_nuget_archive_matches_requested_identity_case_insensitively(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            archive = self._restored_package(root, "Example.Package", "1.2.3", nuspec_id="example.package")

            self.assertEqual(dependency_policy.verify_nuget_package(root, "Example.Package", "1.2.3"), archive)

    def test_restored_nuget_identity_accepts_normalized_zero_revision(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            archive = self._restored_package(root, "Microsoft.WindowsAppSDK", "2.4.0", nuspec_version="2.4.0.0")

            self.assertEqual(dependency_policy.verify_nuget_package(root, "Microsoft.WindowsAppSDK", "2.4.0"), archive)

    def test_restored_nuget_identity_must_match_the_locked_package(self):
        for nuspec_id, nuspec_version in (("Unexpected.Package", None), (None, "2.3.2"), (None, "2.4.0-preview")):
            with self.subTest(nuspec_id=nuspec_id, nuspec_version=nuspec_version):
                with tempfile.TemporaryDirectory() as temp_dir:
                    root = Path(temp_dir)
                    self._restored_package(
                        root, "Microsoft.WindowsAppSDK", "2.4.0", nuspec_id=nuspec_id, nuspec_version=nuspec_version
                    )

                    with self.assertRaisesRegex(dependency_policy.DependencyPolicyError, "identity does not match"):
                        dependency_policy.verify_nuget_package(root, "Microsoft.WindowsAppSDK", "2.4.0")

    def test_vcpkg_port_revision_is_not_part_of_the_upstream_version(self):
        packages = dependency_policy.parse_vcpkg_status(
            "Package: spdlog\nVersion: 1.17.0#1\nArchitecture: x64-windows\n"
            "Status: install ok installed\nFeature: core\n"
        )

        self.assertEqual(packages["spdlog"].version, "1.17.0#1")
        self.assertEqual(packages["spdlog"].upstream_version, "1.17.0")

    def test_vcpkg_status_preserves_separate_port_revision_and_normalizes_zero(self):
        for version, revision_field, expected in (
            ("1.17.0", "", "1.17.0"),
            ("1.17.0", "Port-Version: 0\n", "1.17.0"),
            ("1.17.0", "Port-Version: 3\n", "1.17.0#3"),
            ("1.17.0#3", "", "1.17.0#3"),
            ("1.17.0#3", "Port-Version: 3\n", "1.17.0#3"),
            ("1.17.0#0", "Port-Version: 0\n", "1.17.0"),
        ):
            with self.subTest(version=version, revision_field=revision_field):
                packages = dependency_policy.parse_vcpkg_status(
                    f"Package: spdlog\nVersion: {version}\n{revision_field}"
                    "Architecture: x64-windows\nStatus: install ok installed\n"
                )
                self.assertEqual(packages["spdlog"].version, expected)
                self.assertEqual(packages["spdlog"].upstream_version, "1.17.0")
                self.assertEqual(packages["spdlog"].features, ("core",))

    def test_vcpkg_status_checks_explicit_feature_revision_without_inventing_a_base(self):
        base = "Package: icu\nVersion: 78.3\nPort-Version: 1\nArchitecture: x64-windows\nStatus: install ok installed\n"
        for fields, matches in (
            ("", True),
            ("Version: 78.3\n", True),
            ("Version: 78.3#1\n", True),
            ("Version: 78.3\nPort-Version: 1\n", True),
            ("Port-Version: 1\n", True),
            ("Port-Version: 0\n", False),
            ("Version: 78.3#2\n", False),
            ("Version: 79.0\nPort-Version: 1\n", False),
        ):
            feature = f"Package: icu\nFeature: tools\n{fields}Architecture: x64-windows\nStatus: install ok installed\n"
            self.assertEqual(dependency_policy.parse_vcpkg_status(feature), {})
            for records in ((base, feature), (feature, base)):
                with self.subTest(fields=fields, feature_first=records[0] == feature):
                    if matches:
                        package = dependency_policy.parse_vcpkg_status("\n\n".join(records))["icu"]
                        self.assertEqual(package.version, "78.3#1")
                        self.assertEqual(package.features, ("core", "tools"))
                    else:
                        with self.assertRaisesRegex(dependency_policy.DependencyPolicyError, "conflicting identities"):
                            dependency_policy.parse_vcpkg_status("\n\n".join(records))

    def test_vcpkg_status_merges_versionless_features_before_or_after_installed_base(self):
        base = (
            "Package: icu\nVersion: 78.3\nPort-Version: 1\nArchitecture: x64-aobus-osx\nStatus: install ok installed\n"
        )
        for version_field in ("", "Version: 78.3\n"):
            feature = (
                f"Package: icu\nFeature: tools\n{version_field}Architecture: x64-aobus-osx\n"
                "Status: install ok installed\n"
            )
            for paragraphs in ((base, feature), (feature, base)):
                with self.subTest(version_field=version_field, feature_first=paragraphs[0] == feature):
                    packages = dependency_policy.parse_vcpkg_status("\n\n".join(paragraphs))
                    self.assertEqual(packages["icu"].version, "78.3#1")
                    self.assertEqual(packages["icu"].upstream_version, "78.3")
                    self.assertEqual(packages["icu"].triplet, "x64-aobus-osx")
                    self.assertEqual(packages["icu"].features, ("core", "tools"))

    def test_vcpkg_status_counts_only_installed_packages(self):
        packages = dependency_policy.parse_vcpkg_status(
            "Package: spdlog\nVersion: 1.17.0\nArchitecture: x64-windows\nStatus: install ok installed\n"
            "\n"
            "Package: boost-regex\nVersion: 1.90.0\nArchitecture: x64-windows\nStatus: deinstall ok not-installed\n"
            "\n"
            "Package: fdk-aac\nVersion: 2.0.3\nArchitecture: x64-windows\n"
        )

        self.assertEqual(set(packages), {"spdlog"})


class VcpkgManifestFeatureTest(unittest.TestCase):
    """The vcpkg manifest keeps frontend packages an opt-out feature.

    Provider-manifest contract, independent of any build graph: the frontends
    feature owns the frontend-only packages, stays a default feature, and never
    leaks into the base dependency set that Core-only resolution installs.
    """

    def test_frontend_packages_remain_default_but_not_unconditional_core_inputs(self):
        manifest = json.loads((dependency_policy.PROJECT_ROOT / "vcpkg.json").read_text(encoding="utf-8"))
        base = {entry if isinstance(entry, str) else entry["name"] for entry in manifest["dependencies"]}
        frontend = set(manifest["features"]["frontends"]["dependencies"])
        self.assertEqual(frontend, {"cli11", "ftxui", "stb"})
        self.assertFalse(base & frontend)
        self.assertIn("frontends", manifest["default-features"])
        self.assertIn("tests", manifest["default-features"])


if __name__ == "__main__":
    unittest.main()
