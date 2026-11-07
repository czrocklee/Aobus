"""Protect explicit Core audio compilation, case ownership and support boundaries."""

import ast
import unittest

from test_bazel_query import rule_fields
from test_bazel_test_graph import (
    bazel_rule_source_branches,
    bazel_rule_sources,
    bazel_rule_tags,
    case_sources,
    cmake_platform_sources,
    cmake_target_sources,
    cmake_target_unconditional_sources,
    compilation_units,
)

from ao.core.paths import PROJECT_ROOT

FORMERLY_DEFERRED = {
    "test/audio/backend/PipeWireBackendTest.cpp",
    "test/audio/backend/CoreAudioProviderTest.cpp",
    "test/audio/backend/CoreAudioTestOutputTest.cpp",
    "test/audio/backend/WasapiProviderTest.cpp",
}


def fields_for(directory, kind, name):
    return next(
        fields
        for fields in rule_fields(PROJECT_ROOT / directory / "BUILD.bazel", kind)
        if ast.literal_eval(fields["name"]) == name
    )


def strings(node):
    return [child.value for child in ast.walk(node) if isinstance(child, ast.Constant) and isinstance(child.value, str)]


def selections(node):
    return [
        ast.literal_eval(child.args[0])
        for child in ast.walk(node)
        if isinstance(child, ast.Call) and isinstance(child.func, ast.Name) and child.func.id == "select"
    ]


def copts_names(copts):
    return {node.id for node in ast.walk(copts) if isinstance(node, ast.Name) and node.id != "select"}


