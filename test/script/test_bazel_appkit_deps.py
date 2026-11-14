"""macOS AppKit Boost.Process v2 static SDK import and its refresh projection."""

import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from ao.core import bazel_dependencies as dependencies
from ao.core import bazel_native as native
from ao.core import bazel_providers as providers

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
        policy = providers.load_policy()
        boost_process = next(p for p in policy["packages"]["native"] if p["name"] == "boost_process")
        policy["packages"] = {"native": [boost_process], "test": []}
        self.policy_path = self.root / "policy.json"
        native.write_json(self.policy_path, policy)
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

    def tool(self, name, **kwargs):
        self.assertEqual(name, "pkgconf")
        return str(self.pkgconf)

    def package(self, repository, name):
        return next(p for p in repository["manifest"]["packages"] if p["name"] == name)

    def refresh(self, config, tree):
        with (
            patch.object(providers, "POLICY", self.policy_path),
            patch.object(native, "native_platform", return_value="macos"),
            patch.object(native, "executable", side_effect=self.tool),
        ):
            return dependencies.resolve_dependencies(self.root / tree, config, prefix=self.prefix, group="native")

    def test_static_refresh_projects_each_configurations_archives_and_owned_headers(self):
        for config, selected in (("release", self.prefix), ("debug", self.prefix / "debug")):
            with self.subTest(config=config):
                repository = self.refresh(config, "ok-" + config)
                manifest = repository["manifest"]
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
                    {file["source"] for p in repository["packages"] for tree in p["headers"] for file in tree["files"]}
                    | {library["source"] for p in repository["packages"] for library in p["libraries"]},
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

    def test_discovered_process_metadata_is_rejected_not_consumed_as_ambient(self):
        (self.prefix / "lib/pkgconfig/boost_process.pc").write_text(
            f"Name: boost_process\nVersion: {self.version}\n", encoding="utf-8"
        )
        with self.assertRaisesRegex(native.BazelWorkspaceError, "unreviewed macOS Boost Process pkg-config metadata"):
            self.refresh("release", "ambient-pc")


if __name__ == "__main__":
    unittest.main()
