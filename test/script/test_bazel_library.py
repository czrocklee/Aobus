"""Keep the bounded Bazel storage graph aligned with its CMake-owned sources."""

import ast
import re
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from ao.core import bazel_dependencies as dependencies
from ao.core.paths import PROJECT_ROOT


def cmake_sources(directory: str, target: str) -> set[str]:
    source = (PROJECT_ROOT / directory / "CMakeLists.txt").read_text(encoding="utf-8")
    blocks = re.findall(rf"(?:add_library|target_sources)\(\s*{target}\b([^)]*)\)", source, re.DOTALL)
    if not blocks:
        raise AssertionError(f"No CMake compilation units found for {target}")
    return set(re.findall(r"[\w./-]+\.cpp\b", "\n".join(blocks)))


def bazel_sources(directory: str, targets: set[str]) -> set[str]:
    source = ast.parse((PROJECT_ROOT / directory / "BUILD.bazel").read_text(encoding="utf-8"))
    found = set()
    sources = set()
    for node in ast.walk(source):
        if not isinstance(node, ast.Call) or not isinstance(node.func, ast.Name):
            continue
        if node.func.id not in ("cc_library", "cc_test"):
            continue
        fields = {keyword.arg: keyword.value for keyword in node.keywords}
        name = fields.get("name")
        if not isinstance(name, ast.Constant) or name.value not in targets:
            continue
        found.add(name.value)
        for value in ast.walk(fields["srcs"]):
            if isinstance(value, ast.Constant) and isinstance(value.value, str) and value.value.endswith(".cpp"):
                sources.add(value.value)
    if found != targets:
        raise AssertionError(f"Missing Bazel targets: {targets - found}")
    return sources


class BazelLibraryGraphTest(unittest.TestCase):
    def test_bazel_libraries_compile_the_same_units_as_cmake_on_all_platforms(self):
        for directory, cmake_target, bazel_targets in (
            ("lib/utility", "ao_utility", {"utility", "path"}),
            ("lib/utility", "ao_unicode_text", {"unicode_text"}),
            ("lib/lmdb", "ao_lmdb", {"lmdb"}),
            ("lib/library", "ao_library", {"library"}),
        ):
            with self.subTest(target=cmake_target):
                self.assertEqual(bazel_sources(directory, bazel_targets), cmake_sources(directory, cmake_target))

    def test_library_test_keeps_all_existing_storage_cases(self):
        sources = bazel_sources("test", {"library_test"})
        for directory in ("library", "lmdb", "macos/lmdb"):
            with self.subTest(directory=directory):
                expected = {
                    path.relative_to(PROJECT_ROOT / "test").as_posix()
                    for path in (PROJECT_ROOT / "test" / directory).glob("*Test.cpp")
                }
                actual = {path for path in sources if path.startswith(directory + "/") and path.endswith("Test.cpp")}
                self.assertEqual(actual, expected)

    def test_project_libraries_preserve_cmake_static_linkage(self):
        for directory, names in (
            ("lib/utility", {"utility", "path", "unicode_text"}),
            ("lib/lmdb", {"lmdb"}),
            ("lib/library", {"library"}),
            ("test", {"library_test_support"}),
        ):
            source = ast.parse((PROJECT_ROOT / directory / "BUILD.bazel").read_text(encoding="utf-8"))
            found = set()
            for node in ast.walk(source):
                if not isinstance(node, ast.Call) or not isinstance(node.func, ast.Name):
                    continue
                if node.func.id != "cc_library":
                    continue
                fields = {keyword.arg: keyword.value for keyword in node.keywords}
                name = fields.get("name")
                if not isinstance(name, ast.Constant) or name.value not in names:
                    continue
                with self.subTest(directory=directory, target=name.value):
                    self.assertIs(ast.literal_eval(fields["linkstatic"]), True)
                found.add(name.value)
            self.assertEqual(found, names)

    def test_platform_specific_sources_are_selected_not_compiled_together(self):
        for directory, target, branches in (
            (
                "lib/utility",
                "utility",
                {
                    "@platforms//os:windows": [
                        "AtomicFileWindows.cpp",
                        "FileAllocationWindows.cpp",
                        "PlatformDirectoriesWindows.cpp",
                    ],
                    "//conditions:default": [
                        "AtomicFilePosix.cpp",
                        "FileAllocationPosix.cpp",
                        "PlatformDirectoriesPosix.cpp",
                    ],
                },
            ),
            (
                "lib/lmdb",
                "lmdb",
                {
                    "@platforms//os:windows": ["detail/EnvironmentDataFileWindows.cpp"],
                    "//conditions:default": ["detail/EnvironmentDataFilePosix.cpp"],
                },
            ),
            (
                "lib/library",
                "library",
                {
                    "@platforms//os:windows": ["LockFileWindows.cpp"],
                    "//conditions:default": ["LockFilePosix.cpp"],
                },
            ),
            (
                "test",
                "library_test",
                {
                    "@platforms//os:macos": ["macos/lmdb/LmdbProbeScratchTest.cpp"],
                    "//conditions:default": [],
                },
            ),
        ):
            source = ast.parse((PROJECT_ROOT / directory / "BUILD.bazel").read_text(encoding="utf-8"))
            selected = []
            for node in ast.walk(source):
                if not isinstance(node, ast.Call) or not isinstance(node.func, ast.Name):
                    continue
                if node.func.id not in ("cc_library", "cc_test"):
                    continue
                fields = {keyword.arg: keyword.value for keyword in node.keywords}
                name = fields.get("name")
                if not isinstance(name, ast.Constant) or name.value != target:
                    continue
                for child in ast.walk(fields["srcs"]):
                    if isinstance(child, ast.Call) and isinstance(child.func, ast.Name) and child.func.id == "select":
                        selected.append(ast.literal_eval(child.args[0]))
            with self.subTest(target=target):
                self.assertEqual(selected, [branches])

    def test_project_graph_has_no_generic_dependency_compile_option_obligations(self):
        paths = [
            path
            for directory in ("include", "lib", "test", "app/runtime")
            for path in (PROJECT_ROOT / directory).rglob("BUILD.bazel")
        ]
        self.assertTrue(paths)
        for path in paths:
            source = path.read_text(encoding="utf-8")
            with self.subTest(path=path.relative_to(PROJECT_ROOT).as_posix()):
                self.assertNotIn("interfaces.bzl", source)
                self.assertNotRegex(source, r"\b(?:PACKAGE_COPTS|NATIVE_COPTS|TEST_COPTS)\b")
                # Native audio/test owners and lint-context canaries supply
                # their own ABI/replay flags, not dependency-wide obligations.
                if path.parent in {PROJECT_ROOT / "lib/audio", PROJECT_ROOT / "test", PROJECT_ROOT / "test/lint"}:
                    continue
                for node in ast.walk(ast.parse(source)):
                    if isinstance(node, ast.Call):
                        self.assertNotIn("copts", {keyword.arg for keyword in node.keywords})

    def test_common_header_interface_propagates_watched_toolchain_identity(self):
        tree = ast.parse((PROJECT_ROOT / "include/BUILD.bazel").read_text(encoding="utf-8"))
        fields = next(
            {keyword.arg: keyword.value for keyword in node.keywords}
            for node in ast.walk(tree)
            if isinstance(node, ast.Call)
            and isinstance(node.func, ast.Name)
            and node.func.id == "cc_library"
            and any(
                keyword.arg == "name" and ast.literal_eval(keyword.value) == "utility_headers"
                for keyword in node.keywords
            )
        )
        self.assertEqual(ast.literal_eval(fields["deps"]), ["//bazel:compile_contract"])

    def test_contract_checks_use_a_headerless_project_interface_not_global_rc_options(self):
        tree = ast.parse((PROJECT_ROOT / "bazel/BUILD.bazel").read_text(encoding="utf-8"))
        fields = next(
            {keyword.arg: keyword.value for keyword in node.keywords}
            for node in ast.walk(tree)
            if isinstance(node, ast.Call)
            and isinstance(node.func, ast.Name)
            and node.func.id == "cc_library"
            and any(
                keyword.arg == "name" and ast.literal_eval(keyword.value) == "compile_contract"
                for keyword in node.keywords
            )
        )
        self.assertFalse({"hdrs", "srcs"} & fields.keys())
        self.assertNotIn("deps", fields)
        flags = (PROJECT_ROOT / ".bazelrc").read_text(encoding="utf-8").splitlines()
        self.assertFalse(any("--cxxopt=" in line and "gsl_CONFIG_" in line for line in flags))


