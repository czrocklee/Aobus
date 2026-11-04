"""GTK product graph contracts: CMake membership, resource action, and Linux scope."""

import ast
import posixpath
import unittest
import xml.etree.ElementTree as ElementTree
from types import SimpleNamespace
from unittest.mock import Mock

from test_bazel_library import cmake_sources
from test_bazel_query import rule_fields
from test_bazel_uimodel import fields_for

from ao.core.paths import PROJECT_ROOT

GTK_PACKAGE = PROJECT_ROOT / "app/linux-gtk"
GRESOURCE_RULE = GTK_PACKAGE / "gresource.bzl"


def string_literals(expression):
    if expression is None:
        return []
    return [
        node.value for node in ast.walk(expression) if isinstance(node, ast.Constant) and isinstance(node.value, str)
    ]


def glob_patterns(expression):
    """Patterns a glob() call declares, independent of its argument spelling."""
    if not (
        isinstance(expression, ast.Call) and isinstance(expression.func, ast.Name) and expression.func.id == "glob"
    ):
        raise AssertionError("Unexpected glob expression: " + ast.unparse(expression))
    include = (
        expression.args[0]
        if expression.args
        else next(keyword.value for keyword in expression.keywords if keyword.arg == "include")
    )
    return ast.literal_eval(include)


class DefaultInfo(SimpleNamespace):
    """Recorded DefaultInfo provider payload."""


class RecordedFile:
    """The path surface a Bazel File exposes to a rule implementation."""

    def __init__(self, path, *, short_path=None):
        self.path = path
        self.short_path = path if short_path is None else short_path
        self.dirname = posixpath.dirname(path)


class RecordedDepset(SimpleNamespace):
    """Record API inputs without implementing Bazel's graph or traversal semantics."""


def execute_gresource_rule():
    """Run the real gresource_c implementation against recorded Bazel API fakes."""
    recorded_arguments = []

    def arguments():
        values = []
        recorder = SimpleNamespace(argv=values, add=values.append)
        recorded_arguments.append(recorder)
        return recorder

    def depset(direct=(), transitive=()):
        if any(not isinstance(item, RecordedDepset) for item in transitive):
            raise AssertionError("transitive tool inputs must be depsets")
        return RecordedDepset(direct=list(direct), transitive=list(transitive))

    namespace = {
        "attr": SimpleNamespace(
            label_list=lambda **kwargs: {"kind": "label_list", **kwargs},
            label=lambda **kwargs: {"kind": "label", **kwargs},
            output=lambda **kwargs: {"kind": "output", **kwargs},
        ),
        "DefaultInfo": DefaultInfo,
        "Label": str,
        "depset": depset,
        "rule": lambda **kwargs: kwargs,
    }
    source = GRESOURCE_RULE.read_text(encoding="utf-8")
    exec(compile(source, GRESOURCE_RULE.name, "exec"), namespace)
    descriptor = namespace["gresource_c"]
    xml = RecordedFile("fixture/resources/theme.gresource.xml")
    out = RecordedFile("bazel-out/test-cfg/bin/fixture/generated/theme.c", short_path="fixture/generated/theme.c")
    sources = [RecordedFile("fixture/resources/theme.css"), RecordedFile("fixture/assets/brand.svg")]
    tool = RecordedFile("tools/host/glib-compile-resources")
    runtime = depset([RecordedFile("tools/runtime/libgio-2.0.so.0")])
    actions = SimpleNamespace(args=arguments, run=Mock())
    ctx = SimpleNamespace(
        outputs=SimpleNamespace(out=out),
        file=SimpleNamespace(xml=xml),
        files=SimpleNamespace(srcs=sources),
        executable=SimpleNamespace(_glib_compile_resources=tool),
        attr=SimpleNamespace(
            _glib_compile_resources=Mock(name="tool target"),
            _glib_compile_resources_runtime={DefaultInfo: DefaultInfo(files=runtime)},
        ),
        actions=actions,
    )
    result = descriptor["implementation"](ctx)
    actions.run.assert_called_once()
    run = actions.run.call_args.kwargs
    (passed_arguments,) = run["arguments"]
    return SimpleNamespace(
        descriptor=descriptor,
        recorded_arguments=recorded_arguments,
        argv=passed_arguments.argv,
        run=run,
        result=result,
        xml=xml,
        out=out,
        sources=sources,
        tool=tool,
        runtime=runtime,
    )


def expand_pattern(base, pattern):
    if any(character in pattern for character in "*?["):
        return sorted(path.relative_to(base).as_posix() for path in base.glob(pattern) if path.is_file())
    return [pattern]


