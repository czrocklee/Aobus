"""macOS AppKit Boost.Process v2 static SDK import and its refresh projection."""

import os
import shutil
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from ao.core import bazel_headers
from ao.core import bazel_workspace as workspace

AR_ARCHIVE = b"!<arch>\n" + b"static SDK fixture; ar magic only, not a linkable SDK payload\n"


class AppKitBoostProcessRefreshTest(unittest.TestCase):
    """The real macOS refresh/projection boundary for the static Process v2 SDK.

    The installed vcpkg status, ownership inventories, headers and both
    configuration archives stay real observations of a minimal fixture; only
    the SDK payload bytes and the never-executed pkgconf resolver are fakes.
    Archive symbol/ABI acceptance and the fd_or_signal process-handle behavior
    belong to the native gates, not this projection.
    """

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-appkit-boost-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.version = "1.91.0"
        self.triplet = "x64-aobus-osx"
        self.prefix = self.root / "installed" / self.triplet
        policy = workspace._load_policy()
        boost_process = next(p for p in policy["packages"]["native"] if p["name"] == "boost_process")
        policy["packages"] = {"native": [boost_process], "test": []}
        self.policy_path = self.root / "policy.json"
        workspace._write_json(self.policy_path, policy)
        self.pkgconf = self.root / "resolver/pkgconf"
        self.pkgconf.parent.mkdir(parents=True)
        self.pkgconf.write_bytes(b"fixture resolver; never executed")
        self.pkgconf.chmod(0o755)
        self.install(self.version)

    def install(self, version):
        """Publish one coherent selected release across every observation."""
        major, minor, patch = (int(value) for value in version.split("."))
        include = self.prefix / "include"
        detail = include / "boost/process/v2/detail"
        detail.mkdir(parents=True, exist_ok=True)
        for marker in ("default_launcher.hpp", "environment.hpp", "process.hpp", "stdio.hpp"):
            (include / "boost/process/v2" / marker).write_text("// installed Process v2 API\n")
        (detail / "utf8.hpp").write_text("// installed Process v2 API\n")
        (detail / "config.hpp").write_text(
            "#define BOOST_PROCESS_V2_DECL\n#define BOOST_PROCESS_DYN_LINK\nnamespace filesystem = boost::filesystem;\n"
        )
        (include / "boost/filesystem").mkdir(parents=True, exist_ok=True)
        (include / "boost/filesystem/path.hpp").write_text("// compiled filesystem API\n")
        (include / "boost/filesystem/config.hpp").write_text(
            "#if !defined(BOOST_FILESYSTEM_DYN_LINK) && !defined(BOOST_FILESYSTEM_STATIC_LINK) && "
            "!defined(BOOST_ALL_DYN_LINK) && !defined(BOOST_ALL_STATIC_LINK)\n"
            "#define BOOST_FILESYSTEM_STATIC_LINK\n"
            "#endif\n"
        )
        (include / "boost/version.hpp").write_text(f"#define BOOST_VERSION {major * 100000 + minor * 100 + patch}\n")
        for selected in (self.prefix, self.prefix / "debug"):
            (selected / "lib/pkgconfig").mkdir(parents=True, exist_ok=True)
            for name in ("boost_process", "boost_filesystem"):
                (selected / "lib" / ("lib" + name + ".a")).write_bytes(AR_ARCHIVE)
        self.write_status(version, version)
        info = self.prefix.parent / "vcpkg/info"
        info.mkdir(parents=True, exist_ok=True)
        for path in list(info.iterdir()):
            path.unlink()
        for port, files in (
            (
                "boost-process",
                [
                    "include/boost/process/v2/default_launcher.hpp",
                    "include/boost/process/v2/environment.hpp",
                    "include/boost/process/v2/process.hpp",
                    "include/boost/process/v2/stdio.hpp",
                    "include/boost/process/v2/detail/config.hpp",
                    "include/boost/process/v2/detail/utf8.hpp",
                    "lib/libboost_process.a",
                    "debug/lib/libboost_process.a",
                ],
            ),
            (
                "boost-filesystem",
                [
                    "include/boost/filesystem/path.hpp",
                    "include/boost/filesystem/config.hpp",
                    "lib/libboost_filesystem.a",
                    "debug/lib/libboost_filesystem.a",
                ],
            ),
        ):
            (info / f"{port}_{version}_{self.triplet}.list").write_text(
                "".join(f"{self.triplet}/{file}\n" for file in files), encoding="utf-8"
            )

    def entry(self, name, version):
        return (
            f"Package: {name}\n"
            "Status: install ok installed\n"
            f"Architecture: {self.triplet}\n"
            "Multi-Arch: same\n"
            f"Version: {version}\n"
            "Port-Version: 1\n"
        )

    def write_status(self, process, filesystem):
        status = self.prefix.parent / "vcpkg/status"
        status.parent.mkdir(parents=True, exist_ok=True)
        status.write_text(
            "\n\n".join((self.entry("boost-process", process), self.entry("boost-filesystem", filesystem))) + "\n",
            encoding="utf-8",
        )

    def review(self, **fields):
        policy = workspace._json(self.policy_path)
        policy["library_fallbacks"]["boost_process"]["macos"].update(fields)
        workspace._write_json(self.policy_path, policy)

    def tool(self, name, **kwargs):
        self.assertEqual(name, "pkgconf")
        return str(self.pkgconf)

    def package(self, repository, name):
        return next(p for p in workspace._json(repository / "manifest.json")["packages"] if p["name"] == name)

    def refresh(self, config, tree):
        with (
            patch.object(workspace, "POLICY", self.policy_path),
            patch.object(workspace, "native_platform", return_value="macos"),
            patch.object(workspace, "_executable", side_effect=self.tool),
        ):
            generated = workspace.refresh_dependencies(self.root / tree, config, prefix=self.prefix, group="native")
        return generated / "native"

    def foreign(self, name):
        directory = self.root / "foreign"
        directory.mkdir(exist_ok=True)
        path = directory / name
        path.write_text("// outside the selected provider\n", encoding="utf-8")
        return path

    def test_static_refresh_projects_each_configurations_archives_and_owned_headers(self):
        for config, selected in (("release", self.prefix), ("debug", self.prefix / "debug")):
            with self.subTest(config=config):
                repository = self.refresh(config, "ok-" + config)
                manifest = workspace._json(repository / "manifest.json")
                self.assertEqual(manifest["modules"], {})
                package = self.package(repository, "boost_process")
                self.assertEqual(
                    (package["name"], package["origin"], package["linkage"]),
                    ("boost_process", "manual_fallback", "static"),
                )
                self.assertEqual(
                    package["resolved_libraries"],
                    [
                        {"name": "boost_process", "kind": "static", "file": str(selected / "lib/libboost_process.a")},
                        {
                            "name": "boost_filesystem",
                            "kind": "static",
                            "file": str(selected / "lib/libboost_filesystem.a"),
                        },
                    ],
                )
                self.assertEqual(package["interface"]["header_dirs"], [str(self.prefix / "include")])
                self.assertEqual(package["interface"]["copts"], [])
                self.assertEqual(package["interface"]["linkopts"], [])
                self.assertEqual(
                    set(package["interface"]["defines"]),
                    {
                        "BOOST_PROCESS_STATIC_LINK",
                        "BOOST_PROCESS_NO_LIB",
                        "BOOST_FILESYSTEM_STATIC_LINK",
                        "BOOST_FILESYSTEM_NO_LIB",
                    },
                )
                # Only port-owned headers and the reviewed archives are
                # projected. boost/version.hpp stays deliberately unowned in
                # this minimal fixture, proving no ambient include leaks in;
                # the complete refresh adds the coherent family ports (the
                # common boost package selects e.g. boost-headers) that own it.
                self.assertEqual(
                    {link["source"] for link in workspace._json(repository / "repository.json")["links"]},
                    {str(selected / "lib" / ("lib" + name + ".a")) for name in ("boost_process", "boost_filesystem")}
                    | {
                        str(self.prefix / "include" / relative)
                        for relative in (
                            "boost/filesystem/path.hpp",
                            "boost/filesystem/config.hpp",
                            "boost/process/v2/default_launcher.hpp",
                            "boost/process/v2/environment.hpp",
                            "boost/process/v2/process.hpp",
                            "boost/process/v2/stdio.hpp",
                            "boost/process/v2/detail/config.hpp",
                            "boost/process/v2/detail/utf8.hpp",
                        )
                    },
                )

    def test_missing_configuration_archives_never_use_the_other_configuration(self):
        (self.prefix / "debug/lib/libboost_process.a").unlink()
        with self.assertRaisesRegex(
            workspace.BazelWorkspaceError, "missing/ambiguous reviewed macos debug boost_process artifact"
        ):
            self.refresh("debug", "missing-debug")
        (self.prefix / "lib/libboost_filesystem.a").unlink()
        with self.assertRaisesRegex(
            workspace.BazelWorkspaceError, "missing/ambiguous reviewed macos release boost_filesystem artifact"
        ):
            self.refresh("release", "missing-release")

    def test_a_debug_directory_alias_to_the_release_layout_is_rejected(self):
        if os.name == "nt":
            self.skipTest("Windows symlink privileges are outside this fixture contract")
        shutil.rmtree(self.prefix / "debug")
        (self.prefix / "debug").symlink_to(self.prefix, target_is_directory=True)
        with self.assertRaisesRegex(workspace.BazelWorkspaceError, "escapes intended configuration"):
            self.refresh("debug", "debug-alias")

    def test_foreign_header_sources_fail_at_the_interface_and_inventory_boundaries(self):
        if os.name == "nt":
            self.skipTest("Windows symlink privileges are outside this fixture contract")
        marker = self.prefix / "include/boost/process/v2/process.hpp"
        marker.unlink()
        marker.symlink_to(self.foreign("process.hpp"))
        with self.assertRaisesRegex(workspace.BazelWorkspaceError, "escapes/missing from the selected vcpkg prefix"):
            self.refresh("release", "foreign-marker")
        marker.unlink()
        marker.write_text("// installed Process v2 API\n", encoding="utf-8")
        extra = self.prefix / "include/boost/process/v2/detail/utf8.hpp"
        extra.unlink()
        extra.symlink_to(self.foreign("utf8.hpp"))
        with self.assertRaisesRegex(bazel_headers.HeaderInventoryError, "escapes selected header roots"):
            self.refresh("release", "foreign-inventory")

    def test_missing_process_v2_api_headers_are_rejected(self):
        (self.prefix / "include/boost/process/v2/stdio.hpp").unlink()
        with self.assertRaisesRegex(workspace.BazelWorkspaceError, "escapes/missing from the selected vcpkg prefix"):
            self.refresh("release", "missing-marker")

    def test_process_and_filesystem_ports_must_stay_version_coherent(self):
        self.write_status(self.version, "1.90.0")
        info = self.prefix.parent / "vcpkg/info"
        (info / f"boost-filesystem_{self.version}_{self.triplet}.list").rename(
            info / f"boost-filesystem_1.90.0_{self.triplet}.list"
        )
        with self.assertRaisesRegex(
            workspace.BazelWorkspaceError, "Boost Process ports have inconsistent upstream versions"
        ):
            self.refresh("release", "port-skew")

    def test_installed_header_version_must_match_the_selected_provider(self):
        (self.prefix / "include/boost/version.hpp").write_text("#define BOOST_VERSION 109000\n", encoding="utf-8")
        with self.assertRaisesRegex(workspace.BazelWorkspaceError, "header version mismatch"):
            self.refresh("release", "header-skew")

    def test_a_std_filesystem_compiled_process_header_is_rejected(self):
        header = self.prefix / "include/boost/process/v2/detail/config.hpp"
        header.write_text(
            header.read_text(encoding="utf-8").replace(
                "namespace filesystem = boost::filesystem;", "namespace filesystem = std::filesystem;"
            ),
            encoding="utf-8",
        )
        with self.assertRaisesRegex(workspace.BazelWorkspaceError, "compiled v2 Boost.Filesystem API"):
            self.refresh("release", "std-fs")

    def test_discovered_process_metadata_is_rejected_not_consumed_as_ambient(self):
        (self.prefix / "lib/pkgconfig/boost_process.pc").write_text(
            f"Name: boost_process\nVersion: {self.version}\n", encoding="utf-8"
        )
        with self.assertRaisesRegex(
            workspace.BazelWorkspaceError, "unreviewed macOS Boost Process pkg-config metadata"
        ):
            self.refresh("release", "ambient-pc")

    def test_reviewed_dynamic_or_filesystem_version_macros_are_rejected(self):
        for defines in (
            [
                "BOOST_PROCESS_DYN_LINK",
                "BOOST_PROCESS_NO_LIB",
                "BOOST_FILESYSTEM_STATIC_LINK",
                "BOOST_FILESYSTEM_NO_LIB",
            ],
            [
                "BOOST_PROCESS_STATIC_LINK",
                "BOOST_PROCESS_NO_LIB",
                "BOOST_FILESYSTEM_STATIC_LINK",
                "BOOST_FILESYSTEM_NO_LIB",
                "BOOST_FILESYSTEM_VERSION=3",
            ],
            [
                "BOOST_PROCESS_STATIC_LINK",
                "BOOST_PROCESS_USE_STD_FS",
                "BOOST_FILESYSTEM_STATIC_LINK",
                "BOOST_FILESYSTEM_NO_LIB",
            ],
        ):
            with self.subTest(defines=defines):
                self.review(defines=defines)
                with self.assertRaisesRegex(workspace.BazelWorkspaceError, "consumer definitions must remain static"):
                    self.refresh("release", "macro-" + str(len(defines)))

    def test_non_archive_artifacts_and_dylib_aliases_fail_static_provenance(self):
        archive = self.prefix / "lib/libboost_process.a"
        archive.write_bytes(b"shared SDK fixture; not an ar archive\n")
        with self.assertRaisesRegex(workspace.BazelWorkspaceError, "is not a static ar archive"):
            self.refresh("release", "bad-magic")
        archive.write_bytes(AR_ARCHIVE)
        if os.name == "nt":
            self.skipTest("Windows symlink privileges are outside this fixture contract")
        dylib = self.prefix / "lib/libboost_process.dylib"
        dylib.write_bytes(b"shared SDK fixture; not an ar archive\n")
        archive.unlink()
        archive.symlink_to(dylib)
        with self.assertRaisesRegex(workspace.BazelWorkspaceError, "is not a static ar archive"):
            self.refresh("release", "dylib-alias")

    def test_the_two_archive_closure_is_pinned_by_the_reviewed_contract(self):
        self.review(libraries=["boost_filesystem", "boost_process"])
        with self.assertRaisesRegex(workspace.BazelWorkspaceError, "reviewed two-archive static interface"):
            self.refresh("release", "closure")

    def test_an_unaccepted_release_fails_on_the_shared_contract_not_a_bazel_pin(self):
        self.install("1.93.0")
        with self.assertRaisesRegex(workspace.BazelWorkspaceError, "does not satisfy dependency-contract.json"):
            self.refresh("release", "contract")

    def test_a_coherent_future_release_passes_the_unchanged_structural_interface(self):
        self.install("1.93.0")
        read_json = workspace.dependency_policy.read_json
        contract = read_json(workspace.dependency_policy.CONTRACT_FILE)
        contract["dependencies"]["boost"]["policy"] = {"kind": "exact", "version": "1.93.0"}
        contract["exceptions"] = [item for item in contract["exceptions"] if item["dependency"] != "boost"]

        def policy_input(path):
            return contract if path == workspace.dependency_policy.CONTRACT_FILE else read_json(path)

        # Change external input only; the loader, schema and version policy stay real.
        with patch.object(workspace.dependency_policy, "read_json", side_effect=policy_input):
            repository = self.refresh("release", "future")
        package = self.package(repository, "boost_process")
        self.assertEqual(
            [item["file"] for item in package["resolved_libraries"]],
            [
                str(self.prefix / "lib/libboost_process.a"),
                str(self.prefix / "lib/libboost_filesystem.a"),
            ],
        )


if __name__ == "__main__":
    unittest.main()
