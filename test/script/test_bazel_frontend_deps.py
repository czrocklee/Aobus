"""Actual frontend and catalog consumers of the governed native projection."""

import copy
import json
import os
import subprocess
import sys
import tempfile
import unittest
from contextlib import contextmanager
from pathlib import Path
from unittest.mock import patch

from test_bazel_windows_linkage import pe_image, static_archive

from ao.core import bazel_dependencies as dependencies
from ao.core import bazel_deps, dependency_policy
from ao.core import bazel_host_tools as host_tools
from ao.core import bazel_native as native
from ao.core import bazel_providers as providers


class FrontendDependencyPolicyTest(unittest.TestCase):
    def test_frontend_imports_follow_the_actual_platform_consumers(self):
        policy = native.read_json(providers.POLICY)
        # Only genuinely host-specific consumers are filtered; the module and
        # input inventories are witnessed by the actual refresh projections.
        for host, expected in (("linux", {"giomm"}), ("macos", {"date_tz"}), ("windows", set())):
            with self.subTest(host=host):
                packages = {p["name"] for p in providers.package_groups(policy, host=host)["native"]}
                self.assertEqual(packages & {"giomm", "date_tz"}, expected)

    def test_mimalloc_metadata_translation_uses_the_closed_producer_rule(self):
        # A future dotted release translates through one closed, host-independent
        # rule; no per-host pin remains. The malformed-format and wrong-module
        # rejections below are equivalence classes of that same rule.
        self.assertEqual(
            dependencies._metadata_versions("mimalloc", "4.1.2", ["mimalloc"], "linux"), {"mimalloc": "4.1"}
        )
        for version, expected in (("3.5.3", "3.5"), ("4.10.2", "4.10"), ("100.2.0", "100.2")):
            with self.subTest(version=version):
                self.assertEqual(
                    dependencies._metadata_versions("mimalloc", version, ["mimalloc"], "linux"),
                    {"mimalloc": expected},
                )
        for version in ("3.5", "3.5.3.1", "3.5.3-unstable", "v3.5.3"):
            with (
                self.subTest(version=version),
                self.assertRaisesRegex(native.BazelWorkspaceError, "metadata version"),
            ):
                dependencies._metadata_versions("mimalloc", version, ["mimalloc"], "linux")
        with self.assertRaisesRegex(native.BazelWorkspaceError, "metadata version"):
            dependencies._metadata_versions("mimalloc", "3.5.3", ["other"], "linux")

    def test_unicode_only_macos_filter_queries_its_own_metadata(self):
        policy = native.read_json(providers.POLICY)
        spec = {"platform": "macos", "config": "Release", "provider": {"name": "vcpkg", "version": "selected-status"}}
        with patch.object(dependencies, "_metadata_flags", return_value=["-licuuc", "-licudata", "-lc++"]) as flags:
            result = dependencies._macos_icu_runtime_patch(spec, policy["macos_icu_runtime_remove"], package="icu_uc")
        flags.assert_called_once_with(spec, "--libs", package="icu_uc")
        self.assertEqual(result["when"]["package"], "icu_uc")
        self.assertEqual(result["remove"], {"libraries": [{"name": "c++", "kind": "system"}]})


class FrontendProjectionTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-frontend-deps-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.provider = self.root / "provider"
        self.include = self.provider / "include"
        self.pc = self.provider / "lib/pkgconfig"
        self.pc.mkdir(parents=True)
        self.resolver = self.provider / "bin/resolver"
        self.resolver.parent.mkdir()
        self.resolver.write_bytes(b"fixture resolver; never executed")
        self.resolver.chmod(0o755)
        headers = {
            "unicode/uvernum.h": "#define U_ICU_VERSION_MAJOR_NUM 78\n#define U_ICU_VERSION_MINOR_NUM 3\n",
            "CLI/Version.hpp": (
                "#define CLI11_VERSION_MAJOR 2\n#define CLI11_VERSION_MINOR 6\n#define CLI11_VERSION_PATCH 2\n"
            ),
            "spdlog/spdlog.h": "// compiled interface\n",
            "spdlog/version.h": "#define SPDLOG_VER_MAJOR 1\n#define SPDLOG_VER_MINOR 17\n#define SPDLOG_VER_PATCH 0\n",
            "ftxui/component/component.hpp": "// component\n",
            "mimalloc.h": "#define MI_MALLOC_VERSION 30503\n",
            "stb/stb_image_resize2.h": (
                "/* stb_image_resize2 - fixture release */\n#define stbir_resize_uint8_srgb 1\n#define STBIR_RGBA 1\n"
            ),
            "giomm-2.68/giomm.h": "// giomm\n",
            "gio-unix-2.0/gunixfdlist.h": "// gio unix\n",
        }
        for name, text in headers.items():
            path = self.include / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8")
        policy = native.read_json(providers.POLICY)
        names = {"icu", "icu_uc", "cli11", "spdlog", "ftxui", "mimalloc", "stb", "giomm"}
        policy["packages"] = {"native": [p for p in policy["packages"]["native"] if p["name"] in names], "test": []}
        self.policy = self.root / "policy.json"
        native.write_json(self.policy, policy)
        self.versions = {
            "icu": "78.3",
            "icu_uc": "78.3",
            "cli11": "2.6.2",
            "spdlog": "1.17.0",
            "ftxui": "7.0.3",
            "mimalloc": "3.5.3",
            "stb": "nix-stb-fixture",
            "giomm": "2.88.1",
        }
        self.module_versions = {
            "icu-uc": "78.3",
            "icu-i18n": "78.3",
            "CLI11": "2.6.2",
            "spdlog": "1.17.0",
            "ftxui": "7.0.3",
            "mimalloc": "3.5",
            "giomm-2.68": "2.88.1",
            "glibmm-2.68": "2.88.1",
            "gio-unix-2.0": "2.88.3",
        }
        self.libraries = {
            "icu-uc": ["icuuc", "icudata"],
            "icu-i18n": ["icui18n"],
            "spdlog": ["spdlog"],
            "ftxui": ["ftxui-component", "ftxui-dom", "ftxui-screen"],
            "mimalloc": ["mimalloc"],
            "giomm-2.68": ["giomm-2.68", "glibmm-2.68", "sigc-3.0", "gio-2.0", "gobject-2.0", "glib-2.0"],
        }
        self.defines = ["SPDLOG_SHARED_LIB", "SPDLOG_COMPILED_LIB", "SPDLOG_USE_STD_FORMAT"]
        for module in self.module_versions:
            (self.pc / (module + ".pc")).write_text(
                f"Name: {module}\nVersion: {self.module_versions[module]}\n", encoding="utf-8"
            )
        for module, libraries in self.libraries.items():
            for library in libraries:
                (self.provider / "lib" / ("lib" + library + (".a" if module == "ftxui" else ".so"))).write_bytes(
                    b"fixture library"
                )

    def provider_spec(self, policy, *, identity_path):
        names = [p["name"] for group in policy["packages"].values() for p in group]
        native.write_json(
            identity_path,
            {
                "inputs": {"glib": {"version": "2.88.3"}},
                "packages": {name: {"outputs": [str(self.provider)]} for name in names},
            },
        )
        return (
            {
                "provider": {"name": "nix", "version": "fixture-revision", "evidence": []},
                "allowed_roots": [str(self.provider)],
                "library_dirs": [str(self.provider / "lib")],
                "pkg_config": {"executable": str(self.resolver), "pc_dirs": [str(self.pc)]},
                "elf": {"readelf": str(self.resolver)},
            },
            {name: self.versions[name] for name in names},
            {name: [self.provider] for name in names},
        )

    def query(self, argv, **kwargs):
        self.assertEqual(argv[0], str(self.resolver))
        if "-d" in argv:
            self.assertEqual(argv[1:-1], ["-d"])
            return subprocess.CompletedProcess(argv, 0, "(SONAME) [" + Path(argv[-1]).name + "]", "")
        options = [arg for arg in argv[1:] if arg.startswith("--") and arg != "--static"]
        self.assertEqual(len(options), 1, argv)
        self.assertIn(
            options[0],
            {
                "--modversion",
                "--variable=pcfiledir",
                "--cflags",
                "--libs",
                "--print-requires",
                "--print-requires-private",
            },
        )
        modules = [arg for arg in argv[1:] if not arg.startswith("--")]
        self.assertTrue(modules, argv)
        self.assertTrue(set(modules) <= self.module_versions.keys(), argv)
        if "--modversion" in argv:
            text = self.module_versions[argv[-1]]
        elif "--variable=pcfiledir" in argv:
            text = str(self.pc)
        elif "--cflags" in argv:
            roots = [self.include]
            if "giomm-2.68" in modules:
                roots += [self.include / "giomm-2.68", self.include / "gio-unix-2.0"]
            text = " ".join('-I"' + str(root) + '"' for root in roots)
            if "spdlog" in modules:
                text += " " + " ".join("-D" + define for define in self.defines)
        elif "--libs" in argv:
            libraries = list(dict.fromkeys(library for module in modules for library in self.libraries.get(module, [])))
            text = '-L"' + str(self.provider / "lib") + '" ' + " ".join("-l" + library for library in libraries)
            if "spdlog" in modules:
                text += " -pthread"
        else:
            text = ""
        return subprocess.CompletedProcess(argv, 0, text, "")

    def refresh(self, group="native", config="debug"):
        with (
            patch.object(providers, "POLICY", self.policy),
            patch.object(native, "native_platform", return_value="linux"),
            patch.object(providers, "linux_provider", side_effect=self.provider_spec) as provider,
            patch.object(providers, "verify_linux_resolution"),
            patch.object(bazel_deps.subprocess, "run", side_effect=self.query),
        ):
            resolution = dependencies.resolve_dependencies(self.root / (group + config), config, group=group)
        return resolution, provider.call_args.args[0]

    def test_linux_refresh_projects_each_frontend_interface_from_provider_metadata(self):
        # Dummy archive bytes stand in for real payloads: this is projection
        # coverage over the provider report, not compiler or linker acceptance.
        repository, _ = self.refresh()
        manifest = repository["manifest"]
        packages = {p["name"]: p for p in manifest["packages"]}
        # The unicode-only import keeps a narrower archive closure than full ICU.
        self.assertEqual([p["name"] for p in packages["icu_uc"]["resolved_libraries"]], ["icuuc", "icudata"])
        self.assertIn("icui18n", [p["name"] for p in packages["icu"]["resolved_libraries"]])
        # FTXUI stays a complete three-archive static interface on Linux metadata.
        self.assertEqual(packages["ftxui"]["linkage"], "static")
        self.assertEqual(len(packages["ftxui"]["resolved_libraries"]), 3)
        # Provider metadata defines and link flags project into the interface.
        self.assertEqual(packages["spdlog"]["interface"]["defines"], self.defines)
        self.assertEqual(packages["spdlog"]["interface"]["linkopts"], ["-pthread"])
        admitted = {p["name"]: p for p in repository["packages"]}
        self.assertEqual(admitted["spdlog"]["defines"], self.defines)
        self.assertEqual(admitted["spdlog"]["linkopts"], ["-pthread"])
        self.assertEqual(
            [Path(library["source"]).name for library in admitted["icu_uc"]["libraries"]],
            ["libicuuc.so", "libicudata.so"],
        )
        # Header-only consumers keep their selected include directories.
        self.assertEqual(packages["stb"]["interface"]["header_dirs"], [str(self.include / "stb")])
        self.assertIn(str(self.include / "gio-unix-2.0"), packages["giomm"]["interface"]["header_dirs"])
        # The mimalloc metadata translation is applied end to end.
        self.assertEqual(manifest["modules"]["mimalloc"]["version"], "3.5")

    def test_catalog_refresh_selects_only_icu_and_passes_its_compilation_mode(self):
        # Native Starlark owns mode selection; this witnesses admission forwarding.
        repository, _ = self.refresh("catalog", "debug")
        manifest = repository["manifest"]
        self.assertEqual([p["name"] for p in manifest["packages"]], ["icu"])
        self.assertEqual(manifest["configuration"]["config"], "Debug")
        self.assertEqual([p["name"] for p in repository["packages"]], ["icu"])


class StbInterfaceTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-stb-interface-")
        self.addCleanup(temporary.cleanup)
        self.provider = Path(temporary.name).resolve()
        (self.provider / "include/stb").mkdir(parents=True)

    def test_stb_interface_owns_versions_through_provider_recipes_not_python_pins(self):
        # Linux: an unknown future release passes structurally; the pinned Nix
        # recipe and report own the actual source, so no banner literal or
        # suffix is re-pinned here. The refresh routing keeps its own
        # projection witness above; this exercises the interface contract.
        header = self.provider / "include/stb/stb_image_resize2.h"
        header.write_text(
            "/* stb_image_resize2 - v2.19 - future release */\n"
            "#define stbir_resize_uint8_srgb 1\n"
            "#define STBIR_RGBA 1\n",
            encoding="utf-8",
        )
        self.assertEqual(
            dependencies._stb_interface([self.provider], "linux", "2.19"),
            {"header_dirs": [str(self.provider / "include/stb")]},
        )
        # A banner without the actually consumed resize API is not a valid interface.
        header.write_text("/* stb_image_resize2 - v2.19 - future release */\n", encoding="utf-8")
        with self.assertRaisesRegex(native.BazelWorkspaceError, "stb resize2 interface is missing"):
            dependencies._stb_interface([self.provider], "linux", "2.19")
        # Native hosts still compare the observed version against the overlay owner.
        native_header = self.provider / "include/stb_image_resize2.h"
        native_header.write_text(
            "/* stb_image_resize2 - fixture overlay */\n#define stbir_resize_uint8_srgb 1\n#define STBIR_RGBA 1\n",
            encoding="utf-8",
        )
        with self.assertRaisesRegex(native.BazelWorkspaceError, "does not match the governed overlay"):
            dependencies._stb_interface([self.provider], "windows", "9.9.9")
        overlay = native.read_json(native.PROJECT_ROOT / "cmake/vcpkg-ports/stb/vcpkg.json")
        self.assertEqual(
            dependencies._stb_interface([self.provider], "windows", overlay["version-string"]),
            {"header_dirs": [str(self.provider / "include")]},
        )


class NativeFrontendArtifactTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-native-frontend-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.prefix = self.root / "provider"
        self.policy = native.read_json(providers.POLICY)
        self.packages = {p["name"]: p for p in self.policy["packages"]["native"]}
        for marker in (
            "CLI/Version.hpp",
            "spdlog/spdlog.h",
            "spdlog/common.h",
            "spdlog/tweakme.h",
            "ftxui/component/component.hpp",
            "mimalloc.h",
            "date/tz.h",
        ):
            path = self.prefix / "include" / marker
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("// selected SDK fixture\n")
        (self.prefix / "include/mimalloc.h").write_text(
            "#if 0 // !defined(MI_SHARED_LIB)\n#define mi_decl_export\n#elif defined(MI_SHARED_LIB_EXPORT)\n#endif\n"
        )
        (self.prefix / "include/spdlog/tweakme.h").write_text("#define SPDLOG_USE_STD_FORMAT\n")
        (self.prefix / "include/spdlog/common.h").write_text("#define SPDLOG_API __declspec(dllimport)\n")
        for selected, suffix, mimalloc in (
            (self.prefix, "", "mimalloc"),
            (self.prefix / "debug", "d", "mimalloc-debug"),
        ):
            (selected / "lib").mkdir(parents=True)
            (selected / "bin").mkdir()
            for name in ("CLI11", "ftxui-component", "ftxui-dom", "ftxui-screen"):
                (selected / "lib" / (name + ".lib")).write_bytes(b"static fixture")
                (selected / "lib" / ("lib" + name + ".a")).write_bytes(b"static fixture")
            for name in ("spdlog" + suffix, mimalloc, "date-tz"):
                (selected / "lib" / ("lib" + name + ".a")).write_bytes(b"static fixture")
            (selected / "lib" / ("spdlog" + suffix + ".lib")).write_bytes(b"static fixture")
            (selected / "lib" / (mimalloc + ".dll.lib")).write_bytes(b"import fixture")

    def interface(self, name, host, config):
        return dependencies._library_interface(self.packages[name], [self.prefix], config, host, self.policy)

    def test_native_artifact_names_and_reviewed_linkage_follow_each_configuration(self):
        # Dummy archive bytes stand in for real payloads: this owns the
        # config-sensitive static spelling and selected-root resolution, while
        # compiled ABI/producer compatibility stays a native validation boundary.
        for host in ("windows", "macos"):
            for config in ("debug", "release"):
                selected = self.prefix / "debug" if config == "debug" else self.prefix
                with self.subTest(host=host, config=config):
                    # CLI11 and the FTXUI closure keep one spelling on every configuration.
                    cli = self.interface("cli11", host, config)
                    self.assertEqual([item["name"] for item in cli["libraries"]], ["CLI11"])
                    self.assertTrue(all(item["kind"] == "static" for item in cli["libraries"]))
                    self.assertEqual(
                        cli["libraries"][0]["file"],
                        str(selected / "lib" / ("CLI11.lib" if host == "windows" else "libCLI11.a")),
                    )
                    self.assertEqual(cli.get("linkopts", []), ["Shell32.lib"] if host == "windows" else [])
                    ftxui = self.interface("ftxui", host, config)
                    self.assertEqual(
                        [item["name"] for item in ftxui["libraries"]],
                        ["ftxui-component", "ftxui-dom", "ftxui-screen"],
                    )
                    self.assertTrue(all(item["kind"] == "static" for item in ftxui["libraries"]))
                    # spdlog and mimalloc follow bounded debug spellings and selected roots.
                    spdlog = self.interface("spdlog", host, config)
                    spelling = "spdlogd" if config == "debug" else "spdlog"
                    self.assertEqual(spdlog["libraries"][0]["name"], spelling)
                    self.assertEqual(
                        spdlog["libraries"][0]["file"],
                        str(selected / "lib" / (spelling + ".lib" if host == "windows" else "lib" + spelling + ".a")),
                    )
                    mimalloc = self.interface("mimalloc", host, config)
                    spelling = "mimalloc-debug" if config == "debug" else "mimalloc"
                    library = mimalloc["libraries"][0]
                    self.assertEqual(library["name"], spelling)
                    self.assertEqual(
                        library["file"],
                        str(
                            selected / "lib" / (spelling + ".dll.lib" if host == "windows" else "lib" + spelling + ".a")
                        ),
                    )
                    self.assertEqual(library["kind"], "shared" if host == "windows" else "static")
                    self.assertEqual(mimalloc.get("linkopts", []), [])

    def test_date_tz_artifact_consumer_keeps_its_reviewed_abi_definitions(self):
        # This checks the required macOS artifact consumer ABI, not the
        # metadata-absent refresh route or a successful native link.
        for config in ("debug", "release"):
            interface = self.interface("date_tz", "macos", config)
            with self.subTest(config=config):
                self.assertEqual(interface["defines"], ["ONLY_C_LOCALE=0", "HAS_STRING_VIEW=1", "USE_OS_TZDB=1"])
                self.assertEqual(interface["linkopts"], ["-pthread"])
                self.assertEqual(interface["libraries"][0]["name"], "date-tz")
        with self.assertRaisesRegex(native.BazelWorkspaceError, "no reviewed"):
            self.interface("date_tz", "windows", "debug")

    def test_missing_shared_import_does_not_fall_back_to_an_old_static_archive(self):
        (self.prefix / "lib/mimalloc.dll.lib").unlink()
        (self.prefix / "lib/mimalloc.lib").write_bytes(b"old static fixture")
        with self.assertRaisesRegex(native.BazelWorkspaceError, "missing/ambiguous"):
            self.interface("mimalloc", "windows", "release")
        # Static-only CLI11 still rejects a DLL-bearing producer shape.
        (self.prefix / "bin/CLI11.dll").write_bytes(b"unreviewed shared CLI11")
        with self.assertRaisesRegex(native.BazelWorkspaceError, "unsupported static provider shape"):
            self.interface("cli11", "windows", "release")


