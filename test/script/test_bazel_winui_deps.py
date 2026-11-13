"""Windows WinUI Boost.Process v2 shared SDK import and its refresh projection."""

import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from test_bazel_windows_linkage import import_archive, pe_image

from ao.core import bazel_dependencies as dependencies
from ao.core import bazel_native as native
from ao.core import bazel_providers as providers


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


SHARED_PROCESS_DEFINES = {
    "BOOST_PROCESS_DYN_LINK",
    "BOOST_PROCESS_NO_LIB",
    "BOOST_DATE_TIME_DYN_LINK",
    "BOOST_DATE_TIME_NO_LIB",
    "BOOST_CONTAINER_DYN_LINK",
    "BOOST_CONTAINER_NO_LIB",
    "BOOST_CONTEXT_DYN_LINK=",
    "BOOST_CONTEXT_NO_LIB=",
    "BOOST_CONTEXT_EXPORT=EXPORT",
    "BOOST_FILESYSTEM_DYN_LINK=1",
    "BOOST_FILESYSTEM_NO_LIB",
    "BOOST_ATOMIC_DYN_LINK",
    "BOOST_ATOMIC_NO_LIB",
}


class WinuiBoostProcessRefreshTest(unittest.TestCase):
    """The real Windows refresh/projection boundary for the shared Process v2 SDK.

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
        self.triplet = "x64-windows"
        self.prefix = self.root / "installed" / self.triplet
        policy = providers.load_policy()
        boost_process = next(p for p in policy["packages"]["native"] if p["name"] == "boost_process")
        policy["packages"] = {"native": [boost_process], "test": []}
        self.installed_libraries = list(policy["library_fallbacks"]["boost_process"]["windows"]["libraries"])
        self.policy_path = self.root / "policy.json"
        native.write_json(self.policy_path, policy)
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

    def assert_shared_process_abi(self, defines):
        """Literal installed producer definitions, not values copied from the policy under test."""
        self.assertEqual(set(defines), SHARED_PROCESS_DEFINES)
        macros = {define.split("=", 1)[0] for define in defines}
        for macro in (
            "BOOST_PROCESS_STATIC_LINK",
            "BOOST_FILESYSTEM_STATIC_LINK",
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
            (selected / "bin").mkdir(exist_ok=True)
            for library in self.installed_libraries:
                # Reset only this temporary fixture's producer/configuration
                # variants, so republishing an install cannot retain old tags.
                for previous in [
                    *(selected / "lib").glob(library + "-*.lib"),
                    *(selected / "bin").glob(library + "-*.dll"),
                ]:
                    previous.unlink()
                dll = Path(self.archive(library, version, config)).with_suffix(".dll").name
                (selected / "lib" / self.archive(library, version, config)).write_bytes(import_archive(dll))
                imports = None
                if library == "boost_process":
                    filesystem = Path(self.archive("boost_filesystem", version, config)).with_suffix(".dll").name
                    crt = ("VCRUNTIME140D.dll", "ucrtbased.dll") if config == "debug" else ("VCRUNTIME140.dll",)
                    imports = (filesystem, *crt)
                (selected / "bin" / dll).write_bytes(pe_image(dll, config, imports=imports))
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
                directory = "debug/" if config == "debug" else ""
                archive = self.archive(library, version, config)
                files += [directory + "lib/" + archive, directory + "bin/" + Path(archive).with_suffix(".dll").name]
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

    def selected(self, config):
        return self.prefix if config == "release" else self.prefix / "debug"

    def tool(self, name, **kwargs):
        self.fail(f"no executable selection belongs to this Windows projection: {name}")

    def package(self, repository, name):
        return next(p for p in repository["manifest"]["packages"] if p["name"] == name)

    def refresh(self, config, tree):
        with (
            patch.object(providers, "POLICY", self.policy_path),
            patch.object(native, "native_platform", return_value="windows"),
            patch.object(native, "executable", side_effect=self.tool),
        ):
            return dependencies.resolve_dependencies(self.root / tree, config, prefix=self.prefix, group="native")

    def test_shared_refresh_projects_each_configurations_import_dll_pairs_and_owned_headers(self):
        for config, archives in (
            ("release", ("boost_process-vc145-mt-x64-1_91.lib", "boost_filesystem-vc145-mt-x64-1_91.lib")),
            ("debug", ("boost_process-vc145-mt-gd-x64-1_91.lib", "boost_filesystem-vc145-mt-gd-x64-1_91.lib")),
        ):
            with self.subTest(config=config):
                library_dir = self.selected(config) / "lib"
                repository = self.refresh(config, "ok-" + config)
                manifest = repository["manifest"]
                self.assertEqual(manifest["modules"], {})
                self.assertEqual(manifest["configuration"]["linkage"], "shared")
                package = self.package(repository, "boost_process")
                self.assertEqual(
                    (package["name"], package["origin"], package["linkage"]),
                    ("boost_process", "manual_fallback", "shared"),
                )
                # Literal Process and compiled Filesystem anchors at the selected
                # configuration root; the complete archive closure stays owned by
                # the reviewed policy and the native link gates, not re-expected here.
                runtime_dir = self.selected(config) / "bin"
                resolved = {
                    item["name"]: (item["kind"], item["file"], item["runtime"])
                    for item in package["resolved_libraries"]
                }
                self.assertEqual(
                    resolved["boost_process"],
                    (
                        "shared",
                        str(library_dir / archives[0]),
                        str(runtime_dir / Path(archives[0]).with_suffix(".dll")),
                    ),
                )
                self.assertEqual(
                    resolved["boost_filesystem"],
                    (
                        "shared",
                        str(library_dir / archives[1]),
                        str(runtime_dir / Path(archives[1]).with_suffix(".dll")),
                    ),
                )
                self.assertEqual({Path(file).parent for _, file, _ in resolved.values()}, {library_dir})
                self.assertEqual({Path(dll).parent for _, _, dll in resolved.values()}, {runtime_dir})
                self.assertEqual(package["interface"]["runtime"], [])
                self.assertEqual(package["interface"]["header_dirs"], [str(self.prefix / "include")])
                self.assertEqual(package["interface"]["copts"], [])
                # Representative SDK system-library transport only; the complete
                # system closure is the native WinUI link gate's acceptance.
                self.assertEqual(
                    package["interface"]["linkopts"],
                    [
                        "Synchronization.lib",
                        "ntdll.lib",
                        "shell32.lib",
                        "advapi32.lib",
                        "user32.lib",
                        "ws2_32.lib",
                        "mswsock.lib",
                        "uuid.lib",
                    ],
                )
                self.assert_shared_process_abi(package["interface"]["defines"])
                # Port-owned anchors are projected into the workspace, and the
                # deliberately unowned boost/version.hpp proves no ambient
                # include leaks in; the complete refresh adds the coherent
                # family ports that own it.
                sources = {
                    file["source"] for p in repository["packages"] for tree in p["headers"] for file in tree["files"]
                } | {
                    value
                    for p in repository["packages"]
                    for library in p["libraries"]
                    for value in (library["source"], library["runtime"])
                }
                self.assertIn(str(self.prefix / "include" / "boost/process/v2/process.hpp"), sources)
                self.assertIn(str(library_dir / archives[0]), sources)
                self.assertIn(str(runtime_dir / Path(archives[0]).with_suffix(".dll")), sources)
                self.assertNotIn(str(self.prefix / "include" / "boost/version.hpp"), sources)

    def test_discovered_process_metadata_is_rejected_not_consumed_as_ambient(self):
        (self.prefix / "lib/pkgconfig/boost_process.pc").write_text(
            f"Name: boost_process\nVersion: {self.version}\n", encoding="utf-8"
        )
        with self.assertRaisesRegex(native.BazelWorkspaceError, "unreviewed windows Boost Process pkg-config metadata"):
            self.refresh("release", "ambient-pc")

    def test_bigobj_object_members_remain_static_not_misread_as_short_imports(self):
        archive = self.prefix / "lib" / self.archive("boost_process", self.version, "release")
        archive.write_bytes(BIGOBJ_ARCHIVE)
        self.assertEqual(dependencies.coff_archive_kind(archive, "boost_process", "release"), "static")
        with self.assertRaisesRegex(native.BazelWorkspaceError, "not an import library"):
            self.refresh("release", "bigobj")

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
                self.assertEqual(dependencies.coff_archive_kind(file, file.name, "release"), expected)


if __name__ == "__main__":
    unittest.main()
