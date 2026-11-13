"""Governed audio interfaces without configuring or building the product graph."""

import json
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from test_bazel_windows_linkage import import_archive, pe_image, static_archive

from ao.core import bazel_dependencies as dependencies
from ao.core import bazel_deps, dependency_policy
from ao.core import bazel_native as native
from ao.core import bazel_providers as providers


def observed_sources(resolution):
    return (
        [file["source"] for p in resolution["packages"] for tree in p["headers"] for file in tree["files"]]
        + [library["source"] for p in resolution["packages"] for library in p["libraries"]]
        + [path for p in resolution["packages"] for path in p["runtime"]]
        + [library["runtime"] for p in resolution["packages"] for library in p["libraries"] if library["runtime"]]
    )


class BazelAudioPolicyTest(unittest.TestCase):
    def setUp(self):
        self.policy = native.read_json(providers.POLICY)

    def test_linux_backends_are_selected_only_by_the_explicit_audio_os_contract(self):
        for host in ("linux", "macos", "windows"):
            names = {p["name"] for p in providers.package_groups(self.policy, host=host)["native"]}
            with self.subTest(host=host):
                self.assertEqual(
                    names & {"alsa", "pipewire", "udev"}, {"alsa", "pipewire", "udev"} if host == "linux" else set()
                )

    def test_metadata_version_translations_follow_closed_producer_formats(self):
        # ALAC: a full three-part release, with the producer's optional unstable date suffix.
        for version, release in (
            ("0.0.7-unstable-2026-04-10", "0.0.7"),
            ("0.0.7", "0.0.7"),
            ("1.2.3-unstable-2027-01-05", "1.2.3"),
        ):
            with self.subTest(version=version):
                self.assertEqual(dependencies._metadata_versions("alac", version, ["alac"], "linux"), {"alac": release})
        # Non-Linux ALAC observations keep their own provider's identity spelling.
        self.assertEqual(
            dependencies._metadata_versions("alac", "2017-11-03-c38887c5", ["alac"], "windows"),
            {"alac": "2017-11-03-c38887c5"},
        )
        # udev: any dotted numeric release maps to its major component.
        for version, major in (("261.3", "261"), ("262.0", "262"), ("261.3.1", "261"), ("261", "261")):
            with self.subTest(version=version):
                self.assertEqual(
                    dependencies._metadata_versions("udev", version, ["libudev"], "linux"), {"libudev": major}
                )
        # Identity packages keep the provider's spelling for any future release.
        self.assertEqual(dependencies._metadata_versions("opus", "1.7.0", ["opus"], "linux"), {"opus": "1.7.0"})
        for name, version, modules in (
            ("alac", "0.0.7-unstable-new", ["alac"]),
            ("alac", "0.0.7-unstable-2026-4-10", ["alac"]),
            ("alac", "0.0", ["alac"]),
            ("alac", "0.0.7.1", ["alac"]),
            ("alac", "0.0.7", ["other"]),
            ("udev", "261.x", ["libudev"]),
            ("udev", "261.3", ["other"]),
        ):
            with (
                self.subTest(name=name, version=version, modules=modules),
                self.assertRaisesRegex(native.BazelWorkspaceError, "metadata version"),
            ):
                dependencies._metadata_versions(name, version, modules, "linux")

    def test_opus_parent_include_supplement_is_guarded_and_not_redundant(self):
        spec = {"platform": "linux", "config": "Debug", "provider": {"name": "nix", "version": "revision"}}
        interface = {"header_dirs": ["/provider/include"]}

        def metadata_flags(flags):
            def query(spec, option, *, package=None):
                # The supplement is a compile-interface contract; reject any other
                # metadata query role instead of answering every call identically.
                self.assertEqual((option, package), ("--cflags", "opus"))
                return flags

            return query

        with patch.object(dependencies, "_metadata_flags", side_effect=metadata_flags(["-I/provider/include/opus"])):
            result = dependencies._compile_interface_patch(spec, "opus", interface)
        self.assertEqual(
            result["when"],
            {
                "package": "opus",
                "platform": "linux",
                "config": "Debug",
                "provider": "nix",
                "provider_version": "revision",
            },
        )
        self.assertEqual(result["expect"]["missing"], result["add"])
        self.assertEqual(result["satisfy"], [])
        with patch.object(dependencies, "_metadata_flags", side_effect=metadata_flags(["-I/provider/include"])):
            self.assertIsNone(dependencies._compile_interface_patch(spec, "opus", interface))


class BazelAudioSpecTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-audio-spec-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.tree = self.root / "workspace"
        self.tree.mkdir()
        self.provider = self.root / "provider"
        for marker in ("opus/opus.h", "catch/fakeit.hpp"):
            header = self.provider / "include" / marker
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text("fixture header\n", encoding="utf-8")
        self.pc = self.provider / "lib/pkgconfig"
        self.pc.mkdir(parents=True)
        (self.pc / "opus.pc").write_text("Name: opus\nVersion: 1.6.1\n", encoding="utf-8")
        policy = native.read_json(providers.POLICY)
        policy["packages"] = {
            "native": [p for p in policy["packages"]["native"] if p["name"] == "opus"],
            "test": [p for p in policy["packages"]["test"] if p["name"] == "fakeit"],
        }
        self.policy = self.root / "policy.json"
        self.policy.write_text(json.dumps(policy), encoding="utf-8")
        self.versions = {"opus": "1.6.1", "fakeit": "2.5.0"}
        self.resolver = self.provider / "bin/resolver"
        self.resolver.parent.mkdir()
        self.resolver.touch()
        self.resolver.chmod(0o755)
        (self.provider / "lib/libopus.so").write_bytes(b"fixture shared codec")

    def provider_spec(self, policy, *, identity_path):
        native.write_json(
            identity_path, {"packages": {name: {"outputs": [str(self.provider)]} for name in self.versions}}
        )
        return (
            {
                "provider": {"name": "nix", "version": "reviewed-revision", "evidence": []},
                "allowed_roots": [str(self.provider)],
                "library_dirs": [str(self.provider / "lib")],
                "pkg_config": {"executable": str(self.resolver), "pc_dirs": [str(self.pc)]},
                "elf": {"readelf": str(self.resolver)},
            },
            self.versions,
            {name: self.provider for name in self.versions},
        )

    def query(self, argv, **kwargs):
        self.assertEqual(argv[0], str(self.resolver))
        if "-d" in argv:
            self.assertEqual(argv, [str(self.resolver), "-d", str(self.provider / "lib/libopus.so")])
            return subprocess.CompletedProcess(argv, 0, "(SONAME) [libopus.so]", "")
        # Linux codecs are shared imports; a --static query would change the set.
        self.assertNotIn("--static", argv)
        options = [value for value in argv[1:] if value.startswith("--")]
        self.assertEqual(len(options), 1, argv)
        option, module = options[0], argv[-1]
        self.assertEqual(module, "opus")
        if option == "--modversion":
            text = self.versions["opus"]
        elif option == "--variable=pcfiledir":
            text = str(self.pc)
        elif option in ("--print-requires", "--print-requires-private"):
            text = ""
        elif option == "--cflags":
            text = f'-I"{self.provider / "include/opus"}"'
        elif option == "--libs":
            text = f'-L"{self.provider / "lib"}" -lopus'
        else:
            self.fail(f"unexpected pkg-config query: {argv}")
        return subprocess.CompletedProcess(argv, 0, text, "")

    def refresh(self):
        def metadata_flags(spec, option, *, package=None):
            # Only the opus compile-interface supplement may query metadata here.
            self.assertEqual((option, package), ("--cflags", "opus"))
            return ["-I" + str(self.provider / "include/opus")]

        with (
            patch.object(native, "native_platform", return_value="linux"),
            patch.object(providers, "POLICY", self.policy),
            patch.object(providers, "linux_provider", side_effect=self.provider_spec),
            patch.object(dependencies, "_metadata_flags", side_effect=metadata_flags),
            patch.object(providers, "verify_linux_resolution"),
            patch.object(bazel_deps.subprocess, "run", side_effect=self.query),
            patch.object(bazel_deps, "_Adapter", wraps=bazel_deps._Adapter) as adapters,
        ):
            self.resolutions = {
                group: dependencies.resolve_dependencies(self.tree, "debug", group=group)
                for group in ("native", "test")
            }
            return adapters.call_args_list

    def test_linux_specs_keep_metadata_opus_and_complete_header_only_catch_fakeit(self):
        calls = self.refresh()
        self.assertEqual(len(calls), 2)
        native_config = self.resolutions["native"]["manifest"]["configuration"]
        tests = self.resolutions["test"]["manifest"]["configuration"]
        self.assertEqual(
            native_config["packages"], [{"name": "opus", "modules": ["opus"], "expected_versions": {"opus": "1.6.1"}}]
        )
        self.assertEqual(tests["packages"], [{"name": "fakeit", "modules": []}])
        fallback = calls[1].args[1]["patches"][0]
        self.assertEqual(
            fallback["when"],
            {
                "package": "fakeit",
                "platform": "linux",
                "config": "Debug",
                "provider": "nix",
                "provider_version": "reviewed-revision",
            },
        )
        self.assertEqual(fallback["expect"]["unmet_requirements"], ["manual_interface"])
        self.assertEqual(fallback["add"], {"header_dirs": [str(self.provider / "include/catch")]})
        self.assertEqual(fallback["expect"]["missing"], fallback["add"])
        (self.provider / "include/catch/new-header.hpp").touch()
        self.assertEqual(len(self.refresh()), 2)
        self.assertIn(str(self.provider / "include/catch/new-header.hpp"), observed_sources(self.resolutions["test"]))
        self.assertFalse((self.tree / "generated/test/artifacts").exists())