class WindowsCli11RefreshTest(unittest.TestCase):
    """The real Windows refresh/adapter/export path for the compiled CLI11 consumer.

    Provider discovery is stubbed; installed header macros, ownership manifests
    and pkg-config metadata remain independently mutable. Dummy archive bytes
    stand in for real payloads; no compiler or linker runs here.
    """

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-cli11-refresh-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.prefix = self.root / "installed/x64-windows"
        policy = native.read_json(providers.POLICY)
        cli11 = next(p for p in policy["packages"]["native"] if p["name"] == "cli11")
        policy["packages"] = {"native": [cli11], "test": []}
        self.policy_path = self.root / "cli11-policy.json"
        native.write_json(self.policy_path, policy)
        self.include = self.prefix / "include"
        self.header = self.include / "CLI/Version.hpp"
        self.header.parent.mkdir(parents=True)
        (self.prefix / "lib").mkdir(parents=True)
        (self.prefix / "lib/CLI11.lib").write_bytes(static_archive())
        self.pc_dir = self.prefix / "lib/pkgconfig"
        self.pc_dir.mkdir()
        self.resolver = self.root / "resolver/pkgconf.exe"
        self.resolver.parent.mkdir()
        self.resolver.write_bytes(b"fixture resolver; never executed")
        self.resolver.chmod(0o755)
        self.info = self.prefix.parent / "vcpkg/info"
        self.info.mkdir(parents=True)
        self.observations = {"upstream": "2.6.2", "raw_status": "2.6.2#1", "metadata": "2.6.2"}

    def install(self, version):
        """Publish one coherent selected release across every observation."""
        self.write_header(version)
        self.observations.update(upstream=version, raw_status=version + "#1", metadata=version)
        for path in list(self.info.iterdir()):
            path.unlink()
        (self.info / f"cli11_{version}_{self.prefix.name}.list").write_text(
            f"{self.prefix.name}/include/CLI/Version.hpp\n{self.prefix.name}/lib/CLI11.lib\n", encoding="utf-8"
        )

    def write_header(self, version):
        major, minor, micro = (int(value) for value in version.split("."))
        self.header.write_text(
            f"#define CLI11_VERSION_MAJOR {major}\n"
            f"#define CLI11_VERSION_MINOR {minor}\n"
            f"#define CLI11_VERSION_PATCH {micro}\n",
            encoding="utf-8",
        )

    def provider(self, prefix, config, host, *, identity_path, policy):
        native.write_json(identity_path, {"packages": {"cli11": {"version": self.observations["raw_status"]}}})
        return (
            {
                "provider": {"name": "vcpkg", "version": "fixture", "evidence": []},
                "allowed_roots": [str(self.prefix)],
                "library_dirs": [str(self.prefix / "lib")],
                "pkg_config": {"executable": str(self.resolver), "pc_dirs": [str(self.pc_dir)]},
            },
            {"cli11": self.observations["upstream"]},
        )

    def query(self, argv, **kwargs):
        # The external boundary is part of the contract: the known resolver is
        # queried for the CLI11 module with the reviewed static arguments, so a
        # wrong executable, option or module fails instead of being answered.
        if argv[0] != str(self.resolver):
            raise AssertionError(f"unexpected resolver executable: {argv}")
        environment = kwargs.get("env") or {}
        if environment.get("PKG_CONFIG_LIBDIR") != str(self.pc_dir) or environment.get("PKG_CONFIG_PATH") != "":
            raise AssertionError(f"unisolated pkg-config environment: {environment}")
        if argv == [str(self.resolver), "--variable=pcfiledir", "CLI11"]:
            answer = str(self.pc_dir)
        elif argv == [str(self.resolver), "--modversion", "CLI11"]:
            answer = self.observations["metadata"]
        elif argv in (
            [str(self.resolver), "--print-requires", "CLI11"],
            [str(self.resolver), "--print-requires-private", "CLI11"],
        ):
            answer = ""
        elif argv == [str(self.resolver), "--static", "--cflags", "CLI11"]:
            answer = f'-I"{self.include}" -DCLI11_COMPILE'
        elif argv == [str(self.resolver), "--static", "--libs", "CLI11"]:
            answer = f'-L"{self.prefix / "lib"}" -lCLI11'
        else:
            raise AssertionError(f"unexpected pkg-config query: {argv}")
        return subprocess.CompletedProcess(argv, 0, answer, "")

    def refresh(self, tree, *, metadata=True):
        pcfile = self.pc_dir / "CLI11.pc"
        if metadata:
            pcfile.write_text(f"Name: CLI11\nVersion: {self.observations['metadata']}\n", encoding="utf-8")
        elif pcfile.exists():
            pcfile.unlink()
        with (
            patch.object(providers, "POLICY", self.policy_path),
            patch.object(native, "native_platform", return_value="windows"),
            patch.object(providers, "vcpkg_prefix", return_value=self.prefix),
            patch.object(providers, "native_provider", side_effect=self.provider),
            patch.object(native, "evidence", return_value=[]),
            patch.object(bazel_deps.subprocess, "run", side_effect=self.query),
        ):
            return dependencies.resolve_dependencies(self.root / tree, "release", prefix=self.prefix, group="native")

    def package(self, repository):
        return repository["manifest"]["packages"][0]

    def test_metadata_and_reviewed_fallback_export_the_same_consumer_interface(self):
        self.install("2.6.2")
        interfaces = []
        for metadata in (True, False):
            with self.subTest(metadata=metadata):
                repository = self.refresh("interface-" + str(metadata), metadata=metadata)
                package = self.package(repository)
                self.assertEqual([item["name"] for item in package["resolved_libraries"]], ["CLI11"])
                self.assertEqual(package["interface"]["linkopts"], ["Shell32.lib"])
                self.assertIn("CLI11_COMPILE", package["interface"]["defines"])
                interfaces.append(repository["packages"][0])
        # The reviewed fallback exists so the same consumer interface is
        # exported with or without installed pkg-config metadata.
        self.assertEqual(interfaces[0], interfaces[1])
        self.assertEqual(interfaces[0]["linkopts"], ["Shell32.lib"])
        self.assertEqual(interfaces[0]["libraries"][0]["source"], str(self.prefix / "lib/CLI11.lib"))


