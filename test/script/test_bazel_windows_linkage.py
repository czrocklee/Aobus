"""Bounded Windows SDK admission fixtures, not native compiler/linker acceptance."""

import copy
import hashlib
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from ao.core import bazel_dependencies as dependencies
from ao.core import bazel_deps
from ao.core import bazel_native as native
from ao.core import bazel_providers as providers


def coff_archive(*payloads):
    result = b"!<arch>\n"
    for payload in payloads:
        header = (
            b"fixture.obj/".ljust(16)
            + b"0".ljust(12)
            + b"0".ljust(6)
            + b"0".ljust(6)
            + b"644".ljust(8)
            + str(len(payload)).encode().ljust(10)
            + b"`\n"
        )
        result += header + payload + (b"\n" if len(payload) % 2 else b"")
    return result


def coff_object(section, payload, *, big=False):
    if big:
        header = struct.pack("<HHHHI", 0, 0xFFFF, 2, 0x8664, 0)
        header += bytes.fromhex("c7a1bad1eebaa94baf20faf66aa4dcb8") + bytes(16)
        header += struct.pack("<III", 1, 0, 0)
    else:
        header = struct.pack("<HHIIIHH", 0x8664, 1, 0, 0, 0, 0, 0)
    return header + struct.pack("<8sIIIIIIHHI", section, 0, 0, len(payload), len(header) + 40, 0, 0, 0, 0, 0) + payload


def static_archive(config="release", *, big=False, crt=None):
    runtime = "MDd_DynamicDebug" if config == "debug" else "MD_DynamicRelease"
    default = crt or ("MSVCRTD" if config == "debug" else "MSVCRT")
    iterator = "2" if config == "debug" else "0"
    directives = (
        f'/DEFAULTLIB:{default} /FAILIFMISMATCH:"RuntimeLibrary={runtime}" '
        + f"/FAILIFMISMATCH:_ITERATOR_DEBUG_LEVEL={iterator} /FAILIFMISMATCH:_MSC_VER=1900"
    ).encode()
    return coff_archive(coff_object(b".drectve", directives, big=big))


def uninitialized_object(size, *, big=False, flags=0xC0000080, pointer=0, section=b".bss"):
    """Allocation-only COFF section: its nonzero size has no file-backed payload."""
    result = bytearray(coff_object(section, b"", big=big))
    start = 56 if big else 20
    struct.pack_into("<II", result, start + 16, size, pointer)
    struct.pack_into("<I", result, start + 36, flags)
    return bytes(result)


def import_archive(dll, symbol="fixture_api", *, descriptor=True):
    strings = symbol.encode() + b"\x00" + dll.encode() + b"\x00"
    short = struct.pack("<HHHHIIHH", 0, 0xFFFF, 0, 0x8664, 0, len(strings), 0, 4) + strings
    members = [short]
    if descriptor:
        members.insert(0, coff_object(b".idata$6", dll.encode() + b"\x00"))
    return coff_archive(*members)


