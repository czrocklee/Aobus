"""Complete authored test-case ownership and required support-role contracts.

Source inspection only: this reads BUILD/CMake declarations and expands their
declared globs. It does not execute Bazel, simulate a configured graph, or claim
compiler/linker/runtime acceptance.
"""

import ast
import posixpath
import re
import shlex
import unittest
from functools import cache
from pathlib import Path
from types import SimpleNamespace

from ao.core.paths import PROJECT_ROOT

_COMPILATION_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".m", ".mm"}
_CASE_SUFFIXES = ("Test.cpp", "Benchmark.cpp")
_CMAKE_SOURCE_COMMANDS = {"add_executable", "add_library", "target_sources", "aobus_target_test_sources"}
_CMAKE_CONTROL_COMMANDS = {"if", "elseif", "else", "endif"}
_CMAKE_KEYWORDS = {
    "WIN32",
    "MACOSX_BUNDLE",
    "EXCLUDE_FROM_ALL",
    "STATIC",
    "OBJECT",
    "INTERFACE",
    "SHARED",
    "MODULE",
    "PRIVATE",
    "PUBLIC",
    "IMPORTED",
    "ALIAS",
}


def _package_root(package):
    return PROJECT_ROOT if package in ("", ".") else PROJECT_ROOT / package


def _join(package, path):
    base = "" if package in ("", ".") else package
    return posixpath.normpath(posixpath.join(base, path))


def _project_path(package, value):
    """Resolve one Bazel file token to a project-relative path, or None for labels."""
    if value.startswith("${CMAKE_SOURCE_DIR}/"):
        return value.removeprefix("${CMAKE_SOURCE_DIR}/")
    if value.startswith("${CMAKE_CURRENT_SOURCE_DIR}/"):
        return _join(package, value.removeprefix("${CMAKE_CURRENT_SOURCE_DIR}/"))
    if value.startswith("//conditions:"):
        return None
    if value.startswith("@"):
        return None
    if value.startswith("//"):
        package_name, _, path = value[2:].partition(":")
        if not path or "." not in Path(path).name:
            return None
        return _join(package_name, path)
    if value.startswith(":"):
        path = value[1:]
        if "." not in Path(path).name:
            return None
        return _join(package, path)
    if "$" in value:
        return None
    if Path(value).is_absolute():
        try:
            return Path(value).relative_to(PROJECT_ROOT).as_posix()
        except ValueError:
            return None
    if "." not in Path(value).name:
        return None
    return _join(package, value)


def _expand_glob(package, patterns):
    root = _package_root(package)
    found = set()
    for pattern in patterns:
        for path in root.glob(pattern):
            if path.is_file():
                found.add(path.relative_to(PROJECT_ROOT).as_posix())
    return found


def _glob_sources(package, call):
    include = (
        call.args[0] if call.args else next(keyword.value for keyword in call.keywords if keyword.arg == "include")
    )
    sources = _expand_glob(package, ast.literal_eval(include))
    excluded = next((keyword.value for keyword in call.keywords if keyword.arg == "exclude"), None)
    if excluded is not None:
        sources -= _expand_glob(package, ast.literal_eval(excluded))
    return sources


@cache
def _bazel_module(package):
    tree = ast.parse((_package_root(package) / "BUILD.bazel").read_text(encoding="utf-8"))
    assignments = {
        node.targets[0].id: node.value
        for node in tree.body
        if isinstance(node, ast.Assign) and len(node.targets) == 1 and isinstance(node.targets[0], ast.Name)
    }
    return tree, assignments


def _rule_fields_from_tree(tree, kind):
    return [
        {keyword.arg: keyword.value for keyword in node.keywords}
        for node in ast.walk(tree)
        if isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and node.func.id == kind
    ]


def bazel_rules(package, kind):
    tree, _ = _bazel_module(package)
    return _rule_fields_from_tree(tree, kind)


def bazel_rule_fields(package, kind, name):
    for fields in bazel_rules(package, kind):
        if ast.literal_eval(fields["name"]) == name:
            return fields
    raise AssertionError(f"Missing Bazel rule {kind} {package}:{name}")