class BazelNativeAudioInterfaceTest(unittest.TestCase):
    def test_native_identity_retains_raw_status_port_revision_for_reviewed_interface_guards(self):
        with tempfile.TemporaryDirectory(prefix="aobus-audio-status-") as directory:
            root = Path(directory).resolve()
            prefix = root / "x64-windows"
            for relative in ("include", "debug/lib/pkgconfig"):
                (prefix / relative).mkdir(parents=True)
            host_tools = root / "x64-windows/tools/pkgconf"
            host_tools.mkdir(parents=True)
            for name in ("pkgconf.exe", "pkgconf-8.dll"):
                (host_tools / name).touch()
            info = root / "vcpkg/info"
            info.mkdir(parents=True)
            (info / "pkgconf_3.0.3_x64-windows.list").write_text(
                "x64-windows/tools/pkgconf/pkgconf.exe\nx64-windows/tools/pkgconf/pkgconf-8.dll\n", encoding="utf-8"
            )
            status = root / "vcpkg/status"
            status.parent.mkdir(exist_ok=True)
            status.write_text(
                "Package: alac\nVersion: 2017-11-03-c38887c5\nPort-Version: 4\n"
                "Architecture: x64-windows\nStatus: install ok installed\n\n"
                "Package: pkgconf\nVersion: 3.0.3\nArchitecture: x64-windows\nStatus: install ok installed\n",
                encoding="utf-8",
            )
            policy = {
                "packages": {
                    "native": [{"name": "alac", "modules": ["alac"], "nix": "alac", "vcpkg": ["alac"]}],
                    "test": [],
                }
            }
            identity_path = root / "identity.json"
            _, versions = providers.native_provider(
                prefix, "debug", "windows", identity_path=identity_path, policy=policy
            )
            self.assertEqual(versions, {"alac": "2017-11-03-c38887c5"})
            self.assertEqual(native.read_json(identity_path)["packages"]["alac"]["version"], "2017-11-03-c38887c5#4")
            status.write_text(status.read_text().replace("Port-Version: 4", "Port-Version: invalid"), encoding="utf-8")
            with self.assertRaisesRegex(dependency_policy.DependencyPolicyError, "port revision"):
                providers.native_provider(prefix, "debug", "windows", identity_path=identity_path, policy=policy)

    def test_alac_complete_archive_interface_is_configuration_and_shape_guarded(self):
        with tempfile.TemporaryDirectory(prefix="aobus-alac-interface-") as directory:
            root = Path(directory).resolve()
            header = root / "include/alac/ALACDecoder.h"
            header.parent.mkdir(parents=True)
            header.touch()
            # Read the reviewed fallback from the shipped policy: the fixture archive
            # spellings witness the real JSON instead of mirroring it here.
            policy = {"library_fallbacks": native.read_json(providers.POLICY)["library_fallbacks"]}
            for config in ("debug", "release"):
                selected = root / ("debug" if config == "debug" else "")
                (selected / "lib").mkdir(parents=True, exist_ok=True)
                for host, archive in (("windows", "libalac.lib"), ("macos", "liblibalac.a")):
                    file = selected / "lib" / archive
                    file.write_bytes(b"fixture static archive")
                    with self.subTest(host=host, config=config):
                        interface = dependencies._library_interface({"name": "alac"}, [root], config, host, policy)
                        self.assertEqual(interface["header_dirs"], [str(root / "include")])
                        self.assertEqual(
                            interface["libraries"], [{"name": "libalac", "kind": "static", "file": str(file)}]
                        )
                        file.unlink()
                        with self.assertRaisesRegex(native.BazelWorkspaceError, "missing/ambiguous"):
                            dependencies._library_interface({"name": "alac"}, [root], config, host, policy)
                        file.write_bytes(b"fixture static archive")
            (root / "debug/lib/libalacd.lib").touch()
            with self.assertRaisesRegex(native.BazelWorkspaceError, "unsupported static provider shape"):
                dependencies._library_interface({"name": "alac"}, [root], "debug", "windows", policy)