def pe_image(
    dll,
    config="release",
    *,
    symbols=("fixture_api",),
    imports=None,
    machine=0x8664,
    executable=False,
    forwarder=None,
    ordinal_only=False,
    export_target=None,
):
    """Real-shaped bounded headers/tables, deliberately not a runnable native image."""
    if imports is None:
        imports = (
            ("VCRUNTIME140D.dll", "ucrtbased.dll")
            if config == "debug"
            else ("VCRUNTIME140.dll", "api-ms-win-crt-runtime-l1-1-0.dll")
        )
    raw = bytearray(40)

    def append(value):
        result = 0x1000 + len(raw)
        raw.extend(value)
        return result

    def string(value):
        return append(value.encode() + b"\x00")

    dll_name = string(dll)
    addresses = append(bytes(4 * len(symbols)))
    symbol_names = [string(symbol) for symbol in symbols]
    names = append(b"".join(struct.pack("<I", address) for address in symbol_names))
    ordinals = append(b"".join(struct.pack("<H", index) for index in range(len(symbols))))
    forwarded = string(forwarder) if forwarder is not None else None
    export_size = len(raw)
    for index in range(len(symbols)):
        local = append(b"\xc3")  # Local code/data byte outside the export-directory range.
        target = forwarded if forwarded is not None else export_target if export_target is not None else local
        struct.pack_into("<I", raw, addresses - 0x1000 + index * 4, target)
    struct.pack_into(
        "<IIHHIIIIIII",
        raw,
        0,
        0,
        0,
        0,
        0,
        dll_name,
        1,
        len(symbols),
        0 if ordinal_only else len(symbols),
        addresses,
        names,
        ordinals,
    )
    table = append(bytes(20 * (len(imports) + 1)))
    for index, dependency in enumerate(imports):
        address = string(dependency)
        struct.pack_into("<IIIII", raw, table - 0x1000 + index * 20, 0, 0, 0, address, 0)
    dos = bytearray(64)
    dos[:2] = b"MZ"
    struct.pack_into("<I", dos, 60, 64)
    coff = struct.pack("<HHIIIHH", machine, 1, 0, 0, 0, 240, 2 | (0 if executable else 0x2000))
    optional = bytearray(240)
    struct.pack_into("<H", optional, 0, 0x20B)
    struct.pack_into("<I", optional, 56, (0x1000 + len(raw) + 0xFFF) & ~0xFFF)
    struct.pack_into("<I", optional, 108, 16)
    struct.pack_into(
        "<IIII", optional, 112, 0x1000, export_size, table if imports else 0, 20 * (len(imports) + 1) if imports else 0
    )
    section = struct.pack("<8sIIIIIIHHI", b".rdata", len(raw), 0x1000, len(raw), 512, 0, 0, 0, 0, 0)
    return (bytes(dos) + b"PE\x00\x00" + coff + bytes(optional) + section).ljust(512, b"\x00") + raw


class _WindowsSdkFixture:
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-windows-linkage-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.prefix = self.root / "installed/x64-windows"
        for directory in ("lib/pkgconfig", "bin", "debug/lib/pkgconfig", "debug/bin", "include", "tools/pkgconf"):
            (self.prefix / directory).mkdir(parents=True, exist_ok=True)
        self.info = self.prefix.parent / "vcpkg/info"
        self.info.mkdir(parents=True)
        self.version = "1.0.1"
        self.files = {"lmdb": ["include/lmdb.h"]}
        (self.prefix / "include/lmdb.h").write_text(
            "#define MDB_VERSION_MAJOR 1\n#define MDB_VERSION_MINOR 0\n#define MDB_VERSION_PATCH 1\n"
        )
        for config in ("release", "debug"):
            directory = "debug/" if config == "debug" else ""
            self.install("lmdb", directory + "lib/lmdb.lib", import_archive("lmdb.dll"))
            self.install("lmdb", directory + "bin/lmdb.dll", pe_image("lmdb.dll", config))
            self.install("lmdb", directory + "lib/pkgconfig/lmdb.pc", b"Name: lmdb\nVersion: 1.0.1\n")
        self.ownership()

    def install(self, port, relative, data):
        self.files.setdefault(port, []).append(relative)
        path = self.prefix / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return path

    def ownership(self):
        for port, files in self.files.items():
            version = "1.3.6" if port == "libogg" else self.version
            (self.info / f"{port}_{version}_x64-windows.list").write_text(
                "".join(f"x64-windows/{relative}\n" for relative in files)
            )

    def bind(self, config="release", *, name="lmdb", ports=None):
        interface = bazel_deps._Interface.parse({"libraries": [{"name": "lmdb", "kind": "shared"}]})
        return dependencies._windows_bind_interface(
            self.prefix, config, name, interface, ports or {"lmdb": self.version}, self.version
        )


