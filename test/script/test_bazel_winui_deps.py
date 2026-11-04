"""Windows WinUI Boost.Process v2 static SDK import and its refresh projection."""

import copy
import os
import shutil
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from ao.core import bazel_deps, bazel_headers
from ao.core import bazel_workspace as workspace


def _archive_member(name: bytes, payload: bytes) -> bytes:
    """One well-formed COFF archive member; payload bytes are not a linkable image."""
    if len(payload) % 2:
        payload += b"\n"
    return (
        name.ljust(16, b" ")
        + b"0".ljust(12, b" ")
        + b"0".ljust(6, b" ")
        + b"0".ljust(6, b" ")
        + b"644".ljust(8, b" ")
        + str(len(payload)).encode("ascii").ljust(10, b" ")
        + b"`\n"
        + payload
    )


def _fixture_archive(*members: bytes) -> bytes:
    return b"!<arch>\n" + b"".join(members)


def _coff_section(name: bytes, size: int, offset: int = 60) -> bytes:
    """A real-shaped COFF section header; standard MS COFF constants, not version pins."""
    return (
        name.ljust(8, b"\x00")
        + (0).to_bytes(4, "little")  # virtual size
        + (0).to_bytes(4, "little")  # virtual address
        + size.to_bytes(4, "little")  # raw data size
        + offset.to_bytes(4, "little")  # raw data pointer
        + (0).to_bytes(4, "little")  # relocations
        + (0).to_bytes(4, "little")  # line numbers
        + (0).to_bytes(2, "little")
        + (0).to_bytes(2, "little")
        + (0x60000020).to_bytes(4, "little")  # executable code section
    )


def _coff_object(section: bytes, payload: bytes) -> bytes:
    """A minimal real-shaped AMD64 COFF object: header, one named section, representative data."""
    return (
        (0x8664).to_bytes(2, "little")  # AMD64 machine
        + (1).to_bytes(2, "little")  # one section
        + (0).to_bytes(4, "little")  # timestamp
        + (0).to_bytes(4, "little")  # symbol table
        + (0).to_bytes(4, "little")  # symbol count
        + (0).to_bytes(2, "little")  # no optional header
        + (0).to_bytes(2, "little")  # characteristics
        + _coff_section(section, len(payload))
        + payload
    )


def _short_import_object() -> bytes:
    """A real-shaped COFF short import object (Version 0): a DLL import, not a static member."""
    return (
        (0).to_bytes(2, "little")  # anonymous object signature
        + (0xFFFF).to_bytes(2, "little")
        + (0).to_bytes(2, "little")  # Version: short import object
        + (0x8664).to_bytes(2, "little")
        + (0).to_bytes(4, "little")  # timestamp
        + (12).to_bytes(4, "little")  # name size
        + (0).to_bytes(2, "little")  # hint/ordinal
        + (0).to_bytes(2, "little")  # import type: code
        + b"FIXTURE.dll\x00"
    )


def _bigobj_object() -> bytes:
    """A bounded BigObj header with the standard COFF UUID, one section and data."""
    return (
        (0).to_bytes(2, "little")
        + (0xFFFF).to_bytes(2, "little")
        + (2).to_bytes(2, "little")
        + (0x8664).to_bytes(2, "little")
        + (0).to_bytes(4, "little")
        + bytes.fromhex("c7a1bad1eebaa94baf20faf66aa4dcb8")
        + bytes(16)  # reserved fields
        + (1).to_bytes(4, "little")  # NumberOfSections
        + bytes(8)  # symbol table location/count
        + _coff_section(b".text", 1, offset=96)
        + b"\xc3"
    )


STATIC_ARCHIVE = _fixture_archive(
    # Index members whose payloads mimic import and object shapes prove the
    # skip set; only the real object member provides native provenance.
    _archive_member(b"/", b"\x00\x00\xff\xff\x00\x00" + b"linker symbol index fixture; never an object\n"),
    _archive_member(b"//", b"\x64\x86" + b"long-name string table fixture; never an object\n"),
    _archive_member(b"fixture.obj/", _coff_object(b".text", b"\x48\x89\x5c\x24\x08" + b"\x48\x8b\x05\xc3")),
)
IMPORT_ARCHIVE = _fixture_archive(_archive_member(b"fixture.obj/", _short_import_object()))
DESCRIPTOR_ARCHIVE = _fixture_archive(
    _archive_member(b"fixture.obj/", _coff_object(b".idata$2", b"\x28\x00\x00\x00\x00\x00\x00\x00"))
)
BIGOBJ_ARCHIVE = _fixture_archive(_archive_member(b"fixture.obj/", _bigobj_object()))


