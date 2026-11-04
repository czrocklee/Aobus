"""Protect the complete async primitive slice without application runtime dependencies."""

import ast
import re
import unittest

from test_bazel_library import bazel_sources, cmake_sources
from test_bazel_query import rule_fields

from ao.core.paths import PROJECT_ROOT

RUNTIME_ASYNC_CASES = {
    "runtime/AsyncParallelTest.cpp",
    "runtime/AsyncRuntimeTest.cpp",
    "runtime/ExecutorTestSupportTest.cpp",
    "runtime/LifetimeScopeTest.cpp",
    "runtime/LoopExecutorTest.cpp",
}


def fields_for(directory, kind, name):
    return next(
        fields
        for fields in rule_fields(PROJECT_ROOT / directory / "BUILD.bazel", kind)
        if ast.literal_eval(fields["name"]) == name
    )


class BazelAsyncGraphTest(unittest.TestCase):
    def test_async_compiles_all_cmake_owned_implementation_units(self):
        self.assertEqual(bazel_sources("lib/async", {"async"}), cmake_sources("lib/async", "ao_async"))

    def test_async_keeps_existing_primitive_cases_in_both_owning_directories(self):
        expected = RUNTIME_ASYNC_CASES | {
            path.relative_to(PROJECT_ROOT / "test").as_posix()
            for path in (PROJECT_ROOT / "test/async").glob("*Test.cpp")
        }
        actual = bazel_sources("test", {"async_test"})
        self.assertEqual(actual, expected)
        cmake = (PROJECT_ROOT / "test/CMakeLists.txt").read_text(encoding="utf-8")
        for path in expected:
            with self.subTest(path=path):
                self.assertRegex(cmake, rf"(?m)^\s+{re.escape(path)}\s*$")

    def test_async_preserves_static_linkage_and_public_utility_boost_interface(self):
        fields = fields_for("lib/async", "cc_library", "async")
        self.assertIs(ast.literal_eval(fields["linkstatic"]), True)
        dependencies = ast.literal_eval(fields["deps"])
        self.assertIn("//lib/utility:utility", dependencies)
        self.assertIn("@aobus_native_deps//:boost", dependencies)

    def test_async_support_includes_both_existing_executor_and_coroutine_fixtures(self):
        sources = bazel_sources("test", {"async_test_support"})
        self.assertTrue({"runtime/AsyncTestSupport.cpp", "runtime/ExecutorTestSupport.cpp"} <= sources)
        fields = fields_for("test", "cc_library", "async_test_support")
        self.assertIs(ast.literal_eval(fields["testonly"]), True)
        self.assertIs(ast.literal_eval(fields["linkstatic"]), True)
        headers = ast.literal_eval(fields["hdrs"])
        self.assertIn("runtime/AsyncTestSupport.h", headers)
        self.assertIn("runtime/ExecutorTestSupport.h", headers)
        self.assertEqual(ast.literal_eval(fields["include_prefix"]), "test")
        self.assertEqual(ast.literal_eval(fields["strip_include_prefix"]), ".")

    def test_async_targets_do_not_depend_on_shipping_runtime_or_other_core_modules(self):
        for directory, kind, name in (
            ("lib/async", "cc_library", "async"),
            ("test", "cc_library", "async_test_support"),
            ("test", "cc_test", "async_test"),
        ):
            fields = fields_for(directory, kind, name)
            strings = {
                node.value
                for field in ("deps", "implementation_deps")
                if field in fields
                for node in ast.walk(fields[field])
                if isinstance(node, ast.Constant) and isinstance(node.value, str)
            }
            with self.subTest(target=name):
                self.assertFalse(
                    any(
                        label.startswith(("//runtime", "//app", "//lib/library", "//lib/query", "//lib/media"))
                        for label in strings
                    )
                )
                self.assertNotIn(":library_test_support", strings)
                self.assertNotIn("CoreTestMain.cpp", bazel_sources(directory, {name}))

    def test_async_suite_selects_its_actual_test_binary(self):
        fields = fields_for(".", "test_suite", "async_tests")
        self.assertEqual(ast.literal_eval(fields["tests"]), ["//test:async_test"])
        fields = fields_for("test", "cc_test", "async_test")
        self.assertEqual(ast.unparse(fields["additional_linker_inputs"]), "WINDOWS_UTF8_LINKER_INPUTS")
        self.assertEqual(ast.unparse(fields["linkopts"]), "WINDOWS_UTF8_LINKOPTS")
        self.assertNotIn("hdrs", fields)
        self.assertNotIn("include_prefix", fields)
        self.assertNotIn("strip_include_prefix", fields)


if __name__ == "__main__":
    unittest.main()
