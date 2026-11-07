"""UIModel interfaces and complete case ownership, not native acceptance."""

import ast
import unittest

from test_bazel_query import rule_fields
from test_bazel_test_graph import bazel_rule_sources, cmake_target_sources

from ao.core.paths import PROJECT_ROOT


def fields_for(directory, kind, name):
    return next(
        fields
        for fields in rule_fields(PROJECT_ROOT / directory / "BUILD.bazel", kind)
        if ast.literal_eval(fields["name"]) == name
    )


class BazelUIModelGraphTest(unittest.TestCase):
    def test_regular_header_interfaces_own_audio_runtime_and_uimodel_headers(self):
        audio = fields_for("include", "cc_library", "audio_headers")
        self.assertEqual(ast.literal_eval(audio["hdrs"]), [":audio_public_headers"])
        self.assertIn("//app/include:__pkg__", ast.literal_eval(audio["visibility"]))
        public = fields_for("include", "filegroup", "audio_public_headers")
        self.assertEqual(ast.unparse(public["srcs"]), "glob(['ao/audio/**/*.h'])")
        runtime = fields_for("app/include", "cc_library", "runtime_headers")
        self.assertEqual(ast.unparse(runtime["hdrs"]), "glob(['ao/rt/**/*.h'])")
        self.assertIn("//include:audio_headers", ast.literal_eval(runtime["deps"]))
        model = fields_for("app/include", "cc_library", "uimodel_headers")
        self.assertEqual(ast.unparse(model["hdrs"]), "glob(['ao/uimodel/**/*.h'])")
        self.assertIn(":runtime_headers", ast.literal_eval(model["deps"]))
        for fields in (audio, runtime, model):
            with self.subTest(target=ast.literal_eval(fields["name"])):
                self.assertEqual(ast.literal_eval(fields["includes"]), ["."])
                self.assertFalse(
                    {"srcs", "implementation_deps", "include_prefix", "strip_include_prefix"} & fields.keys()
                )

    def test_all_uimodel_cases_reuse_cmake_sources_and_focused_support(self):
        fields = fields_for("test", "cc_test", "uimodel_test")
        cmake = PROJECT_ROOT / "test/CMakeLists.txt"
        cmake_cases = {
            source
            for source in cmake_target_sources(cmake, "ao_core_test")
            if source.startswith("test/uimodel/") and source.endswith("Test.cpp")
        }
        self.assertTrue(cmake_cases)
        self.assertEqual(bazel_rule_sources("test", "cc_test", "uimodel_test"), cmake_cases)
        support = {
            source
            for source in cmake_target_sources(cmake, "ao_core_test")
            if source.startswith("test/uimodel/") and source.endswith("TestSupport.cpp")
        }
        self.assertEqual(bazel_rule_sources("test", "cc_library", "uimodel_test_support"), support)
        deps = set(ast.literal_eval(fields["deps"]))
        self.assertTrue(
            {
                ":audio_fixture_support",
                ":common_test_support",
                ":message_catalog_test_support",
                ":runtime_test_support",
                ":uimodel_test_support",
                "//app/uimodel:uimodel",
                "//app/uimodel:test_headers",
                "//app/runtime:test_headers",
                "//lib/audio:test_headers",
            }
            <= deps
        )
        self.assertIn("//test/data/audio:audio_fixtures", ast.literal_eval(fields["data"]))
        self.assertIn("//:doc/reference/shell/component-vocabulary.md", ast.literal_eval(fields["data"]))
        self.assertEqual(ast.unparse(fields["additional_linker_inputs"]), "WINDOWS_UTF8_LINKER_INPUTS")
        self.assertEqual(ast.unparse(fields["linkopts"]), "WINDOWS_UTF8_LINKOPTS")
        suite = fields_for(".", "test_suite", "uimodel_tests")
        self.assertEqual(ast.literal_eval(suite["tests"]), ["//test:uimodel_test"])


if __name__ == "__main__":
    unittest.main()
