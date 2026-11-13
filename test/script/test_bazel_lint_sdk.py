"""Ready-only lint SDK selection and its projected Bazel consumer interfaces.

Tool queries are the external I/O seam. Tiny native metadata and archive files
exercise real selection, validation and the checked-in Starlark projection;
these are not linker or ABI acceptance tests and do not reconstruct the complete
native SDK graph.
"""

import json
import os
import re
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stderr
from io import StringIO
from pathlib import Path
from unittest.mock import patch

from ao.core import bazel_lint_sdk as sdk
from ao.core import windows_llvm_sdk


def _write(path, text="fixture\n"):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")
    return path


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
            return sdk._tidy_main_requirements([root], root / "lib/clangTidyMain.lib", root / "bin/llvm-nm.exe")

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

    def test_missing_windows_selected_files_project_the_exact_typed_lint_cause(self):
        manifest = _write(
            self.root / "windows.json",
            json.dumps({"version": "31.2.3", "archiveSha256": "b" * 64, "requiredFiles": ["bin/llvm-config.exe"]}),
        )
        sdk_root = self.root / "windows-sdk"
        _write(sdk_root / "lib/clang/31/include/stddef.h")
        with self.assertRaises(sdk.LintSdkSelectedFileMissing) as caught:
            sdk.discover_sdk(
                "windows", {"AOBUS_LLVM_SDK_ROOT": str(sdk_root)}, manifest, self.root / "unused-macos.json"
            )
        self.assertEqual(caught.exception.path, sdk_root / "bin/llvm-config.exe")
        base = self.root / "cache"
        cached = base / "toolchains/llvm-31.2.3-x86_64-windows-msvc"
        _write(cached / "bin/llvm-config.exe")
        _write(cached / "lib/clang/31/include/stddef.h")
        with self.assertRaises(sdk.LintSdkSelectedFileMissing) as caught:
            sdk.discover_sdk(
                "windows", {"AOBUS_LLVM_SDK_CACHE_ROOT": str(base)}, manifest, self.root / "unused-macos.json"
            )
        self.assertEqual(caught.exception.path, cached / windows_llvm_sdk.MARKER)
        _write(cached / windows_llvm_sdk.MARKER, "stale marker\n")
        with self.assertRaises(windows_llvm_sdk.WindowsLlvmSdkError) as caught:
            sdk.discover_sdk(
                "windows", {"AOBUS_LLVM_SDK_CACHE_ROOT": str(base)}, manifest, self.root / "unused-macos.json"
            )
        self.assertNotIsInstance(caught.exception, windows_llvm_sdk.WindowsLlvmSdkSelectedFileMissing)


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

    def test_the_cli_payload_carries_flat_admitted_facts_only(self):
        projection = self.discover()
        facts = sdk._projection_facts(projection)
        # No materialized links/files plan protocol rides along with the facts.
        self.assertNotIn("links", facts)
        self.assertNotIn("files", facts)
        self.assertEqual(facts["host"], "linux")
        self.assertEqual(facts["clang_tidy"], self.tidy.as_posix())
        self.assertEqual(facts["libraries"], [path.as_posix() for path in projection.libraries])

    def test_explicit_clang_roots_override_ambient_split_output_candidates(self):
        self.environment["AOBUS_LINT_CLANG_ROOTS"] = json.dumps([str(self.clang_dev), str(self.clang_lib)])
        self.environment["NIXPKGS_CMAKE_PREFIX_PATH"] = str(self.root / "clang-99.0.0")
        self.assertIn(self.clang_cpp, self.discover().libraries)

    def cli(self, *extra):
        output = self.root / "cli-projection.json"
        arguments = [
            "bazel_lint_sdk.py",
            "--host",
            "linux",
            "--windows-manifest",
            str(self.root / "unused-windows.json"),
            "--macos-contract",
            str(self.root / "unused-macos.json"),
            "--environment-json",
            json.dumps(self.environment),
            "--output",
            str(output),
            *extra,
        ]
        with patch.object(sys, "argv", arguments), patch.object(sdk.subprocess, "run", side_effect=self.query):
            with redirect_stderr(StringIO()) as stderr:
                status = sdk.main()
        return status, output, stderr.getvalue()

    def test_only_a_genuinely_missing_initial_selection_is_a_typed_cause(self):
        slot = self.root / "absent-slot/llvm-config"
        self.environment["AOBUS_LINT_LLVM_CONFIG"] = str(slot)
        with self.assertRaises(sdk.LintSdkSelectedFileMissing) as caught:
            self.discover()
        self.assertEqual(caught.exception.path, slot)
        self.assertIn(str(slot), str(caught.exception))
        # A relative selector stays a hard configuration failure, not absence.
        self.environment["AOBUS_LINT_LLVM_CONFIG"] = "relative/llvm-config"
        with self.assertRaisesRegex(sdk.LintSdkError, "must select an absolute"):
            self.discover()

    def test_the_internal_optin_writes_the_exact_unavailable_observation(self):
        slot = self.root / "absent-slot/llvm-config"
        self.environment["AOBUS_LINT_LLVM_CONFIG"] = str(slot)
        status, output, _ = self.cli("--allow-unavailable")
        self.assertEqual(status, 0)
        observation = json.loads(output.read_text(encoding="utf-8"))
        self.assertEqual(observation["schema"], 1)
        self.assertEqual(observation["host"], "linux")
        self.assertEqual(observation["state"], "unavailable")
        self.assertEqual(observation["cause"], "selected-file-missing")
        self.assertEqual(observation["watch_files"], [slot.as_posix()])
        self.assertEqual(observation["watch_directories"], [])


class RepositoryHelperTest(unittest.TestCase):
    def test_repository_rule_watches_the_helper_import_closure(self):
        # An unwatched helper module would leave the SDK repository stale after an edit.
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
