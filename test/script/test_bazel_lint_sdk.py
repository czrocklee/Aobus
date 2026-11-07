"""Ready-only lint SDK selection and its projected Bazel consumer interfaces.

Tool queries are the external I/O seam. Tiny native metadata and archive files
exercise real selection, validation and the checked-in Starlark projection;
these are not linker or ABI acceptance tests and do not reconstruct the complete
native SDK graph.
"""

import ast
import json
import os
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path, PurePosixPath
from types import SimpleNamespace
from unittest.mock import patch

from ao.core import bazel_lint_sdk as sdk


def _write(path, text="fixture\n"):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")
    return path


def _calls(source, name):
    return [
        node
        for node in ast.walk(ast.parse(source))
        if isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and node.func.id == name
    ]


def _starlark(path, names, functions):
    """Execute selected declarations of a checked-in Starlark file."""
    tree = ast.parse(path.read_text(encoding="utf-8"))
    declarations = [
        node
        for node in tree.body
        if (isinstance(node, ast.FunctionDef) and node.name in names)
        or (
            isinstance(node, ast.Assign)
            and any(isinstance(target, ast.Name) and target.id in names for target in node.targets)
        )
    ]
    scope = dict(functions)
    exec(compile(ast.fix_missing_locations(ast.Module(body=declarations, type_ignores=[])), str(path), "exec"), scope)
    return scope


def _fail(message):
    raise AssertionError(message)


class _RepositoryPath(str):
    def get_child(self, name):
        return _RepositoryPath(self.rstrip("/\\") + "/" + name)


class _Repository:
    """Repository_ctx double that records the projected repository layout."""

    def __init__(self):
        self.symlinks = {}
        self.files = {}

    def path(self, value):
        return _RepositoryPath(value)

    def symlink(self, source, destination):
        assert destination not in self.symlinks, destination
        self.symlinks[destination] = str(source)

    def file(self, path, content, executable=False):
        self.files[path] = content


class _Projection:
    """The checked-in Starlark projection applied to admitted native facts."""

    def __init__(self, facts):
        root = Path(__file__).resolve().parents[2]
        repository = _Repository()
        rule = _starlark(
            root / "bazel/lint/sdk.bzl",
            {"_SDK_BUILD", "_basename", "_project_sdk"},
            {
                # Starlark's json.encode emits compact UTF-8 JSON, like the rule.
                "json": SimpleNamespace(
                    encode=lambda value: json.dumps(value, ensure_ascii=False, separators=(",", ":"))
                )
            },
        )
        rule["_project_sdk"](repository, facts["host"], facts)
        self.symlinks = repository.symlinks
        self.files = repository.files
        self.metadata = json.loads(self.files["metadata.json"])
        self.data = ast.literal_eval(ast.parse(self.files["sdk_data.bzl"]).body[0].value)
        self.flags = {
            node.targets[0].id: ast.literal_eval(node.value) for node in ast.parse(self.files["flags.bzl"]).body
        }
        self.calls = []
        self.exports = []
        self.globs = []
        imports = _starlark(
            root / "bazel/lint/imports.bzl",
            {"lint_sdk_imports"},
            {
                "cc_import": lambda **fields: self.calls.append(("cc_import", fields)),
                "cc_library": lambda **fields: self.calls.append(("cc_library", fields)),
                "native": SimpleNamespace(
                    exports_files=lambda names: self.exports.extend(names),
                    glob=self._glob,
                    filegroup=lambda **fields: self.calls.append(("filegroup", fields)),
                ),
                "fail": _fail,
            },
        )
        imports["lint_sdk_imports"](self.data)

    def _glob(self, patterns):
        self.globs.append(patterns)
        if patterns == ["include/**"]:
            return ["include/0/llvm/Config/llvm-config.h"]
        assert patterns == ["resource/**"], patterns
        return ["resource/include/stddef.h"]

    def named(self, rule):
        return [fields for name, fields in self.calls if name == rule]


class WindowsReadySdkTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-lint-sdk-windows-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()

    def module_sdk(self):
        root = self.root / "module-sdk"
        _write(root / "lib/clangTidyMain.lib", "!<arch>\n")
        _write(root / "bin/llvm-nm.exe")
        _write(root / "lib/clangTidyReadabilityModule.lib", "!<arch>\n")
        _write(root / "lib/clangTidyModernizeModule.lib", "!<arch>\n")
        _write(
            root / "include/clang-tidy/ClangTidyForceLinker.h",
            """
namespace clang::tidy {
extern volatile int ReadabilityModuleAnchorSource;
extern volatile int ModernizeModuleAnchorSource;
#if CLANG_TIDY_ENABLE_QUERY_BASED_CUSTOM_CHECKS
extern volatile int CustomModuleAnchorSource;
#endif
}
""",
        )
        self.module_symbols = """
ClangTidyMain.cpp.obj:
                 U int volatile clang::tidy::ReadabilityModuleAnchorSource
                 U clang::tidy::ModernizeModuleAnchorSource
                 U clang::tidy::ModernizeModuleAnchorSource
                 U clang::tidy::exportReplacements
"""
        return root

    def main_requirements(self, root):
        def query(command, **_kwargs):
            self.assertEqual(
                command,
                [
                    str(root / "bin/llvm-nm.exe"),
                    "--undefined-only",
                    "--demangle",
                    str(root / "lib/clangTidyMain.lib"),
                ],
            )
            return subprocess.CompletedProcess(command, 0, self.module_symbols, "")

        with patch.object(sdk.subprocess, "run", side_effect=query):
            return sdk._tidy_main_requirements(
                [root],
                root / "include/clang-tidy/ClangTidyForceLinker.h",
                root / "lib/clangTidyMain.lib",
                root / "bin/llvm-nm.exe",
            )

    def test_required_module_anchors_project_normal_archives_without_whole_linking(self):
        root = self.module_sdk()
        modules, _ = self.main_requirements(root)
        self.assertCountEqual(
            [path.name for path in modules],
            [
                "clangTidyReadabilityModule.lib",
                "clangTidyModernizeModule.lib",
            ],
        )
        # This configured archive does not reference the optional Custom module.
        self.assertFalse((root / "lib/clangTidyCustomModule.lib").exists())
        support = _write(root / "lib/LLVMSupport.lib", "!<arch>\n")
        projection = sdk.LintSdkProjection(
            host="windows",
            version="31.2.3",
            resource_dir=root / "lib/clang/31",
            sdk_root=root,
            clang=root / "bin/clang-cl.exe",
            clang_tidy=root / "bin/clang-tidy.exe",
            includes=[root / "include"],
            libraries=[root / "lib/clangTidyMain.lib", *modules, support],
            copts=[],
            linkopts=["advapi32.lib"],
        )
        projected = _Projection(sdk._projection_facts(projection))
        self.assertEqual(
            projected.symlinks,
            {
                "include/0": (root / "include").as_posix(),
                "bin/clang-tidy.exe": (root / "bin/clang-tidy.exe").as_posix(),
                "bin/clang-cl.exe": (root / "bin/clang-cl.exe").as_posix(),
                "resource/include": (root / "lib/clang/31/include").as_posix(),
                "lib/0/clangTidyMain.lib": (root / "lib/clangTidyMain.lib").as_posix(),
                "lib/1/clangTidyModernizeModule.lib": (root / "lib/clangTidyModernizeModule.lib").as_posix(),
                "lib/2/clangTidyReadabilityModule.lib": (root / "lib/clangTidyReadabilityModule.lib").as_posix(),
                "lib/3/LLVMSupport.lib": (root / "lib/LLVMSupport.lib").as_posix(),
            },
        )
        imports = projected.named("cc_import")
        self.assertEqual(
            [(fields["name"], fields["static_library"]) for fields in imports],
            [
                ("library_0", "lib/0/clangTidyMain.lib"),
                ("library_1", "lib/1/clangTidyModernizeModule.lib"),
                ("library_2", "lib/2/clangTidyReadabilityModule.lib"),
                ("library_3", "lib/3/LLVMSupport.lib"),
            ],
        )
        self.assertTrue(all("alwayslink" not in fields for _, fields in projected.calls))
        (library,) = projected.named("cc_library")
        self.assertEqual(library["name"], "sdk")
        self.assertEqual(library["includes"], ["include/0"])
        self.assertEqual(library["deps"], [":library_0", ":library_1", ":library_2", ":library_3"])
        self.assertEqual(library["linkopts"], ["advapi32.lib"])
        clang_tidy, tools = projected.named("filegroup")
        self.assertEqual(clang_tidy["srcs"], ["bin/clang-tidy.exe"])
        self.assertEqual(
            tools["srcs"],
            [":clang_tidy", "metadata.json", "resource/include/stddef.h", "bin/clang-cl.exe"],
        )
        self.assertEqual(projected.metadata["sdk_root"], root.as_posix())
        self.assertEqual(projected.metadata["platform"], "windows")
        self.assertEqual(projected.metadata["host_kind"], "standalone")
        self.assertEqual(projected.flags, {"LINT_SDK_HOST": "windows", "LINT_SDK_COPTS": []})

    def test_missing_required_module_archive_is_not_silently_omitted(self):
        root = self.module_sdk()
        (root / "lib/clangTidyModernizeModule.lib").unlink()
        with self.assertRaisesRegex(sdk.LintSdkError, "Missing clangTidyModernizeModule"):
            self.main_requirements(root)

    def test_archive_module_reference_must_belong_to_the_installed_force_linker(self):
        root = self.module_sdk()
        self.module_symbols += "                 U clang::tidy::UnownedModuleAnchorSource\n"
        with self.assertRaisesRegex(sdk.LintSdkError, "Cannot match.*UnownedModule"):
            self.main_requirements(root)

    def test_main_archive_without_module_references_cannot_admit_an_empty_registry(self):
        root = self.module_sdk()
        self.module_symbols = "ClangTidyMain.cpp.obj:\n                 U clang::tidy::exportReplacements\n"
        with self.assertRaisesRegex(sdk.LintSdkError, "missing module declarations or references"):
            self.main_requirements(root)

    def test_c_initializer_roles_select_only_referenced_targets_and_forward_their_components(self):
        root = self.module_sdk()
        self.module_symbols += """
                 U LLVMInitializeX86TargetInfo
                 U __imp_LLVMInitializeX86TargetMC
                 U _LLVMInitializeX86AsmParser
                 U LLVMInitializeNVPTXTargetInfo
                 U __imp__LLVMInitializeNVPTXTargetMC
                 U __imp_LLVMInitializeX86TargetInfo
"""
        _, components = self.main_requirements(root)
        self.assertCountEqual(components, ["x86info", "x86desc", "x86asmparser", "nvptxinfo", "nvptxdesc"])
        config = _write(root / "bin/llvm-config.exe")
        target_library = _write(root / "lib/LLVMX86Info.lib", "!<arch>\n")
        support = _write(root / "lib/LLVMSupport.lib", "!<arch>\n")

        def query(command, **_kwargs):
            self.assertEqual(command[0], str(config))
            if command[1:] == ["--components"]:
                output = "x86info x86desc x86asmparser nvptxinfo nvptxdesc"
            elif command[1:3] in (["--link-static", "--libfiles"], ["--link-static", "--system-libs"]):
                self.assertTrue({"x86info", "x86desc", "x86asmparser", "nvptxinfo", "nvptxdesc"} <= set(command[3:]))
                self.assertIn("support", command[3:])
                self.assertFalse({"--all", "all", "all-targets", "x86codegen", "nvptxasmparser"} & set(command[3:]))
                output = f'"{target_library}" "{support}"' if command[2] == "--libfiles" else "advapi32.lib"
            else:
                self.fail(f"Unexpected LLVM link query: {command}")
            return subprocess.CompletedProcess(command, 0, output, "")

        with patch.object(sdk.subprocess, "run", side_effect=query):
            libraries, systems = sdk._llvm_link_interface(config, "windows", components)
        self.assertEqual(libraries, [target_library, support])
        self.assertEqual(systems, ["advapi32.lib"])

    def test_target_component_missing_from_the_sdk_is_rejected_before_link_queries(self):
        config = _write(self.root / "bin/llvm-config.exe")

        def query(command, **_kwargs):
            self.assertEqual(command, [str(config), "--components"])
            return subprocess.CompletedProcess(command, 0, "x86info", "")

        with patch.object(sdk.subprocess, "run", side_effect=query):
            with self.assertRaisesRegex(sdk.LintSdkError, "unavailable LLVM target components: x86desc"):
                sdk._llvm_link_interface(config, "windows", ["x86info", "x86desc"])

    def test_unmapped_llvm_initializer_is_rejected_instead_of_importing_codegen(self):
        root = self.module_sdk()
        self.module_symbols += "                 U __imp_LLVMInitializeX86Target\n"
        with self.assertRaisesRegex(sdk.LintSdkError, "Unsupported.*LLVMInitializeX86Target"):
            self.main_requirements(root)

    def test_unused_global_xml_token_is_dropped_only_after_scanning_selected_archives(self):
        root = self.module_sdk()
        support = _write(root / "lib/LLVMSupport.lib", "!<arch>\n")
        selected = [root / "lib/clangTidyMain.lib", support]
        inspected = []

        def query(command, **_kwargs):
            self.assertEqual(command[:3], [str(root / "bin/llvm-nm.exe"), "--undefined-only", "--format=posix"])
            self.assertEqual(len(command), 4)
            archive = Path(command[3])
            self.assertIn(archive, selected)
            inspected.append(archive)
            output = "LLVMInitializeX86TargetInfo U 0 0\n" if archive == selected[0] else "malloc U 0 0\n"
            return subprocess.CompletedProcess(command, 0, output, "")

        with patch.object(sdk.subprocess, "run", side_effect=query):
            systems, extra = sdk._windows_system_libraries(
                ["advapi32.lib", "xml2s.lib"], [root], selected, root / "bin/llvm-nm.exe"
            )
        self.assertCountEqual(inspected, selected)
        self.assertEqual(systems, ["advapi32.lib"])
        self.assertEqual(extra, [])

    def test_selected_manifest_library_needs_real_xml_bytes_in_a_governed_root(self):
        root = self.module_sdk()
        manifest = _write(root / "lib/LLVMWindowsManifest.lib", "!<arch>\n")
        with patch.object(sdk.subprocess, "run", side_effect=AssertionError("manifest already proves XML is required")):
            with self.assertRaisesRegex(sdk.LintSdkError, "Missing xml2s.lib required"):
                sdk._windows_system_libraries(["xml2s.lib"], [root], [manifest], root / "bin/llvm-nm.exe")
            xml = _write(root / "lib/xml2s.lib", "!<arch>\n")
            systems, extra = sdk._windows_system_libraries(["xml2s.lib"], [root], [manifest], root / "bin/llvm-nm.exe")
        self.assertEqual(systems, [])
        self.assertEqual(extra, [xml])

    def test_xml_import_without_the_manifest_archive_cannot_be_silently_dropped(self):
        root = self.module_sdk()
        support = _write(root / "lib/LLVMSupport.lib", "!<arch>\n")

        def query(command, **_kwargs):
            self.assertEqual(
                command,
                [
                    str(root / "bin/llvm-nm.exe"),
                    "--undefined-only",
                    "--format=posix",
                    str(support),
                ],
            )
            return subprocess.CompletedProcess(command, 0, "__imp_xmlReadMemory U 0 0\n", "")

        with patch.object(sdk.subprocess, "run", side_effect=query):
            with self.assertRaisesRegex(sdk.LintSdkError, "Missing xml2s.lib required"):
                sdk._windows_system_libraries(["xml2s.lib"], [root], [support], root / "bin/llvm-nm.exe")


class PosixSdkProjectionTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-lint-sdk-posix-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.llvm_dev = self.root / "llvm-31.2.3-dev"
        self.llvm_lib = self.root / "llvm-31.2.3-lib/lib"
        self.clang_dev = self.root / "clang-31.2.3-dev"
        self.clang_lib = self.root / "clang-31.2.3-lib"
        self.clang_out = self.root / "clang-31.2.3"
        self.llvm_config = _write(self.llvm_dev / "bin/llvm-config")
        self.clang = _write(self.clang_out / "bin/clang")
        self.clangxx = _write(self.clang_out / "bin/clang++")
        self.xcrun = _write(self.root / "native-tools/xcrun")
        self.macos_sdk = self.root / "MacOSX.sdk"
        _write(self.macos_sdk / "SDKSettings.json", "{}")
        self.shim = self.root / "expected-shim"
        _write(self.shim / "expected")
        self.tidy = _write(self.clang_out / "bin/clang-tidy")
        self.llvm_header = _write(
            self.llvm_dev / "include/llvm/Config/llvm-config.h", '#define LLVM_VERSION_STRING "31.2.3"\n'
        )
        _write(self.llvm_dev / "include/llvm/Config/abi-breaking.h", "#define LLVM_ENABLE_ABI_BREAKING_CHECKS 0\n")
        self.clang_header = _write(
            self.clang_dev / "include/clang/Basic/Version.inc", '#define CLANG_VERSION_STRING "31.2.3"\n'
        )
        _write(self.clang_dev / "include/clang-tidy/ClangTidy.h")
        self.resource_dir = self.clang_out / "lib/clang/31"
        _write(self.resource_dir / "include/stddef.h")
        self.llvm_library = _write(self.llvm_lib / "libLLVM.so.31.2")
        self.clang_cpp = _write(self.clang_lib / "lib/libclang-cpp.so.31.2")
        for name in ("clangTidy", "clangTidyUtils", "clangTidyReadabilityModule"):
            _write(self.clang_lib / "lib" / f"lib{name}.a", "!<arch>\n")
        self.environment = {
            "AOBUS_LINT_LLVM_CONFIG": str(self.llvm_config),
            "AOBUS_LINT_CLANG": str(self.clang),
            "AOBUS_LINT_CLANGXX": str(self.clangxx),
            "AOBUS_LINT_XCRUN": str(self.xcrun),
            "AOBUS_LINT_CLANG_TIDY": str(self.tidy),
            "NIXPKGS_CMAKE_PREFIX_PATH": os.pathsep.join(
                map(str, [self.clang_out, self.clang_dev, self.clang_lib, self.root / "unrelated"])
            ),
        }
        self.cxxflags = f'-I"{self.llvm_dev}/include" -std=c++17 -D_GLIBCXX_USE_CXX11_ABI=1 -fno-exceptions'
        self.queries = []

    def query(self, command, **kwargs):
        executable, *arguments = command
        self.queries.append(tuple(command))
        if executable == str(self.llvm_config):
            answers = {
                ("--version",): "31.2.3",
                ("--prefix",): str(self.llvm_dev),
                ("--includedir",): str(self.llvm_dev / "include"),
                ("--libdir",): str(self.llvm_lib),
                ("--cxxflags",): self.cxxflags,
                ("--has-rtti",): "YES",
                ("--assertion-mode",): "OFF",
                ("--link-shared", "--libfiles", "support"): json.dumps(
                    self.llvm_library.as_posix(), ensure_ascii=False
                ),
                ("--link-shared", "--system-libs", "support"): "-lpthread",
            }
            self.assertIn(tuple(arguments), answers, "only the selected LLVM query is supported")
            stdout, stderr = answers[tuple(arguments)], ""
        elif executable in (str(self.clang), str(self.clangxx), str(self.tidy)) and arguments == ["--version"]:
            stdout, stderr = "LLVM version 31.2.3\n", ""
        elif executable == str(self.xcrun) and arguments == ["--show-sdk-path"]:
            stdout, stderr = str(self.macos_sdk), ""
        elif executable == str(self.clang) and arguments == ["-print-resource-dir"]:
            stdout, stderr = str(self.resource_dir), ""
        elif executable == str(self.clang) and arguments == ["-E", "-x", "c++", "-", "-v"]:
            self.assertEqual(kwargs["input"], "")
            stdout, stderr = "", "#include <...> search starts here:\nEnd of search list.\n"
        else:
            self.fail(f"Unexpected external SDK query: {command}")
        return subprocess.CompletedProcess(command, 0, stdout + "\n", stderr)

    def discover(self, host="linux"):
        with patch.object(sdk.subprocess, "run", side_effect=self.query):
            return sdk.discover_sdk(
                host, self.environment, self.root / "unused-windows.json", self.root / "unused-macos.json"
            )

    def test_split_native_outputs_project_real_tidy_clang_and_llvm_link_inputs(self):
        projection = self.discover()
        projected = _Projection(sdk._projection_facts(projection))
        metadata = projected.metadata
        self.assertEqual(metadata["schema"], 1)
        self.assertEqual(metadata["platform"], "linux")
        self.assertEqual(metadata["sdk_root"], self.llvm_dev.as_posix())
        self.assertEqual(metadata["clang"], self.clangxx.as_posix())
        self.assertEqual(metadata["version"], "31.2.3")
        self.assertEqual(metadata["host_kind"], "plugin")
        self.assertEqual(metadata["resource_dir"], self.resource_dir.as_posix())
        self.assertEqual(projection.includes, [self.llvm_dev / "include", self.clang_dev / "include"])
        self.assertIn(self.clang_cpp, projection.libraries)
        self.assertIn(self.llvm_library, projection.libraries)
        self.assertIn("-D_GLIBCXX_USE_CXX11_ABI=1", projection.copts)
        self.assertIn("-fno-exceptions", projection.copts)
        self.assertIn("-frtti", projection.copts)
        self.assertIn("-DNDEBUG", projection.copts)
        self.assertNotIn("-std=c++17", projection.copts)
        imports = projected.named("cc_import")
        self.assertEqual(
            [fields.get("static_library", fields.get("shared_library")) for fields in imports],
            [
                "lib/0/libclangTidyReadabilityModule.a",
                "lib/1/libclangTidyUtils.a",
                "lib/2/libclangTidy.a",
                "lib/3/libclang-cpp.so.31.2",
                "lib/4/libLLVM.so.31.2",
            ],
        )
        self.assertTrue(all("alwayslink" not in fields for _, fields in projected.calls))
        (library,) = projected.named("cc_library")
        self.assertEqual(library["name"], "sdk")
        self.assertEqual(library["hdrs"], ["include/0/llvm/Config/llvm-config.h"])
        self.assertEqual(library["includes"], ["include/0", "include/1"])
        self.assertEqual(library["deps"], [":library_0", ":library_1", ":library_2", ":library_3", ":library_4"])
        self.assertEqual(library["linkopts"], ["-lpthread"])
        # Graph data stays repository-relative; metadata keeps the native paths.
        for name in ("sdk_data.bzl", "BUILD.bazel"):
            self.assertNotIn(str(self.root), projected.files[name])
        self.assertEqual(projected.symlinks["bin/clang-tidy"], self.tidy.as_posix())
        self.assertEqual(projected.symlinks["bin/clang++"], self.clangxx.as_posix())
        self.assertEqual(projected.symlinks["lib/3/libclang-cpp.so.31.2"], self.clang_cpp.as_posix())
        self.assertEqual(
            projected.data["runtime"],
            ["runtime/0/libclang-cpp.so.31.2", "runtime/1/libLLVM.so.31.2"],
        )
        for fields in imports:
            self.assertIn(fields.get("static_library", fields.get("shared_library")), projected.symlinks)
        self.assertIn(self.llvm_header, projection.watch_files)
        # The macro owns the exports and the package-local globs.
        self.assertEqual(projected.exports, ["metadata.json", "flags.bzl"])
        self.assertEqual(projected.globs, [["include/**"], ["resource/**"]])
        build = projected.files["BUILD.bazel"]
        self.assertIn('load("@aobus//bazel/lint:imports.bzl", "lint_sdk_imports")', build)
        self.assertIn('load(":sdk_data.bzl", "SDK")', build)
        # The loader passes only SDK data; the macro owns the graph policy.
        (call,) = _calls(build, "lint_sdk_imports")
        self.assertEqual(call.args[0].id, "SDK")
        self.assertEqual(call.keywords, [])

    def test_the_cli_payload_carries_flat_admitted_facts_only(self):
        projection = self.discover()
        facts = sdk._projection_facts(projection)
        # No materialized links/files plan protocol rides along with the facts.
        self.assertNotIn("links", facts)
        self.assertNotIn("files", facts)
        self.assertEqual(facts["host"], "linux")
        self.assertEqual(facts["clang_tidy"], self.tidy.as_posix())
        self.assertEqual(facts["libraries"], [path.as_posix() for path in projection.libraries])

    def test_native_names_with_spaces_and_non_ascii_keep_their_identity(self):
        projection = self.discover()
        exotic = _write(self.root / "LLVM SDK ünïcode/lib/libclang-cpp.so.31 2")
        # A literal backslash is a legal POSIX file name. Keep the case purely
        # lexical: a real fixture would create a directory on NT instead.
        backslash = PurePosixPath(self.root.as_posix() + "/store/lib/libOdd\\Name.so")
        projection.libraries = [exotic, backslash]
        projection.runtime = [exotic]
        projected = _Projection(sdk._projection_facts(projection))
        self.assertEqual(
            projected.symlinks,
            {
                "include/0": (self.llvm_dev / "include").as_posix(),
                "include/1": (self.clang_dev / "include").as_posix(),
                "bin/clang-tidy": self.tidy.as_posix(),
                "bin/clang++": self.clangxx.as_posix(),
                "resource/include": (self.resource_dir / "include").as_posix(),
                "lib/0/libclang-cpp.so.31 2": exotic.as_posix(),
                "lib/1/libOdd\\Name.so": backslash.as_posix(),
                "runtime/0/libclang-cpp.so.31 2": exotic.as_posix(),
            },
        )
        self.assertEqual(
            projected.data["libraries"],
            ["lib/0/libclang-cpp.so.31 2", "lib/1/libOdd\\Name.so"],
        )
        self.assertEqual(projected.metadata["libraries"], [exotic.as_posix(), backslash.as_posix()])
        imports = projected.named("cc_import")
        self.assertEqual([fields["shared_library"] for fields in imports], projected.data["libraries"])

    def test_explicit_clang_roots_override_ambient_split_output_candidates(self):
        self.environment["AOBUS_LINT_CLANG_ROOTS"] = json.dumps([str(self.clang_dev), str(self.clang_lib)])
        self.environment["NIXPKGS_CMAKE_PREFIX_PATH"] = str(self.root / "clang-99.0.0")
        self.assertIn(self.clang_cpp, self.discover().libraries)

    def test_header_version_skew_fails_before_generating_a_repository(self):
        self.clang_header.write_text('#define CLANG_VERSION_STRING "31.2.2"\n')
        with self.assertRaisesRegex(sdk.LintSdkError, "Clang headers disagree"):
            self.discover()
        self.assertFalse((self.root / "BUILD.bazel").exists())

    def test_missing_clang_cpp_is_not_replaced_by_llvm_config_support_only(self):
        self.clang_cpp.unlink()
        with self.assertRaisesRegex(sdk.LintSdkError, "Missing clang-cpp"):
            self.discover()

    def test_mac_compiler_major_and_libcxx_contract_stay_native_owned(self):
        self.environment["AOBUS_LLVM_ROOT"] = str(self.clang_out)
        self.environment["AOBUS_LIBCXX_EXPECTED_SHIM"] = str(self.shim)
        _write(
            self.root / "unused-macos.json",
            json.dumps({"deploymentTarget": "19.0", "homebrew": {"llvmMajorVersion": "31"}}),
        )
        self.cxxflags += " -stdlib=libc++"
        projection = self.discover("macos")
        self.assertIn("-stdlib=libc++", projection.copts)
        self.assertIn("-stdlib=libc++", projection.linkopts)
        self.assertIn("-mmacosx-version-min=19.0", projection.fixture_arguments)
        self.assertIn(str(self.macos_sdk), projection.fixture_arguments)
        self.assertIn("-isystem" + str(self.shim), projection.fixture_arguments)
        self.assertNotIn(str(self.macos_sdk / "usr/include"), projection.fixture_arguments)
        _write(self.root / "unused-macos.json", json.dumps({"homebrew": {"llvmMajorVersion": "30"}}))
        with self.assertRaisesRegex(sdk.LintSdkError, "governed macOS compiler major"):
            self.discover("macos")