class WindowsBinaryAdmissionTest(_WindowsSdkFixture, unittest.TestCase):
    def test_import_payload_is_paired_to_owned_same_configuration_pe_and_real_exports(self):
        interface, manifests, evidence = self.bind()
        self.assertEqual(
            interface["libraries"],
            [
                {
                    "name": "lmdb",
                    "kind": "shared",
                    "file": str(self.prefix / "lib/lmdb.lib"),
                    "runtime": str(self.prefix / "bin/lmdb.dll"),
                }
            ],
        )
        self.assertEqual(manifests, [self.info / "lmdb_1.0.1_x64-windows.list"])
        self.assertEqual(evidence["bin/lmdb.dll"]["crt"], ["vcruntime140.dll", "api-ms-win-crt-runtime-l1-1-0.dll"])
        debug, _, _ = self.bind("debug")
        self.assertEqual(debug["libraries"][0]["runtime"], str(self.prefix / "debug/bin/lmdb.dll"))

    def test_nonempty_uninitialized_sections_are_allocations_not_backed_bytes(self):
        file = self.prefix / "lib/CLI11.lib"
        for big in (False, True):
            payload = (
                static_archive(big=big)
                + coff_archive(uninitialized_object(4, big=big), uninitialized_object(0x128, big=big))[8:]
            )
            file.write_bytes(payload)
            with self.subTest(big=big):
                observed = dependencies._windows_coff_evidence(file, "CLI11", "release")
                self.assertEqual(observed["kind"], "static")
                self.assertIn("msvcrt", observed["crt"])
                self.assertEqual(observed["sha256"], hashlib.sha256(payload).hexdigest())

    def test_local_data_and_incremental_link_aliases_remain_exports_but_unmapped_targets_fail(self):
        file = self.prefix / "bin/lmdb.dll"
        payload = bytearray(pe_image("lmdb.dll", symbols=("fixture_api", "local_ilt_alias")))
        optional = 64 + 24
        eat = 512 + struct.unpack_from("<I", payload, 512 + 28)[0] - 0x1000
        first = struct.unpack_from("<I", payload, eat)[0]
        struct.pack_into("<I", payload, eat + 4, first)
        file.write_bytes(payload)
        self.assertTrue(
            {1, 2, "fixture_api", "local_ilt_alias"} <= dependencies._windows_pe(file, "release")["exports"]
        )
        # A zero-filled local data tail is mapped even though it has no raw bytes.
        section = optional + 240
        raw_size = struct.unpack_from("<I", payload, section + 16)[0]
        struct.pack_into("<I", payload, section + 8, raw_size + 8)
        struct.pack_into("<I", payload, eat, 0x1000 + raw_size + 4)
        file.write_bytes(payload)
        self.assertIn("fixture_api", dependencies._windows_pe(file, "release")["exports"])
        for invalid in (0x1000 + raw_size + 8, 0xFFFFFF00):
            file.write_bytes(pe_image("lmdb.dll", export_target=invalid))
            with self.subTest(target=invalid), self.assertRaisesRegex(native.BazelWorkspaceError, "export target"):
                dependencies._windows_pe(file, "release")
        malformed = bytearray(pe_image("lmdb.dll"))
        declared_raw = struct.unpack_from("<I", malformed, section + 16)[0]
        struct.pack_into("<I", malformed, section + 16, declared_raw + 1)
        file.write_bytes(malformed)
        with self.assertRaisesRegex(native.BazelWorkspaceError, "export target backing"):
            dependencies._windows_pe(file, "release")
        malformed = bytearray(pe_image("lmdb.dll"))
        struct.pack_into("<H", malformed, 64 + 6, 2)
        struct.pack_into("<I", malformed, optional + 56, 0x4000)
        malformed[section + 40 : section + 80] = struct.pack(
            "<8sIIIIIIHHI", b".data", 4, 0x3000, 4, 0, 0, 0, 0, 0, 0xC0000040
        )
        single_eat = 512 + struct.unpack_from("<I", malformed, 512 + 28)[0] - 0x1000
        struct.pack_into("<I", malformed, single_eat, 0x3000)
        file.write_bytes(malformed)
        with self.assertRaisesRegex(native.BazelWorkspaceError, "export target backing"):
            dependencies._windows_pe(file, "release")
        data = self.prefix / "bin/icudt78.dll"
        data.write_bytes(pe_image("icudt78.dll", symbols=("icudt78_dat",), imports=()))
        self.assertTrue(dependencies._windows_pe(data, "release")["data_only"])

    def test_mimalloc_redirect_is_an_owned_runtime_not_a_second_link_library(self):
        self.files["mimalloc"] = []
        self.install("mimalloc", "lib/mimalloc.dll.lib", import_archive("mimalloc.dll"))
        self.install(
            "mimalloc",
            "bin/mimalloc.dll",
            pe_image("mimalloc.dll", imports=("mimalloc-redirect.dll", "VCRUNTIME140.dll")),
        )
        self.install(
            "mimalloc",
            "bin/mimalloc-redirect.dll",
            pe_image(
                "mimalloc-redirect.dll",
                imports=("ntdll.dll",),
                symbols=(
                    "mi_allocator_done",
                    "mi_allocator_init",
                    "mi_redirect_disable",
                    "mi_redirect_enable",
                    "mi_redirect_enable_term",
                ),
            ),
        )
        self.ownership()
        interface = bazel_deps._Interface.parse({"libraries": [{"name": "mimalloc", "kind": "shared"}]})
        bound, _, observed = dependencies._windows_bind_interface(
            self.prefix, "release", "mimalloc", interface, {"mimalloc": self.version}, self.version
        )
        self.assertEqual([item["name"] for item in bound["libraries"]], ["mimalloc"])
        self.assertEqual(bound["runtime"], [str(self.prefix / "bin/mimalloc-redirect.dll")])
        self.assertTrue(observed["bin/mimalloc-redirect.dll"]["redirect"])