def asset_filegroup_fields(name):
    return next(
        fields
        for fields in rule_fields(PROJECT_ROOT / "asset/BUILD.bazel", "filegroup")
        if ast.literal_eval(fields["name"]) == name
    )


def declared_paths(node, base):
    """Absolute file paths a srcs expression declares, without executing it."""
    if isinstance(node, ast.List):
        result = []
        for element in node.elts:
            result.extend(declared_paths(element, base))
        return result
    if isinstance(node, ast.BinOp) and isinstance(node.op, ast.Add):
        return declared_paths(node.left, base) + declared_paths(node.right, base)
    if isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and node.func.id == "glob":
        result = {path for pattern in glob_patterns(node) for path in expand_pattern(base, pattern)}
        excluded = next((keyword.value for keyword in node.keywords if keyword.arg == "exclude"), None)
        if excluded is not None:
            result.difference_update(
                path for pattern in ast.literal_eval(excluded) for path in expand_pattern(base, pattern)
            )
        return [(base / path).as_posix() for path in sorted(result)]
    if isinstance(node, ast.Constant) and isinstance(node.value, str):
        entry = node.value
        if entry.startswith("//asset:"):
            filegroup = asset_filegroup_fields(entry.split(":", 1)[1])
            return declared_paths(filegroup["srcs"], PROJECT_ROOT / "asset")
        return [(base / entry).as_posix()]
    raise AssertionError("Unexpected resource input expression: " + ast.unparse(node))


def bundle_input_paths():
    fields = fields_for("app/linux-gtk", "gresource_c", "generated_resources")
    return declared_paths(fields["srcs"], GTK_PACKAGE)


def bundle_xml_paths():
    root = ElementTree.parse(GTK_PACKAGE / "aobus.gresource.xml").getroot()
    return [element.text.strip() for element in root.iter("file")]


def build_assignments():
    tree = ast.parse((GTK_PACKAGE / "BUILD.bazel").read_text(encoding="utf-8"))
    return {
        node.targets[0].id: node.value
        for node in tree.body
        if isinstance(node, ast.Assign) and isinstance(node.targets[0], ast.Name)
    }


def package_cc_targets():
    """Every compiling cc target in the GTK package, keyed by target name."""
    targets = {}
    for kind in ("cc_library", "cc_binary"):
        for fields in rule_fields(GTK_PACKAGE / "BUILD.bazel", kind):
            targets[ast.literal_eval(fields["name"])] = fields
    return targets


def gtk_header_closure(targets):
    """cc targets whose compilation consumes GTK interface headers.

    The seeds reference the GTK interface directly; package-local dependents
    compile the same headers through their own sources, so they join too.
    """
    closure = {
        name
        for name, fields in targets.items()
        if "@aobus_native_deps//:gtkmm"
        in string_literals(fields.get("deps")) + string_literals(fields.get("implementation_deps"))
    }
    changed = True
    while changed:
        changed = False
        for name, fields in targets.items():
            if name in closure:
                continue
            labels = string_literals(fields.get("deps")) + string_literals(fields.get("implementation_deps"))
            if any(label.startswith(":") and label[1:] in closure for label in labels):
                closure.add(name)
                changed = True
    return closure


def copts_beyond_header_contract(copts):
    """Extra literal options beyond GTK_HEADER_COPTS, or None if not honoring it."""
    if isinstance(copts, ast.Name):
        return [] if copts.id == "GTK_HEADER_COPTS" else None
    if isinstance(copts, ast.BinOp) and isinstance(copts.op, ast.Add) and isinstance(copts.left, ast.Name):
        if copts.left.id != "GTK_HEADER_COPTS":
            return None
        return string_literals(copts.right)
    return None


