"""Ready-only lint SDK selection and its generated Bazel consumer interfaces.

Tool queries are the external I/O seam. Tiny native metadata and archive files
exercise real selection, validation and generation; these are not linker or ABI
acceptance tests and do not reconstruct the complete native SDK graph.
"""

import ast
import json
import os
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
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


def _fields(call):
    return {entry.arg: ast.literal_eval(entry.value) for entry in call.keywords}


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

    def test_required_module_anchors_generate_normal_archives_without_whole_linking(self):
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
        plan = sdk.repository_plan(projection)
        imports = {
            Path(fields["static_library"]).name: fields
            for call in _calls(plan["files"]["BUILD.bazel"], "cc_import")
            if (fields := _fields(call))
        }
        self.assertIn("clangTidyReadabilityModule.lib", imports)
        self.assertIn("clangTidyModernizeModule.lib", imports)
        self.assertTrue(all("alwayslink" not in fields for fields in imports.values()))
        metadata = json.loads(plan["files"]["metadata.json"])
        self.assertEqual(metadata["sdk_root"], root.as_posix())
        self.assertEqual(metadata["platform"], "windows")
        self.assertEqual(metadata["host_kind"], "standalone")

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

    def test_split_native_outputs_supply_real_tidy_clang_and_llvm_link_inputs(self):
        projection = self.discover()
        plan = sdk.repository_plan(projection)
        metadata = json.loads(plan["files"]["metadata.json"])
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
        build = plan["files"]["BUILD.bazel"]
        imports = [_fields(call) for call in _calls(build, "cc_import")]
        self.assertEqual(sum("static_library" in call for call in imports), 3)
        self.assertEqual(sum("shared_library" in call for call in imports), 2)
        self.assertTrue(all(not call.get("alwayslink") for call in imports))
        self.assertNotIn(str(self.root), build)
        self.assertIn('"-lpthread"', build)
        declared = {item["destination"]: Path(item["source"]) for item in plan["links"]}
        for call in imports:
            self.assertIn(call.get("static_library", call.get("shared_library")), declared)
        self.assertIn(self.tidy, declared.values())
        self.assertIn(self.clang_cpp, declared.values())
        self.assertIn(self.llvm_header, projection.watch_files)

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
    def test_repository_rule_watches_the_helper_import_closure(self):
        root = Path(__file__).resolve().parents[2]
        rule = (root / "bazel/lint/sdk.bzl").read_text(encoding="utf-8")
        block = re.search(r'"modules": attr\.label_list\(\s*default = \[(.*?)\]', rule, re.DOTALL)
        self.assertIsNotNone(block)
        declared = set(re.findall(r'Label\("//:(script/ao/[^"]+\.py)"\)', block[1]))
        # Import the helper as a script exactly as the repository rule starts it.
        probe = (
            "import pathlib, runpy, sys\n"
            "runpy.run_path('script/ao/core/bazel_lint_sdk.py', run_name='sdk_probe')\n"
            "root = pathlib.Path('script').absolute()\n"
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