class FrontendNixClosureTest(unittest.TestCase):
    def test_giomm_declares_only_the_three_selected_derivations_outputs(self):
        with tempfile.TemporaryDirectory(prefix="aobus-giomm-outputs-") as directory:
            root = Path(directory).resolve()
            policy = native.read_json(providers.POLICY)
            policy["packages"] = {
                "native": [p for p in policy["packages"]["native"] if p["name"] == "giomm"],
                "test": [],
            }
            roots = {name: [root / (name + "-out"), root / (name + "-dev")] for name in ("glibmm", "glib", "libsigcxx")}
            for selected in roots.values():
                for output in selected:
                    (output / "lib/pkgconfig").mkdir(parents=True)
            report = root / "report.json"
            native.write_json(
                report,
                {
                    "schemaVersion": 1,
                    "nixpkgsRevision": native.read_json(native.PROJECT_ROOT / "nixpkgs.json")["rev"],
                },
            )

            def dependency(report, name):
                return (
                    roots[name][0],
                    "2.88.3" if name == "glib" else "fixture",
                    "/nix/store/" + name + ".drv",
                    roots[name],
                )

            def query(argv, **kwargs):
                owner = "glib" if argv[-1] == "gio-unix-2.0" else "glibmm"
                return subprocess.CompletedProcess(argv, 0, str(roots[owner][1] / "lib/pkgconfig"), "")

            def nix_root(path):
                return next(output for outputs in roots.values() for output in outputs if path.is_relative_to(output))

            with (
                patch.dict(os.environ, {"AOBUS_NIX_DEPENDENCY_REPORT": str(report)}),
                patch.object(providers, "nix_dependency", side_effect=dependency) as selected_dependencies,
                patch.object(providers, "nix_root", side_effect=nix_root),
                patch.object(native, "executable", return_value="/selected/resolver"),
                patch.object(providers.subprocess, "run", side_effect=query),
            ):
                base, versions, expected = providers.linux_provider(policy, identity_path=root / "identity.json")
            all_outputs = sorted(output for outputs in roots.values() for output in outputs)
            self.assertEqual(expected, {"giomm": all_outputs})
            self.assertEqual(
                {call.args[1] for call in selected_dependencies.call_args_list}, {"glibmm", "glib", "libsigcxx"}
            )
            self.assertEqual(versions, {"giomm": "fixture"})
            self.assertTrue(set(map(str, all_outputs)) <= set(base["allowed_roots"]))
            self.assertNotIn("/nix/store", base["allowed_roots"])
            identity = native.read_json(root / "identity.json")
            self.assertEqual(set(identity["inputs"]), {"glib", "libsigcxx"})
            self.assertEqual(identity["inputs"]["glib"]["outputs"], list(map(str, roots["glib"])))