class CheckerOwnershipTest(unittest.TestCase):
    def test_bazel_checker_uses_the_existing_cmake_source_ownership(self):
        root = Path(__file__).resolve().parents[2]
        cmake = (root / "tool/lint/CMakeLists.txt").read_text(encoding="utf-8")
        ownership = re.search(r"set\(AOBUS_LINT_PLUGIN_SOURCES\s+(.*?)\n\)", cmake, re.DOTALL)
        self.assertIsNotNone(ownership)
        cmake_sources = ownership[1].split()
        declarations = _calls((root / "tool/lint/BUILD.bazel").read_text(encoding="utf-8"), "lint_checker")
        self.assertEqual(len(declarations), 1)
        bazel_sources = ast.literal_eval(next(field.value for field in declarations[0].keywords if field.arg == "srcs"))
        self.assertCountEqual(bazel_sources, cmake_sources)
        self.assertEqual(len(bazel_sources), len(set(bazel_sources)))
        self.assertNotIn("AobusClangTidyMain.cpp", bazel_sources)

    def test_windows_flags_use_the_sdk_abi_without_a_second_crt_selection(self):
        flags = sdk._compile_flags(
            '-I"C:\\LLVM SDK\\include" -std=c++17 -DNDEBUG -fno-exceptions -fno-rtti /MDd', "NO", "OFF", "windows"
        )
        self.assertIn("/GR-", flags)
        self.assertIn("/EHs-c-", flags)
        self.assertIn("/DNDEBUG", flags)
        self.assertIn("/DCLANG_BUILD_STATIC", flags)
        self.assertIn("/DLLVM_BUILD_STATIC", flags)
        self.assertFalse(any(re.fullmatch(r"/M[DT]d?", flag) for flag in flags))
        self.assertFalse(any("LLVM SDK" in flag for flag in flags))