class WindowsSharedRefreshTest(_WindowsSdkFixture, unittest.TestCase):
    def setUp(self):
        super().setUp()
        self.policy = copy.deepcopy(providers.load_policy())
        lmdb = next(package for package in self.policy["packages"]["native"] if package["name"] == "lmdb")
        self.policy["packages"] = {"native": [lmdb], "test": []}
        self.policy_path = self.root / "policy.json"
        native.write_json(self.policy_path, self.policy)
        self.install("pkgconf", "tools/pkgconf/pkgconf.exe", b"fixture resolver; never executed").chmod(0o755)
        self.install("pkgconf", "tools/pkgconf/pkgconf-8.dll", b"owned resolver payload; never executed")
        (self.info / "pkgconf_3.0.3_x64-windows.list").write_text(
            "x64-windows/tools/pkgconf/pkgconf.exe\nx64-windows/tools/pkgconf/pkgconf-8.dll\n"
        )
        status = self.prefix.parent / "vcpkg/status"
        status.write_text(
            "Package: lmdb\nStatus: install ok installed\nArchitecture: x64-windows\nVersion: 1.0.1\n\n"
            "Package: pkgconf\nStatus: install ok installed\nArchitecture: x64-windows\nVersion: 3.0.3\n"
        )

    def query(self, argv, **kwargs):
        self.assertEqual(argv[0], str(self.prefix / "tools/pkgconf/pkgconf.exe"))
        self.assertNotIn("--static", argv)
        option, *modules = argv[1:]
        self.assertEqual(modules, ["lmdb"])
        environment = kwargs["env"]
        self.assertEqual(environment["PKG_CONFIG_DISABLE_UNINSTALLED"], "1")
        self.assertEqual(environment["PKG_CONFIG_PATH"], "")
        self.assertEqual(environment["PKG_CONFIG_LIBDIR"], str(self.prefix / "lib/pkgconfig"))
        outputs = {
            "--modversion": "1.0.1",
            "--variable=pcfiledir": str(self.prefix / "lib/pkgconfig"),
            "--print-requires": "",
            "--cflags": f'-I"{self.prefix / "include"}"',
            "--libs": f'-L"{self.prefix / "lib"}" -llmdb',
        }
        self.assertIn(option, outputs)
        return subprocess.CompletedProcess(argv, 0, outputs[option], "")

    def refresh(self):
        with (
            patch.object(providers, "POLICY", self.policy_path),
            patch.object(native, "native_platform", return_value="windows"),
            patch.object(bazel_deps.subprocess, "run", side_effect=self.query),
        ):
            return dependencies.resolve_dependencies(
                self.root / "tree", "release", group="native", prefix=str(self.prefix)
            )

    def test_refresh_keeps_metadata_real_and_adds_only_guarded_owned_dll_pairs(self):
        result = self.refresh()
        manifest = result["manifest"]
        package = manifest["packages"][0]
        self.assertEqual(package["linkage"], "shared")
        self.assertEqual(package["origin"], "automatic")
        self.assertEqual(package["resolved_libraries"][0]["runtime"], str(self.prefix / "bin/lmdb.dll"))
        self.assertEqual(manifest["applied_patches"], ["windows-lmdb-binary-pairs"])
        identity = native.read_json(self.root / "tree/windows-binaries.json")
        self.assertEqual(identity["crt"], "/MD")
        self.assertEqual(identity["packages"]["lmdb"]["lib/lmdb.lib"]["import_count"], 1)

    def test_resolver_failure_does_not_switch_to_a_complete_artifact_fallback(self):
        with (
            patch.object(providers, "POLICY", self.policy_path),
            patch.object(native, "native_platform", return_value="windows"),
            patch.object(
                bazel_deps.subprocess, "run", return_value=subprocess.CompletedProcess([], 1, "", "rejected metadata")
            ),
            self.assertRaisesRegex(bazel_deps.BazelDependencyError, "external query failed"),
        ):
            dependencies.resolve_dependencies(self.root / "failed", "release", group="native", prefix=str(self.prefix))