class BazelWindowsAudioInterfaceTest(unittest.TestCase):
    """Shared Windows ICU/FLAC contracts; binary fixtures do not prove a native link."""

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-windows-audio-interface-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.prefix, self.tree = self.root / "x64-windows", self.root / "workspace"
        self.tree.mkdir()
        for marker, content in (
            ("unicode/uvernum.h", "#define U_ICU_VERSION_MAJOR_NUM 78\n#define U_ICU_VERSION_MINOR_NUM 3\n"),
            ("FLAC/export.h", "#if 0 && !(defined(DLL_EXPORT))\n#define FLAC_API\n#endif\n"),
            ("unicode/platform.h", "#define U_IMPORT __declspec(dllimport)\n"),
            (
                "unicode/utypes.h",
                "#if defined(U_COMBINED_IMPLEMENTATION)\n#elif defined(U_STATIC_IMPLEMENTATION)\n"
                "#define U_DATA_API\n#define U_COMMON_API\n#define U_I18N_API\n#define U_LAYOUT_API\n"
                "#define U_LAYOUTEX_API\n#define U_IO_API\n#define U_TOOLUTIL_API\n"
                "#elif defined(U_COMMON_IMPLEMENTATION)\n#endif\n",
            ),
        ):
            header = self.prefix / "include" / marker
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text(content, encoding="utf-8")
        policy = native.read_json(providers.POLICY)
        policy["packages"] = {
            "native": [p for p in policy["packages"]["native"] if p["name"] in {"icu", "flac"}],
            "test": [],
        }
        self.policy_path = self.root / "policy.json"
        self.policy_path.write_text(json.dumps(policy), encoding="utf-8")
        self.versions = {"icu": "78.3", "flac": "1.5.0"}
        self.status_versions = {"libflac": "1.5.0", "libogg": "1.3.6#1"}
        self.icu_status = "78.3#1"
        self.resolver = self.prefix / "resolver"
        self.resolver.touch()
        self.resolver.chmod(0o755)
        info = self.root / "vcpkg/info"
        info.mkdir(parents=True)
        for port, version, owned in (
            ("icu", "78.3", ["unicode/uvernum.h", "unicode/utypes.h", "unicode/platform.h"]),
            ("libflac", "1.5.0", ["FLAC/export.h"]),
            ("libogg", "1.3.6", []),
        ):
            files = ["include/" + header for header in owned]
            libraries = {"icu": ["icuin", "icuuc", "icudt"], "libflac": ["FLAC"], "libogg": ["ogg"]}[port]
            for config in ("debug", "release"):
                directory = "debug/" if config == "debug" else ""
                for name in libraries:
                    spelling = name + ("d" if name.startswith("icu") and config == "debug" else "")
                    files += [
                        directory + "lib/" + spelling + ".lib",
                        directory + "bin/" + spelling + ("78" if name.startswith("icu") else "") + ".dll",
                    ]
            (info / f"{port}_{version}_x64-windows.list").write_text(
                "".join(f"x64-windows/{relative}\n" for relative in files), encoding="utf-8"
            )
        for config in ("debug", "release"):
            selected = self.selected(config)
            library, pcdir = selected / "lib", selected / "lib/pkgconfig"
            pcdir.mkdir(parents=True)
            for module in ("flac", "icu-i18n", "icu-uc"):
                (pcdir / (module + ".pc")).touch()
            for name in ("FLAC", "ogg", "icuin", "icuuc", "icudt"):
                spelling = name + ("d" if name.startswith("icu") and config == "debug" else "")
                dll = spelling + ("78" if name.startswith("icu") else "") + ".dll"
                (library / (spelling + ".lib")).write_bytes(import_archive(dll))
                runtime = selected / "bin" / dll
                runtime.parent.mkdir(exist_ok=True)
                crt = ("VCRUNTIME140D.dll", "ucrtbased.dll") if config == "debug" else ("VCRUNTIME140.dll",)
                suffix = "d" if config == "debug" else ""
                private = {
                    "FLAC": ["ogg.dll"],
                    "icuin": [f"icuuc{suffix}78.dll"],
                    "icuuc": [f"icudt{suffix}78.dll"],
                }.get(name, [])
                runtime.write_bytes(
                    pe_image(
                        dll,
                        config,
                        symbols=("icudt78_dat",) if name == "icudt" else ("fixture_api",),
                        imports=() if name == "icudt" else (*private, *crt),
                    )
                )

    def selected(self, config):
        return self.prefix / ("debug" if config == "debug" else "")

    def provider(self, prefix, config, host, *, identity_path, policy):
        native.write_json(
            identity_path,
            {
                "packages": {
                    name: {"version": version}
                    for name, version in {"icu": self.icu_status, **self.status_versions}.items()
                }
            },
        )
        selected = self.selected(config)
        return {
            "provider": {"name": "vcpkg", "version": "status-hash", "evidence": []},
            "allowed_roots": [str(self.prefix)],
            "library_dirs": [str(selected / "lib")],
            "pkg_config": {"executable": str(self.resolver), "pc_dirs": [str(selected / "lib/pkgconfig")]},
        }, self.versions

    def query(self, argv, **kwargs):
        self.assertEqual(argv[0], str(self.resolver))
        options = [value for value in argv[1:] if value.startswith("--") and value != "--static"]
        self.assertEqual(len(options), 1, argv)
        option, module = options[0], argv[-1]
        self.assertIn(module, ("flac", "icu-uc", "icu-i18n"))
        self.assertNotIn("--static", argv)
        selected = self.selected(self.active_config)
        if option == "--modversion":
            text = self.versions["flac"] if module == "flac" else self.versions["icu"]
        elif option == "--variable=pcfiledir":
            text = str(selected / "lib/pkgconfig")
        elif option in ("--print-requires", "--print-requires-private"):
            text = ""
        elif option == "--cflags":
            text = f'-I"{self.prefix / "include"}"'
        elif option == "--libs":
            libraries = ["FLAC"] if module == "flac" else ["icuin", "icuuc"]
            if module != "flac" and self.active_config == "debug":
                libraries = [name + "d" for name in libraries]
            text = f'-L"{selected / "lib"}" ' + " ".join("-l" + name for name in libraries)
        else:
            self.fail(f"unexpected pkg-config query: {argv}")
        return subprocess.CompletedProcess(argv, 0, text, "")

    def refresh(self, config, contract=None):
        self.active_config = config
        with (
            patch.object(native, "native_platform", return_value="windows"),
            patch.object(providers, "POLICY", self.policy_path),
            patch.object(providers, "vcpkg_prefix", return_value=self.prefix),
            patch.object(providers, "native_provider", side_effect=self.provider),
            patch.object(bazel_deps.subprocess, "run", side_effect=self.query),
            patch.object(
                dependency_policy,
                "load_contract",
                return_value=dependency_policy.load_contract() if contract is None else contract,
            ),
        ):
            self.resolution = dependencies.resolve_dependencies(self.tree, config, group="native")
        return self.resolution["manifest"]

    def plan_sources(self):
        return observed_sources(self.resolution)

    def test_shared_interfaces_separate_public_imports_from_private_runtime(self):
        for config in ("debug", "release"):
            with self.subTest(config=config):
                manifest = self.refresh(config)
                self.assertEqual(manifest["configuration"]["linkage"], "shared")
                self.assertEqual(manifest["configuration"]["config"], config.title())
                packages = {package["name"]: package for package in manifest["packages"]}
                selected = self.selected(config)
                flac = packages["flac"]
                self.assertEqual(
                    flac["resolved_libraries"],
                    [
                        {
                            "name": "FLAC",
                            "kind": "shared",
                            "file": str(selected / "lib/FLAC.lib"),
                            "runtime": str(selected / "bin/FLAC.dll"),
                        }
                    ],
                )
                self.assertNotIn("FLAC__NO_DLL", flac["interface"]["defines"])
                self.assertEqual(flac["interface"]["runtime"], [str(selected / "bin/ogg.dll")])
                suffix = "d" if config == "debug" else ""
                icu = packages["icu"]
                self.assertEqual(
                    icu["resolved_libraries"],
                    [
                        {
                            "name": name + suffix,
                            "kind": "shared",
                            "file": str(selected / "lib" / (name + suffix + ".lib")),
                            "runtime": str(selected / "bin" / (name + suffix + "78.dll")),
                        }
                        for name in ("icuin", "icuuc")
                    ],
                )
                self.assertNotIn("U_STATIC_IMPLEMENTATION", icu["interface"]["defines"])
                self.assertEqual(icu["interface"]["linkopts"], [])
                self.assertEqual(icu["interface"]["runtime"], [str(selected / "bin" / ("icudt" + suffix + "78.dll"))])
                self.assertIn("windows-icu-binary-pairs", manifest["applied_patches"])
                admitted = next(p for p in self.resolution["packages"] if p["name"] == "icu")
                self.assertEqual(admitted["linkopts"], icu["interface"]["linkopts"])
                self.assertEqual(
                    [library["source"] for library in admitted["libraries"]],
                    [str(selected / "lib" / (name + suffix + ".lib")) for name in ("icuin", "icuuc")],
                )
                sources = self.plan_sources()
                self.assertIn(str(selected / "lib/FLAC.lib"), sources)
                self.assertNotIn(str(selected / "lib/ogg.lib"), sources)
                self.assertIn(str(selected / "bin/ogg.dll"), sources)
                self.assertIn(str(self.prefix / "include/FLAC/export.h"), sources)