class RepositoryHelperTest(unittest.TestCase):
    def test_the_lint_package_exports_the_graph_declaration_loaded_by_the_repository(self):
        root = Path(__file__).resolve().parents[2]
        build = ast.parse((root / "bazel/lint/BUILD.bazel").read_text(encoding="utf-8"))
        exported = [
            element.value
            for call in ast.walk(build)
            if isinstance(call, ast.Call) and isinstance(call.func, ast.Name) and call.func.id == "exports_files"
            for element in call.args[0].elts
        ]
        # The generated repository loads the declaration from "@aobus//bazel/lint".
        self.assertIn("imports.bzl", exported)
        self.assertTrue((root / "bazel/lint/imports.bzl").is_file())

    def test_repository_rule_watches_the_helper_import_closure(self):
        root = Path(__file__).resolve().parents[2]
        rule = (root / "bazel/lint/sdk.bzl").read_text(encoding="utf-8")
        block = re.search(r'"modules": attr\.label_list\(\s*default = \[(.*?)\]', rule, re.DOTALL)
        self.assertIsNotNone(block)
        declared = set(re.findall(r'Label\("//:(script/ao/[^"]+\.py)"\)', block[1]))
        # The repository rule passes an absolute helper path. Keep that shape
        # even at a UNC share root, where relative Path.absolute() is unreliable.
        probe = (
            "import pathlib, runpy, sys\n"
            f"root = pathlib.Path({str(root / 'script')!r})\n"
            "runpy.run_path(str(root / 'ao/core/bazel_lint_sdk.py'), run_name='sdk_probe')\n"
            "for module in list(sys.modules.values()):\n"
            "    path = pathlib.Path(getattr(module, '__file__', None) or '/')\n"
            "    if path.is_relative_to(root):\n"
            "        print(path.relative_to(root.parent).as_posix())\n"
        )
        result = subprocess.run(
            [sys.executable, "-I", "-S", "-B", "-X", "utf8", "-c", probe], cwd=root, capture_output=True, text=True
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(set(result.stdout.split()) - {"script/ao/core/bazel_lint_sdk.py"}, declared)


if __name__ == "__main__":
    unittest.main()
