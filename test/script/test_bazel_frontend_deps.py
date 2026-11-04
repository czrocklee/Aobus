"""Actual frontend and catalog consumers of the governed native projection."""

import ast
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

from ao.core import bazel_deps, bazel_resolver, dependency_policy
from ao.core import bazel_workspace as workspace


class FrontendDependencyPolicyTest(unittest.TestCase):
    def test_frontend_imports_follow_the_actual_platform_consumers(self):
        policy = workspace._json(workspace.POLICY)
        # Only genuinely host-specific consumers are filtered; the module and
        # input inventories are witnessed by the actual refresh projections.
        for host, expected in (("linux", {"giomm"}), ("macos", {"date_tz"}), ("windows", set())):
            with self.subTest(host=host):
                packages = {p["name"] for p in workspace._package_groups(policy, host=host)["native"]}
                self.assertEqual(packages & {"giomm", "date_tz"}, expected)
        for name, platforms in (("giomm", ["windows"]), ("date_tz", ["linux"])):
            changed = copy.deepcopy(policy)
            next(p for p in changed["packages"]["native"] if p["name"] == name)["platforms"] = platforms
            with self.subTest(name=name), self.assertRaisesRegex(workspace.BazelWorkspaceError, "platform"):
                workspace._package_groups(changed, host="windows")

    def test_mimalloc_metadata_translation_is_one_closed_producer_rule_on_every_host(self):
        # A future dotted release translates identically on every host; no per-host
        # pin remains. The malformed-format and wrong-module rejections below are
        # equivalence classes of one closed rule, not per-host repetitions.
        for host in ("linux", "macos", "windows"):
            with self.subTest(host=host):
                self.assertEqual(
                    workspace._metadata_versions("mimalloc", "4.1.2", ["mimalloc"], host), {"mimalloc": "4.1"}
                )
        for version, expected in (("3.5.3", "3.5"), ("4.10.2", "4.10"), ("100.2.0", "100.2")):
            with self.subTest(version=version):
                self.assertEqual(
                    workspace._metadata_versions("mimalloc", version, ["mimalloc"], "linux"),
                    {"mimalloc": expected},
                )
        for version in ("3.5", "3.5.3.1", "3.5.3-unstable", "v3.5.3"):
            with (
                self.subTest(version=version),
                self.assertRaisesRegex(workspace.BazelWorkspaceError, "metadata version"),
            ):
                workspace._metadata_versions("mimalloc", version, ["mimalloc"], "linux")
        with self.assertRaisesRegex(workspace.BazelWorkspaceError, "metadata version"):
            workspace._metadata_versions("mimalloc", "3.5.3", ["other"], "linux")

    def test_unicode_only_macos_filter_queries_its_own_metadata(self):
        policy = workspace._json(workspace.POLICY)
        spec = {"platform": "macos", "config": "Release", "provider": {"name": "vcpkg", "version": "selected-status"}}
        with patch.object(workspace, "_metadata_flags", return_value=["-licuuc", "-licudata", "-lc++"]) as flags:
            result = workspace._macos_icu_runtime_patch(spec, policy["macos_icu_runtime_remove"], package="icu_uc")
        flags.assert_called_once_with(spec, "--libs", package="icu_uc")
        self.assertEqual(result["when"]["package"], "icu_uc")
        self.assertEqual(result["remove"], {"libraries": [{"name": "c++", "kind": "system"}]})

    def test_path_has_one_real_object_owner_and_unicode_uses_only_uc(self):
        tree = ast.parse((workspace.PROJECT_ROOT / "lib/utility/BUILD.bazel").read_text())
        rules = {}
        for node in tree.body:
            if (
                isinstance(node, ast.Expr)
                and isinstance(node.value, ast.Call)
                and getattr(node.value.func, "id", None) == "cc_library"
            ):
                fields = {field.arg: field.value for field in node.value.keywords}
                rules[ast.literal_eval(fields["name"])] = fields
        # One real object owner for Path.cpp; utility consumes the compiled target.
        self.assertIn(
            "Path.cpp", [node.value for node in ast.walk(rules["path"]["srcs"]) if isinstance(node, ast.Constant)]
        )
        self.assertIn(":path", ast.literal_eval(rules["utility"]["deps"]))
        self.assertNotIn(
            "Path.cpp", [node.value for node in ast.walk(rules["utility"]["srcs"]) if isinstance(node, ast.Constant)]
        )
        self.assertEqual(
            ast.literal_eval(rules["unicode_text"]["implementation_deps"]), ["@aobus_native_deps//:icu_uc"]
        )


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
        policy = workspace._json(workspace.POLICY)
        names = {"icu", "icu_uc", "cli11", "spdlog", "ftxui", "mimalloc", "stb", "giomm"}
        policy["packages"] = {"native": [p for p in policy["packages"]["native"] if p["name"] in names], "test": []}
        self.policy = self.root / "policy.json"
        workspace._write_json(self.policy, policy)
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
        workspace._write_json(
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
            patch.object(workspace, "POLICY", self.policy),
            patch.object(workspace, "native_platform", return_value="linux"),
            patch.object(workspace, "_linux_provider", side_effect=self.provider_spec) as provider,
            patch.object(workspace, "_verify_linux_resolution"),
            patch.object(bazel_deps.subprocess, "run", side_effect=self.query),
        ):
            generated = workspace.refresh_dependencies(self.root / (group + config), config, group=group)
        return generated / group, provider.call_args.args[0]

    def test_linux_refresh_projects_each_frontend_interface_from_provider_metadata(self):
        # Dummy archive bytes stand in for real payloads: this is projection
        # coverage over the provider report, not compiler or linker acceptance.
        repository, _ = self.refresh()
        manifest = workspace._json(repository / "manifest.json")
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
        # Header-only consumers keep their selected include directories.
        self.assertEqual(packages["stb"]["interface"]["header_dirs"], [str(self.include / "stb")])
        self.assertIn(str(self.include / "gio-unix-2.0"), packages["giomm"]["interface"]["header_dirs"])
        # The mimalloc metadata translation is applied end to end.
        self.assertEqual(manifest["modules"]["mimalloc"]["version"], "3.5")

    def test_incompatible_spdlog_abi_and_incomplete_ftxui_are_rejected(self):
        for defines in (
            ["SPDLOG_COMPILED_LIB"],
            [*self.defines, "SPDLOG_FMT_EXTERNAL=1"],
            [*self.defines, "SPDLOG_HEADER_ONLY"],
        ):
            self.defines = defines
            with (
                self.subTest(defines=defines),
                self.assertRaisesRegex(workspace.BazelWorkspaceError, "compiled std::format ABI"),
            ):
                self.refresh()
        self.defines = ["SPDLOG_SHARED_LIB", "SPDLOG_COMPILED_LIB", "SPDLOG_USE_STD_FORMAT"]
        self.libraries["ftxui"].pop()
        with self.assertRaisesRegex(workspace.BazelWorkspaceError, "complete component/dom/screen"):
            self.refresh()

    def test_future_provider_observations_pass_the_unchanged_structural_interface(self):
        # Independent fixture observations, not review pins: a future mimalloc release
        # keeps the same metadata translation rule and header/interface contract.
        self.versions["mimalloc"] = "4.0.1"
        self.module_versions["mimalloc"] = "4.0"
        (self.include / "mimalloc.h").write_text("#define MI_MALLOC_VERSION 40001\n")
        (self.pc / "mimalloc.pc").write_text("Name: mimalloc\nVersion: 4.0\n", encoding="utf-8")
        repository, _ = self.refresh()
        manifest = workspace._json(repository / "manifest.json")
        self.assertEqual(manifest["modules"]["mimalloc"]["version"], "4.0")

    def test_catalog_refresh_selects_only_icu_and_passes_its_compilation_mode(self):
        # The generated select/no_match_error behavior is owned by the adapter
        # tests; this keeps the refresh-level mode pass-through witness.
        for config, mode in (("debug", "dbg"), ("release", "opt")):
            repository, _ = self.refresh("catalog", config)
            with self.subTest(config=config):
                manifest = workspace._json(repository / "manifest.json")
                self.assertEqual([p["name"] for p in manifest["packages"]], ["icu"])
                build = (repository / "BUILD.bazel").read_text()
                self.assertIn('"compilation_mode": "' + mode + '"', build)


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
            workspace._stb_interface([self.provider], "linux", "2.19"),
            {"header_dirs": [str(self.provider / "include/stb")]},
        )
        # A banner without the actually consumed resize API is not a valid interface.
        header.write_text("/* stb_image_resize2 - v2.19 - future release */\n", encoding="utf-8")
        with self.assertRaisesRegex(workspace.BazelWorkspaceError, "stb resize2 interface is missing"):
            workspace._stb_interface([self.provider], "linux", "2.19")
        # Native hosts still compare the observed version against the overlay owner.
        native = self.provider / "include/stb_image_resize2.h"
        native.write_text(
            "/* stb_image_resize2 - fixture overlay */\n#define stbir_resize_uint8_srgb 1\n#define STBIR_RGBA 1\n",
            encoding="utf-8",
        )
        with self.assertRaisesRegex(workspace.BazelWorkspaceError, "does not match the governed overlay"):
            workspace._stb_interface([self.provider], "windows", "9.9.9")
        overlay = workspace._json(workspace.PROJECT_ROOT / "cmake/vcpkg-ports/stb/vcpkg.json")
        self.assertEqual(
            workspace._stb_interface([self.provider], "windows", overlay["version-string"]),
            {"header_dirs": [str(self.provider / "include")]},
        )


class NativeFrontendArtifactTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-native-frontend-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.prefix = self.root / "provider"
        self.policy = workspace._json(workspace.POLICY)
        self.packages = {p["name"]: p for p in self.policy["packages"]["native"]}
        for marker in (
            "CLI/Version.hpp",
            "spdlog/spdlog.h",
            "ftxui/component/component.hpp",
            "mimalloc.h",
            "date/tz.h",
        ):
            path = self.prefix / "include" / marker
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("// selected SDK fixture\n")
        (self.prefix / "include/mimalloc.h").write_text(
            "#if !defined(MI_SHARED_LIB)\n#define mi_decl_export\n#elif defined(MI_SHARED_LIB_EXPORT)\n#endif\n"
        )
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
            (selected / "lib" / (mimalloc + ".lib")).write_bytes(b"static fixture")

    def interface(self, name, host, config):
        return workspace._library_interface(self.packages[name], [self.prefix], config, host, self.policy)

    def test_native_static_artifacts_follow_each_configuration(self):
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
                        str(selected / "lib" / (spelling + ".lib" if host == "windows" else "lib" + spelling + ".a")),
                    )
                    if host == "windows":
                        self.assertEqual(
                            mimalloc["linkopts"],
                            ["psapi.lib", "shell32.lib", "user32.lib", "advapi32.lib", "bcrypt.lib"],
                        )

    def test_reviewed_invalid_static_interfaces_are_rejected(self):
        # These guards sit before any host/configuration branch in the interface
        # projection, so one representative combination carries each rule.
        for name, mutation, message in (
            ("spdlog", {"linkage": "shared"}, "unsupported reviewed linkage"),
            ("mimalloc", {"runtime": ["injected.dll"]}, "unsupported extra frontend runtime interface"),
        ):
            with (
                self.subTest(name=name, mutation=next(iter(mutation))),
                patch.dict(self.policy["library_fallbacks"][name]["windows"], mutation),
                self.assertRaisesRegex(workspace.BazelWorkspaceError, message),
            ):
                self.interface(name, "windows", "release")

    def test_static_mimalloc_rejects_shared_installed_header_instead_of_inventing_a_macro(self):
        header = self.prefix / "include/mimalloc.h"
        static = header.read_text(encoding="utf-8")
        header.write_text(static.replace("#if !defined(MI_SHARED_LIB)", "#if 0"), encoding="utf-8")
        with self.assertRaisesRegex(workspace.BazelWorkspaceError, "static mimalloc"):
            self.interface("mimalloc", "windows", "release")

    def test_date_tz_artifact_consumer_keeps_its_reviewed_abi_definitions(self):
        # This checks the required macOS artifact consumer ABI, not the
        # metadata-absent refresh route or a successful native link.
        for config in ("debug", "release"):
            interface = self.interface("date_tz", "macos", config)
            with self.subTest(config=config):
                self.assertEqual(interface["defines"], ["ONLY_C_LOCALE=0", "HAS_STRING_VIEW=1", "USE_OS_TZDB=1"])
                self.assertEqual(interface["linkopts"], ["-pthread"])
                self.assertEqual(interface["libraries"][0]["name"], "date-tz")
        with self.assertRaisesRegex(workspace.BazelWorkspaceError, "no reviewed"):
            self.interface("date_tz", "windows", "debug")

    def test_missing_archive_and_old_dll_shapes_never_change_static_linkage(self):
        missing = self.prefix / "lib/mimalloc.lib"
        missing.unlink()
        with self.assertRaisesRegex(workspace.BazelWorkspaceError, "missing/ambiguous"):
            self.interface("mimalloc", "windows", "release")
        # Restore the valid baseline so the old-shape negatives below prove the
        # shape rejection itself, not the missing archive.
        missing.write_bytes(b"static fixture")
        for config in ("debug", "release"):
            selected = self.prefix / "debug" if config == "debug" else self.prefix
            spelling = "mimalloc-debug" if config == "debug" else "mimalloc"
            for relative in ("lib/" + spelling + ".dll.lib", "bin/" + spelling + ".dll", "bin/mimalloc-redirect.dll"):
                old = selected / relative
                old.write_bytes(b"old shared SDK artifact")
                with (
                    self.subTest(config=config, relative=relative),
                    self.assertRaisesRegex(workspace.BazelWorkspaceError, "unsupported static provider shape"),
                ):
                    self.interface("mimalloc", "windows", config)
                old.unlink()


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
        self.prefix = self.root / "installed/x64-windows-static-md"
        policy = workspace._json(workspace.POLICY)
        cli11 = next(p for p in policy["packages"]["native"] if p["name"] == "cli11")
        policy["packages"] = {"native": [cli11], "test": []}
        self.policy_path = self.root / "cli11-policy.json"
        workspace._write_json(self.policy_path, policy)
        self.include = self.prefix / "include"
        self.header = self.include / "CLI/Version.hpp"
        self.header.parent.mkdir(parents=True)
        (self.prefix / "lib").mkdir(parents=True)
        (self.prefix / "lib/CLI11.lib").write_bytes(b"static fixture")
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
            f"{self.prefix.name}/include/CLI/Version.hpp\n", encoding="utf-8"
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
        workspace._write_json(identity_path, {"packages": {"cli11": {"version": self.observations["raw_status"]}}})
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
            patch.object(workspace, "POLICY", self.policy_path),
            patch.object(workspace, "native_platform", return_value="windows"),
            patch.object(workspace, "vcpkg_prefix", return_value=self.prefix),
            patch.object(workspace, "_native_provider", side_effect=self.provider),
            patch.object(workspace, "_evidence", return_value=[]),
            patch.object(bazel_deps.subprocess, "run", side_effect=self.query),
        ):
            generated = workspace.refresh_dependencies(self.root / tree, "release", prefix=self.prefix, group="native")
        return generated / "native"

    def package(self, repository):
        return workspace._json(repository / "manifest.json")["packages"][0]

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
                interfaces.append(
                    (
                        {
                            field: package["interface"][field]
                            for field in ("header_dirs", "defines", "linkopts", "copts", "runtime")
                        },
                        package["resolved_libraries"],
                    )
                )
        # The reviewed fallback exists so the same consumer interface is
        # exported with or without installed pkg-config metadata.
        self.assertEqual(interfaces[0], interfaces[1])
        build = (repository / "BUILD.bazel").read_text()
        self.assertIn('linkopts = ["Shell32.lib"]', build)

    def test_a_coherent_future_cli11_release_passes_the_structural_interface(self):
        self.install("2.7.0")
        repository = self.refresh("upgraded")
        package = self.package(repository)
        self.assertEqual([item["name"] for item in package["resolved_libraries"]], ["CLI11"])

    def test_incoherent_metadata_or_header_observations_fail(self):
        self.install("2.6.2")
        baseline = dict(self.observations)
        for observation, value, error, message in (
            ("metadata", "2.6.1", bazel_deps.BazelDependencyError, "module version mismatch: CLI11"),
            ("header", "2.6.1", workspace.BazelWorkspaceError, "header version mismatch"),
        ):
            with self.subTest(observation=observation):
                if observation == "header":
                    self.write_header(value)
                else:
                    self.observations[observation] = value
                with self.assertRaisesRegex(error, message):
                    self.refresh("wrong-" + observation)
                if observation == "header":
                    self.write_header("2.6.2")
                else:
                    self.observations[observation] = baseline[observation]