class BazelAudioGraphTest(unittest.TestCase):
    def test_native_audio_observation_targets_do_not_reuse_cached_results(self):
        for name in ("audio_test", "audio_platform_test"):
            with self.subTest(target=name):
                fields = fields_for("test", "cc_test", name)
                self.assertEqual(ast.literal_eval(fields["tags"]), ["external"])

    def test_audio_compiles_every_cmake_unit_with_complete_platform_selection(self):
        cmake = PROJECT_ROOT / "lib/audio/CMakeLists.txt"
        self.assertEqual(
            compilation_units(bazel_rule_sources("lib/audio", "cc_library", "audio")),
            compilation_units(cmake_target_sources(cmake, "ao_audio")),
        )
        common = compilation_units(cmake_target_unconditional_sources(cmake, "ao_audio"))
        platform = {
            platform: compilation_units(sources)
            for platform, sources in cmake_platform_sources(cmake, "ao_audio").items()
        }
        branches = {
            platform: compilation_units(sources)
            for platform, sources in bazel_rule_source_branches("lib/audio", "cc_library", "audio").items()
        }
        self.assertEqual(branches["//conditions:default"], common)
        for name, sources in platform.items():
            with self.subTest(platform=name):
                self.assertEqual(branches[name], common | sources)
        self.assertEqual(set(branches) - {"//conditions:default"}, set(platform))

    def test_production_audio_keeps_static_core_and_private_codec_sdk_boundaries(self):
        fields = fields_for("lib/audio", "cc_library", "audio")
        self.assertIs(ast.literal_eval(fields["linkstatic"]), True)
        self.assertEqual(
            ast.literal_eval(fields["deps"]),
            ["//include:audio_headers", "//lib/async:async", "//lib/media:media", "//lib/utility:utility"],
        )
        private = set(strings(fields["implementation_deps"]))
        self.assertEqual(
            private,
            {"//lib/media:mpeg_decoder_headers", "//conditions:default", "@platforms//os:linux"}
            | {
                "@aobus_native_deps//:" + name
                for name in ("boost", "flac", "alac", "fdk_aac", "mpg123", "opus", "alsa", "pipewire", "udev")
            },
        )
        self.assertFalse(
            any(label.startswith(("//app", "//runtime", "//lib/library", "//lib/query")) for label in private)
        )
        self.assertEqual(
            selections(fields["linkopts"]),
            [
                {
                    "@platforms//os:windows": ["avrt.lib", "ole32.lib"],
                    "@platforms//os:macos": [
                        "-framework AudioToolbox",
                        "-framework CoreAudio",
                        "-framework CoreFoundation",
                    ],
                    "//conditions:default": [],
                }
            ],
        )

    def test_mpeg_decoder_friend_exposes_only_its_three_headers(self):
        fields = fields_for("lib/media", "cc_library", "mpeg_decoder_headers")
        self.assertEqual(
            ast.literal_eval(fields["hdrs"]), ["file/mpeg/ApeTag.h", "file/mpeg/Frame.h", "file/mpeg/FrameLayout.h"]
        )
        self.assertEqual(ast.literal_eval(fields["deps"]), ["//include:media_headers"])
        self.assertEqual(ast.literal_eval(fields["visibility"]), ["//lib/audio:__pkg__"])
        self.assertFalse({"testonly", "includes", "include_prefix", "strip_include_prefix"} & fields.keys())
        public = fields_for("include", "filegroup", "audio_public_headers")
        self.assertEqual(ast.unparse(public["srcs"]), "glob(['ao/audio/**/*.h'])")
        private = fields_for("lib/audio", "cc_library", "test_headers")
        self.assertIs(ast.literal_eval(private["testonly"]), True)
        self.assertEqual(
            set(ast.literal_eval(private["visibility"])),
            {"//test:__pkg__", "//app/windows-winui:__pkg__"},
        )
        self.assertEqual(ast.literal_eval(private["include_prefix"]), "lib/audio")

    def test_common_case_files_are_owned_once_including_fatal_concurrency(self):
        cmake = PROJECT_ROOT / "test/CMakeLists.txt"
        common = {
            source
            for source in case_sources(cmake_target_unconditional_sources(cmake, "ao_core_test"))
            if source.startswith("test/audio/")
        }
        platform = {
            source
            for sources in cmake_platform_sources(cmake, "ao_core_test").values()
            for source in case_sources(sources)
            if source.startswith("test/audio/")
        }
        backend = {
            source
            for source in case_sources(cmake_target_sources(cmake, "ao_backend_test"))
            if source.startswith("test/audio/")
        }
        self.assertTrue(common)
        self.assertEqual(case_sources(bazel_rule_sources("test", "cc_test", "audio_test")), common)
        self.assertFalse(common & platform)
        self.assertFalse(common & backend)
        self.assertTrue(
            {
                "test/audio/EngineFatalProbeTest.cpp",
                "test/audio/EngineConcurrencyTest.cpp",
                "test/audio/backend/detail/CallbackFenceTest.cpp",
            }
            <= common
        )

    def test_platform_cases_preserve_exact_core_ownership_and_leave_backend_to_cmake(self):
        cmake = PROJECT_ROOT / "test/CMakeLists.txt"
        expected_platform = {
            platform: {source for source in case_sources(sources) if source.startswith("test/audio/")}
            for platform, sources in cmake_platform_sources(cmake, "ao_core_test").items()
        }
        actual_platform = {
            platform: case_sources(sources)
            for platform, sources in bazel_rule_source_branches("test", "cc_test", "audio_platform_test").items()
            if platform != "//conditions:default"
        }
        expected_backend = {
            platform: case_sources(sources)
            for platform, sources in cmake_platform_sources(cmake, "ao_backend_test").items()
        }
        actual_backend = {
            platform: case_sources(sources)
            for platform, sources in bazel_rule_source_branches("test", "cc_test", "backend_test").items()
            if platform != "//conditions:default"
        }
        self.assertEqual(actual_platform, expected_platform)
        self.assertEqual(actual_backend, expected_backend)
        common = {
            source
            for source in case_sources(cmake_target_unconditional_sources(cmake, "ao_core_test"))
            if source.startswith("test/audio/")
        }
        all_cmake = {
            source
            for source in case_sources(cmake_target_sources(cmake, "ao_core_test"))
            if source.startswith("test/audio/")
        } | {
            source
            for source in case_sources(cmake_target_sources(cmake, "ao_backend_test"))
            if source.startswith("test/audio/")
        }
        platform_cases = set().union(*expected_platform.values())
        backend_cases = set().union(*expected_backend.values())
        self.assertEqual(common | platform_cases | backend_cases, all_cmake)
        self.assertFalse(platform_cases & backend_cases)
        self.assertLessEqual(FORMERLY_DEFERRED, platform_cases)
        self.assertLessEqual(FORMERLY_DEFERRED, set().union(*actual_platform.values()))
        self.assertFalse(FORMERLY_DEFERRED & backend_cases)
        fields = fields_for("test", "cc_test", "audio_platform_test")
        self.assertIn(":ao_coreaudio_output_probe", strings(fields["data"]))
        self.assertTrue({"manual", "external"} <= bazel_rule_tags(fields_for("test", "cc_test", "backend_test")))
        suite = fields_for(".", "test_suite", "audio_tests")
        self.assertEqual(ast.literal_eval(suite["tests"]), ["//test:audio_test", "//test:audio_platform_test"])

    def test_pipewire_header_requirements_are_independent_local_roles(self):
        required = {
            "@platforms//os:linux": ["-fno-strict-aliasing", "-fno-strict-overflow"],
            "//conditions:default": [],
        }
        audio_tree = ast.parse((PROJECT_ROOT / "lib/audio/BUILD.bazel").read_text(encoding="utf-8"))
        audio_assignments = {
            node.targets[0].id: node.value
            for node in audio_tree.body
            if isinstance(node, ast.Assign) and isinstance(node.targets[0], ast.Name)
        }
        self.assertEqual(selections(audio_assignments["PIPEWIRE_HEADER_COPTS"]), [required])
        fields = fields_for("lib/audio", "cc_library", "audio")
        self.assertIn("PIPEWIRE_HEADER_COPTS", copts_names(fields["copts"]))
        self.assertEqual(
            selections(fields["copts"]),
            [{"@platforms//os:windows": ["/Ilib/audio"], "//conditions:default": ["-iquotelib/audio"]}],
        )

        test_tree = ast.parse((PROJECT_ROOT / "test/BUILD.bazel").read_text(encoding="utf-8"))
        test_assignments = {
            node.targets[0].id: node.value
            for node in test_tree.body
            if isinstance(node, ast.Assign) and isinstance(node.targets[0], ast.Name)
        }
        warning_name = "TEST_WARNING_COPTS"
        self.assertEqual(
            selections(test_assignments[warning_name]),
            [{"@platforms//os:windows": [], "//conditions:default": ["-Wno-array-bounds"]}],
        )
        audio_copts = test_assignments["AUDIO_TEST_COPTS"]
        self.assertEqual(copts_names(audio_copts), {warning_name})
        self.assertEqual(selections(audio_copts), [required])
        cpu = fields_for("test", "config_setting", "gtk_x86_64_cpu")
        self.assertEqual(ast.literal_eval(cpu["constraint_values"]), ["@platforms//cpu:x86_64"])
        self.assertEqual(
            selections(test_assignments["GTK_HEADER_COPTS"]),
            [{":gtk_x86_64_cpu": ["-mfpmath=sse", "-msse", "-msse2"], "//conditions:default": []}],
        )

    def test_audio_copts_consumers_keep_independent_flag_roles(self):
        targets = {
            ast.literal_eval(fields["name"]): fields
            for kind in ("cc_library", "cc_test", "cc_binary")
            for fields in rule_fields(PROJECT_ROOT / "test/BUILD.bazel", kind)
        }
        consumers = {
            name: copts_names(fields["copts"])
            for name, fields in targets.items()
            if fields.get("copts") is not None and "AUDIO_TEST_COPTS" in copts_names(fields["copts"])
        }
        self.assertTrue(
            {
                "audio_engine_test_support",
                "audio_platform_test",
                "audio_test",
                "audio_test_support",
                "gtk_test",
                "gtk_test_support",
                "runtime_test",
                "uimodel_test",
            }
            <= set(consumers)
        )
        self.assertNotIn("utility_test", consumers)
        self.assertNotIn("ao_gapplication_probe", consumers)
        for name, names in consumers.items():
            with self.subTest(target=name):
                self.assertLessEqual(names, {"AUDIO_TEST_COPTS", "GTK_HEADER_COPTS"})
                if name in {"gtk_test", "gtk_test_support"}:
                    self.assertIn("GTK_HEADER_COPTS", names)
                else:
                    self.assertNotIn("GTK_HEADER_COPTS", names)
                copts = targets[name]["copts"]
                self.assertFalse(
                    any(
                        isinstance(node, ast.Constant) and isinstance(node.value, str) and node.value.startswith("-")
                        for node in ast.walk(copts)
                    )
                )

    def test_selected_linux_sdk_dependencies_remain_mandatory(self):
        for directory, name, keys in (
            ("lib/audio", "audio", {"alsa", "pipewire", "udev"}),
            ("test", "audio_platform_test_support", {"alsa", "pipewire"}),
            ("test", "audio_platform_test", {"alsa", "pipewire"}),
        ):
            kind = "cc_test" if name == "audio_platform_test" else "cc_library"
            fields = fields_for(directory, kind, name)
            with self.subTest(target=name):
                dependency_field = "implementation_deps" if name == "audio" else "deps"
                branches = selections(fields[dependency_field])
                self.assertEqual(len(branches), 1)
                self.assertEqual(
                    set(branches[0]["@platforms//os:linux"]), {"@aobus_native_deps//:" + key for key in keys}
                )
                self.assertEqual(branches[0]["//conditions:default"], [])

    def test_fixture_object_has_one_shared_owner_without_audio_dependency(self):
        owners = [
            ast.literal_eval(fields["name"])
            for fields in rule_fields(PROJECT_ROOT / "test/BUILD.bazel", "cc_library")
            if "srcs" in fields and "audio/AudioFixtureSupport.cpp" in strings(fields["srcs"])
        ]
        self.assertEqual(owners, ["audio_fixture_support"])
        fields = fields_for("test", "cc_library", "audio_fixture_support")
        self.assertEqual(
            ast.literal_eval(fields["hdrs"]), ["audio/AudioFixtureDirectory.h", "audio/AudioFixtureSupport.h"]
        )
        self.assertEqual(ast.literal_eval(fields["local_defines"]), ["AOBUS_BAZEL_AUDIO_FIXTURES"])
        for name in ("media_test_support", "audio_test_support"):
            self.assertIn(":audio_fixture_support", ast.literal_eval(fields_for("test", "cc_library", name)["deps"]))
        data = fields_for("test/data/audio", "filegroup", "audio_fixtures")
        patterns = ast.literal_eval(data["srcs"].args[0])
        files = {
            path for pattern in patterns for path in (PROJECT_ROOT / "test/data/audio").glob(pattern) if path.is_file()
        }
        self.assertTrue(files)
        self.assertEqual(
            files,
            {
                path
                for path in (PROJECT_ROOT / "test/data/audio").rglob("*")
                if path.is_file() and path.suffix in {".wav", ".flac", ".m4a", ".mp3", ".opus"}
            },
        )

    def test_direct_fixture_consumers_use_only_the_narrow_directory_contract(self):
        for name in ("DecoderIntegrationTest.cpp", "GraphIntegrityTest.cpp"):
            source = (PROJECT_ROOT / "test/audio" / name).read_text(encoding="utf-8")
            with self.subTest(source=name):
                self.assertIn('#include "test/audio/AudioFixtureDirectory.h"', source)
                self.assertIn("audioFixtureDirectory()", source)
                self.assertNotIn("AUDIO_TEST_DATA_DIR", source)
        decoder = (PROJECT_ROOT / "test/audio/DecoderIntegrationTest.cpp").read_text(encoding="utf-8")
        # Its local requireAudioFixture/readFileBytes must not acquire competing
        # declarations from the broader assertion-support header.
        self.assertNotIn("AudioFixtureSupport.h", decoder)
        fields = fields_for("test", "cc_library", "audio_fixture_support")
        self.assertIn("audio/AudioFixtureDirectory.h", ast.literal_eval(fields["hdrs"]))
        header = (PROJECT_ROOT / "test/audio/AudioFixtureDirectory.h").read_text(encoding="utf-8")
        self.assertIn("std::filesystem::path audioFixtureDirectory();", header)
        self.assertNotIn("requireAudioFixture", header)
        self.assertNotIn("readFileBytes", header)

    def test_fatal_child_is_declared_data_and_has_no_catch_or_application_graph(self):
        fields = fields_for("test", "cc_binary", "ao_audio_fatal_probe")
        self.assertEqual(
            set(strings(fields["srcs"])),
            {
                "fatal/AudioFatalProbeMain.cpp",
                "fatal/AudioFatalProbeScenario.cpp",
                "fatal/AudioFatalProbeScenarioMacos.cpp",
                "@platforms//os:macos",
                "//conditions:default",
            },
        )
        self.assertEqual(
            selections(fields["srcs"]),
            [{"@platforms//os:macos": ["fatal/AudioFatalProbeScenarioMacos.cpp"], "//conditions:default": []}],
        )
        for name in ("audio_test", "audio_platform_test"):
            fields = fields_for("test", "cc_test", name)
            self.assertIn(":ao_audio_fatal_probe", strings(fields["data"]))
        engine = fields_for("test", "cc_library", "audio_engine_test_support")
        self.assertEqual(
            bazel_rule_sources("test", "cc_library", "audio_engine_test_support"),
            {
                "test/audio/EngineTestSupport.cpp",
                "test/audio/FakeCapturingBackend.cpp",
                "test/audio/ScriptedDecoderSession.cpp",
            },
        )
        self.assertNotIn("@aobus_test_deps//:catch2", strings(engine["deps"]))
        support = fields_for("test", "cc_library", "fatal_probe_support")
        self.assertEqual(
            compilation_units(bazel_rule_sources("test", "cc_library", "fatal_probe_support")),
            compilation_units(cmake_target_sources(PROJECT_ROOT / "test/CMakeLists.txt", "ao_fatal_probe_support")),
        )
        self.assertFalse(
            any(
                source.endswith("Scenario.cpp")
                for source in bazel_rule_sources("test", "cc_library", "fatal_probe_support")
            )
        )
        self.assertEqual(ast.literal_eval(support["deps"]), ["//bazel:compile_contract"])
        for name, kind in (
            ("ao_audio_fatal_probe", "cc_binary"),
            ("audio_engine_test_support", "cc_library"),
            ("fatal_probe_support", "cc_library"),
        ):
            fields = fields_for("test", kind, name)
            self.assertFalse(
                any(
                    label.startswith(("//app", "//runtime"))
                    or label in {":library_test_support", ":audio_test_support"}
                    for label in strings(fields.get("deps", ast.List(elts=[])))
                )
            )
        library = fields_for("test", "cc_library", "library_test_support")
        self.assertIn(":fatal_probe_support", strings(library["deps"]))
        self.assertFalse(any("ProbeProcess" in source for source in strings(library["srcs"])))

    def test_executables_own_utf8_manifests_and_keep_contract_dependencies(self):
        for name, kind in (
            ("audio_test", "cc_test"),
            ("audio_platform_test", "cc_test"),
            ("ao_audio_fatal_probe", "cc_binary"),
        ):
            fields = fields_for("test", kind, name)
            with self.subTest(target=name):
                self.assertEqual(ast.unparse(fields["additional_linker_inputs"]), "WINDOWS_UTF8_LINKER_INPUTS")
                self.assertEqual(ast.unparse(fields["linkopts"]), "WINDOWS_UTF8_LINKOPTS")
                self.assertFalse({"hdrs", "include_prefix", "strip_include_prefix", "includes"} & fields.keys())
        for name in ("audio_test", "audio_platform_test"):
            fields = fields_for("test", "cc_test", name)
            self.assertEqual(ast.unparse(fields["copts"]), "AUDIO_TEST_COPTS")
            self.assertIn("@aobus_native_deps//:gsl_lite", strings(fields["deps"]))
            self.assertIn("@aobus_test_deps//:fakeit", strings(fields["deps"]))
        audio_headers = fields_for("include", "cc_library", "audio_headers")
        self.assertEqual(ast.literal_eval(audio_headers["deps"]), [":utility_headers"])
        utility_headers = fields_for("include", "cc_library", "utility_headers")
        self.assertEqual(ast.literal_eval(utility_headers["deps"]), ["//bazel:compile_contract"])
        contract = fields_for("bazel", "cc_library", "compile_contract")
        self.assertNotIn("deps", contract)
        flags = (PROJECT_ROOT / ".bazelrc").read_text(encoding="utf-8").splitlines()
        self.assertFalse(any("--cxxopt=" in line and "gsl_CONFIG_" in line for line in flags))

    def test_static_windows_audio_keeps_fixtures_and_probes_without_ogg_staging(self):
        for name in ("audio_test", "audio_platform_test"):
            data = fields_for("test", "cc_test", name)["data"]
            with self.subTest(target=name):
                self.assertIn(":ao_audio_fatal_probe", strings(data))
                self.assertIn("//test/data/audio:audio_fixtures", strings(data))
        for kind in ("cc_test", "cc_binary", "genrule"):
            for fields in rule_fields(PROJECT_ROOT / "test/BUILD.bazel", kind):
                with self.subTest(kind=kind, target=ast.literal_eval(fields["name"])):
                    values = strings(ast.List(elts=list(fields.values())))
                    self.assertFalse(
                        {
                            "audio_ogg_runtime",
                            ":audio_ogg_runtime",
                            "ogg.dll",
                            "@aobus_native_deps//:flac_runtime_extra",
                        }
                        & set(values)
                    )


if __name__ == "__main__":
    unittest.main()
