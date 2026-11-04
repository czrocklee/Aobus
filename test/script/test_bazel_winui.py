"""Authored WinUI source ownership shared by the CMake and Bazel WinUI products.

Native action and artifact checks own generated-unit compilation, linker flags,
platform compatibility and the runnable layout. These comparisons retain the
independent CMake ownership authority, not a snapshot of private Bazel targets.
The cross-host model and the eight independently entered probes stay derived
from their CMake owners rather than a second source inventory.
"""

import ast
import re
import unittest
from pathlib import PureWindowsPath
from types import SimpleNamespace

from test_bazel_gtk import string_literals
from test_bazel_uimodel import fields_for

from ao.core.paths import PROJECT_ROOT

WINUI_PACKAGE = PROJECT_ROOT / "app/windows-winui"


def build_variables(path, names):
    """Top-level list assignments a BUILD file exposes by name."""
    variables = {}
    for node in ast.parse(path.read_text(encoding="utf-8")).body:
        if not isinstance(node, ast.Assign):
            continue
        for target in node.targets:
            if isinstance(target, ast.Name) and target.id in names:
                variables[target.id] = ast.literal_eval(node.value)
    return variables


def evaluate(expression, variables):
    """Evaluate the small declarative BUILD expressions used by source ownership."""
    if isinstance(expression, ast.Constant):
        return expression.value
    if isinstance(expression, ast.Name):
        return variables[expression.id]
    if isinstance(expression, ast.Attribute):
        return getattr(evaluate(expression.value, variables), expression.attr)
    if isinstance(expression, ast.List):
        return [evaluate(item, variables) for item in expression.elts]
    if isinstance(expression, ast.BinOp) and isinstance(expression.op, ast.Add):
        return evaluate(expression.left, variables) + evaluate(expression.right, variables)
    if isinstance(expression, ast.ListComp):
        (generator,) = expression.generators
        if not isinstance(generator.target, ast.Name):
            raise AssertionError("Unexpected BUILD comprehension target")
        result = []
        for item in evaluate(generator.iter, variables):
            local = {**variables, generator.target.id: item}
            if all(evaluate(condition, local) for condition in generator.ifs):
                result.append(evaluate(expression.elt, local))
        return result
    if isinstance(expression, ast.Compare):
        left = evaluate(expression.left, variables)
        for operator, comparator in zip(expression.ops, expression.comparators, strict=True):
            right = evaluate(comparator, variables)
            if isinstance(operator, ast.In):
                matched = left in right
            elif isinstance(operator, ast.NotIn):
                matched = left not in right
            else:
                raise AssertionError("Unexpected BUILD comparison: " + ast.unparse(expression))
            if not matched:
                return False
            left = right
        return True
    raise AssertionError("Unexpected BUILD expression: " + ast.unparse(expression))


def label_path(label):
    """Repository-relative path of a same-repository Bazel source label."""
    if label.startswith("//"):
        package, _, target = label[2:].partition(":")
        return f"{package}/{target}" if package else target
    return label


def cmake_core_model_sources():
    """The app/windows-winui units CMake compiles into the cross-host core test."""
    text = (PROJECT_ROOT / "test/CMakeLists.txt").read_text(encoding="utf-8")
    block = re.search(r"add_executable\(ao_core_test\b(.*?)\n\)", text, re.DOTALL)
    if block is None:
        raise AssertionError("CMake ao_core_test declaration was not found")
    prefix = "app/windows-winui/"
    return {
        path.removeprefix(prefix)
        for path in re.findall(r"\$\{CMAKE_SOURCE_DIR\}/(app/windows-winui/[\w./-]+\.cpp)", block.group(1))
    }


def cmake_probe_sources():
    """Per-probe test-owned C++ sources from the native WinUI CMake declarations."""
    text = (WINUI_PACKAGE / "CMakeLists.txt").read_text(encoding="utf-8")
    result = {}
    for name, body in re.findall(r"add_executable\((ao_winui_[a-z_]+_probe)\b([^)]*)\)", text, re.DOTALL):
        result[name] = {
            path.removeprefix("${CMAKE_SOURCE_DIR}/")
            for path in re.findall(r"\$\{CMAKE_SOURCE_DIR\}/(test/[\w./-]+\.cpp)\b", body)
        }
    return result


def bazel_probe_sources():
    """Per-probe test-owned sources from the WinUI BUILD entries."""
    tree = ast.parse((WINUI_PACKAGE / "BUILD.bazel").read_text(encoding="utf-8"))
    result = {}
    for node in ast.walk(tree):
        if not isinstance(node, ast.Call) or not isinstance(node.func, ast.Name) or node.func.id != "winui_probe":
            continue
        fields = {keyword.arg: keyword.value for keyword in node.keywords}
        name = ast.literal_eval(fields["name"])
        result[name] = {
            label_path(label)
            for label in string_literals(fields.get("srcs"))
            if label_path(label).startswith("test/") and label_path(label).endswith(".cpp")
        }
    return result