class BazelWindowsAudioCodecProjectionTest(unittest.TestCase):
    """Representative codec metadata and SDK tails projected through the real adapter."""

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-windows-codec-projection-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.prefix = self.root / "x64-windows"
        self.include = self.prefix / "include"
        for marker in ("mpg123.h", "fdk-aac.h", "opus.h", "opus/opus.h"):
            header = self.include / marker
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text("fixture header\n", encoding="utf-8")
        self.library_dir = self.prefix / "lib"
        self.library_dir.mkdir(parents=True)
        (self.prefix / "bin").mkdir()
        for library in ("mpg123", "fdk-aac", "opus"):
            (self.library_dir / (library + ".lib")).write_bytes(import_archive(library + ".dll"))
            (self.prefix / "bin" / (library + ".dll")).write_bytes(pe_image(library + ".dll"))
        self.pcdir = self.library_dir / "pkgconfig"
        self.pcdir.mkdir()
        for module in ("libmpg123", "fdk-aac", "opus"):
            (self.pcdir / (module + ".pc")).touch()
        info = self.root / "vcpkg/info"
        info.mkdir(parents=True)
        for port, version, owned in (
            ("mpg123", "1.33.7", ["mpg123.h"]),
            ("fdk-aac", "2.0.3", ["fdk-aac.h"]),
            ("opus", "1.6.1", ["opus.h", "opus/opus.h"]),
        ):
            (info / f"{port}_{version}_x64-windows.list").write_text(
                "".join(f"x64-windows/include/{header}\n" for header in owned)
                + f"x64-windows/lib/{port}.lib\nx64-windows/bin/{port}.dll\n",
                encoding="utf-8",
            )
        self.resolver = self.root / "resolver"
        self.resolver.touch()
        self.resolver.chmod(0o755)
        self.versions = {"fdk_aac": "2.0.3", "mpg123": "1.33.7", "opus": "1.6.1"}
        # Producer observations; the closure comparison against the shipped policy
        # is the behavior under test, not a mirror of it.
        self.metadata_libraries = {"fdk-aac": ["fdk-aac"], "libmpg123": ["mpg123"], "opus": ["opus"]}
        policy = native.read_json(providers.POLICY)
        policy["packages"] = {
            "native": [p for p in policy["packages"]["native"] if p["name"] in {"fdk_aac", "mpg123", "opus"}],
            "test": [],
        }
        self.policy_path = self.root / "policy.json"
        self.policy_path.write_text(json.dumps(policy), encoding="utf-8")
        self.tree = self.root / "workspace"
        self.tree.mkdir()

    def provider(self, prefix, config, host, *, identity_path, policy):
        status = {"mpg123": "1.33.7", "fdk-aac": "2.0.3", "opus": "1.6.1"}
        native.write_json(
            identity_path,
            {"packages": {port: {"version": version + "#0"} for port, version in status.items()}},
        )
        return {
            "provider": {"name": "vcpkg", "version": "status-hash", "evidence": []},
            "allowed_roots": [str(self.prefix)],
            "library_dirs": [str(self.library_dir)],
            "pkg_config": {"executable": str(self.resolver), "pc_dirs": [str(self.pcdir)]},
        }, self.versions

    def query(self, argv, **kwargs):
        self.assertEqual(argv[0], str(self.resolver))
        options = [value for value in argv[1:] if value.startswith("--") and value != "--static"]
        self.assertEqual(len(options), 1, argv)
        option, module = options[0], argv[-1]
        self.assertNotIn("--static", argv)
        versions = {
            "fdk-aac": self.versions["fdk_aac"],
            "libmpg123": self.versions["mpg123"],
            "opus": self.versions["opus"],
        }
        self.assertIn(module, versions)
        if option == "--modversion":
            text = versions[module]
        elif option == "--variable=pcfiledir":
            text = str(self.pcdir)
        elif option in ("--print-requires", "--print-requires-private"):
            text = ""
        elif option == "--cflags":
            # The reviewed opus port installs headers under an opus/ child directory.
            text = f'-I"{(self.include / "opus").as_posix()}"' if module == "opus" else f'-I"{self.include.as_posix()}"'
        elif option == "--libs":
            text = f'-L"{self.library_dir.as_posix()}" ' + " ".join(
                "-l" + name for name in self.metadata_libraries[module]
            )
        else:
            self.fail(f"unexpected pkg-config query: {argv}")
        return subprocess.CompletedProcess(argv, 0, text, "")

    def refresh(self):
        with (
            patch.object(native, "native_platform", return_value="windows"),
            patch.object(providers, "POLICY", self.policy_path),
            patch.object(providers, "vcpkg_prefix", return_value=self.prefix),
            patch.object(providers, "native_provider", side_effect=self.provider),
            patch.object(bazel_deps.subprocess, "run", side_effect=self.query),
        ):
            resolution = dependencies.resolve_dependencies(self.tree, "release", group="native")
        return resolution["manifest"], observed_sources(resolution)

    def test_windows_shared_codecs_pair_dlls_without_linking_static_private_sdk_tails(self):
        manifest, sources = self.refresh()
        packages = {package["name"]: package for package in manifest["packages"]}
        mpg123 = packages["mpg123"]
        self.assertEqual(mpg123["interface"]["linkopts"], [])
        self.assertEqual(
            mpg123["resolved_libraries"],
            [
                {
                    "name": "mpg123",
                    "kind": "shared",
                    "file": str(self.library_dir / "mpg123.lib"),
                    "runtime": str(self.prefix / "bin/mpg123.dll"),
                }
            ],
        )
        fdk_aac = packages["fdk_aac"]
        self.assertEqual(fdk_aac["interface"]["linkopts"], [])
        self.assertEqual(
            fdk_aac["resolved_libraries"],
            [
                {
                    "name": "fdk-aac",
                    "kind": "shared",
                    "file": str(self.library_dir / "fdk-aac.lib"),
                    "runtime": str(self.prefix / "bin/fdk-aac.dll"),
                }
            ],
        )
        opus = packages["opus"]
        self.assertEqual(
            opus["resolved_libraries"],
            [
                {
                    "name": "opus",
                    "kind": "shared",
                    "file": str(self.library_dir / "opus.lib"),
                    "runtime": str(self.prefix / "bin/opus.dll"),
                }
            ],
        )
        self.assertEqual(opus["interface"]["header_dirs"], [str(self.include / "opus"), str(self.include)])
        for source in (
            str(self.library_dir / "mpg123.lib"),
            str(self.library_dir / "fdk-aac.lib"),
            str(self.library_dir / "opus.lib"),
            str(self.include / "mpg123.h"),
            str(self.include / "fdk-aac.h"),
            str(self.include / "opus.h"),
            str(self.include / "opus/opus.h"),
        ):
            self.assertIn(source, sources)
        self.assertIn(str(self.prefix / "bin/mpg123.dll"), sources)