class BazelGtkGraphTest(unittest.TestCase):
    def test_gtk_library_owns_the_complete_cmake_product_membership(self):
        fields = fields_for("app/linux-gtk", "cc_library", "gtk")
        sources = string_literals(fields["srcs"])
        self.assertCountEqual(sources, cmake_sources("app/linux-gtk", "aobus-gtk-lib"))
        # The entry point stays with the executable instead of the product library.
        self.assertNotIn("main.cpp", sources)
        self.assertEqual(glob_patterns(fields["hdrs"]), ["**/*.h"])
        self.assertEqual(ast.literal_eval(fields["includes"]), ["."])
        self.assertIs(ast.literal_eval(fields["linkstatic"]), True)
        self.assertEqual(ast.literal_eval(fields["target_compatible_with"]), ["@platforms//os:linux"])
        # The Linux product always compiles the MPRIS system-media adapter in.
        self.assertEqual(ast.literal_eval(fields["local_defines"]), ["AOBUS_HAS_SYSTEM_MEDIA=1"])
        self.assertIn("//app/systemmedia:linux", ast.literal_eval(fields["implementation_deps"]))

    def test_aobus_gtk_binary_links_the_entry_point_and_anchors_the_resource_bundle(self):
        product = fields_for("app/linux-gtk", "cc_binary", "aobus-gtk")
        self.assertEqual(ast.literal_eval(product["srcs"]), ["main.cpp"])
        self.assertIs(ast.literal_eval(product["linkstatic"]), True)
        self.assertEqual(ast.literal_eval(product["target_compatible_with"]), ["@platforms//os:linux"])
        self.assertIn("//visibility:public", ast.literal_eval(product["visibility"]))
        # The linker anchor must match the C symbol prefix the resource action
        # actually emits.
        c_name = next(argument for argument in execute_gresource_rule().argv if argument.startswith("--c-name="))
        self.assertEqual(
            ast.literal_eval(product["linkopts"]),
            [f"-Wl,-u,{c_name.removeprefix('--c-name=')}_get_resource"],
        )
        self.assertIn(":gtk", ast.literal_eval(product["deps"]))

    def test_desktop_launch_owns_the_cmake_sources_and_public_utility_interface(self):
        fields = fields_for("app/desktop", "cc_library", "desktop_launch")
        self.assertCountEqual(
            string_literals(fields["srcs"]),
            cmake_sources("app/desktop", "ao_desktop_launch"),
        )
        self.assertIs(ast.literal_eval(fields["linkstatic"]), True)
        # Boost::process stays a private mechanism dependency like in CMake.
        self.assertEqual(ast.literal_eval(fields["implementation_deps"]), ["@aobus_native_deps//:boost_process"])
        self.assertIn("//app/include:desktop_headers", ast.literal_eval(fields["deps"]))
        self.assertIn("//app/linux-gtk:__pkg__", ast.literal_eval(fields["visibility"]))

    def test_declared_bundle_inputs_cover_every_xml_resource(self):
        bundle = fields_for("app/linux-gtk", "gresource_c", "generated_resources")
        self.assertEqual(ast.literal_eval(bundle["xml"]), "aobus.gresource.xml")
        self.assertEqual(ast.literal_eval(bundle["out"]), "resources.c")
        declared = set(bundle_input_paths())
        self.assertTrue(declared)
        # The XML stays the single authority: every referenced file must resolve
        # against one of the two source roots and be declared to the action.
        roots = [GTK_PACKAGE, PROJECT_ROOT]
        for entry in bundle_xml_paths():
            with self.subTest(resource=entry):
                resolved = [root / entry for root in roots if (root / entry).is_file()]
                self.assertTrue(resolved, f"{entry} does not resolve against a bundle source root")
                self.assertIn(resolved[0].as_posix(), declared, f"{entry}'s first source-root match is not declared")

    def test_asset_filegroup_overapproximates_for_the_bundle_consumer_only(self):
        filegroup = asset_filegroup_fields("gresource_files")
        self.assertCountEqual(ast.literal_eval(filegroup["visibility"]), ["//app/linux-gtk:__pkg__"])
        for pattern in string_literals(filegroup["srcs"]):
            with self.subTest(pattern=pattern):
                self.assertTrue(pattern.startswith(("brand/", "ui/no-cover/")))

    def test_gtk_interface_consumers_restate_the_admitted_sse_header_contract(self):
        assignments = build_assignments()
        self.assertIn("GTK_HEADER_COPTS", assignments)
        cpu = fields_for("app/linux-gtk", "config_setting", "gtk_x86_64_cpu")
        self.assertEqual(ast.literal_eval(cpu["constraint_values"]), ["@platforms//cpu:x86_64"])
        # The SDK inspection admitted exactly the gtkmm-4.0 x86_64 SSE Cflags;
        # nothing else may ride along and non-x86_64 builds get none.
        self.assertEqual(
            ast.literal_eval(assignments["GTK_HEADER_COPTS"].args[0]),
            {
                ":gtk_x86_64_cpu": ["-mfpmath=sse", "-msse", "-msse2"],
                "//conditions:default": [],
            },
        )
        targets = package_cc_targets()
        closure = {name for name in gtk_header_closure(targets) if targets[name].get("srcs")}
        self.assertTrue(closure, "no GTK interface consumers found in the package")
        carrier = {
            name for name, fields in targets.items() if ":generated_resources" in string_literals(fields.get("srcs"))
        }
        for name in closure:
            with self.subTest(gtk_interface_consumer=name):
                extras = copts_beyond_header_contract(targets[name].get("copts"))
                self.assertIsNotNone(
                    extras,
                    f"{name} compiles GTK interface headers and must restate GTK_HEADER_COPTS",
                )
                self.assertLessEqual(set(extras), {"-Wno-overlength-strings"})
                if name in carrier:
                    self.assertEqual(extras, ["-Wno-overlength-strings"])
                else:
                    self.assertEqual(extras, [])
        self.assertLessEqual(carrier, closure)

    def test_resources_library_compiles_only_the_generated_bundle(self):
        fields = fields_for("app/linux-gtk", "cc_library", "resources")
        self.assertEqual(ast.literal_eval(fields["srcs"]), [":generated_resources"])
        # The generated C strings exceed the standard limit; only this target
        # carries the suppression the CMake build sets on resources.c, on top of
        # the shared GTK header contract.
        self.assertEqual(copts_beyond_header_contract(fields["copts"]), ["-Wno-overlength-strings"])
        self.assertIs(ast.literal_eval(fields["linkstatic"]), True)
        self.assertEqual(ast.literal_eval(fields["target_compatible_with"]), ["@platforms//os:linux"])
        self.assertEqual(ast.literal_eval(fields["implementation_deps"]), ["@aobus_native_deps//:gtkmm"])