def _collect_sources(node, package, assignments, seen):
    if isinstance(node, ast.Name):
        if node.id in seen or node.id not in assignments:
            return set()
        return _collect_sources(assignments[node.id], package, assignments, seen | {node.id})
    if isinstance(node, (ast.List, ast.Tuple)):
        sources = set()
        for element in node.elts:
            sources |= _collect_sources(element, package, assignments, seen)
        return sources
    if isinstance(node, ast.BinOp) and isinstance(node.op, ast.Add):
        return _collect_sources(node.left, package, assignments, seen) | _collect_sources(
            node.right, package, assignments, seen
        )
    if isinstance(node, ast.Constant) and isinstance(node.value, str):
        path = _project_path(package, node.value)
        return {path} if path is not None else set()
    if isinstance(node, ast.Call) and isinstance(node.func, ast.Name):
        if node.func.id == "glob":
            return _glob_sources(package, node)
        if node.func.id == "select":
            sources = set()
            for value in node.args[0].values:
                sources |= _collect_sources(value, package, assignments, seen)
            return sources
    raise AssertionError("Unsupported Bazel source expression: " + ast.unparse(node))


def _merge_branch_maps(left, right):
    result = {}
    for key in set(left) | set(right):
        if key == "//conditions:default":
            result[key] = left.get(key, set()) | right.get(key, set())
        else:
            result[key] = left.get(key, left.get("//conditions:default", set())) | right.get(
                key, right.get("//conditions:default", set())
            )
    return result


def _collect_source_branches(node, package, assignments, seen):
    if isinstance(node, ast.Name):
        if node.id in seen or node.id not in assignments:
            return {"//conditions:default": set()}
        return _collect_source_branches(assignments[node.id], package, assignments, seen | {node.id})
    if isinstance(node, ast.BinOp) and isinstance(node.op, ast.Add):
        return _merge_branch_maps(
            _collect_source_branches(node.left, package, assignments, seen),
            _collect_source_branches(node.right, package, assignments, seen),
        )
    if isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and node.func.id == "select":
        return {
            ast.literal_eval(key): _collect_sources(value, package, assignments, seen)
            for key, value in zip(node.args[0].keys, node.args[0].values, strict=True)
        }
    return {"//conditions:default": _collect_sources(node, package, assignments, seen)}


def bazel_rule_sources(package, kind, name, attribute="srcs"):
    fields = bazel_rule_fields(package, kind, name)
    _, assignments = _bazel_module(package)
    return _collect_sources(fields[attribute], package, assignments, set())


def bazel_rule_source_branches(package, kind, name, attribute="srcs"):
    fields = bazel_rule_fields(package, kind, name)
    _, assignments = _bazel_module(package)
    return _collect_source_branches(fields[attribute], package, assignments, set())


def bazel_rule_tags(fields):
    tags = fields.get("tags")
    return set(ast.literal_eval(tags)) if tags is not None else set()


def bazel_rule_target_compatible(fields):
    values = fields.get("target_compatible_with")
    return set(ast.literal_eval(values)) if values is not None else set()


def bazel_rule_has_manifest(fields):
    return (
        "additional_linker_inputs" in fields
        and "WINDOWS_UTF8_LINKER_INPUTS" in ast.unparse(fields["additional_linker_inputs"])
        and "linkopts" in fields
        and "WINDOWS_UTF8_LINKOPTS" in ast.unparse(fields["linkopts"])
    )


def declared_labels(node):
    return {
        child.value
        for child in ast.walk(node)
        if isinstance(child, ast.Constant)
        and isinstance(child.value, str)
        and (child.value.startswith(":") or child.value.startswith("//") or child.value.startswith("@"))
        and child.value != "//conditions:default"
        and not child.value.startswith("@platforms//")
    }


def compilation_units(paths):
    return {path for path in paths if Path(path).suffix in _COMPILATION_SUFFIXES}


def case_sources(paths):
    return {path for path in paths if path.endswith(_CASE_SUFFIXES)}


