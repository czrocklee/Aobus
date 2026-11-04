"""Utility production ownership and the complete portable test-consumer contract."""

import ast
import unittest

from test_bazel_library import cmake_sources
from test_bazel_query import rule_fields
from test_bazel_test_graph import bazel_rule_sources, cmake_target_sources

from ao.core.paths import PROJECT_ROOT


class BazelUtilityGraphTest(unittest.TestCase):
    def test_utility_directly_owns_all_non_path_units_and_keeps_the_unicode_facade(self):
        libraries = {
            ast.literal_eval(fields["name"]): fields
            for fields in rule_fields(PROJECT_ROOT / "lib/utility/BUILD.bazel", "cc_library")
        }
        self.assertTrue({"utility", "path", "unicode_text"} <= set(libraries))
        test_headers = libraries.get("test_headers")
        self.assertIsNotNone(test_headers)
        self.assertIs(ast.literal_eval(test_headers["testonly"]), True)
        self.assertNotIn("srcs", test_headers)
        self.assertIn("AtomicFileTransaction.h", ast.literal_eval(test_headers["hdrs"]))
        utility = libraries["utility"]
        sources = [
            node.value
            for node in ast.walk(utility["srcs"])
            if isinstance(node, ast.Constant) and isinstance(node.value, str) and node.value.endswith(".cpp")
        ]
        self.assertCountEqual(sources, cmake_sources("lib/utility", "ao_utility") - {"Path.cpp"})
        base64_owners = [
            name
            for name, fields in libraries.items()
            if fields.get("srcs") is not None
            for node in ast.walk(fields["srcs"])
            if isinstance(node, ast.Constant) and node.value == "Base64.cpp"
        ]
        self.assertEqual(base64_owners, ["utility"])
        self.assertIn(":path", ast.literal_eval(utility["deps"]))
        path = libraries["path"]
        self.assertEqual(ast.literal_eval(path["srcs"]), ["Path.cpp"])
        self.assertEqual(ast.literal_eval(path["deps"]), ["//include:utility_headers"])
        catalog = rule_fields(PROJECT_ROOT / "tool/catalog/BUILD.bazel", "cc_binary")
        self.assertEqual(len(catalog), 1)
        self.assertIn("//lib/utility:path", ast.literal_eval(catalog[0]["deps"]))
        self.assertNotIn("//lib/utility:utility", ast.literal_eval(catalog[0]["deps"]))
        unicode = libraries["unicode_text"]
        self.assertEqual(ast.literal_eval(unicode["srcs"]), ["UnicodeText.cpp"])
        self.assertEqual(ast.literal_eval(unicode["deps"]), ["//include:utility_headers"])
        self.assertEqual(ast.literal_eval(unicode["implementation_deps"]), ["@aobus_native_deps//:icu_uc"])
        for attribute in ("deps", "implementation_deps"):
            self.assertFalse(
                any("icu" in label or "unicode_text" in label for label in ast.literal_eval(utility[attribute]))
            )

    def test_portable_cases_reuse_the_complete_cmake_inventory_and_real_implementation_owners(self):
        fields = next(
            fields
            for fields in rule_fields(PROJECT_ROOT / "test/BUILD.bazel", "cc_test")
            if ast.literal_eval(fields["name"]) == "utility_test"
        )
        cmake_cases = {
            source
            for source in cmake_target_sources(PROJECT_ROOT / "test/CMakeLists.txt", "ao_core_test")
            if source.startswith(("test/utility/", "test/compat/", "test/core/"))
            or source == "test/perf/report/PerformanceReportTest.cpp"
        }
        self.assertTrue(cmake_cases)
        self.assertEqual(bazel_rule_sources("test", "cc_test", "utility_test"), cmake_cases)
        dependencies = set(ast.literal_eval(fields["deps"]))
        self.assertTrue(
            {
                ":common_test_support",
                ":fatal_probe_support",
                ":performance_report_support",
                "//include:yaml_headers",
                "//lib/utility:test_headers",
                "//lib/utility:utility",
                "//lib/utility:unicode_text",
            }
            <= dependencies
        )
        self.assertNotIn(":library_test_support", dependencies)
        self.assertNotIn(":runtime_test_support", dependencies)
        self.assertEqual(ast.literal_eval(fields["data"]), [":ao_fatal_probe"])


if __name__ == "__main__":
    unittest.main()