class WinuiBoostProcessRefreshTest(unittest.TestCase):
    """The real Windows refresh/projection boundary for the static Process v2 SDK.

    The installed vcpkg status, host pkgconf admission, ownership inventories,
    headers, the reviewed configuration and the dependency contract stay real
    observations of a minimal fixture; an alternate release enters only as
    valid contract input at the load_contract boundary, never as a faked
    policy acceptance. Only the SDK payload bytes and the never-executed
    pkgconf resolver are fakes. Archive symbol/ABI acceptance and the native
    WinUI link belong to the native gates, not this projection.
    """

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-winui-boost-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.version = "1.91.0"
        self.toolset = "vc145"
        self.triplet = "x64-windows-static-md"
        self.prefix = self.root / "installed" / self.triplet
        policy = workspace._load_policy()
        boost_process = next(p for p in policy["packages"]["native"] if p["name"] == "boost_process")
        policy["packages"] = {"native": [boost_process], "test": []}
        self.installed_libraries = list(policy["library_fallbacks"]["boost_process"]["windows"]["libraries"])
        self.policy_path = self.root / "policy.json"
        workspace._write_json(self.policy_path, policy)
        host_tools = self.prefix.parent / "x64-windows/tools/pkgconf"
        host_tools.mkdir(parents=True)
        resolver = host_tools / "pkgconf.exe"
        resolver.write_bytes(b"fixture resolver; never executed")
        resolver.chmod(0o755)
        (host_tools / "pkgconf-8.dll").write_bytes(b"fixture resolver; never executed")
        self.install(self.version)

    def archive(self, library, version, config):
        """The installed producer spelling: observed toolset ABI plus the shared status version."""
        major, minor, *_ = version.split(".")
        return f"{library}-{self.toolset}-mt{'-gd' if config == 'debug' else ''}-x64-{major}_{minor}.lib"

    def ports(self):
        return [library.replace("_", "-") for library in self.installed_libraries]

    def assert_static_process_abi(self, defines):
        """Consumer ABI anchors: static Process/Filesystem linkage, never a dynamic or pinned variant."""
        macros = {define.split("=", 1)[0] for define in defines}
        for macro in (
            "BOOST_PROCESS_STATIC_LINK",
            "BOOST_PROCESS_NO_LIB",
            "BOOST_FILESYSTEM_STATIC_LINK",
            "BOOST_FILESYSTEM_NO_LIB",
        ):
            self.assertIn(macro, macros)
        for macro in (
            "BOOST_PROCESS_DYN_LINK",
            "BOOST_FILESYSTEM_DYN_LINK",
            "BOOST_PROCESS_USE_STD_FS",
            "BOOST_PROCESS_V2_STANDALONE",
            "BOOST_FILESYSTEM_VERSION",
        ):
            self.assertNotIn(macro, macros)

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
        for config in ("release", "debug"):
            selected = self.selected(config)
            (selected / "lib/pkgconfig").mkdir(parents=True, exist_ok=True)
            for library in self.installed_libraries:
                # Reset only this temporary fixture's producer/configuration
                # variants, so republishing an install cannot retain old tags.
                for previous in (selected / "lib").glob(library + "-*.lib"):
                    previous.unlink()
                (selected / "lib" / self.archive(library, version, config)).write_bytes(STATIC_ARCHIVE)
        self.write_status({port: version for port in self.ports()})
        info = self.prefix.parent / "vcpkg/info"
        info.mkdir(parents=True, exist_ok=True)
        for path in list(info.iterdir()):
            path.unlink()
        (info / "pkgconf_3.0.3_x64-windows.list").write_text(
            "x64-windows/tools/pkgconf/pkgconf.exe\nx64-windows/tools/pkgconf/pkgconf-8.dll\n", encoding="utf-8"
        )
        process_headers = [
            "boost/process/v2/default_launcher.hpp",
            "boost/process/v2/environment.hpp",
            "boost/process/v2/process.hpp",
            "boost/process/v2/stdio.hpp",
            "boost/process/v2/detail/config.hpp",
            "boost/process/v2/detail/utf8.hpp",
        ]
        owned_headers = {
            "boost-process": [f"include/{relative}" for relative in process_headers],
            "boost-filesystem": ["include/boost/filesystem/path.hpp", "include/boost/filesystem/config.hpp"],
        }
        for library in self.installed_libraries:
            port = library.replace("_", "-")
            files = list(owned_headers.get(port, []))
            for config in ("release", "debug"):
                files.append(("debug/" if config == "debug" else "") + "lib/" + self.archive(library, version, config))
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

    def write_status(self, versions):
        """versions maps each installed closure port to its status version."""
        status = self.prefix.parent / "vcpkg/status"
        status.parent.mkdir(parents=True, exist_ok=True)
        entries = [self.entry(port, version) for port, version in versions.items()]
        entries.append("Package: pkgconf\nStatus: install ok installed\nArchitecture: x64-windows\nVersion: 3.0.3\n")
        status.write_text("\n\n".join(entries) + "\n", encoding="utf-8")

    def review(self, **fields):
        policy = workspace._json(self.policy_path)
        policy["library_fallbacks"]["boost_process"]["windows"].update(fields)
        workspace._write_json(self.policy_path, policy)

    def alternate_release_contract(self, version):
        """A self-contained contract copy whose Boost policy alone admits the alternate release."""
        contract = copy.deepcopy(workspace.dependency_policy.load_contract())
        contract["dependencies"]["boost"]["policy"] = {"kind": "exact", "version": version}
        contract["exceptions"] = [item for item in contract["exceptions"] if item["dependency"] != "boost"]
        return contract

    def selected(self, config):
        return self.prefix if config == "release" else self.prefix / "debug"

    def tool(self, name, **kwargs):
        self.fail(f"no executable selection belongs to this Windows projection: {name}")

    def package(self, repository, name):
        return next(p for p in workspace._json(repository / "manifest.json")["packages"] if p["name"] == name)

    def refresh(self, config, tree):
        with (
            patch.object(workspace, "POLICY", self.policy_path),
            patch.object(workspace, "native_platform", return_value="windows"),
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
        for config, archives in (
            ("release", ("boost_process-vc145-mt-x64-1_91.lib", "boost_filesystem-vc145-mt-x64-1_91.lib")),
            ("debug", ("boost_process-vc145-mt-gd-x64-1_91.lib", "boost_filesystem-vc145-mt-gd-x64-1_91.lib")),
        ):
            with self.subTest(config=config):
                library_dir = self.selected(config) / "lib"
                repository = self.refresh(config, "ok-" + config)
                manifest = workspace._json(repository / "manifest.json")
                self.assertEqual(manifest["modules"], {})
                self.assertEqual(manifest["configuration"]["linkage"], "static")
                package = self.package(repository, "boost_process")
                self.assertEqual(
                    (package["name"], package["origin"], package["linkage"]),
                    ("boost_process", "manual_fallback", "static"),
                )
                # Literal Process and compiled Filesystem anchors at the selected
                # configuration root; the complete archive closure stays owned by
                # the reviewed policy and the native link gates, not re-expected here.
                resolved = {item["name"]: (item["kind"], item["file"]) for item in package["resolved_libraries"]}
                self.assertEqual(resolved["boost_process"], ("static", str(library_dir / archives[0])))
                self.assertEqual(resolved["boost_filesystem"], ("static", str(library_dir / archives[1])))
                self.assertEqual({Path(file).parent for _, file in resolved.values()}, {library_dir})
                self.assertEqual(package["interface"]["header_dirs"], [str(self.prefix / "include")])
                self.assertEqual(package["interface"]["copts"], [])
                # Representative SDK system-library transport only; the complete
                # system closure is the native WinUI link gate's acceptance.
                self.assertIn("Synchronization.lib", package["interface"]["linkopts"])
                self.assertIn("ntdll.lib", package["interface"]["linkopts"])
                self.assert_static_process_abi(package["interface"]["defines"])
                # Port-owned anchors are projected into the workspace, and the
                # deliberately unowned boost/version.hpp proves no ambient
                # include leaks in; the complete refresh adds the coherent
                # family ports that own it.
                sources = {link["source"] for link in workspace._json(repository / "repository.json")["links"]}
                self.assertIn(str(self.prefix / "include" / "boost/process/v2/process.hpp"), sources)
                self.assertIn(str(library_dir / archives[0]), sources)
                self.assertNotIn(str(self.prefix / "include" / "boost/version.hpp"), sources)

    def test_missing_configuration_archives_never_use_the_other_configuration(self):
        (self.prefix / "debug/lib" / self.archive("boost_process", self.version, "debug")).unlink()
        with self.assertRaisesRegex(
            workspace.BazelWorkspaceError, "missing/ambiguous reviewed windows debug boost_process artifact"
        ):
            self.refresh("debug", "missing-debug")
        (self.prefix / "lib" / self.archive("boost_filesystem", self.version, "release")).unlink()
        with self.assertRaisesRegex(
            workspace.BazelWorkspaceError, "missing/ambiguous reviewed windows release boost_filesystem artifact"
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

    def test_closure_ports_are_required_and_stay_version_coherent(self):
        info = self.prefix.parent / "vcpkg/info"
        for port in ("boost-filesystem", "boost-date-time", "boost-atomic"):
            with self.subTest(port=port):
                versions = {name: self.version for name in self.ports()}
                versions[port] = "1.90.0"
                self.write_status(versions)
                (info / f"{port}_{self.version}_{self.triplet}.list").rename(
                    info / f"{port}_1.90.0_{self.triplet}.list"
                )
                with self.assertRaisesRegex(
                    workspace.BazelWorkspaceError, "Boost Process ports have inconsistent upstream versions"
                ):
                    self.refresh("release", "port-skew-" + port)
                self.install(self.version)
        versions = {name: self.version for name in self.ports() if name != "boost-atomic"}
        self.write_status(versions)
        (info / f"boost-atomic_{self.version}_{self.triplet}.list").unlink()
        with self.assertRaisesRegex(workspace.BazelWorkspaceError, "boost-atomic is not installed"):
            self.refresh("release", "atomic-missing")

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
            workspace.BazelWorkspaceError, "unreviewed windows Boost Process pkg-config metadata"
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

    def test_import_library_and_non_archive_artifacts_fail_static_provenance(self):
        archive = self.prefix / "lib" / self.archive("boost_process", self.version, "release")
        for content, message in (
            (b"shared SDK fixture; not a COFF archive\n", "is not a static COFF archive"),
            (IMPORT_ARCHIVE, "is an import library"),
            # A full import-descriptor object is an import-library payload too.
            (DESCRIPTOR_ARCHIVE, "is an import library"),
            (b"!<arch>\n" + b"truncated fixture member header\n", "malformed reviewed windows release boost_process"),
            (b"!<arch>\n", "no native object members"),
            (
                _fixture_archive(_archive_member(b"/", b"index-only fixture; no object payload\n")),
                "no native object members",
            ),
            (
                _fixture_archive(_archive_member(b"fixture.obj/", b"fixture payload; not a COFF object\n")),
                "is not an x64 object",
            ),
        ):
            with self.subTest(message=message):
                archive.write_bytes(content)
                with self.assertRaisesRegex(workspace.BazelWorkspaceError, message):
                    self.refresh("release", "provenance")
        archive.write_bytes(STATIC_ARCHIVE)

    def test_bigobj_object_members_are_admitted_not_misread_as_imports(self):
        archive = self.prefix / "lib" / self.archive("boost_process", self.version, "release")
        archive.write_bytes(BIGOBJ_ARCHIVE)
        repository = self.refresh("release", "bigobj")
        package = self.package(repository, "boost_process")
        self.assertIn(str(archive), [item["file"] for item in package["resolved_libraries"]])

    def test_anonymous_signature_alone_does_not_admit_a_bigobj(self):
        archive = self.prefix / "lib" / self.archive("boost_process", self.version, "release")
        valid = _bigobj_object()
        for payload in (
            valid[:20],
            valid[:56],  # full header, missing its section table
            valid[:12] + bytes(16) + valid[28:],  # wrong COFF UUID
            valid[:6] + (0x14C).to_bytes(2, "little") + valid[8:],  # not AMD64
            valid[:44] + bytes(4) + valid[48:],  # no object sections
        ):
            with self.subTest(payload=payload.hex()):
                archive.write_bytes(_fixture_archive(_archive_member(b"fixture.obj/", payload)))
                with self.assertRaises(workspace.BazelWorkspaceError):
                    self.refresh("release", "invalid-bigobj")

    def test_the_coff_classifier_admits_native_and_import_payload_kinds(self):
        directory = self.root / "kinds"
        directory.mkdir()
        for index, (content, expected) in enumerate(
            (
                (STATIC_ARCHIVE, "static"),
                (BIGOBJ_ARCHIVE, "static"),
                (IMPORT_ARCHIVE, "interface"),
                (DESCRIPTOR_ARCHIVE, "interface"),
                (
                    # A mixed native/import archive classifies as interface;
                    # the SDK consumer renders the import role from this kind.
                    _fixture_archive(
                        _archive_member(b"fixture.obj/", _coff_object(b".text", b"\x48\x8b\x05\xc3")),
                        _archive_member(b"fixture.dll/", _short_import_object()),
                    ),
                    "interface",
                ),
            )
        ):
            with self.subTest(expected=expected):
                file = directory / f"fixture-{index}.lib"
                file.write_bytes(content)
                self.assertEqual(workspace._windows_coff_archive_kind(file, file.name, "release"), expected)

    def test_a_coherent_alternative_producer_toolset_is_admitted_without_a_pin(self):
        self.toolset = "vc150"
        self.install(self.version)
        repository = self.refresh("release", "alternative-toolset")
        package = self.package(repository, "boost_process")
        self.assertTrue(all(f"-{self.toolset}-" in item["file"] for item in package["resolved_libraries"]))

    def test_incoherent_or_ambiguous_toolset_archives_fail_the_installed_producer_abi(self):
        library = self.prefix / "lib"
        (library / self.archive("boost_atomic", self.version, "release")).rename(
            library / "boost_atomic-vc144-mt-x64-1_91.lib"
        )
        with self.assertRaisesRegex(workspace.BazelWorkspaceError, "incoherent installed producer toolset"):
            self.refresh("release", "toolset-skew")
        self.install(self.version)
        process = library / self.archive("boost_process", self.version, "release")
        (library / "boost_process-vc146-mt-x64-1_91.lib").write_bytes(process.read_bytes())
        with self.assertRaisesRegex(
            workspace.BazelWorkspaceError, "missing/ambiguous reviewed windows release boost_process artifact"
        ):
            self.refresh("release", "toolset-ambiguous")

    def test_static_crt_and_wrong_configuration_spellings_are_rejected(self):
        library = self.prefix / "lib"
        for name in (
            "boost_process-vc145-mt-gd-x64-1_91.lib",
            "boost_process-vc145-mt-s-x64-1_91.lib",
            "boost_process-vc145-mt-sgd-x64-1_91.lib",
        ):
            with self.subTest(name=name):
                (library / name).write_bytes(STATIC_ARCHIVE)
                with self.assertRaisesRegex(workspace.BazelWorkspaceError, "unsupported static provider shape"):
                    self.refresh("release", "shape")
                (library / name).unlink()

    def test_the_process_to_filesystem_closure_order_is_pinned_by_the_reviewed_contract(self):
        for libraries in (
            ["boost_filesystem", "boost_process", "boost_atomic"],
            ["boost_process", "boost_atomic"],
        ):
            with self.subTest(libraries=libraries):
                self.review(libraries=libraries)
                with self.assertRaisesRegex(workspace.BazelWorkspaceError, "reviewed static archive closure"):
                    self.refresh("release", "closure-" + str(len(libraries)))

    def test_system_library_tails_must_stay_within_the_reviewed_spec(self):
        self.review(linkopts=["crypt32.lib"])
        with self.assertRaisesRegex(bazel_deps.BazelDependencyError, "unsupported link flag: crypt32.lib"):
            self.refresh("release", "foreign-tail")

    def test_an_unaccepted_release_fails_on_the_shared_contract_not_a_bazel_pin(self):
        self.install("1.93.0")
        with self.assertRaisesRegex(workspace.BazelWorkspaceError, "does not satisfy dependency-contract.json"):
            self.refresh("release", "contract")

    def test_a_coherent_future_release_passes_the_unchanged_structural_interface(self):
        self.install("1.93.0")
        # Only external JSON input changes; the real loader, schema validator,
        # version policy and structural projection still execute.
        contract = self.alternate_release_contract("1.93.0")
        read_json = workspace.dependency_policy.read_json

        def policy_input(path):
            return contract if path == workspace.dependency_policy.CONTRACT_FILE else read_json(path)

        with patch.object(workspace.dependency_policy, "read_json", side_effect=policy_input):
            repository = self.refresh("release", "future")
        package = self.package(repository, "boost_process")
        resolved = {item["name"]: item["file"] for item in package["resolved_libraries"]}
        # The observed Process archive follows the alternate release's own
        # version tag; the installed header version stays real validation.
        self.assertEqual(resolved["boost_process"], str(self.prefix / "lib" / "boost_process-vc145-mt-x64-1_93.lib"))
        self.assert_static_process_abi(package["interface"]["defines"])


if __name__ == "__main__":
    unittest.main()