def _scan_parenthesized(text, start):
    depth = 0
    quoted = False
    index = start
    while index < len(text):
        character = text[index]
        if character == '"':
            quoted = not quoted
        elif not quoted:
            if character == "(":
                depth += 1
            elif character == ")":
                depth -= 1
                if depth == 0:
                    return index
        index += 1
    raise AssertionError("Unbalanced CMake command")


def _cmake_args(body):
    return [
        token[1:-1] if token.startswith('"') and token.endswith('"') else token
        for token in re.findall(r'"[^"]*"|\S+', body)
    ]


def _cmake_commands(path):
    text = path.read_text(encoding="utf-8")
    lines = text.splitlines(keepends=True)
    offsets = []
    offset = 0
    for line in lines:
        offsets.append(offset)
        offset += len(line)
    conditions = []
    commands = []
    index = 0
    while index < len(lines):
        stripped = lines[index].lstrip()
        if not stripped or stripped.startswith("#"):
            index += 1
            continue
        match = re.match(r"([A-Za-z_][A-Za-z0-9_]*)\s*\(", stripped)
        if not match:
            index += 1
            continue
        name = match.group(1).lower()
        start = offsets[index] + len(lines[index]) - len(stripped) + match.end() - 1
        end = _scan_parenthesized(text, start)
        body = text[start + 1 : end]
        end_line = text.count("\n", 0, end)
        if name in _CMAKE_CONTROL_COMMANDS:
            condition = " ".join(body.split())
            if name == "if":
                conditions.append(condition)
            elif name == "elseif":
                if conditions:
                    conditions[-1] = condition
                else:
                    conditions.append(condition)
            elif name == "else":
                if conditions:
                    conditions[-1] = "else"
                else:
                    conditions.append("else")
            elif conditions:
                conditions.pop()
        elif name in _CMAKE_SOURCE_COMMANDS:
            commands.append((name, tuple(_cmake_args(body)), conditions[-1] if conditions else None))
        index = end_line + 1
    return commands


def _normalize_condition(value):
    if value is None:
        return None
    value = value.strip()
    if value.startswith("${") and value.endswith("}"):
        value = value[2:-1]
    return value or None


def _cmake_source(package, token):
    if token.startswith("${CMAKE_SOURCE_DIR}/"):
        return token.removeprefix("${CMAKE_SOURCE_DIR}/")
    if token.startswith("${CMAKE_CURRENT_SOURCE_DIR}/"):
        return _join(package, token.removeprefix("${CMAKE_CURRENT_SOURCE_DIR}/"))
    if "$" in token:
        return None
    if Path(token).is_absolute():
        try:
            return Path(token).relative_to(PROJECT_ROOT).as_posix()
        except ValueError:
            return None
    if "." not in Path(token).name:
        return None
    return _join(package, token)


def _cmake_declarations(path, target):
    package = path.parent.relative_to(PROJECT_ROOT).as_posix()
    declarations = []
    for name, arguments, context in _cmake_commands(path):
        if not arguments or arguments[0] != target:
            continue
        condition = context
        if name in {"add_executable", "add_library"}:
            tokens = arguments[1:]
        elif name == "target_sources":
            tokens = arguments[1:]
        else:
            condition = _normalize_condition(arguments[1]) if len(arguments) > 1 else None
            tokens = arguments[3:] if len(arguments) > 3 else []
        sources = {
            source
            for token in tokens
            if token not in _CMAKE_KEYWORDS
            for source in [_cmake_source(package, token)]
            if source is not None
        }
        if sources:
            declarations.append((_normalize_condition(condition), sources))
    return declarations


def cmake_target_sources(path, target):
    sources = set()
    for _, declaration in _cmake_declarations(path, target):
        sources |= declaration
    return sources


def cmake_target_conditional_sources(path, target):
    result = {}
    for condition, sources in _cmake_declarations(path, target):
        if condition is not None:
            result.setdefault(condition, set()).update(sources)
    return result


def cmake_target_unconditional_sources(path, target):
    sources = set()
    for condition, declaration in _cmake_declarations(path, target):
        if condition is None:
            sources |= declaration
    return sources