class BazelNativeFakeItHeaderTest(unittest.TestCase):
    """The reviewed native FakeIt child-include spellings, witnessed through real refreshes."""

    def arrange(self, host, *, fakeit_child=True):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-native-fakeit-")
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name).resolve()
        triplet = "x64-windows" if host == "windows" else "arm64-osx"
        prefix = root / triplet
        include = prefix / "include"
        macros = include / "catch2/catch_version_macros.hpp"
        macros.parent.mkdir(parents=True)
        macros.write_text(
            "#define CATCH_VERSION_MAJOR 3\n#define CATCH_VERSION_MINOR 7\n#define CATCH_VERSION_PATCH 0\n",
            encoding="utf-8",
        )
        child = include / "fakeit/single_header/catch/fakeit.hpp"
        child.parent.mkdir(parents=True)
        if fakeit_child:
            child.write_text("fixture header\n", encoding="utf-8")
        library_dir = prefix / "lib"
        library_dir.mkdir(parents=True)
        archive = library_dir / ("Catch2.lib" if host == "windows" else "libCatch2.a")
        archive.write_bytes(static_archive() if host == "windows" else b"fixture static Catch2 archive")
        if host == "windows":
            main = library_dir / "manual-link/Catch2Main.lib"
            main.parent.mkdir()
            main.write_bytes(static_archive())
        pcdir = library_dir / "pkgconfig"
        pcdir.mkdir()
        # Catch2 keeps its metadata; FakeIt has none and must use the reviewed fallback.
        (pcdir / "catch2-with-main.pc").write_text("Name: Catch2\nVersion: 3.7.0\n", encoding="utf-8")
        if host == "windows":
            (prefix / "bin").mkdir()  # Probed for staged Catch2 DLLs; none may exist.
        info = root / "vcpkg/info"
        info.mkdir(parents=True)
        (info / f"catch2_3.7.0_{triplet}.list").write_text(
            f"{triplet}/include/catch2/catch_version_macros.hpp\n"
            + (f"{triplet}/lib/Catch2.lib\n{triplet}/lib/manual-link/Catch2Main.lib\n" if host == "windows" else ""),
            encoding="utf-8",
        )
        (info / f"fakeit_2.5.0_{triplet}.list").write_text(
            f"{triplet}/include/fakeit/single_header/catch/fakeit.hpp\n", encoding="utf-8"
        )
        resolver = root / "resolver"
        resolver.touch()
        resolver.chmod(0o755)
        versions = {"catch2": "3.7.0", "fakeit": "2.5.0"}
        policy = native.read_json(providers.POLICY)
        policy["packages"] = {"native": [], "test": policy["packages"]["test"]}
        policy_path = root / "policy.json"
        policy_path.write_text(json.dumps(policy), encoding="utf-8")
        tree = root / "workspace"
        tree.mkdir()

        def provider(selected_prefix, config, selected_host, *, identity_path, policy):
            native.write_json(
                identity_path,
                {"packages": {name: {"version": version + "#0"} for name, version in versions.items()}},
            )
            return {
                "provider": {"name": "vcpkg", "version": "status-hash", "evidence": []},
                "allowed_roots": [str(prefix)],
                "library_dirs": [str(library_dir)],
                "pkg_config": {"executable": str(resolver), "pc_dirs": [str(pcdir)]},
            }, versions

        def query(argv, **kwargs):
            self.assertEqual(argv[0], str(resolver))
            options = [value for value in argv[1:] if value.startswith("--") and value != "--static"]
            self.assertEqual(len(options), 1, argv)
            option, module = options[0], argv[-1]
            self.assertEqual(module, "catch2-with-main")
            # Static interfaces carry --static exactly on flag queries; the adapter
            # probes versions and metadata locations without it.
            self.assertEqual("--static" in argv, option in ("--cflags", "--libs"))
            if option == "--modversion":
                text = "3.7.0"
            elif option == "--variable=pcfiledir":
                text = str(pcdir)
            elif option in ("--print-requires", "--print-requires-private"):
                text = ""
            elif option == "--cflags":
                text = f'-I"{include.as_posix()}"'
            elif option == "--libs":
                text = (
                    f'-L"{library_dir.as_posix()}/manual-link" -lCatch2Main ' if host == "windows" else ""
                ) + f'-L"{library_dir.as_posix()}" -lCatch2'
            else:
                self.fail(f"unexpected pkg-config query: {argv}")
            return subprocess.CompletedProcess(argv, 0, text, "")

        def refresh():
            with (
                patch.object(native, "native_platform", return_value=host),
                patch.object(providers, "POLICY", policy_path),
                patch.object(providers, "vcpkg_prefix", return_value=prefix),
                patch.object(providers, "native_provider", side_effect=provider),
                patch.object(bazel_deps.subprocess, "run", side_effect=query),
            ):
                resolution = dependencies.resolve_dependencies(tree, "release", group="test")
            return resolution["manifest"], observed_sources(resolution)

        return prefix, refresh

    def test_native_fakeit_publishes_only_the_reviewed_child_header_interface(self):
        for host in ("windows", "macos"):
            with self.subTest(host=host):
                prefix, refresh = self.arrange(host)
                manifest, sources = refresh()
                self.assertEqual(manifest["configuration"]["platform"], host)
                self.assertEqual(manifest["configuration"]["linkage"], "static")
                packages = {package["name"]: package for package in manifest["packages"]}
                catch2 = packages["catch2"]
                self.assertEqual(catch2["origin"], "automatic")
                archive = prefix / "lib" / ("Catch2.lib" if host == "windows" else "libCatch2.a")
                self.assertEqual(
                    catch2["resolved_libraries"],
                    (
                        [
                            {
                                "name": "Catch2Main",
                                "kind": "static",
                                "file": str(prefix / "lib/manual-link/Catch2Main.lib"),
                            }
                        ]
                        if host == "windows"
                        else []
                    )
                    + [{"name": "Catch2", "kind": "static", "file": str(archive)}],
                )
                fakeit = packages["fakeit"]
                self.assertEqual(fakeit["origin"], "manual_fallback")
                child = prefix / "include/fakeit/single_header/catch"
                self.assertEqual(fakeit["interface"]["header_dirs"], [str(child)])
                self.assertEqual(fakeit["declared_headers"], 1)
                self.assertEqual(fakeit["resolved_libraries"], [])
                self.assertIn(str(child / "fakeit.hpp"), sources)
                self.assertIn(str(prefix / "include/catch2/catch_version_macros.hpp"), sources)
                self.assertIn(str(archive), sources)


class BazelMacAudioMetadataTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-mac-codec-metadata-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.policy = native.read_json(providers.POLICY)
        self.cases = {
            "flac": ("flac", "1.5.0", ["FLAC", "m", "ogg"], "m"),
            "fdk_aac": ("fdk-aac", "2.0.3", ["fdk-aac", "c++"], "c++"),
            "mpg123": ("libmpg123", "1.33.7", ["mpg123", "m"], "m"),
            "opus": ("opus", "1.6.1", ["opus", "m"], "m"),
        }

    def spec(self, config="Debug"):
        return {
            "platform": "macos",
            "config": config,
            "linkage": "static",
            "provider": {"name": "vcpkg", "version": "selected-prefix:status-sha256"},
            "packages": [{"name": name, "modules": [data[0]]} for name, data in self.cases.items()],
        }

    def guarded_patch(self, name, *, flags=None, spec=None):
        _, _, libraries, _ = self.cases[name]
        spec = self.spec() if spec is None else spec
        flags = ["-L/selected/config/lib", *["-l" + library for library in libraries]] if flags is None else flags
        with patch.object(dependencies, "_metadata_flags", return_value=flags) as query:
            result = dependencies._macos_audio_runtime_patch(
                spec, name, self.policy["macos_audio_runtime_remove"][name]
            )
            query.assert_called_once_with(spec, "--libs", package=name)
            return result

    def test_macos_codec_patches_keep_archive_order_and_remove_only_implicit_runtime_libraries(self):
        for name, (_, _version, libraries, implicit) in self.cases.items():
            with self.subTest(name=name):
                result = self.guarded_patch(name)
                present = result["expect"]["present"]["libraries"]
                self.assertEqual([item["name"] for item in present], libraries)
                self.assertEqual(
                    [item["kind"] for item in present],
                    ["system" if library == implicit else "static" for library in libraries],
                )
                self.assertEqual(result["remove"], {"libraries": [{"name": implicit, "kind": "system"}]})

    def test_codec_patch_guards_bind_the_selected_platform_config_and_provider(self):
        result = self.guarded_patch("flac", spec=self.spec("Debug"))
        self.assertEqual(
            result["when"],
            {
                "package": "flac",
                "platform": "macos",
                "config": "Debug",
                "provider": "vcpkg",
                "provider_version": "selected-prefix:status-sha256",
            },
        )

    def test_adapter_preserves_static_order_and_automatic_counts_without_implicit_system_libs(self):
        provider = self.root / "provider with spaces"
        include = provider / "include"
        include.mkdir(parents=True)
        (include / "fixture.h").write_text("fixture header\n", encoding="utf-8")
        pkgconf = provider / "pkgconf"
        pkgconf.write_bytes(b"mocked resolver, not executed")
        pkgconf.chmod(0o755)
        config = "Debug"
        libdir = provider / config.lower() / "lib"
        pcdir = libdir / "pkgconfig"
        pcdir.mkdir(parents=True)
        versions = {data[0]: data[1] for data in self.cases.values()} | {"ogg": "1.3.6"}
        for module, version in versions.items():
            (pcdir / (module + ".pc")).write_text(f"Name: {module}\nVersion: {version}\n", encoding="utf-8")
        for library in ("FLAC", "ogg", "fdk-aac", "mpg123", "opus"):
            (libdir / ("lib" + library + ".a")).write_bytes(b"fixture static archive")
        spec = self.spec(config)
        spec.update(
            schema=1,
            allowed_roots=[str(provider)],
            library_dirs=[str(libdir)],
            system_libraries=["m", "c++"],
            pkg_config={"executable": str(pkgconf), "pc_dirs": [str(pcdir)]},
        )
        for package in spec["packages"]:
            package["expected_versions"] = {package["modules"][0]: versions[package["modules"][0]]}
        patches = [self.guarded_patch(name, spec=spec) for name in self.cases]
        tree = self.root / config.lower()
        tree.mkdir()
        out = tree / "out"
        module_cases = {data[0]: data for data in self.cases.values()}

        def query(argv, **kwargs):
            args = [value for value in argv[1:] if value != "--static"]
            option, module = args[:2]
            if option == "--modversion":
                text = versions[module]
            elif option == "--variable=pcfiledir":
                text = str(pcdir)
            elif option == "--cflags":
                text = f'-I"{include.as_posix()}"'
            elif option == "--libs":
                text = f'-L"{libdir.as_posix()}" ' + " ".join("-l" + value for value in module_cases[module][2])
            elif option == "--print-requires":
                text = "ogg" if module == "flac" else ""
            else:
                self.assertEqual(option, "--print-requires-private")
                text = ""
            return subprocess.CompletedProcess(argv, 0, text, "")

        with patch.object(bazel_deps.subprocess, "run", side_effect=query):
            with self.assertRaisesRegex(
                bazel_deps.BazelDependencyError, "static provider/system library link order unsupported"
            ):
                adapter = bazel_deps._Adapter(spec, {"schema": 1, "patches": []})
                adapter.prepare()
                adapter.resolve()
            adapter = bazel_deps._Adapter(spec, {"schema": 1, "patches": patches})
            adapter.prepare()
            resolution = adapter.resolve()
            manifest = resolution["manifest"]
        self.assertEqual(
            next(p for p in resolution["packages"] if p["name"] == "flac")["libraries"][0]["source"],
            str(libdir / "libFLAC.a"),
        )
        self.assertFalse(out.exists())
        self.assertEqual(manifest["counts"], {"automatic": 4, "manual_fallback": 0})
        for package in manifest["packages"]:
            libraries = self.cases[package["name"]][2]
            implicit = self.cases[package["name"]][3]
            self.assertEqual(
                [value["name"] for value in package["resolved_libraries"]],
                [value for value in libraries if value != implicit],
            )
            self.assertEqual(package["interface"]["linkopts"], [])


if __name__ == "__main__":
    unittest.main()
