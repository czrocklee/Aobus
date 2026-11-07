"""Declared real CLI/TUI graph parity, not native compilation or loader evidence."""

import ast
import json
import re
import unittest

from test_bazel_library import cmake_sources
from test_bazel_query import rule_fields
from test_bazel_test_graph import bazel_rule_sources, cmake_target_sources, compilation_units
from test_bazel_uimodel import fields_for

from ao.core.paths import PROJECT_ROOT


def string_literals(expression):
    return [
        node.value for node in ast.walk(expression) if isinstance(node, ast.Constant) and isinstance(node.value, str)
    ]


def cpp_literals(expression):
    return [value for value in string_literals(expression) if value.endswith(".cpp")]


def product_dependencies(expression, platform):
    if isinstance(expression, ast.List):
        return ast.literal_eval(expression)
    if isinstance(expression, ast.BinOp) and isinstance(expression.op, ast.Add):
        return product_dependencies(expression.left, platform) + product_dependencies(expression.right, platform)
    if isinstance(expression, ast.Call) and isinstance(expression.func, ast.Name) and expression.func.id == "select":
        choices = ast.literal_eval(expression.args[0])
        return choices.get("@platforms//os:" + platform, choices["//conditions:default"])
    raise AssertionError("Unexpected product dependency expression: " + ast.unparse(expression))


def cmake_executable_sources(directory, target):
    text = (PROJECT_ROOT / directory / "CMakeLists.txt").read_text(encoding="utf-8")
    blocks = re.findall(
        rf"(?:add_executable|aobus_target_test_sources)\(\s*{re.escape(target)}\b([^)]*)\)", text, re.DOTALL
    )
    if not blocks:
        raise AssertionError(f"No CMake executable sources for {target}")
    return set(re.findall(r"[\w./]+\.cpp\b", "\n".join(blocks)))