def cmake_condition_platform(condition):
    if not condition:
        return None
    text = condition.upper()
    if "NOT WIN32" in text:
        return "//conditions:default"
    if "WIN32" in text:
        return "@platforms//os:windows"
    if "APPLE" in text:
        return "@platforms//os:macos"
    if "LINUX" in text or "SYSTEM_MEDIA" in text:
        return "@platforms//os:linux"
    return None


def cmake_platform_sources(path, target):
    result = {}
    for condition, sources in cmake_target_conditional_sources(path, target).items():
        platform = cmake_condition_platform(condition)
        if platform is not None:
            result.setdefault(platform, set()).update(sources)
    return result


def cmake_registered_cases(path):
    cases = set()
    for _, arguments, _ in _cmake_commands(path):
        for token in arguments:
            source = _cmake_source(path.parent.relative_to(PROJECT_ROOT).as_posix(), token)
            if source is not None and source.endswith(_CASE_SUFFIXES):
                cases.add(source)
    return cases


def bazel_case_owners(package="test"):
    owners = {}
    for kind in ("cc_test", "cc_binary"):
        for fields in bazel_rules(package, kind):
            name = ast.literal_eval(fields["name"])
            if kind == "cc_binary" and "manual" not in bazel_rule_tags(fields):
                continue
            for source in case_sources(bazel_rule_sources(package, kind, name)):
                owners.setdefault(source, set()).add((kind, name))
    return owners


class BazelTestGraphOwnershipTest(unittest.TestCase):
    def test_every_cmake_case_has_exactly_one_bazel_owner(self):
        cmake_cases = cmake_registered_cases(PROJECT_ROOT / "test/CMakeLists.txt")
        owners = bazel_case_owners()
        self.assertTrue(cmake_cases)
        self.assertEqual(set(owners), cmake_cases)
        for source, targets in owners.items():
            with self.subTest(source=source):
                self.assertEqual(len(targets), 1)

    def test_custom_test_mains_are_owned_by_their_runtime_targets(self):
        runtime = bazel_rule_sources("test", "cc_test", "runtime_test")
        gtk = bazel_rule_sources("test", "cc_test", "gtk_test")
        self.assertIn("test/CoreTestMain.cpp", runtime)
        self.assertIn("test/linux-gtk/GtkTestMain.cpp", gtk)
        for fields in bazel_rules("test", "cc_test"):
            name = ast.literal_eval(fields["name"])
            sources = bazel_rule_sources("test", "cc_test", name)
            with self.subTest(target=name):
                if name != "runtime_test":
                    self.assertNotIn("test/CoreTestMain.cpp", sources)
                if name != "gtk_test":
                    self.assertNotIn("test/linux-gtk/GtkTestMain.cpp", sources)

    def test_standalone_probe_mains_match_their_cmake_consumers(self):
        cmake = PROJECT_ROOT / "test/CMakeLists.txt"
        for target, kind in (
            ("ao_fatal_probe", "cc_binary"),
            ("ao_audio_fatal_probe", "cc_binary"),
            ("ao_library_probe", "cc_binary"),
            ("ao_tui_signal_probe", "cc_binary"),
            ("ao_coreaudio_output_probe", "cc_binary"),
            ("ao_detached_process_probe", "cc_binary"),
            ("ao_gapplication_probe", "cc_binary"),
        ):
            with self.subTest(target=target):
                self.assertEqual(
                    compilation_units(bazel_rule_sources("test", kind, target)),
                    compilation_units(cmake_target_sources(cmake, target)),
                )

    def test_shared_fatal_protocols_stay_out_of_scenario_owners(self):
        cmake = PROJECT_ROOT / "test/CMakeLists.txt"
        support = compilation_units(bazel_rule_sources("test", "cc_library", "fatal_probe_support"))
        self.assertEqual(support, compilation_units(cmake_target_sources(cmake, "ao_fatal_probe_support")))
        self.assertFalse(any(source.endswith("Scenario.cpp") for source in support))
        self.assertIn("test/fatal/RuntimeFatalProbeScenario.cpp", bazel_rule_sources("test", "cc_test", "runtime_test"))
        self.assertIn("test/fatal/FatalProbeScenario.cpp", bazel_rule_sources("test", "cc_binary", "ao_fatal_probe"))
        self.assertIn(
            "test/fatal/AudioFatalProbeScenario.cpp",
            bazel_rule_sources("test", "cc_binary", "ao_audio_fatal_probe"),
        )

    def test_winui_probes_match_their_cmake_consumers(self):
        cmake = PROJECT_ROOT / "app/windows-winui/CMakeLists.txt"
        probes = {
            target
            for target in (
                arguments[0] for name, arguments, _ in _cmake_commands(cmake) if name == "add_executable" and arguments
            )
            if target.startswith("ao_winui_") and target.endswith("_probe")
        }
        self.assertTrue(probes)
        for target in probes:
            with self.subTest(target=target):
                self.assertEqual(
                    compilation_units(bazel_rule_sources("app/windows-winui", "winui_probe", target)),
                    {
                        source
                        for source in compilation_units(cmake_target_sources(cmake, target))
                        if source.startswith("test/")
                    },
                )

    def test_appkit_smoke_reuses_the_cmake_scenario_units(self):
        cmake = PROJECT_ROOT / "test/CMakeLists.txt"
        expected = {source for source in cmake_target_sources(cmake, "ao_appkit_smoke") if source.endswith(".mm")}
        actual = {
            source
            for source in bazel_rule_sources("app/macos-appkit", "objc_library", "appkit_smoke")
            if source.endswith(".mm")
        }
        self.assertTrue(expected)
        self.assertEqual(actual, expected)
        fields = bazel_rule_fields("app/macos-appkit", "objc_library", "appkit_smoke")
        self.assertEqual(bazel_rule_target_compatible(fields), {"@platforms//os:macos"})