class CatalogSdkTest(unittest.TestCase):
    """The installed ICU/Python SDK composition behind catalog actions.

    The real _catalog_sdk helper and its filesystem admission stay unmocked;
    only the Nix report lookup, the host Python configuration and the genrb
    version query are faked at their external boundaries. Contract admission is
    checked against a self-contained copy of the shared contract with a
    fixture-controlled ICU policy, never against the live pin.
    """

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-catalog-sdk-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.icu_version = "78.3"  # selected provider observation
        self.icu_report = "78.3"  # genrb -V report, an independent observation
        self.genrb = None  # the selected genrb executable
        self.genrb_queries = []

    def fixture_contract(self, version):
        contract = copy.deepcopy(dependency_policy.load_contract())
        contract["dependencies"]["icu"]["policy"] = {"kind": "exact", "version": version}
        contract["exceptions"] = [item for item in contract["exceptions"] if item["dependency"] != "icu"]
        return contract

    def contract(self, version):
        return patch.object(dependency_policy, "load_contract", return_value=self.fixture_contract(version))

    def write_uvernum(self, uvernum, version):
        components = version.split(".")
        lines = [
            f"#define U_ICU_VERSION_MAJOR_NUM {components[0]}\n",
            f"#define U_ICU_VERSION_MINOR_NUM {components[1]}\n",
        ]
        if len(components) == 3:
            lines.append(f"#define U_ICU_VERSION_PATCHLEVEL_NUM {components[2]}\n")
        uvernum.write_text("".join(lines), encoding="utf-8")

    def genrb_version(self, argv, **kwargs):
        # The external tool boundary is part of the contract: exactly the
        # selected genrb executable is queried with "-V", without check and
        # with captured output, in a compiler-stripped child environment. A
        # wrong executable or missing flag fails instead of being answered.
        if argv != [str(self.genrb), "-V"]:
            raise AssertionError(f"unexpected genrb version query: {argv}")
        if kwargs.get("check") is not False or not kwargs.get("capture_output") or not kwargs.get("text"):
            raise AssertionError(f"unexpected genrb query options: {kwargs}")
        environment = kwargs.get("env") or {}
        if any(key.startswith("NIX_CFLAGS_") or key == "CMAKE_PREFIX_PATH" for key in environment):
            raise AssertionError(f"ambient compiler environment leaked to genrb: {sorted(environment)}")
        self.genrb_queries.append(argv)
        # ICU can print its valid version with usage and exit 1 for -V.
        return subprocess.CompletedProcess(argv, 1, "", f"genrb version 56 (ICU version {self.icu_report}).\nusage")

    @contextmanager
    def python_sdk(self, host, base):
        """Select a tiny host Python SDK for the duration of one fixture operation."""
        python = base / "python"
        stdlib = python / "Lib"
        stdlib.mkdir(parents=True)
        (stdlib / "os.py").write_text("# SDK fixture\n", encoding="utf-8")
        extension_dir = python / ("DLLs" if host == "windows" else "lib-dynload")
        extension_dir.mkdir()
        extension = extension_dir / ("_struct.pyd" if host == "windows" else "_struct.so")
        extension.write_bytes(b"extension fixture")
        executable = python / ("python.exe" if host == "windows" else "bin/python3.14")
        executable.parent.mkdir(parents=True, exist_ok=True)
        executable.write_bytes(b"Python launcher fixture")
        native_runtime = python / ("python314.dll" if host == "windows" else "Python")
        native_runtime.write_bytes(b"Python runtime fixture")

        def config(name):
            return {
                "DESTSHARED": str(extension_dir),
                "PYTHONFRAMEWORK": "Python" if host == "macos" else "",
            }.get(name)

        with (
            patch.object(sys, "executable", str(executable)),
            patch.object(sys, "base_prefix", str(python)),
            patch.object(host_tools.sysconfig, "get_path", return_value=str(stdlib)),
            patch.object(host_tools.sysconfig, "get_config_var", side_effect=config),
        ):
            yield executable, extension, native_runtime

    def test_linux_sdk_declares_stdlib_and_extensions_without_ambient_site_packages(self):
        root = self.root
        provider = root / "icu"
        for name in ("bin/genrb", "bin/pkgdata", "lib/libicuuc.so.78"):
            path = provider / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"selected ICU SDK fixture")
        uvernum = provider / "include/unicode/uvernum.h"
        uvernum.parent.mkdir(parents=True)
        self.write_uvernum(uvernum, self.icu_version)
        self.genrb = provider / "bin/genrb"
        stdlib = root / "python/Lib"
        for name in ("json/__init__.py", "os.py", "site-packages/ambient.py", "__pycache__/stale.py"):
            path = stdlib / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"# SDK fixture\n")
        extensions = root / "python/lib-dynload"
        extensions.mkdir()
        extension = extensions / "_struct.so"
        extension.write_bytes(b"native Python extension fixture")
        report = root / "report.json"
        native.write_json(
            report,
            {
                "schemaVersion": 1,
                "nixpkgsRevision": native.read_json(native.PROJECT_ROOT / "nixpkgs.json")["rev"],
            },
        )
        with (
            patch.dict(
                os.environ,
                {
                    "AOBUS_NIX_DEPENDENCY_REPORT": str(report),
                    "NIX_CFLAGS_SET": "-I/ambient",
                    "CMAKE_PREFIX_PATH": "/ambient",
                },
            ),
            patch.object(
                providers,
                "nix_dependency",
                side_effect=lambda report, name: (
                    provider,
                    self.icu_version,
                    "/nix/store/icu.drv",
                    [provider],
                ),
            ),
            patch.object(providers, "nix_root", return_value=provider),
            patch.object(host_tools.sysconfig, "get_path", return_value=str(stdlib)),
            patch.object(
                host_tools.sysconfig,
                "get_config_var",
                side_effect=lambda name: str(extensions) if name == "DESTSHARED" else None,
            ),
            patch.object(subprocess, "run", side_effect=self.genrb_version),
            self.contract(self.icu_version),
        ):
            tools, inputs, evidence = host_tools._catalog_sdk(root / "resolution", None, "linux")
            self.assertEqual(self.genrb_queries, [[str(self.genrb), "-V"]])
            self.assertIn(native.PROJECT_ROOT / "dependency-contract.json", evidence)
            self.assertIn(uvernum, evidence)
            # A tool report that drifts from the selected provider version fails.
            for reported in ("77.1", "78.3.1", "78.30", "78.3rc", "78.3-beta"):
                self.icu_report = reported
                with (
                    self.subTest(reported=reported),
                    self.assertRaisesRegex(native.BazelWorkspaceError, "does not report ICU version 78.3"),
                ):
                    host_tools._catalog_sdk(root / ("wrong-" + reported), None, "linux")
            self.icu_report = "78.3"
            # The installed header must carry the actual provider macros.
            self.write_uvernum(uvernum, "78.4")
            with self.assertRaisesRegex(native.BazelWorkspaceError, "header version mismatch"):
                host_tools._catalog_sdk(root / "wrong-header", None, "linux")
            self.write_uvernum(uvernum, "78.3")
            # A future three-component release needs the fixture contract's approval.
            self.icu_version = "79.2.1"
            self.icu_report = "79.2.1"
            self.write_uvernum(uvernum, "79.2.1")
            with self.assertRaisesRegex(
                native.BazelWorkspaceError, "ICU 79.2.1 does not satisfy dependency-contract.json"
            ):
                host_tools._catalog_sdk(root / "contract-drift", None, "linux")
            with self.contract("79.2.1"):
                tools, inputs, evidence = host_tools._catalog_sdk(root / "upgraded", None, "linux")
            self.assertIn(uvernum, evidence)
            # Only two- or three-component numeric ICU releases are producer formats.
            self.icu_version = "78.3.1.2"
            self.icu_report = "78.3.1.2"
            self.write_uvernum(uvernum, "78.3.1.2")
            with self.contract("78.3.1.2"):
                with self.assertRaisesRegex(native.BazelWorkspaceError, "unsupported ICU provider version"):
                    host_tools._catalog_sdk(root / "bad-format", None, "linux")
            self.icu_version = "78.3"
            self.icu_report = "78.3"
            self.write_uvernum(uvernum, "78.3")
        self.assertEqual(tools["python"], str(Path(sys.executable).absolute()))
        self.assertEqual(tools["genrb"], str(provider / "bin/genrb"))
        self.assertEqual(tools["pkgdata"], str(provider / "bin/pkgdata"))
        self.assertIn(stdlib / "os.py", inputs)
        self.assertIn(stdlib / "json/__init__.py", inputs)
        self.assertIn(extension, inputs)
        self.assertIn(provider / "lib/libicuuc.so.78", inputs)
        self.assertNotIn(stdlib / "site-packages/ambient.py", inputs)
        self.assertNotIn(stdlib / "__pycache__/stale.py", inputs)
        self.assertIn(report, evidence)
        self.assertIn(root / "upgraded/catalog-sdk.json", inputs)

    def test_native_catalog_tools_keep_installed_paths_and_their_execution_inputs(self):
        for host in ("windows", "macos"):
            with self.subTest(host=host):
                base = self.root / host
                prefix = base / "installed" / ("x64-windows" if host == "windows" else "triplet")
                tool_bin = prefix / "tools/icu/bin"
                tool_bin.mkdir(parents=True)
                for name in ("genrb", "pkgdata"):
                    filename = name + (".exe" if host == "windows" else "")
                    (tool_bin / filename).write_bytes(
                        pe_image(filename, executable=True, imports=("icuuc78.dll", "VCRUNTIME140.dll"))
                        if host == "windows"
                        else b"installed tool fixture"
                    )
                if host == "windows":
                    (tool_bin / "icuuc78.dll").write_bytes(pe_image("icuuc78.dll"))
                status = prefix.parent / "vcpkg/status"
                status.parent.mkdir()

                def status_text(revision, prefix=prefix):
                    return (
                        f"Package: icu\nVersion: {self.icu_version}\nPort-Version: {revision}\n"
                        f"Architecture: {prefix.name}\nStatus: install ok installed\n\nPackage: icu\n"
                        f"Feature: tools\nArchitecture: {prefix.name}\nStatus: install ok installed\n"
                    )

                status.write_text(status_text("1"), encoding="utf-8")
                uvernum = prefix / "include/unicode/uvernum.h"
                uvernum.parent.mkdir(parents=True)
                self.write_uvernum(uvernum, self.icu_version)
                self.genrb = tool_bin / ("genrb.exe" if host == "windows" else "genrb")
                environment = {"NIX_CFLAGS_SET": "-I/ambient", "CMAKE_PREFIX_PATH": "/ambient"}
                if host == "windows":
                    environment["SYSTEMROOT"] = str(base / "windows")
                    environment["USERPROFILE"] = str(base / "profile")
                    info = prefix.parent / "vcpkg/info"
                    info.mkdir()
                    ownership = info / f"icu_{self.icu_version}_{prefix.name}.list"
                    ownership.write_text(
                        f"{prefix.name}/tools/icu/bin/genrb.exe\n{prefix.name}/tools/icu/bin/pkgdata.exe\n{prefix.name}/tools/icu/bin/icuuc78.dll\n",
                        encoding="utf-8",
                    )
                with (
                    patch.dict(os.environ, environment),
                    self.python_sdk(host, base) as (executable, extension, native_runtime),
                    patch.object(subprocess, "run", side_effect=self.genrb_version),
                    self.contract(self.icu_version),
                ):
                    tools, inputs, evidence = host_tools._catalog_sdk(base / "resolution", prefix, host)
                    self.assertEqual(self.genrb_queries[-1], [str(self.genrb), "-V"])
                    self.assertEqual(Path(tools["genrb"]).parent, tool_bin)
                    self.assertIn(executable, inputs)
                    self.assertIn(native_runtime, inputs)
                    self.assertIn(extension, inputs)
                    self.assertIn(uvernum, evidence)
                    self.assertIn(native.PROJECT_ROOT / "dependency-contract.json", evidence)
                    if host == "windows":
                        self.assertEqual(tools["SystemRoot"], str(base / "windows"))
                        self.assertEqual(tools["UserProfile"], str(base / "profile"))
                        self.assertIn(tool_bin / "genrb.exe", inputs)
                        self.assertIn(tool_bin / "pkgdata.exe", inputs)
                        self.assertIn(tool_bin / "icuuc78.dll", inputs)
                        self.assertNotIn(prefix / "bin/icuuc78.dll", inputs)
                        (tool_bin / "icutu78.dll").write_bytes(b"old shared SDK artifact")
                        with self.assertRaisesRegex(
                            native.BazelWorkspaceError, "unowned Windows ICU host-tool runtime"
                        ):
                            host_tools._catalog_sdk(base / "old-shared-runtime", prefix, host)
                        (tool_bin / "icutu78.dll").unlink()
                        ownership.write_text(f"{prefix.name}/tools/icu/bin/genrb.exe\n", encoding="utf-8")
                        with self.assertRaisesRegex(native.BazelWorkspaceError, "tools manifest"):
                            host_tools._catalog_sdk(base / "unowned-pkgdata", prefix, host)
                        ownership.write_text(
                            f"{prefix.name}/tools/icu/bin/genrb.exe\n{prefix.name}/tools/icu/bin/pkgdata.exe\n{prefix.name}/tools/icu/bin/icuuc78.dll\n",
                            encoding="utf-8",
                        )
                        # The serialized diagnostic input omits only the fixture-only
                        # profile: a profile change keeps the cache-keyed action input
                        # identical, while the returned tools map still carries the
                        # observed profile for the portal fixture environment.
                        identity_path = base / "resolution/catalog-sdk.json"
                        identity = json.loads(identity_path.read_text(encoding="utf-8"))
                        self.assertNotIn("UserProfile", identity["tools"])
                        self.assertEqual(
                            identity["tools"],
                            {
                                "python": str(executable),
                                "genrb": str(tool_bin / "genrb.exe"),
                                "pkgdata": str(tool_bin / "pkgdata.exe"),
                                "SystemRoot": str(base / "windows"),
                            },
                        )
                        self.assertEqual(identity["icu_version"], self.icu_version)
                        original = identity_path.read_bytes()
                        with patch.dict(os.environ, {"USERPROFILE": str(base / "other-profile")}):
                            regenerated, _, _ = host_tools._catalog_sdk(base / "resolution", prefix, host)
                            self.assertEqual(regenerated["UserProfile"], str(base / "other-profile"))
                            self.assertEqual(identity_path.read_bytes(), original)
                        # SystemRoot remains an identity field and still invalidates.
                        with patch.dict(os.environ, {"SYSTEMROOT": str(base / "other-windows")}):
                            host_tools._catalog_sdk(base / "resolution", prefix, host)
                        self.assertNotEqual(identity_path.read_bytes(), original)
                    # A changed raw port revision with the same upstream stays structurally valid.
                    status.write_text(status_text("2"), encoding="utf-8")
                    tools, _, _ = host_tools._catalog_sdk(base / "revised-port", prefix, host)
                    self.assertEqual(Path(tools["genrb"]).parent, tool_bin)

    def test_windows_sdk_admission_follows_the_contract_and_raw_features(self):
        base = self.root / "admission"
        prefix = base / "installed/x64-windows"
        tool_bin = prefix / "tools/icu/bin"
        tool_bin.mkdir(parents=True)
        for name in ("genrb", "pkgdata"):
            (tool_bin / (name + ".exe")).write_bytes(
                pe_image(name + ".exe", executable=True, imports=("icuuc79.dll", "VCRUNTIME140.dll"))
            )
        (tool_bin / "icuuc79.dll").write_bytes(pe_image("icuuc79.dll"))
        self.icu_version = "79.1"
        self.icu_report = "79.1"
        status = prefix.parent / "vcpkg/status"
        status.parent.mkdir()
        status.write_text(
            "Package: icu\nVersion: 79.1\nPort-Version: 2\nArchitecture: x64-windows\n"
            "Status: install ok installed\n\nPackage: icu\nFeature: tools\n"
            "Architecture: x64-windows\nStatus: install ok installed\n",
            encoding="utf-8",
        )
        uvernum = prefix / "include/unicode/uvernum.h"
        uvernum.parent.mkdir(parents=True)
        self.write_uvernum(uvernum, self.icu_version)
        self.genrb = tool_bin / "genrb.exe"
        info = prefix.parent / "vcpkg/info"
        info.mkdir()
        ownership = info / "icu_79.1_x64-windows.list"
        ownership.write_text(
            "x64-windows/tools/icu/bin/genrb.exe\nx64-windows/tools/icu/bin/pkgdata.exe\nx64-windows/tools/icu/bin/icuuc79.dll\n",
            encoding="utf-8",
        )
        with (
            patch.dict(
                os.environ,
                {
                    "SYSTEMROOT": str(base / "windows"),
                    "NIX_CFLAGS_SET": "-I/ambient",
                    "CMAKE_PREFIX_PATH": "/ambient",
                },
            ),
            self.python_sdk("windows", base),
            patch.object(subprocess, "run", side_effect=self.genrb_version),
            self.contract("78.3"),
        ):
            # The fixture contract admits only its own ICU release on the native branch.
            with self.assertRaisesRegex(
                native.BazelWorkspaceError, "ICU 79.1 does not satisfy dependency-contract.json"
            ):
                host_tools._catalog_sdk(base / "contract-drift", prefix, "windows")
            with self.contract("79.1"):
                # An approved contract change plus a coherent next SDK passes structurally;
                # the catalog helper reads the contract, never a Bazel policy pin.
                tools, _, evidence = host_tools._catalog_sdk(base / "resolution", prefix, "windows")
                self.assertEqual(self.genrb_queries[-1], [str(self.genrb), "-V"])
                self.assertEqual(tools["genrb"], str(tool_bin / "genrb.exe"))
                self.assertIn(uvernum, evidence)
                self.assertIn(ownership, evidence)
                self.assertIn(native.PROJECT_ROOT / "dependency-contract.json", evidence)
                # The installed header must match the actual provider observation.
                self.write_uvernum(uvernum, "78.3")
                with self.assertRaisesRegex(native.BazelWorkspaceError, "header version mismatch"):
                    host_tools._catalog_sdk(base / "wrong-header", prefix, "windows")
                self.write_uvernum(uvernum, "79.1")
                # The actual tool report must match the provider version.
                self.icu_report = "78.3"
                with self.assertRaisesRegex(native.BazelWorkspaceError, "does not report ICU version 79.1"):
                    host_tools._catalog_sdk(base / "wrong-tool", prefix, "windows")
                self.icu_report = "79.1"
            # Raw core/tools features remain required independently of contract admission.
            status.write_text(
                "Package: icu\nVersion: 79.1\nPort-Version: 2\nArchitecture: x64-windows\n"
                "Status: install ok installed\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(
                native.BazelWorkspaceError, "catalog tools require installed ICU core/tools features"
            ):
                host_tools._catalog_sdk(base / "missing-tools-feature", prefix, "windows")

    def test_private_bus_tool_is_selected_from_reported_outputs_not_path(self):
        with tempfile.TemporaryDirectory(prefix="aobus-dbus-sdk-") as directory:
            root = Path(directory).resolve()
            provider = root / "dbus"
            daemon = provider / "bin/dbus-daemon"
            daemon.parent.mkdir(parents=True)
            daemon.write_bytes(b"private bus SDK fixture")
            report = root / "report.json"
            native.write_json(
                report,
                {
                    "schemaVersion": 1,
                    "nixpkgsRevision": native.read_json(native.PROJECT_ROOT / "nixpkgs.json")["rev"],
                },
            )
            with (
                patch.dict(os.environ, {"AOBUS_NIX_DEPENDENCY_REPORT": str(report), "PATH": ""}),
                patch.object(
                    providers, "nix_dependency", return_value=(provider, "fixture", "/nix/store/dbus.drv", [provider])
                ) as selected,
                patch.object(providers, "nix_root", return_value=provider),
            ):
                identity, executable, runtime, evidence = host_tools._linux_dbus()
            self.assertEqual(selected.call_args.args[1], "dbus")
            self.assertEqual(executable, daemon)
            self.assertEqual(identity["outputs"], [str(provider)])
            self.assertEqual(runtime, [])
            self.assertIn(report, evidence)