class BazelFrontendGraphTest(unittest.TestCase):
    def test_windows_cmake_presets_separate_static_targets_from_host_tools(self):
        presets = json.loads((PROJECT_ROOT / "CMakePresets.json").read_text(encoding="utf-8"))
        configured = {preset["name"]: preset for preset in presets["configurePresets"]}
        for name in ("windows-base", "windows-winui"):
            with self.subTest(preset=name):
                variables = configured[name]["cacheVariables"]
                self.assertEqual(variables["VCPKG_TARGET_TRIPLET"], "x64-windows-static-md")
                self.assertEqual(variables["VCPKG_HOST_TRIPLET"], "x64-windows")
                self.assertNotIn("CMAKE_MSVC_RUNTIME_LIBRARY", variables)
                self.assertFalse(any(key.startswith(("CMAKE_C_FLAGS", "CMAKE_CXX_FLAGS")) for key in variables))
        for name in ("windows-debug", "windows-release"):
            self.assertEqual(configured[name]["inherits"], "windows-base")
        self.assertEqual(configured["windows-tidy"]["inherits"], "windows-release")
        dependencies = (PROJECT_ROOT / "cmake/Dependencies.cmake").read_text(encoding="utf-8")
        self.assertRegex(
            dependencies,
            r"(?s)if\(WIN32\)\s+set\(AOBUS_MIMALLOC_TARGET mimalloc-static\)\s+else\(\)\s+"
            r"set\(AOBUS_MIMALLOC_TARGET mimalloc\)\s+endif\(\)",
        )
        for directory in ("app/cli", "app/tui"):
            cmake = (PROJECT_ROOT / directory / "CMakeLists.txt").read_text(encoding="utf-8")
            self.assertIn("${AOBUS_MIMALLOC_TARGET}", cmake)

    def test_cli_and_tui_compile_the_complete_cmake_product_membership(self):
        for directory, library, cmake_library, binary in (
            ("app/cli", "cli", "aobus-cli-lib", "aobus"),
            ("app/tui", "tui", "aobus-tui-lib", "aobus-tui"),
        ):
            with self.subTest(product=binary):
                fields = fields_for(directory, "cc_library", library)
                sources = cpp_literals(fields["srcs"])
                if library == "tui":
                    cover = fields_for(directory, "cc_library", "cover_art")
                    sources += cpp_literals(cover["srcs"])
                self.assertCountEqual(sources, cmake_sources(directory, cmake_library))
                product = fields_for(directory, "cc_binary", binary)
                self.assertCountEqual(cpp_literals(product["srcs"]), cmake_executable_sources(directory, binary))
                self.assertIs(ast.literal_eval(fields["linkstatic"]), True)
                self.assertIs(ast.literal_eval(product["linkstatic"]), True)
                for platform in ("linux", "macos", "windows"):
                    self.assertIn("@aobus_native_deps//:mimalloc", product_dependencies(product["deps"], platform))

    def test_windows_products_keep_executable_cpp_allocation_operators_with_static_mimalloc(self):
        allocator = "@aobus_native_deps//:mimalloc"
        for directory, binary, entry in (("app/cli", "aobus", "main.cpp"), ("app/tui", "aobus-tui", "Main.cpp")):
            expression = fields_for(directory, "cc_binary", binary)["deps"]
            windows = product_dependencies(expression, "windows")
            macos = product_dependencies(expression, "macos")
            linux = product_dependencies(expression, "linux")
            with self.subTest(binary=binary):
                self.assertEqual(windows.count(allocator), 1)
                self.assertCountEqual(windows, macos)
                self.assertEqual(linux, macos + (["//app/systemmedia:linux"] if directory == "app/tui" else []))
                source = (PROJECT_ROOT / directory / entry).read_text(encoding="utf-8")
                self.assertEqual(source.count("#include <mimalloc-new-delete.h>"), 1)
                self.assertRegex(source, r"(?s)#ifdef _MSC_VER\b.*?#include <mimalloc-new-delete.h>.*?#endif")

    def test_runtime_directly_owns_all_cmake_sources_and_library_headers(self):
        runtime = fields_for("app/runtime", "cc_library", "runtime")
        sources = cpp_literals(runtime["srcs"])
        expected = cmake_sources("app/runtime", "ao_app_runtime")
        self.assertCountEqual(sources, expected)
        self.assertFalse((PROJECT_ROOT / "app/runtime/library/BUILD.bazel").exists())
        prepared = fields_for("app/runtime", "cc_library", "prepared_next_contract")
        self.assertEqual(cpp_literals(prepared["srcs"]), ["playback/PreparedNextContract.cpp"])
        self.assertNotIn("playback/PreparedNextContract.cpp", sources)
        private = fields_for("app/runtime", "cc_library", "private_headers")
        headers = private["hdrs"]
        self.assertEqual(headers.func.id, "glob")
        included = (
            headers.args[0]
            if headers.args
            else next(keyword.value for keyword in headers.keywords if keyword.arg == "include")
        )
        self.assertCountEqual(ast.literal_eval(included), ["**/*.h"])
        excluded = next(keyword.value for keyword in headers.keywords if keyword.arg == "exclude")
        self.assertCountEqual(ast.literal_eval(excluded), ["playback/PreparedNextContract.h"])
        self.assertIn(":prepared_next_contract", ast.literal_eval(runtime["implementation_deps"]))

    def test_full_uimodel_owns_all_cmake_sources_once_and_keeps_macos_timezone_leaf(self):
        model = fields_for("app/uimodel", "cc_library", "uimodel")
        sources = cpp_literals(model["srcs"])
        self.assertCountEqual(sources, cmake_sources("app/uimodel", "ao_app_uimodel"))
        self.assertCountEqual(
            ast.literal_eval(model["deps"]),
            ["//app/include:uimodel_headers", "//app/runtime:runtime"],
        )
        self.assertEqual(ast.literal_eval(model["implementation_deps"].left), ["//app/i18n:i18n"])
        self.assertEqual(
            ast.literal_eval(model["implementation_deps"].right.args[0]),
            {
                "@platforms//os:macos": ["@aobus_native_deps//:date_tz"],
                "//conditions:default": [],
            },
        )
        imports = json.loads((PROJECT_ROOT / "config/bazel/dependencies.json").read_text(encoding="utf-8"))
        timezone = imports["library_fallbacks"]["date_tz"]["macos"]
        self.assertEqual(timezone["libraries"], ["date-tz"])
        self.assertIn("USE_OS_TZDB=1", timezone["defines"])

    def test_cover_art_configuration_is_source_local_and_native_sources_are_selected(self):
        cover = fields_for("app/tui", "cc_library", "cover_art")
        tui = fields_for("app/tui", "cc_library", "tui")
        self.assertCountEqual(ast.literal_eval(cover["srcs"]), ["CoverArt.cpp", "CoverArt.h"])
        self.assertEqual(
            ast.literal_eval(cover["local_defines"]), ["STBI_MAX_DIMENSIONS=::ao::tui::kMaximumCoverArtDimension"]
        )
        self.assertNotIn("local_defines", tui)
        self.assertNotIn("CoverArt.cpp", cpp_literals(tui["srcs"]))
        branches = ast.literal_eval(tui["srcs"].right.args[0])
        expected = {
            "@platforms//os:windows": ["SignalExitWatcherWindows.cpp", "TerminalCellMetricsWindows.cpp"],
            "//conditions:default": ["SignalExitWatcherPosix.cpp", "TerminalCellMetricsPosix.cpp"],
        }
        self.assertCountEqual(branches, expected)
        for platform, sources in expected.items():
            with self.subTest(platform=platform):
                self.assertCountEqual(branches[platform], sources)
        i18n = fields_for("app/i18n", "cc_library", "i18n")
        self.assertEqual(
            ast.literal_eval(i18n["srcs"].right.args[0]),
            {
                "@platforms//os:windows": ["SystemLocaleWindows.cpp"],
                "@platforms//os:macos": ["SystemLocaleMacos.cpp"],
                "//conditions:default": ["SystemLocaleLinux.cpp"],
            },
        )
        self.assertEqual(
            ast.literal_eval(i18n["linkopts"].args[0]),
            {
                "@platforms//os:macos": ["-framework CoreFoundation"],
                "//conditions:default": [],
            },
        )

    def test_public_tui_headers_share_one_tree_without_a_second_physical_include_root(self):
        cover = fields_for("app/tui", "cc_library", "cover_art")
        tui = fields_for("app/tui", "cc_library", "tui")
        for attribute in ("hdrs", "includes", "include_prefix", "strip_include_prefix"):
            self.assertNotIn(attribute, cover)
        self.assertIn("CoverArt.h", ast.literal_eval(cover["srcs"]))
        self.assertEqual(ast.literal_eval(tui["include_prefix"]), "tui")
        self.assertEqual(ast.literal_eval(tui["strip_include_prefix"]), ".")
        headers = tui["hdrs"]
        self.assertEqual(headers.func.id, "glob")
        included = (
            headers.args[0]
            if headers.args
            else next(keyword.value for keyword in headers.keywords if keyword.arg == "include")
        )
        self.assertCountEqual(ast.literal_eval(included), ["*.h"])
        excluded = next(keyword.value for keyword in headers.keywords if keyword.arg == "exclude")
        self.assertCountEqual(ast.literal_eval(excluded), ["App.h"])
        self.assertNotIn("includes", tui)

    def test_declared_cli_closure_uses_icu_uc_without_interactive_catalogs(self):
        # Traverse only declaration deps, including every select branch. This is
        # not an assertion about a configured link line or native shared objects.
        pending = ["//app/cli:aobus"]
        visited = set()
        external = set()
        while pending:
            label = pending.pop()
            if label in visited:
                continue
            visited.add(label)
            directory, name = label[2:].split(":", 1)
            path = PROJECT_ROOT / directory / "BUILD.bazel"
            candidates = rule_fields(path, "cc_library") + rule_fields(path, "cc_binary")
            fields = next(item for item in candidates if ast.literal_eval(item["name"]) == name)
            for attribute in ("deps", "implementation_deps"):
                if attribute not in fields:
                    continue
                for node in ast.walk(fields[attribute]):
                    if not isinstance(node, ast.Constant) or not isinstance(node.value, str):
                        continue
                    dependency = node.value
                    if dependency.startswith("@"):
                        external.add(dependency)
                    elif dependency.startswith(":"):
                        pending.append("//" + directory + dependency)
                    elif dependency.startswith("//") and dependency != "//conditions:default":
                        pending.append(dependency)
        self.assertIn("//app/runtime:runtime", visited)
        self.assertIn("//lib/utility:unicode_text", visited)
        self.assertIn("@aobus_native_deps//:icu_uc", external)
        self.assertNotIn("@aobus_native_deps//:icu", external)
        self.assertFalse(
            any(label.startswith(("//app/i18n:", "//app/uimodel:", "//tool/catalog:")) for label in visited)
        )

    def test_runtime_and_public_logging_headers_require_compiled_std_format_spdlog(self):
        for directory, name in (("app/runtime", "runtime"), ("app/include", "runtime_headers")):
            fields = fields_for(directory, "cc_library", name)
            self.assertIn("@aobus_native_deps//:spdlog", ast.literal_eval(fields["deps"]))
        declarations = json.loads((PROJECT_ROOT / "config/bazel/dependencies.json").read_text(encoding="utf-8"))
        package = next(item for item in declarations["packages"]["native"] if item["name"] == "spdlog")
        self.assertEqual(package["modules"], ["spdlog"])
        for host in ("macos", "windows"):
            interface = declarations["library_fallbacks"]["spdlog"][host]
            self.assertEqual(interface["libraries"], ["spdlog"])
            self.assertTrue({"SPDLOG_COMPILED_LIB", "SPDLOG_USE_STD_FORMAT"} <= set(interface["defines"]))
            self.assertFalse({"SPDLOG_HEADER_ONLY", "SPDLOG_FMT_EXTERNAL"} & set(interface["defines"]))
        # Actual ABI acceptance/rejection belongs to FrontendProjectionTest in
        # test_bazel_frontend_deps.py, not a source-spelling check of its guard.

    def test_real_tui_composition_selects_linux_mpris_and_interactive_leaves(self):
        product = fields_for("app/tui", "cc_binary", "aobus-tui")
        self.assertEqual(
            ast.literal_eval(product["local_defines"].args[0]),
            {
                "@platforms//os:linux": ["AOBUS_HAS_SYSTEM_MEDIA=1"],
                "//conditions:default": [],
            },
        )
        self.assertEqual(
            ast.literal_eval(product["deps"].right.args[0]),
            {
                "@platforms//os:linux": ["//app/systemmedia:linux"],
                "//conditions:default": [],
            },
        )
        self.assertTrue(
            {
                "//app/i18n:completion",
                "//app/i18n:ordering",
                "//app/i18n:i18n",
                "//app/runtime:runtime",
                "//app/uimodel:uimodel",
                "//lib/audio:audio",
            }
            <= set(product_dependencies(product["deps"], "linux"))
        )
        mpris = fields_for("app/systemmedia", "cc_library", "linux")
        self.assertCountEqual(cpp_literals(mpris["srcs"]), cmake_sources("app/systemmedia", "ao_system_media_linux"))
        self.assertEqual(ast.literal_eval(mpris["target_compatible_with"]), ["@platforms//os:linux"])
        self.assertIn("@aobus_native_deps//:giomm", ast.literal_eval(mpris["deps"]))

    def test_frontend_tests_reuse_exact_cmake_cases_and_their_existing_support(self):
        cmake = PROJECT_ROOT / "test/CMakeLists.txt"
        for frontend, target, cmake_target in (
            ("cli", "cli_test", "ao_cli_test"),
            ("tui", "tui_test", "ao_tui_test"),
        ):
            with self.subTest(frontend=frontend):
                test = fields_for("test", "cc_test", target)
                self.assertEqual(
                    compilation_units(bazel_rule_sources("test", "cc_test", target))
                    | compilation_units(bazel_rule_sources("test", "cc_library", frontend + "_test_support")),
                    compilation_units(cmake_target_sources(cmake, cmake_target)),
                )
                self.assertIn("//test/data/audio:audio_fixtures", ast.literal_eval(test["data"]))
                self.assertIn("@aobus_test_deps//:catch2", ast.literal_eval(test["deps"]))
        runtime = fields_for("test", "cc_library", "runtime_test_support")
        self.assertIn(":async_test_support", ast.literal_eval(runtime["deps"]))
        expected = compilation_units(cmake_target_sources(cmake, "ao_test_support_runtime")) - {
            "test/runtime/AsyncTestSupport.cpp",
            "test/runtime/ExecutorTestSupport.cpp",
        }
        self.assertEqual(compilation_units(bazel_rule_sources("test", "cc_library", "runtime_test_support")), expected)

    def test_fixture_consumers_use_runfiles_owner_not_absolute_build_paths(self):
        fixture = fields_for("test", "cc_library", "audio_fixture_support")
        self.assertEqual(ast.literal_eval(fixture["local_defines"]), ["AOBUS_BAZEL_AUDIO_FIXTURES"])
        self.assertIn(":common_test_support", ast.literal_eval(fixture["deps"]))
        cli = fields_for("test", "cc_library", "cli_test_support")
        self.assertIn(":audio_fixture_support", ast.literal_eval(cli["deps"]))
        self.assertNotIn("local_defines", cli)
        source = (PROJECT_ROOT / "test/cli/CliTestSupport.cpp").read_text(encoding="utf-8")
        self.assertIn("audio::test::audioFixtureDirectory() / sourceName", source)
        self.assertNotIn("AUDIO_TEST_DATA_DIR", source)
        self.assertNotIn("/home/", source)

    def test_systemmedia_test_preserves_real_bus_and_pty_contract(self):
        process = fields_for("test", "cc_test", "systemmedia_test")
        cmake = PROJECT_ROOT / "test/CMakeLists.txt"
        self.assertEqual(
            compilation_units(bazel_rule_sources("test", "cc_test", "systemmedia_test")),
            compilation_units(cmake_target_sources(cmake, "ao_systemmedia_test")),
        )
        self.assertIn("//app/tui:aobus-tui", ast.literal_eval(process["data"]))
        self.assertIn("//test/data/audio:audio_fixtures", ast.literal_eval(process["data"]))
        self.assertIn("@aobus_native_tools//:dbus_daemon", ast.literal_eval(process["data"]))
        self.assertEqual(ast.literal_eval(process["target_compatible_with"]), ["@platforms//os:linux"])
        self.assertEqual(ast.literal_eval(process["local_defines"]), ["AOBUS_BAZEL_TUI_EXECUTABLE"])
        self.assertEqual(ast.literal_eval(process["tags"]), ["external"])
        self.assertIn(":common_test_support", string_literals(process["deps"]))
        environment = compile(ast.Expression(process["env"]), "test/BUILD.bazel", "eval")
        for tools, expected in (
            ({"dbus_daemon": "/fixture/tools/dbus-daemon"}, "/fixture/tools/dbus-daemon"),
            ({}, ""),
        ):
            with self.subTest(tools=tools):
                self.assertEqual(
                    eval(environment, {"TEST_TOOLS": tools}),
                    {"AOBUS_TEST_DBUS_DAEMON": expected},
                )
        support = (PROJECT_ROOT / "test/systemmedia/linux/MprisTestSupport.cpp").read_text(encoding="utf-8")
        self.assertIn('std::getenv("AOBUS_TEST_DBUS_DAEMON")', support)
        self.assertIn("!std::filesystem::path{daemon}.is_absolute()", support)
        self.assertIn('auto const* daemon = "dbus-daemon";', support)
        source = (PROJECT_ROOT / "test/systemmedia/linux/TuiMprisProcessTest.cpp").read_text(encoding="utf-8")
        self.assertIn("#ifdef AOBUS_BAZEL_TUI_EXECUTABLE", source)
        self.assertIn('ao::test::resolveBazelTestRunfile("app/tui/aobus-tui")', source)
        self.assertIn("std::filesystem::absolute(path).string()", source)
        self.assertIn("return AOBUS_TUI_EXECUTABLE;", source)

    def test_static_windows_products_and_tests_keep_manifests_without_dependency_dll_staging(self):
        for directory, kind, name in (
            ("app/cli", "cc_binary", "aobus"),
            ("app/tui", "cc_binary", "aobus-tui"),
            ("test", "cc_test", "media_fixture_test"),
            ("test", "cc_test", "uimodel_test"),
            ("test", "cc_test", "cli_test"),
            ("test", "cc_test", "tui_test"),
            ("test", "cc_binary", "ao_tui_signal_probe"),
        ):
            with self.subTest(binary=name):
                fields = fields_for(directory, kind, name)
                data = product_dependencies(fields.get("data", ast.List(elts=[])), "windows")
                self.assertFalse({":ogg_runtime", ":audio_ogg_runtime", ":mimalloc_redirect_runtime"} & set(data))
                for attribute, symbol in (
                    ("additional_linker_inputs", "WINDOWS_UTF8_LINKER_INPUTS"),
                    ("linkopts", "WINDOWS_UTF8_LINKOPTS"),
                ):
                    self.assertIsInstance(fields[attribute], ast.Name)
                    self.assertEqual(fields[attribute].id, symbol)
        for directory in ("app/cli", "app/tui", "test"):
            build = (PROJECT_ROOT / directory / "BUILD.bazel").read_text(encoding="utf-8")
            with self.subTest(directory=directory):
                self.assertNotIn("flac_runtime_extra", build)
                self.assertNotIn("mimalloc_runtime_extra", build)
                self.assertNotIn("ogg.dll", build)
                self.assertNotIn("mimalloc-redirect.dll", build)


if __name__ == "__main__":
    unittest.main()