class BazelTestGraphDefaultsTest(unittest.TestCase):
    def test_default_targets_are_the_single_root_suite(self):
        from ao.core.bazel_workspace import DEFAULT_TEST_TARGETS

        self.assertEqual(DEFAULT_TEST_TARGETS, ("//:tests",))
        root = bazel_rule_fields(".", "test_suite", "tests")
        root_tests = declared_labels(root["tests"])
        non_manual = {
            "//test:" + ast.literal_eval(fields["name"])
            for fields in bazel_rules("test", "cc_test")
            if "manual" not in bazel_rule_tags(fields)
        }
        self.assertEqual(root_tests, non_manual)

    def test_convenience_suites_remain_subsets_of_the_root_suite(self):
        root_tests = declared_labels(bazel_rule_fields(".", "test_suite", "tests")["tests"])
        suites = {
            ast.literal_eval(fields["name"]): declared_labels(fields["tests"])
            for fields in bazel_rules(".", "test_suite")
            if ast.literal_eval(fields["name"]) != "tests"
        }
        self.assertTrue(suites)
        for name, tests in suites.items():
            with self.subTest(suite=name):
                self.assertTrue(tests)
                self.assertLessEqual(tests, root_tests)
        self.assertIn("//test:systemmedia_test", suites["tui_system_media_tests"])
        self.assertEqual(suites["audio_tests"], {"//test:audio_test", "//test:audio_platform_test"})
        self.assertEqual(suites["media_tests"], {"//test:media_test", "//test:media_fixture_test"})

    def test_manual_opt_in_targets_are_not_default(self):
        root_tests = declared_labels(bazel_rule_fields(".", "test_suite", "tests")["tests"])
        for kind, name in (
            ("cc_test", "backend_test"),
            ("cc_test", "gtk_test"),
            ("cc_binary", "ao_perf_baseline"),
            ("cc_binary", "ao_tui_bench"),
        ):
            fields = bazel_rule_fields("test", kind, name)
            with self.subTest(target=name):
                self.assertIn("manual", bazel_rule_tags(fields))
                self.assertNotIn("//test:" + name, root_tests)

    def test_declared_test_input_manifests_execute_the_real_rule(self):
        manifest = bazel_rule_fields("test", "test_input_manifest", "winui_test_inputs")
        self.assertEqual(ast.literal_eval(manifest["srcs"]), ["//app/windows-winui:native_test_inputs"])
        output_name = ast.literal_eval(manifest["output_name"])
        self.assertEqual(output_name, "winui_test_inputs.txt")
        inputs = bazel_rule_sources("app/windows-winui", "filegroup", "native_test_inputs")
        self.assertTrue(inputs)
        self.assertTrue({"app/windows-winui/App.xaml", "app/windows-winui/MainWindow.xaml"} <= inputs)
        test = bazel_rule_fields("test", "cc_test", "winui_test")
        self.assertIn("//app/windows-winui:model_test_inputs", declared_labels(test["data"]))
        self.assertIn(":winui_test_inputs", declared_labels(test["data"]))
        self.assertIn(":winui_test_inputs", ast.literal_eval(test["data"].right.args[0])["@platforms//os:windows"])
        defines = ast.literal_eval(test["local_defines"])
        self.assertEqual(len(defines), 1)
        macro = shlex.split(defines[0])
        self.assertEqual(len(macro), 1)
        name, _, value = macro[0].partition("=")
        self.assertEqual(name, "AOBUS_BAZEL_TEST_INPUTS")
        self.assertEqual(value, f'"test/{output_name}"')

        class RecordedFile:
            def __init__(self, path, short_path=None):
                self.path = path
                self.short_path = path if short_path is None else short_path

        class RecordedDepset:
            def __init__(self, direct=()):
                self.direct = list(direct)

        class RecordedRunfiles:
            def __init__(self, files=()):
                self.files = list(files)

        class RecordedDefaultInfo:
            def __init__(self, files=None, runfiles=None):
                self.files = files
                self.runfiles = runfiles

        class RecordedActions:
            def __init__(self):
                self.declared = []
                self.writes = []

            def declare_file(self, name):
                declared = RecordedFile("bazel-out/test/bin/" + name, short_path=name)
                self.declared.append(declared)
                return declared

            def write(self, output, content):
                self.writes.append((output, content))

        def depset(direct=()):
            return RecordedDepset(direct)

        def rule(**kwargs):
            return kwargs

        namespace = {
            "attr": SimpleNamespace(
                label_list=lambda **kwargs: {"kind": "label_list", **kwargs},
                string=lambda **kwargs: {"kind": "string", **kwargs},
            ),
            "DefaultInfo": RecordedDefaultInfo,
            "depset": depset,
            "rule": rule,
        }
        source = (PROJECT_ROOT / "test/input_files.bzl").read_text(encoding="utf-8")
        exec(compile(source, "test/input_files.bzl", "exec"), namespace)
        descriptor = namespace["test_input_manifest"]
        self.assertEqual(descriptor["attrs"]["srcs"]["kind"], "label_list")
        self.assertIs(descriptor["attrs"]["srcs"]["allow_files"], True)
        self.assertEqual(descriptor["attrs"]["output_name"]["kind"], "string")
        self.assertIs(descriptor["attrs"]["output_name"]["mandatory"], True)
        first = RecordedFile("bazel-out/k8-fastbuild/bin/test/native/one.cpp", short_path="test/one.cpp")
        second = RecordedFile("bazel-out/k8-fastbuild/bin/test/native/two.h", short_path="test/two.h")
        actions = RecordedActions()
        ctx = SimpleNamespace(
            attr=SimpleNamespace(output_name=output_name),
            files=SimpleNamespace(srcs=[first, second]),
            actions=actions,
            runfiles=lambda files=(): RecordedRunfiles(files),
        )
        result = descriptor["implementation"](ctx)
        self.assertEqual(len(result), 1)
        info = result[0]
        self.assertIsInstance(info, RecordedDefaultInfo)
        manifest_file = actions.declared[0]
        self.assertIs(actions.writes[0][0], manifest_file)
        self.assertEqual(actions.writes[0][1], "test/one.cpp\ntest/two.h\n")
        self.assertEqual(info.files.direct, [manifest_file])
        self.assertEqual(info.runfiles.files, [first, second, manifest_file])
        self.assertNotEqual(manifest_file.path, manifest_file.short_path)


if __name__ == "__main__":
    unittest.main()