class AppVersionProjectionTest(unittest.TestCase):
    def test_version_is_derived_from_the_shared_root_plaintext_file(self):
        with tempfile.TemporaryDirectory(prefix="aobus-app-version-") as directory:
            root = Path(directory)
            path = root / "VERSION.txt"
            path.write_text("1.2.3\n", encoding="utf-8")
            # A legacy bare VERSION must not be admitted as the version.
            (root / "VERSION").write_text("9.9.9\n", encoding="utf-8")
            self.assertEqual(host_tools._app_version(root), "1.2.3")
            for text in (
                "",
                "1.2",
                "1.2.3.4",
                "v1.2.3\n",
                "01.2.3\n",
                "1.2.3\n1.2.4\n",
                "# release\n1.2.3\n",
                "release 1.2.3\n",
                "1٢.3.4\n",
                "\u00a01.2.3\u00a0",
            ):
                path.write_text(text, encoding="utf-8")
                with self.subTest(text=text), self.assertRaisesRegex(ValueError, "x.y.z"):
                    host_tools._app_version(root)
            path.unlink()
            # The ignored bare VERSION is not a fallback for the missing fact.
            with self.assertRaisesRegex(ValueError, "missing shared root version file"):
                host_tools._app_version(root)

    def test_checked_in_root_version_file_is_admitted_by_the_shared_reader(self):
        # The real repository fact must stay admissible; CMake reads the same
        # bytes before project(), so this is the one shared version source.
        self.assertEqual(
            host_tools._app_version(native.PROJECT_ROOT),
            (native.PROJECT_ROOT / "VERSION.txt").read_text(encoding="utf-8").strip(),
        )


if __name__ == "__main__":
    unittest.main()