class WindowsBoostSharedInterfaceTest(_WindowsSdkFixture, unittest.TestCase):
    def setUp(self):
        super().setUp()
        self.policy = providers.load_policy()
        self.reviewed = self.policy["library_fallbacks"]["boost_process"]["windows"]
        headers = {
            "boost/version.hpp": "#define BOOST_VERSION 109100\n",
            "boost/process/v2/detail/config.hpp": (
                "#define BOOST_PROCESS_V2_DECL\n#define BOOST_PROCESS_DYN_LINK\n"
                "namespace filesystem = boost::filesystem;\n"
            ),
            "boost/filesystem/config.hpp": (
                "#if !defined(BOOST_FILESYSTEM_DYN_LINK) && !defined(BOOST_FILESYSTEM_STATIC_LINK) && "
                "!defined(BOOST_ALL_DYN_LINK) && !defined(BOOST_ALL_STATIC_LINK)\n"
                "#define BOOST_FILESYSTEM_STATIC_LINK\n#endif\n"
            ),
            "boost/filesystem/path.hpp": "// compiled Boost.Filesystem API\n",
        }
        for marker in ("default_launcher.hpp", "environment.hpp", "process.hpp", "stdio.hpp"):
            headers["boost/process/v2/" + marker] = "// selected compiled Process v2 API\n"
        for marker, content in headers.items():
            path = self.prefix / "include" / marker
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content)
        self.ports = {}
        for name in self.reviewed["libraries"]:
            port = name.replace("_", "-")
            self.ports[port] = "1.91.0"
            files = []
            for config in ("debug", "release"):
                directory = "debug/" if config == "debug" else ""
                spelling = name + "-vc145-mt" + ("-gd" if config == "debug" else "") + "-x64-1_91"
                for relative, payload in (
                    (directory + "lib/" + spelling + ".lib", import_archive(spelling + ".dll")),
                    (directory + "bin/" + spelling + ".dll", pe_image(spelling + ".dll", config)),
                ):
                    (self.prefix / relative).write_bytes(payload)
                    files.append("x64-windows/" + relative)
            (self.info / f"{port}_1.91.0_x64-windows.list").write_text("\n".join(files) + "\n")
        self.spec = {
            "platform": "windows",
            "config": "Debug",
            "linkage": "shared",
            "provider": {"name": "vcpkg", "version": "status-hash"},
        }

    def interface(self, config="debug"):
        return dependencies._windows_boost_process_interface(self.spec, self.prefix, config, "1.91.0", self.policy)

    def test_shared_boost_keeps_filesystem_semantics_exact_import_macros_and_toolset_join(self):
        interface = self.interface()
        self.assertIn("BOOST_PROCESS_DYN_LINK", interface["defines"])
        self.assertIn("BOOST_FILESYSTEM_DYN_LINK=1", interface["defines"])
        self.assertIn("BOOST_CONTEXT_EXPORT=EXPORT", interface["defines"])
        self.assertNotIn("BOOST_FILESYSTEM_VERSION", interface["defines"])
        typed = bazel_deps._Interface.parse(interface)
        bound, _, _ = dependencies._windows_bind_interface(
            self.prefix, "debug", "boost_process", typed, self.ports, "1.91.0"
        )
        self.assertTrue(
            all(
                library["kind"] == "shared" and "-vc145-mt-gd-x64-1_91.dll" in library["runtime"]
                for library in bound["libraries"]
            )
        )
        self.assertIn("boost_filesystem", [library["name"] for library in bound["libraries"]])