class FrontendNixClosureTest(unittest.TestCase):
    def test_giomm_declares_only_the_three_selected_derivations_outputs(self):
        with tempfile.TemporaryDirectory(prefix="aobus-giomm-outputs-") as directory:
            root = Path(directory).resolve()
            policy = workspace._json(workspace.POLICY)
            policy["packages"] = {
                "native": [p for p in policy["packages"]["native"] if p["name"] == "giomm"],
                "test": [],
            }
            roots = {name: [root / (name + "-out"), root / (name + "-dev")] for name in ("glibmm", "glib", "libsigcxx")}
            for selected in roots.values():
                for output in selected:
                    (output / "lib/pkgconfig").mkdir(parents=True)
            report = root / "report.json"
            workspace._write_json(
                report,
                {
                    "schemaVersion": 1,
                    "nixpkgsRevision": workspace._json(workspace.PROJECT_ROOT / "nixpkgs.json")["rev"],
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
                patch.object(workspace, "_nix_dependency", side_effect=dependency) as dependencies,
                patch.object(workspace, "_nix_root", side_effect=nix_root),
                patch.object(workspace, "_executable", return_value="/selected/resolver"),
                patch.object(workspace.subprocess, "run", side_effect=query),
            ):
                base, versions, expected = workspace._linux_provider(policy, identity_path=root / "identity.json")
            all_outputs = sorted(output for outputs in roots.values() for output in outputs)
            self.assertEqual(expected, {"giomm": all_outputs})
            self.assertEqual({call.args[1] for call in dependencies.call_args_list}, {"glibmm", "glib", "libsigcxx"})
            self.assertEqual(versions, {"giomm": "fixture"})
            self.assertTrue(set(map(str, all_outputs)) <= set(base["allowed_roots"]))
            self.assertNotIn("/nix/store", base["allowed_roots"])
            identity = workspace._json(root / "identity.json")
            self.assertEqual(set(identity["inputs"]), {"glib", "libsigcxx"})
            self.assertEqual(identity["inputs"]["glib"]["outputs"], list(map(str, roots["glib"])))


class CatalogToolsRepositoryTest(unittest.TestCase):
    def test_tools_repository_prefixes_catalog_imports_and_exposes_selected_action_sdks(self):
        with tempfile.TemporaryDirectory(prefix="aobus-catalog-tools-") as directory:
            root = Path(directory).resolve() / "Åsa" / "音乐"
            root.mkdir(parents=True)
            (root / "CMakeLists.txt").write_text("project(aobus VERSION 4.5.6)\n")
            request = root / "request.json"
            workspace._write_json(request, {"schema": 1, "host": "linux", "config": "debug"})
            executable = root / "provider/bin/gperf"
            executable.parent.mkdir(parents=True)
            executable.write_bytes(b"SDK fixture")
            sdk = root / "provider/stdlib-音乐.py"
            sdk.write_bytes(b"# selected Python SDK fixture\n")
            python = root / "Python/bin/python3"
            python.parent.mkdir(parents=True)
            python.write_bytes(b"selected Python launcher fixture")
            daemon = executable.with_name("dbus-daemon")
            daemon.write_bytes(b"selected D-Bus SDK fixture")
            resource_compiler = root / "glib-dev/bin/glib-compile-resources"
            resource_compiler.parent.mkdir(parents=True)
            resource_compiler.write_bytes(b"selected GLib resource compiler fixture")
            resource_runtime = root / "glib-out/lib/libgio-2.0.so.0"
            resource_runtime.parent.mkdir(parents=True)
            resource_runtime.write_bytes(b"selected GLib resource runtime fixture")
            resource_evidence = root / "glib-report-音乐.json"
            resource_evidence.write_text("{}\n", encoding="utf-8")
            resource_identity = {
                "provider": "nix",
                "version": "fixture-glib",
                "root": str(resource_runtime.parent.parent),
            }
            output = root / "repository/resolution"

            def imports(tree, config, *, prefix, group):
                self.assertEqual(group, "catalog")
                generated = tree / "generated"
                repository = generated / "catalog"
                repository.mkdir(parents=True)
                workspace._write_json(
                    repository / "repository.json",
                    {
                        "links": [{"source": str(sdk), "destination": "artifacts/icu/header.h"}],
                        "watch_files": [str(root / (config + "-status"))],
                        "watch_directories": [str(root / (config + "-pc"))],
                    },
                )
                (repository / "BUILD.bazel").write_text('cc_library(name = "icu")\n')
                (repository / "manifest.json").write_text("{}\n")
                return generated

            tools = {
                "python": str(python),
                "genrb": str(executable.with_name("genrb")),
                "pkgdata": str(executable.with_name("pkgdata")),
            }
            argv = [
                "bazel_resolver.py",
                "--request",
                str(request),
                "--group",
                "tools",
                "--output",
                str(output),
                "--environment-json",
                "{}",
            ]
            with (
                patch.object(sys, "argv", argv),
                patch.dict(os.environ, {}, clear=True),
                patch.object(sys, "addaudithook"),
                patch("ao.core.paths.PROJECT_ROOT", root),
                patch.object(workspace, "_INPUT_PROBE", None),
                patch.object(workspace, "native_platform", return_value="linux"),
                patch.object(workspace, "_linux_gperf", return_value=({"version": "3.3"}, executable, [], [])),
                patch.object(workspace, "refresh_dependencies", side_effect=imports) as refresh,
                patch.object(workspace, "_catalog_sdk", return_value=(tools, [sdk, python], [])),
                patch.object(workspace, "_linux_dbus", return_value=({"provider": "nix"}, daemon, [], [])),
                patch.object(
                    workspace,
                    "_linux_gresource",
                    return_value=(resource_identity, resource_compiler, [resource_runtime], [resource_evidence]),
                ),
                patch.object(subprocess, "run", return_value=subprocess.CompletedProcess([], 0, "GNU gperf 3.3\n", "")),
            ):
                self.assertEqual(bazel_resolver.main(), 0)
            self.assertEqual({call.args[1] for call in refresh.call_args_list}, {"debug", "release"})
            plan = workspace._json(output / "repository.json")
            destinations = [link["destination"] for link in plan["links"]]
            self.assertIn("catalog-debug/artifacts/icu/header.h", destinations)
            self.assertIn("catalog-release/artifacts/icu/header.h", destinations)
            self.assertIn(str(root / "debug-status"), plan["watch_files"])
            self.assertIn(str(root / "release-pc"), plan["watch_directories"])
            self.assertEqual(plan["files"]["app_version.bzl"], 'APP_VERSION = "4.5.6"\n')
            self.assertEqual(
                ast.literal_eval(plan["files"]["test_tools.bzl"].split("=", 1)[1].strip()),
                {"dbus_daemon": str(daemon), "dbus_daemon_directory": str(daemon.parent)},
            )
            self.assertEqual(ast.literal_eval(plan["files"]["catalog_tools.bzl"].split("=", 1)[1].strip()), tools)
            for name in ("catalog_tools.bzl", "test_tools.bzl"):
                text = plan["files"][name]
                self.assertIn("Åsa", text)
                self.assertIn("音乐", text)
                # Python parses JSON Unicode escapes, but Starlark does not.
                self.assertNotRegex(text, r"\\u[0-9a-fA-F]{4}")
            build = plan["files"]["BUILD.bazel"]
            ast.parse(build)
            # The selected stdlib SDK is reachable without freezing its enumeration index.
            self.assertIn("stdlib-音乐.py", build)
            self.assertNotRegex(build, r"\\u[0-9a-fA-F]{4}")
            self.assertIn('"//catalog-debug:icu"', build)
            self.assertIn('"//catalog-release:icu"', build)
            self.assertIn('name = "catalog_tools"', build)
            self.assertIn('name = "dbus_daemon"', build)
            self.assertNotIn("//conditions:default", build)
            rules = {
                ast.literal_eval(next(field.value for field in node.value.keywords if field.arg == "name")): {
                    field.arg: ast.literal_eval(field.value) for field in node.value.keywords
                }
                for node in ast.parse(build).body
                if isinstance(node, ast.Expr)
                and isinstance(node.value, ast.Call)
                and getattr(node.value.func, "id", None) in {"alias", "filegroup"}
                and not any(isinstance(field.value, ast.Call) for field in node.value.keywords)
            }
            links = {link["source"]: link["destination"] for link in plan["links"]}
            self.assertEqual(rules["glib_compile_resources"]["actual"], links[str(resource_compiler)])
            self.assertEqual(
                set(rules["glib_compile_resources_runtime"]["srcs"]),
                {links[str(resource_runtime)], links[str(resource_evidence)]},
            )
            self.assertLessEqual(
                {str(resource_compiler), str(resource_runtime), str(resource_evidence)}, set(plan["watch_files"])
            )
            self.assertIn(str(resource_compiler.parent), plan["watch_directories"])
            self.assertEqual(json.loads(plan["files"]["manifest.json"])["resource_tool"], resource_identity)


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
        return patch.object(workspace.dependency_policy, "load_contract", return_value=self.fixture_contract(version))

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
            patch.object(workspace.sysconfig, "get_path", return_value=str(stdlib)),
            patch.object(workspace.sysconfig, "get_config_var", side_effect=config),
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
        workspace._write_json(
            report,
            {
                "schemaVersion": 1,
                "nixpkgsRevision": workspace._json(workspace.PROJECT_ROOT / "nixpkgs.json")["rev"],
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
                workspace,
                "_nix_dependency",
                side_effect=lambda report, name: (
                    provider,
                    self.icu_version,
                    "/nix/store/icu.drv",
                    [provider],
                ),
            ),
            patch.object(workspace, "_nix_root", return_value=provider),
            patch.object(workspace.sysconfig, "get_path", return_value=str(stdlib)),
            patch.object(
                workspace.sysconfig,
                "get_config_var",
                side_effect=lambda name: str(extensions) if name == "DESTSHARED" else None,
            ),
            patch.object(subprocess, "run", side_effect=self.genrb_version),
            self.contract(self.icu_version),
        ):
            tools, inputs, evidence = workspace._catalog_sdk(root / "resolution", None, "linux")
            self.assertEqual(self.genrb_queries, [[str(self.genrb), "-V"]])
            self.assertIn(workspace.PROJECT_ROOT / "dependency-contract.json", evidence)
            self.assertIn(uvernum, evidence)
            # A tool report that drifts from the selected provider version fails.
            for reported in ("77.1", "78.3.1", "78.30", "78.3rc", "78.3-beta"):
                self.icu_report = reported
                with (
                    self.subTest(reported=reported),
                    self.assertRaisesRegex(workspace.BazelWorkspaceError, "does not report ICU version 78.3"),
                ):
                    workspace._catalog_sdk(root / ("wrong-" + reported), None, "linux")
            self.icu_report = "78.3"
            # The installed header must carry the actual provider macros.
            self.write_uvernum(uvernum, "78.4")
            with self.assertRaisesRegex(workspace.BazelWorkspaceError, "header version mismatch"):
                workspace._catalog_sdk(root / "wrong-header", None, "linux")
            self.write_uvernum(uvernum, "78.3")
            # A future three-component release needs the fixture contract's approval.
            self.icu_version = "79.2.1"
            self.icu_report = "79.2.1"
            self.write_uvernum(uvernum, "79.2.1")
            with self.assertRaisesRegex(
                workspace.BazelWorkspaceError, "ICU 79.2.1 does not satisfy dependency-contract.json"
            ):
                workspace._catalog_sdk(root / "contract-drift", None, "linux")
            with self.contract("79.2.1"):
                tools, inputs, evidence = workspace._catalog_sdk(root / "upgraded", None, "linux")
            self.assertIn(uvernum, evidence)
            # Only two- or three-component numeric ICU releases are producer formats.
            self.icu_version = "78.3.1.2"
            self.icu_report = "78.3.1.2"
            self.write_uvernum(uvernum, "78.3.1.2")
            with self.contract("78.3.1.2"):
                with self.assertRaisesRegex(workspace.BazelWorkspaceError, "unsupported ICU provider version"):
                    workspace._catalog_sdk(root / "bad-format", None, "linux")
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
                prefix = base / "installed" / ("x64-windows-static-md" if host == "windows" else "triplet")
                tool_bin = prefix / "tools/icu/bin"
                tool_bin.mkdir(parents=True)
                for name in ("genrb", "pkgdata"):
                    (tool_bin / (name + (".exe" if host == "windows" else ""))).write_bytes(b"installed tool fixture")
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
                        f"{prefix.name}/tools/icu/bin/genrb.exe\n{prefix.name}/tools/icu/bin/pkgdata.exe\n",
                        encoding="utf-8",
                    )
                with (
                    patch.dict(os.environ, environment),
                    self.python_sdk(host, base) as (executable, extension, native_runtime),
                    patch.object(subprocess, "run", side_effect=self.genrb_version),
                    self.contract(self.icu_version),
                ):
                    tools, inputs, evidence = workspace._catalog_sdk(base / "resolution", prefix, host)
                    self.assertEqual(self.genrb_queries[-1], [str(self.genrb), "-V"])
                    self.assertEqual(Path(tools["genrb"]).parent, tool_bin)
                    self.assertIn(executable, inputs)
                    self.assertIn(native_runtime, inputs)
                    self.assertIn(extension, inputs)
                    self.assertIn(uvernum, evidence)
                    self.assertIn(workspace.PROJECT_ROOT / "dependency-contract.json", evidence)
                    if host == "windows":
                        self.assertEqual(tools["SystemRoot"], str(base / "windows"))
                        self.assertEqual(tools["UserProfile"], str(base / "profile"))
                        self.assertIn(tool_bin / "genrb.exe", inputs)
                        self.assertIn(tool_bin / "pkgdata.exe", inputs)
                        self.assertFalse(
                            any(path.name.lower().startswith("icu") and path.suffix == ".dll" for path in inputs)
                        )
                        (tool_bin / "icutu78.dll").write_bytes(b"old shared SDK artifact")
                        with self.assertRaisesRegex(workspace.BazelWorkspaceError, "DLL-bearing Windows static ICU"):
                            workspace._catalog_sdk(base / "old-shared-runtime", prefix, host)
                        (tool_bin / "icutu78.dll").unlink()
                        ownership.write_text(f"{prefix.name}/tools/icu/bin/genrb.exe\n", encoding="utf-8")
                        with self.assertRaisesRegex(workspace.BazelWorkspaceError, "tools manifest"):
                            workspace._catalog_sdk(base / "unowned-pkgdata", prefix, host)
                        ownership.write_text(
                            f"{prefix.name}/tools/icu/bin/genrb.exe\n{prefix.name}/tools/icu/bin/pkgdata.exe\n",
                            encoding="utf-8",
                        )
                    # A changed raw port revision with the same upstream stays structurally valid.
                    status.write_text(status_text("2"), encoding="utf-8")
                    tools, _, _ = workspace._catalog_sdk(base / "revised-port", prefix, host)
                    self.assertEqual(Path(tools["genrb"]).parent, tool_bin)

    def test_windows_sdk_admission_follows_the_contract_and_raw_features(self):
        base = self.root / "admission"
        prefix = base / "installed/x64-windows-static-md"
        tool_bin = prefix / "tools/icu/bin"
        tool_bin.mkdir(parents=True)
        for name in ("genrb", "pkgdata"):
            (tool_bin / (name + ".exe")).write_bytes(b"installed tool fixture")
        self.icu_version = "79.1"
        self.icu_report = "79.1"
        status = prefix.parent / "vcpkg/status"
        status.parent.mkdir()
        status.write_text(
            "Package: icu\nVersion: 79.1\nPort-Version: 2\nArchitecture: x64-windows-static-md\n"
            "Status: install ok installed\n\nPackage: icu\nFeature: tools\n"
            "Architecture: x64-windows-static-md\nStatus: install ok installed\n",
            encoding="utf-8",
        )
        uvernum = prefix / "include/unicode/uvernum.h"
        uvernum.parent.mkdir(parents=True)
        self.write_uvernum(uvernum, self.icu_version)
        self.genrb = tool_bin / "genrb.exe"
        info = prefix.parent / "vcpkg/info"
        info.mkdir()
        ownership = info / "icu_79.1_x64-windows-static-md.list"
        ownership.write_text(
            "x64-windows-static-md/tools/icu/bin/genrb.exe\nx64-windows-static-md/tools/icu/bin/pkgdata.exe\n",
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
                workspace.BazelWorkspaceError, "ICU 79.1 does not satisfy dependency-contract.json"
            ):
                workspace._catalog_sdk(base / "contract-drift", prefix, "windows")
            with self.contract("79.1"):
                # An approved contract change plus a coherent next SDK passes structurally;
                # the catalog helper reads the contract, never a Bazel policy pin.
                tools, _, evidence = workspace._catalog_sdk(base / "resolution", prefix, "windows")
                self.assertEqual(self.genrb_queries[-1], [str(self.genrb), "-V"])
                self.assertEqual(tools["genrb"], str(tool_bin / "genrb.exe"))
                self.assertIn(uvernum, evidence)
                self.assertIn(ownership, evidence)
                self.assertIn(workspace.PROJECT_ROOT / "dependency-contract.json", evidence)
                # The installed header must match the actual provider observation.
                self.write_uvernum(uvernum, "78.3")
                with self.assertRaisesRegex(workspace.BazelWorkspaceError, "header version mismatch"):
                    workspace._catalog_sdk(base / "wrong-header", prefix, "windows")
                self.write_uvernum(uvernum, "79.1")
                # The actual tool report must match the provider version.
                self.icu_report = "78.3"
                with self.assertRaisesRegex(workspace.BazelWorkspaceError, "does not report ICU version 79.1"):
                    workspace._catalog_sdk(base / "wrong-tool", prefix, "windows")
                self.icu_report = "79.1"
            # Raw core/tools features remain required independently of contract admission.
            status.write_text(
                "Package: icu\nVersion: 79.1\nPort-Version: 2\nArchitecture: x64-windows-static-md\n"
                "Status: install ok installed\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(
                workspace.BazelWorkspaceError, "catalog tools require installed ICU core/tools features"
            ):
                workspace._catalog_sdk(base / "missing-tools-feature", prefix, "windows")

    def test_private_bus_tool_is_selected_from_reported_outputs_not_path(self):
        with tempfile.TemporaryDirectory(prefix="aobus-dbus-sdk-") as directory:
            root = Path(directory).resolve()
            provider = root / "dbus"
            daemon = provider / "bin/dbus-daemon"
            daemon.parent.mkdir(parents=True)
            daemon.write_bytes(b"private bus SDK fixture")
            report = root / "report.json"
            workspace._write_json(
                report,
                {
                    "schemaVersion": 1,
                    "nixpkgsRevision": workspace._json(workspace.PROJECT_ROOT / "nixpkgs.json")["rev"],
                },
            )
            with (
                patch.dict(os.environ, {"AOBUS_NIX_DEPENDENCY_REPORT": str(report), "PATH": ""}),
                patch.object(
                    workspace, "_nix_dependency", return_value=(provider, "fixture", "/nix/store/dbus.drv", [provider])
                ) as selected,
                patch.object(workspace, "_nix_root", return_value=provider),
            ):
                identity, executable, runtime, evidence = workspace._linux_dbus()
            self.assertEqual(selected.call_args.args[1], "dbus")
            self.assertEqual(executable, daemon)
            self.assertEqual(identity["outputs"], [str(provider)])
            self.assertEqual(runtime, [])
            self.assertIn(report, evidence)


class AppVersionProjectionTest(unittest.TestCase):
    def test_version_is_derived_from_the_sole_project_declaration(self):
        with tempfile.TemporaryDirectory(prefix="aobus-app-version-") as directory:
            root = Path(directory)
            path = root / "CMakeLists.txt"
            path.write_text("# project(aobus VERSION 9.9.9)\nproject(aobus\n VERSION 1.2.3)\n")
            self.assertEqual(bazel_resolver._app_version(root), "1.2.3")
            for text in (
                "",
                "project(other VERSION 1.2.3)",
                "project(aobus VERSION ${version})",
                "project(aobus VERSION 1.2.3)\nproject(aobus VERSION 1.2.4)",
            ):
                path.write_text(text)
                with self.subTest(text=text), self.assertRaisesRegex(ValueError, "project"):
                    bazel_resolver._app_version(root)


if __name__ == "__main__":
    unittest.main()