class WinuiSourceOwnershipTest(unittest.TestCase):
    def test_authored_winui_library_sources_agree_with_cmake_without_duplicate_owners(self):
        package = PROJECT_ROOT / "app/windows-winui"
        cmake = (package / "CMakeLists.txt").read_text(encoding="utf-8")
        block = re.search(r"set\(\s*AOBUS_WINUI_LIBRARY_SOURCES\b(.*?)\n\)", cmake, re.DOTALL)
        self.assertIsNotNone(block, "CMake WinUI authored-source declaration was not found")
        expected = re.findall(r"[\w./-]+\.cpp\b", block.group(1))
        module = ast.parse((package / "BUILD.bazel").read_text(encoding="utf-8"))
        declaration = next(
            node
            for node in module.body
            if isinstance(node, ast.Assign)
            and any(isinstance(target, ast.Name) and target.id == "WINUI_SOURCES" for target in node.targets)
        )

        declared = ast.literal_eval(declaration.value)

        self.assertCountEqual(declared, expected)
        self.assertEqual(len(declared), len(set(declared)))

    def test_model_sources_match_the_cross_host_core_test_declaration(self):
        variables = build_variables(WINUI_PACKAGE / "BUILD.bazel", {"WINUI_SOURCES", "MODEL_SOURCES"})
        expected = cmake_core_model_sources()
        self.assertTrue(expected, "CMake ao_core_test WinUI model sources were not found")
        self.assertCountEqual(variables["MODEL_SOURCES"], expected)
        self.assertLessEqual(set(expected), set(variables["WINUI_SOURCES"]))

        model = fields_for("app/windows-winui", "cc_library", "model")
        self.assertCountEqual(evaluate(model["srcs"], variables), expected)
        self.assertIs(ast.literal_eval(model["linkstatic"]), True)
        self.assertNotIn("target_compatible_with", model)
        dependencies = set(string_literals(model["deps"])) | set(string_literals(model.get("implementation_deps")))
        self.assertFalse(any(label.startswith("@aobus_winui_sdk//") for label in dependencies))
        self.assertNotIn(":generated", dependencies)
        self.assertNotIn(":error_boundary", dependencies)

    def test_shipping_winui_filters_model_and_error_boundary_units_and_depends_on_model(self):
        variables = build_variables(WINUI_PACKAGE / "BUILD.bazel", {"WINUI_SOURCES", "MODEL_SOURCES"})
        winui = fields_for("app/windows-winui", "cc_library", "winui")
        sources = evaluate(winui["srcs"], variables)
        expected = [
            source
            for source in variables["WINUI_SOURCES"]
            if source not in variables["MODEL_SOURCES"] and source != "platform/WinUiErrorBoundary.cpp"
        ] + [":generated_cpp"]
        self.assertEqual(sources, expected)
        self.assertFalse(set(variables["MODEL_SOURCES"]) & set(sources))
        self.assertNotIn("platform/WinUiErrorBoundary.cpp", sources)

        dependencies = set(string_literals(winui["deps"]))
        self.assertIn(":model", dependencies)
        self.assertIn(":error_boundary", dependencies)
        self.assertIn(":generated", dependencies)
        self.assertEqual(ast.literal_eval(winui["target_compatible_with"]), ["@platforms//os:windows"])

    def test_error_boundary_stays_native_only_while_model_remains_cross_host(self):
        error = fields_for("app/windows-winui", "cc_library", "error_boundary")
        self.assertEqual(ast.literal_eval(error["target_compatible_with"]), ["@platforms//os:windows"])
        self.assertIn(":generated", set(string_literals(error["deps"])))
        model = fields_for("app/windows-winui", "cc_library", "model")
        self.assertNotIn("target_compatible_with", model)

    def test_eight_probe_entries_mirror_their_cmake_owned_sources_and_mains(self):
        cmake = cmake_probe_sources()
        bazel = bazel_probe_sources()
        self.assertEqual(len(cmake), 8, "expected the eight CMake WinUI probe declarations")
        self.assertEqual(set(bazel), set(cmake))
        mains = set()
        for name in sorted(cmake):
            with self.subTest(probe=name):
                self.assertEqual(bazel[name], cmake[name])
                (main,) = [source for source in bazel[name] if source.endswith("Probe.cpp")]
                self.assertNotIn(main, mains)
                mains.add(main)
        self.assertEqual(len(mains), len(bazel))

    def test_codegen_compilation_context_excludes_private_msbuild_diagnostics(self):
        tree = ast.parse((WINUI_PACKAGE / "winui.bzl").read_text(encoding="utf-8"))
        implementation = next(
            node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name == "_codegen_impl"
        )
        # Follow the helper's output roles, not their variable names or path spelling.
        outputs = {
            node.args[0].value: node.args[1].value.id
            for node in ast.walk(implementation)
            if isinstance(node, ast.Call)
            and isinstance(node.func, ast.Attribute)
            and node.func.attr == "add"
            and len(node.args) == 2
            and isinstance(node.args[0], ast.Constant)
            and isinstance(node.args[1], ast.Attribute)
            and node.args[1].attr == "path"
            and isinstance(node.args[1].value, ast.Name)
        }
        payload, private = outputs["--headers"], outputs["--private"]
        self.assertNotEqual(payload, private)
        context = next(
            node
            for node in ast.walk(implementation)
            if isinstance(node, ast.Call)
            and isinstance(node.func, ast.Attribute)
            and node.func.attr == "create_compilation_context"
        )
        headers = next(keyword.value for keyword in context.keywords if keyword.arg == "headers")
        header_names = {node.id for node in ast.walk(headers) if isinstance(node, ast.Name)}
        self.assertIn(payload, header_names)
        self.assertNotIn(private, header_names)
        groups = next(
            node
            for node in ast.walk(implementation)
            if isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and node.func.id == "OutputGroupInfo"
        )
        diagnostics = next(keyword.value for keyword in groups.keywords if keyword.arg == "diagnostics")
        self.assertIn(private, {node.id for node in ast.walk(diagnostics) if isinstance(node, ast.Name)})

    def test_private_outputs_leave_room_for_full_clr_temporary_batch_paths(self):
        tree = ast.parse((WINUI_PACKAGE / "winui.bzl").read_text(encoding="utf-8"))
        # This package-prefix budget reproduces the native long-tree failure.
        # Full-CLR Exec adds both GUID components even for generation-only work.
        package = PureWindowsPath("C:/") / ("x" * 148)
        batch = PureWindowsPath("t") / ("MSBuildTemp" + "0" * 32) / ("tmp" + "0" * 32 + ".exec.cmd")
        for rule, target in (("_codegen_impl", "generated"), ("_resources_impl", "resources")):
            with self.subTest(rule=rule):
                implementation = next(
                    node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name == rule
                )
                private = next(
                    node.args[1].value.id
                    for node in ast.walk(implementation)
                    if isinstance(node, ast.Call)
                    and isinstance(node.func, ast.Attribute)
                    and node.func.attr == "add"
                    and len(node.args) == 2
                    and isinstance(node.args[0], ast.Constant)
                    and node.args[0].value == "--private"
                )
                declaration = next(
                    node.value
                    for node in implementation.body
                    if isinstance(node, ast.Assign)
                    and any(isinstance(name, ast.Name) and name.id == private for name in node.targets)
                )
                self.assertEqual(declaration.func.attr, "declare_directory")
                relative = evaluate(declaration.args[0], {"ctx": SimpleNamespace(label=SimpleNamespace(name=target))})
                self.assertLess(len(str(package / relative / batch)), 260)
                self.assertGreaterEqual(len(str(package / target / "private" / batch)), 260)

    def test_winui_app_rule_keeps_default_identity_and_declared_consumption_roles(self):
        tree = ast.parse((WINUI_PACKAGE / "winui.bzl").read_text(encoding="utf-8"))
        rule = next(
            node.value
            for node in tree.body
            if isinstance(node, ast.Assign)
            and any(isinstance(target, ast.Name) and target.id == "winui_app" for target in node.targets)
        )
        self.assertIs(
            ast.literal_eval(next(keyword.value for keyword in rule.keywords if keyword.arg == "executable")),
            True,
        )
        attrs = next(keyword.value for keyword in rule.keywords if keyword.arg == "attrs")
        attributes = {key.value: value for key, value in zip(attrs.keys, attrs.values, strict=True)}
        identity = attributes["identity"]
        self.assertEqual(identity.func.value.id, "attr")
        self.assertEqual(identity.func.attr, "string")
        self.assertEqual(
            ast.literal_eval(next(keyword.value for keyword in identity.keywords if keyword.arg == "default")),
            "Aobus",
        )
        for name in ("deps", "resources", "runtime"):
            attribute = attributes[name]
            self.assertEqual(attribute.func.value.id, "attr")
            self.assertIs(
                ast.literal_eval(next(keyword.value for keyword in attribute.keywords if keyword.arg == "mandatory")),
                True,
            )

    def test_shared_windows_facade_retains_the_cmake_source_and_public_link_interface(self):
        cmake = (PROJECT_ROOT / "app/CMakeLists.txt").read_text(encoding="utf-8")
        declaration = re.search(r"add_library\(ao_windows_platform\s+STATIC([^)]*)\)", cmake, re.DOTALL)
        interface = re.search(r"target_link_libraries\(ao_windows_platform\s+PUBLIC\s+([^)]+)\)", cmake, re.DOTALL)
        self.assertIsNotNone(declaration, "CMake Windows facade source owner was not found")
        self.assertIsNotNone(interface, "CMake Windows facade public link interface was not found")
        expected_sources = [
            entry.removeprefix("windows/") for entry in re.findall(r"[\w./-]+\.cpp\b", declaration.group(1))
        ]
        expected_headers = [
            entry.removeprefix("windows/") for entry in re.findall(r"[\w./-]+\.h\b", declaration.group(1))
        ]

        facade = fields_for("app/windows", "cc_library", "windows_platform")

        self.assertCountEqual(ast.literal_eval(facade["srcs"]), expected_sources)
        self.assertCountEqual(ast.literal_eval(facade["hdrs"]), expected_headers)
        self.assertEqual(
            set(ast.literal_eval(facade["linkopts"])), {name + ".lib" for name in interface.group(1).split()}
        )


if __name__ == "__main__":
    unittest.main()