class BazelCompileInterfacePatchTest(unittest.TestCase):
    def test_equivalent_metadata_include_paths_do_not_create_redundant_patches(self):
        with tempfile.TemporaryDirectory(prefix="bazel provider ") as directory:
            root = Path(directory).resolve()
            include = root / "include"
            include.mkdir()
            pc = root / "debug/lib/pkgconfig"
            pc.mkdir(parents=True)
            equivalent = pc / "../../../include"
            spec = {"platform": "macos", "config": "Debug", "provider": {"name": "vcpkg", "version": "fixture"}}
            for flags in (
                [f"-I{equivalent}"],
                [f"-isystem{equivalent}"],
                ["-I", str(equivalent)],
                ["-isystem", str(equivalent)],
            ):
                with self.subTest(flags=flags), patch.object(dependencies, "_metadata_flags", return_value=flags):
                    self.assertIsNone(
                        dependencies._compile_interface_patch(spec, "lmdb", {"header_dirs": [str(include)]})
                    )

    def test_metadata_flag_tokenization_matches_the_adapter_for_quoted_paths(self):
        spec = {
            "linkage": "static",
            "pkg_config": {"executable": "pkgconf", "pc_dirs": []},
            "packages": [{"name": "lmdb", "modules": ["lmdb"]}],
        }
        output = '-I"/provider with spaces/include" -DC4_CPP=17 /permissive-'
        with patch.object(dependencies.subprocess, "run", return_value=subprocess.CompletedProcess([], 0, output)):
            self.assertEqual(
                dependencies._metadata_flags(spec, "--cflags", package="lmdb"),
                ["-I/provider with spaces/include", "-DC4_CPP=17", "/permissive-"],
            )


if __name__ == "__main__":
    unittest.main()