class GResourceRuleTest(unittest.TestCase):
    """The real gresource_c rule protocol with only external Bazel APIs recorded."""

    def setUp(self):
        self.rule = execute_gresource_rule()

    def test_gresource_rule_renders_output_and_source_root_arguments(self):
        self.assertCountEqual(
            self.rule.argv,
            [
                "--target=bazel-out/test-cfg/bin/fixture/generated/theme.c",
                "--sourcedir=fixture/resources",
                "--sourcedir=.",
                "--generate-source",
                "--c-name=aobus",
                "fixture/resources/theme.gresource.xml",
            ],
        )
        # Root priority matters if both roots contain a resource with the same name.
        self.assertEqual(
            [argument for argument in self.rule.argv if argument.startswith("--sourcedir=")],
            ["--sourcedir=fixture/resources", "--sourcedir=."],
        )

    def test_gresource_rule_declares_the_resource_action_inputs_outputs_and_tool_runtime(self):
        run = self.rule.run
        # The XML stays the membership authority; input declaration order is immaterial.
        self.assertCountEqual(run["inputs"], [self.rule.xml, *self.rule.sources])
        self.assertEqual(run["outputs"], [self.rule.out])
        # The host tool is the exec-config executable, and its runtime filegroup
        # rides along as the action's transitive tool input.
        self.assertIs(run["executable"], self.rule.tool)
        (arguments,) = run["arguments"]
        self.assertTrue(any(arguments is recorded for recorded in self.rule.recorded_arguments))
        self.assertEqual(run["tools"].direct, [self.rule.tool])
        self.assertEqual(run["tools"].transitive, [self.rule.runtime])
        # The rule publishes exactly the generated source.
        self.assertEqual(self.rule.result[0].files.direct, [self.rule.out])

    def test_gresource_rule_declares_the_exec_tool_and_runtime_attributes(self):
        attrs = self.rule.descriptor["attrs"]
        for name, kind in (
            ("srcs", "label_list"),
            ("xml", "label"),
            ("out", "output"),
            ("_glib_compile_resources", "label"),
            ("_glib_compile_resources_runtime", "label"),
        ):
            with self.subTest(attribute=name):
                self.assertEqual(attrs[name]["kind"], kind)
        tool = attrs["_glib_compile_resources"]
        self.assertIs(tool["executable"], True)
        self.assertIs(tool["allow_single_file"], True)
        self.assertEqual(tool["cfg"], "exec")
        self.assertEqual(tool["default"], "@aobus_native_tools//:glib_compile_resources")
        runtime = attrs["_glib_compile_resources_runtime"]
        self.assertFalse(runtime.get("executable", False))
        self.assertEqual(runtime["cfg"], "exec")
        self.assertEqual(runtime["default"], "@aobus_native_tools//:glib_compile_resources_runtime")
        # The XML and declared srcs stay mandatory inputs; the generated C file
        # is a declared output.
        self.assertIs(attrs["srcs"]["mandatory"], True)
        self.assertIs(attrs["srcs"]["allow_files"], True)
        self.assertEqual(attrs["xml"]["allow_single_file"], [".gresource.xml"])
        self.assertIs(attrs["xml"]["mandatory"], True)
        self.assertIs(attrs["out"]["mandatory"], True)


if __name__ == "__main__":
    unittest.main()
