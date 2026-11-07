"""Keep encoded-media compilation, dispatch generation, cases and data explicit."""

import ast
import re
import unittest

from test_bazel_library import bazel_sources, cmake_sources
from test_bazel_query import rule_fields

from ao.core.paths import PROJECT_ROOT


def fields_for(directory, kind, name):
    return next(
        fields
        for fields in rule_fields(PROJECT_ROOT / directory / "BUILD.bazel", kind)
        if ast.literal_eval(fields["name"]) == name
    )


class BazelMediaGraphTest(unittest.TestCase):
    def test_media_compiles_all_cmake_owned_implementation_units(self):
        self.assertEqual(bazel_sources("lib/media", {"media"}), cmake_sources("lib/media", "ao_media"))

    def test_all_four_dispatch_headers_use_their_tracked_gperf_inputs(self):
        cmake = (PROJECT_ROOT / "lib/media/CMakeLists.txt").read_text(encoding="utf-8")
        expected = {
            (source, "generated/" + output)
            for source, output in re.findall(
                r'add_gperf_header\(\s*\w+\s+"\$\{CMAKE_CURRENT_SOURCE_DIR\}/([^"\n]+)"\s+"([^"\n]+)"\s*\)',
                cmake,
            )
        }
        self.assertEqual(len(expected), 4)
        actual = {
            (ast.literal_eval(fields["src"]), ast.literal_eval(fields["out"]))
            for fields in rule_fields(PROJECT_ROOT / "lib/media/BUILD.bazel", "gperf_header")
        }
        self.assertEqual(actual, expected)

    def test_every_existing_media_case_source_is_owned_exactly_once(self):
        pure = bazel_sources("test", {"media_test"})
        integration = bazel_sources("test", {"media_fixture_test"})
        expected = {
            path.relative_to(PROJECT_ROOT / "test").as_posix()
            for path in (PROJECT_ROOT / "test/media").rglob("*Test.cpp")
        }
        self.assertFalse(pure & integration)
        self.assertEqual(pure | integration, expected)
        self.assertEqual(integration, {"media/file/MediaFileFixtureTest.cpp", "media/file/MpegFileTrailingTagTest.cpp"})
        cmake = (PROJECT_ROOT / "test/CMakeLists.txt").read_text(encoding="utf-8")
        for path in expected:
            with self.subTest(path=path):
                self.assertRegex(cmake, rf"(?m)^\s+{re.escape(path)}\s*$")

    def test_production_media_preserves_static_utility_boundary(self):
        fields = fields_for("lib/media", "cc_library", "media")
        self.assertIs(ast.literal_eval(fields["linkstatic"]), True)
        self.assertIn("//lib/utility:utility", ast.literal_eval(fields["deps"]))
        self.assertIn("//lib/utility:unicode_text", ast.literal_eval(fields["implementation_deps"]))
        for key in ("deps", "implementation_deps"):
            for node in ast.walk(fields.get(key, ast.List(elts=[]))):
                if isinstance(node, ast.Constant) and isinstance(node.value, str):
                    self.assertFalse(
                        node.value.startswith(("//app", "//runtime", "//lib/audio", "//lib/library", "//lib/query"))
                    )

    def test_media_suite_includes_pure_and_runtime_integration_cases(self):
        fields = fields_for(".", "test_suite", "media_tests")
        self.assertEqual(ast.literal_eval(fields["tests"]), ["//test:media_test", "//test:media_fixture_test"])
        for name in ("media_test", "media_fixture_test"):
            fields = fields_for("test", "cc_test", name)
            with self.subTest(target=name):
                self.assertEqual(ast.unparse(fields["additional_linker_inputs"]), "WINDOWS_UTF8_LINKER_INPUTS")
                self.assertEqual(ast.unparse(fields["linkopts"]), "WINDOWS_UTF8_LINKOPTS")
                self.assertFalse({"hdrs", "include_prefix", "strip_include_prefix"} & fields.keys())

    def test_fixture_cases_use_regular_runtime_headers_and_the_actual_runtime(self):
        fixture = fields_for("test", "cc_test", "media_fixture_test")
        self.assertEqual(ast.unparse(fixture["deps"].left), "STORAGE_TEST_DEPS")
        dependencies = ast.literal_eval(fixture["deps"].right)
        self.assertIn("//app/runtime:test_headers", dependencies)
        self.assertFalse(any(label.startswith("//app/runtime/library:") for label in dependencies))
        headers = fields_for("app/runtime", "cc_library", "test_headers")
        self.assertIs(ast.literal_eval(headers["testonly"]), True)
        self.assertEqual(ast.literal_eval(headers["visibility"]), ["//test:__pkg__"])
        self.assertEqual(ast.literal_eval(headers["deps"]), [":private_headers", ":runtime"])
        self.assertFalse({"srcs", "hdrs"} & headers.keys())
        pure = fields_for("test", "cc_test", "media_test")
        self.assertFalse(any(label.startswith("//app/") for label in ast.literal_eval(pure["deps"])))
        self.assertEqual(ast.literal_eval(pure["data"]), ["//test/data/audio:audio_fixtures"])

    def test_fixture_directory_uses_standard_runfiles_and_retains_the_cmake_macro(self):
        support = fields_for("test", "cc_library", "audio_fixture_support")
        self.assertIn(":common_test_support", ast.literal_eval(support["deps"]))
        self.assertIn("AOBUS_BAZEL_AUDIO_FIXTURES", ast.literal_eval(support["local_defines"]))
        common = fields_for("test", "cc_library", "common_test_support")
        self.assertEqual(ast.literal_eval(common["local_defines"]), ["AOBUS_BAZEL_TEST_RUNFILES"])
        self.assertIn("@rules_cc//cc/runfiles", ast.literal_eval(common["deps"]))
        media_support = fields_for("test", "cc_library", "media_test_support")
        self.assertIn(":audio_fixture_support", ast.literal_eval(media_support["deps"]))
        self.assertNotIn("audio/AudioFixtureSupport.cpp", ast.literal_eval(media_support["srcs"]))
        source = (PROJECT_ROOT / "test/audio/AudioFixtureSupport.cpp").read_text(encoding="utf-8")
        self.assertIn('ao::test::resolveBazelTestRunfile("test/data/audio/basic_metadata.m4a")', source)
        helper = (PROJECT_ROOT / "test/TestFixtureSupport.cpp").read_text(encoding="utf-8")
        self.assertIn("#ifdef AOBUS_BAZEL_TEST_RUNFILES", helper)
        self.assertIn("Runfiles::CreateForTest", helper)
        self.assertIn("Rlocation", helper)
        self.assertIn("AUDIO_TEST_DATA_DIR", source)
        self.assertNotIn("Y:/", source)
        self.assertNotIn(str(PROJECT_ROOT), source)
        fixture = (PROJECT_ROOT / "test/media/file/MediaFileFixtureTest.cpp").read_text(encoding="utf-8")
        self.assertIn("audio::test::audioFixtureDirectory()", fixture)

    def test_media_tests_declare_fixture_data_without_source_absolute_paths(self):
        path = PROJECT_ROOT / "test/BUILD.bazel"
        text = path.read_text(encoding="utf-8")
        self.assertIn("AOBUS_BAZEL_AUDIO_FIXTURES", text)
        self.assertNotIn(str(PROJECT_ROOT / "test/data/audio"), text)
        self.assertNotIn("Y:/", text)
        self.assertNotIn("Y:\\\\", text)
        data = PROJECT_ROOT / "test/data/audio/BUILD.bazel"
        self.assertTrue(data.is_file())
        self.assertIn("filegroup", data.read_text(encoding="utf-8"))
        source = ast.parse(text)
        labels = {
            node.value
            for node in ast.walk(source)
            if isinstance(node, ast.Constant)
            and isinstance(node.value, str)
            and node.value.startswith("//test/data/audio:")
        }
        self.assertTrue(labels)


if __name__ == "__main__":
    unittest.main()
