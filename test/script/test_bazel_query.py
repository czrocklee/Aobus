"""Protect the complete query slice and its declared gperf action boundary."""

import ast
import unittest

from test_bazel_library import bazel_sources, cmake_sources

from ao.core.paths import PROJECT_ROOT


def rule_fields(path, kind):
    tree = ast.parse(path.read_text(encoding="utf-8"))
    return [
        {keyword.arg: keyword.value for keyword in node.keywords}
        for node in ast.walk(tree)
        if isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and node.func.id == kind
    ]


class BazelQueryGraphTest(unittest.TestCase):
    def test_query_compiles_all_cmake_owned_implementation_units(self):
        self.assertEqual(bazel_sources("lib/query", {"query"}), cmake_sources("lib/query", "ao_query"))

    def test_query_keeps_all_existing_query_case_sources(self):
        expected = {
            path.relative_to(PROJECT_ROOT / "test").as_posix()
            for path in (PROJECT_ROOT / "test/query").glob("*Test.cpp")
        }
        actual = {
            path
            for path in bazel_sources("test", {"query_test"})
            if path.startswith("query/") and path.endswith("Test.cpp")
        }
        self.assertEqual(actual, expected)

    def test_query_preserves_static_linkage_and_private_parser_dependencies(self):
        fields = next(
            fields
            for fields in rule_fields(PROJECT_ROOT / "lib/query/BUILD.bazel", "cc_library")
            if ast.literal_eval(fields["name"]) == "query"
        )
        self.assertIs(ast.literal_eval(fields["linkstatic"]), True)
        self.assertIn("//lib/library:library", ast.literal_eval(fields["deps"]))
        private = ast.literal_eval(fields["implementation_deps"])
        self.assertIn("//lib/utility:unicode_text", private)
        self.assertIn("@aobus_native_deps//:lexy", private)

    def test_executable_rules_do_not_use_library_only_header_attributes(self):
        path = PROJECT_ROOT / "test/BUILD.bazel"
        for kind in ("cc_test", "cc_binary"):
            for fields in rule_fields(path, kind):
                with self.subTest(rule=kind, name=ast.literal_eval(fields["name"])):
                    self.assertFalse({"hdrs", "include_prefix", "strip_include_prefix"} & fields.keys())

    def test_unit_dispatch_is_generated_from_its_real_source_not_cmake_output(self):
        fields = rule_fields(PROJECT_ROOT / "lib/query/BUILD.bazel", "gperf_header")
        self.assertEqual(len(fields), 1)
        self.assertEqual(ast.literal_eval(fields[0]["src"]), "UnitDispatch.gperf")
        self.assertEqual(ast.literal_eval(fields[0]["out"]), "generated/query/UnitDispatch.h")

    def test_gperf_action_declares_source_tool_runtime_and_output(self):
        tree = ast.parse((PROJECT_ROOT / "bazel/gperf.bzl").read_text(encoding="utf-8"))
        actions = [
            node
            for node in ast.walk(tree)
            if isinstance(node, ast.Call)
            and isinstance(node.func, ast.Attribute)
            and isinstance(node.func.value, ast.Attribute)
            and isinstance(node.func.value.value, ast.Name)
            and node.func.value.value.id == "ctx"
            and node.func.value.attr == "actions"
            and node.func.attr == "run"
        ]
        self.assertEqual(len(actions), 1)
        fields = {keyword.arg: keyword.value for keyword in actions[0].keywords}
        self.assertEqual(ast.unparse(fields["inputs"]), "[ctx.file.src]")
        self.assertEqual(ast.unparse(fields["outputs"]), "[ctx.outputs.out]")
        self.assertEqual(ast.unparse(fields["executable"]), "ctx.executable._gperf")
        self.assertIn("ctx.attr._runtime[DefaultInfo].files", ast.unparse(fields["tools"]))
        self.assertEqual(ast.literal_eval(fields["env"]), {"LC_ALL": "C"})
        self.assertEqual(ast.literal_eval(fields["mnemonic"]), "AobusGperf")
        self.assertNotIn("shell", fields)


if __name__ == "__main__":
    unittest.main()
