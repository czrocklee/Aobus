"""Selected MSVC metadata and the narrow pinned-private toolchain bridge."""

import ast
import hashlib
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from ao.core import bazel_resolver as resolver
from ao.core import bazel_workspace as workspace


class WindowsToolchainProjectionTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-windows-toolchain-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.vc = self.root / "VC/Tools/MSVC/selected"
        self.sdk = self.root / "Windows Kits/10"
        self.include = [
            self.vc / "include",
            self.sdk / "Include/10.0.selected/ucrt",
            self.sdk / "Include/10.0.selected/um",
        ]
        self.lib = [self.vc / "lib/x64", self.sdk / "Lib/10.0.selected/um/x64"]
        for directory in [*self.include, *self.lib]:
            directory.mkdir(parents=True)
        self.tools = {}
        for role in ("cl", "link", "lib", "ml64"):
            tool = self.vc / "bin/Hostx64/x64" / (role + ".exe")
            tool.parent.mkdir(parents=True, exist_ok=True)
            tool.write_text("selected tool fixture\n")
            self.tools[role] = tool
        self.mt = self.sdk / "bin/10.0.selected/x64/mt.exe"
        self.markers = [
            self.sdk / "Include/10.0.selected/um/Windows.h",
            self.sdk / "Include/10.0.selected/ucrt/corecrt.h",
        ]
        for path in [self.mt, *self.markers]:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"native SDK identity")
        self.environment = {
            "INCLUDE": ";".join(map(str, self.include)) + ";",
            "LIB": ";".join(map(str, self.lib)) + ";",
            "PATH": str(self.vc / "bin/Hostx64/x64") + ";original-selected-path",
            "TMP": str(self.root),
            "VCToolsInstallDir": str(self.vc),
            "WindowsSdkDir": str(self.sdk),
            "WindowsSDKVersion": "10.0.selected\\",
        }

    def selected_tool(self, name, *, path):
        self.assertEqual(path, self.environment["PATH"])
        return str(self.tools[name.removesuffix(".exe")])

    def metadata(self, generated):
        return {
            node.targets[0].id: ast.literal_eval(node.value)
            for node in ast.parse((generated / "toolchain/native_paths.bzl").read_text(encoding="utf-8")).body
            if isinstance(node, ast.Assign)
        }

    def render(self):
        generated = self.root / "generated"
        with patch.object(workspace, "_executable", side_effect=self.selected_tool):
            links = workspace._windows_toolchain(generated, self.environment)
        return generated, links, self.metadata(generated)

    def identity(self):
        generated = self.root / "generated"
        environment = dict(self.environment, VCToolsVersion="selected-vc", UCRTVersion="selected-ucrt")
        with (
            patch.dict(os.environ, {"INCLUDE": "ambient", "LIB": "ambient", "WindowsSdkDir": "ambient"}, clear=True),
            patch.object(workspace, "_host_machine", return_value="AMD64"),
            patch.object(workspace.platform, "machine", side_effect=AssertionError("no uname/WMI query")),
            patch.object(workspace, "_executable", side_effect=self.selected_tool),
        ):
            workspace._toolchain_identity(generated, "windows", compiler_environment=environment)
        return generated / "toolchain/identity.json"

    def assert_thin_build(self, source):
        calls = [node.value for node in ast.parse(source).body]
        self.assertEqual([ast.unparse(call.func) for call in calls], ["load", "load", "package", "windows_toolchain"])
        self.assertEqual(
            [ast.literal_eval(arg) for arg in calls[0].args],
            ["@aobus//bazel:windows_toolchain.bzl", "windows_toolchain"],
        )
        self.assertEqual(ast.literal_eval(calls[1].args[0]), ":native_paths.bzl")
        self.assertEqual(
            {ast.literal_eval(arg) for arg in calls[1].args[1:]},
            {"TOOLS", "TOOL_INPUTS", "BUILTIN_INCLUDE_DIRECTORIES", "MSVC_ENVIRONMENT"},
        )
        self.assertEqual(ast.literal_eval(calls[2].keywords[0].value), ["//visibility:public"])
        self.assertEqual(
            {field.arg: ast.unparse(field.value) for field in calls[3].keywords},
            {
                "tools": "TOOLS",
                "tool_inputs": "TOOL_INPUTS",
                "builtin_include_directories": "BUILTIN_INCLUDE_DIRECTORIES",
                "msvc_environment": "MSVC_ENVIRONMENT",
            },
        )

    def test_generated_build_loads_only_the_windows_macro_without_reading_policy(self):
        with patch.object(workspace, "PROJECT_ROOT", self.root / "no-policy-templates"):
            generated, _, _ = self.render()
        self.assert_thin_build((generated / "toolchain/BUILD.bazel").read_text(encoding="utf-8"))

    def test_unicode_environment_is_emitted_as_utf8_without_starlark_unicode_escapes(self):
        temporary = self.root / "Åsa/音乐"
        path_entry = temporary / "bin"
        path_entry.mkdir(parents=True)
        self.environment["TMP"] = str(temporary)
        self.environment["PATH"] += ";" + str(path_entry)
        generated, _, metadata = self.render()
        source = (generated / "toolchain/native_paths.bzl").read_text(encoding="utf-8")
        self.assertIn("Åsa", source)
        self.assertIn("音乐", source)
        # Escaped Windows path separators remain valid; Unicode escapes do not.
        self.assertNotRegex(source, r"(?<!\\)(?:\\\\)*\\[uU]")
        self.assertEqual(
            metadata["MSVC_ENVIRONMENT"], {name: self.environment[name] for name in ("INCLUDE", "LIB", "PATH", "TMP")}
        )

    def netfx_sdk(self):
        root = self.sdk.parent / "NETFXSDK/selected-netfx"
        include, lib = root / "include/um", root / "lib/um/x64"
        include.mkdir(parents=True)
        lib.mkdir(parents=True)
        self.environment["NETFXSDKDir"] = str(root)
        return root, include, lib

    def test_vc_auxiliary_interface_requires_the_selected_toolset_parent_layout(self):
        auxiliary = self.vc.parents[2] / "Auxiliary/VS/include"
        auxiliary.mkdir(parents=True)
        self.environment["INCLUDE"] += str(auxiliary)
        _, _, metadata = self.render()
        self.assertEqual(metadata["BUILTIN_INCLUDE_DIRECTORIES"][-1], auxiliary.as_posix())
        self.assertEqual(metadata["MSVC_ENVIRONMENT"]["INCLUDE"], self.environment["INCLUDE"])
        self.environment["INCLUDE"] = str(auxiliary)
        self.environment["VCToolsInstallDir"] = str(self.root / "VC")
        with self.assertRaisesRegex(workspace.BazelWorkspaceError, "INCLUDE.*exact selected"):
            self.render()

    def test_vc_auxiliary_parent_and_nested_interfaces_are_not_builtin_roots(self):
        auxiliary = self.vc.parents[2] / "Auxiliary/VS/include"
        (auxiliary / "unmodeled").mkdir(parents=True)
        for directory in (auxiliary.parent, auxiliary / "unmodeled"):
            self.environment["INCLUDE"] = str(directory)
            with (
                self.subTest(directory=directory),
                self.assertRaisesRegex(workspace.BazelWorkspaceError, "INCLUDE.*exact selected"),
            ):
                self.render()

    def test_selected_netfx_preserves_exact_interfaces_environment_and_watches(self):
        root, include, lib = self.netfx_sdk()
        self.environment["INCLUDE"] += str(include)
        self.environment["LIB"] += str(lib)
        observed = []
        with patch.object(workspace, "_INPUT_PROBE", observed.append):
            _, _, metadata = self.render()
        self.assertEqual(metadata["BUILTIN_INCLUDE_DIRECTORIES"][-1], include.as_posix())
        self.assertEqual(metadata["MSVC_ENVIRONMENT"]["INCLUDE"], self.environment["INCLUDE"])
        self.assertEqual(metadata["MSVC_ENVIRONMENT"]["LIB"], self.environment["LIB"])
        self.assertLessEqual({root, include, lib}, set(observed))

    def test_netfx_interfaces_without_selected_metadata_fail_closed(self):
        _, include, lib = self.netfx_sdk()
        del self.environment["NETFXSDKDir"]
        for name, directory in (("INCLUDE", include), ("LIB", lib)):
            original = self.environment[name]
            self.environment[name] += str(directory)
            with (
                self.subTest(name=name),
                self.assertRaisesRegex(workspace.BazelWorkspaceError, name + ".*exact selected"),
            ):
                self.render()
            self.environment[name] = original

    def test_netfx_metadata_rejects_unselected_parent_provider_root_and_traversal(self):
        root, _, _ = self.netfx_sdk()
        provider = self.root / "vcpkg/NETFXSDK/selected-netfx"
        provider.mkdir(parents=True)
        for directory in (provider, root.parent, root.parent / "..", Path("relative/NETFXSDK/selected-netfx")):
            self.environment["NETFXSDKDir"] = str(directory)
            with (
                self.subTest(directory=directory),
                self.assertRaisesRegex(workspace.BazelWorkspaceError, "NETFXSDKDir.*installed version root"),
            ):
                self.render()
        self.environment["NETFXSDKDir"] = str(root.parent / "missing")
        with self.assertRaisesRegex(workspace.BazelWorkspaceError, "NETFXSDKDir.*installed version root"):
            self.render()
        self.environment["NETFXSDKDir"] = None
        with self.assertRaisesRegex(workspace.BazelWorkspaceError, "NETFXSDKDir.*string"):
            self.render()

    def test_netfx_metadata_cannot_admit_a_root_inside_the_checkout(self):
        self.netfx_sdk()
        with patch.object(workspace, "PROJECT_ROOT", self.root):
            with self.assertRaisesRegex(workspace.BazelWorkspaceError, "NETFXSDKDir.*installed version root"):
                self.render()

    def test_netfx_only_admits_exact_selected_include_and_x64_library_interfaces(self):
        root, _, _ = self.netfx_sdk()
        other = root.parent / "other-version/include/um"
        other.mkdir(parents=True)
        wrong_arch = root / "lib/um/x86"
        wrong_arch.mkdir(parents=True)
        nested = root / "include/um/unmodeled"
        nested.mkdir()
        for name, directory in (
            ("INCLUDE", root),
            ("INCLUDE", root / "include"),
            ("INCLUDE", other),
            ("INCLUDE", nested),
            ("LIB", root / "lib/um"),
            ("LIB", wrong_arch),
        ):
            original = self.environment[name]
            self.environment[name] += str(directory)
            with (
                self.subTest(name=name, directory=directory),
                self.assertRaisesRegex(workspace.BazelWorkspaceError, name + ".*exact selected"),
            ):
                self.render()
            self.environment[name] = original

    @unittest.skipIf(os.name == "nt", "fixture requires POSIX symlink creation, not a native privilege grant")
    def test_auxiliary_and_netfx_aliases_cannot_admit_provider_interfaces(self):
        provider = self.root / "provider/include"
        provider.mkdir(parents=True)
        auxiliary = self.vc.parents[2] / "Auxiliary/VS/include"
        auxiliary.parent.mkdir(parents=True)
        auxiliary.symlink_to(provider, target_is_directory=True)
        original = self.environment["INCLUDE"]
        self.environment["INCLUDE"] += str(auxiliary)
        with self.assertRaisesRegex(workspace.BazelWorkspaceError, "INCLUDE.*exact selected"):
            self.render()
        self.environment["INCLUDE"] = original
        root, include, lib = self.netfx_sdk()
        alias = root.parent / "alias-version"
        alias.symlink_to(root, target_is_directory=True)
        self.environment["NETFXSDKDir"] = str(alias)
        with self.assertRaisesRegex(workspace.BazelWorkspaceError, "NETFXSDKDir.*installed version root"):
            self.render()
        self.environment["NETFXSDKDir"] = str(root)
        for name, directory in (("INCLUDE", include), ("LIB", lib)):
            directory.rmdir()
            directory.symlink_to(provider, target_is_directory=True)
            self.environment[name] += str(directory)
            with (
                self.subTest(name=name),
                self.assertRaisesRegex(workspace.BazelWorkspaceError, name + ".*exact selected"),
            ):
                self.render()
            self.environment[name] = self.environment[name].removesuffix(str(directory))

    def test_original_environment_and_exact_builtin_directories_are_preserved(self):
        with patch.dict(os.environ, {"TMP": "repository-owned-scratch", "INCLUDE": "ambient-provider"}):
            generated, links, metadata = self.render()
        self.assertEqual(
            metadata["MSVC_ENVIRONMENT"], {name: self.environment[name] for name in ("INCLUDE", "LIB", "PATH", "TMP")}
        )
        self.assertEqual(metadata["BUILTIN_INCLUDE_DIRECTORIES"], [path.as_posix() for path in self.include])
        self.assertEqual(metadata["TOOLS"]["cl"], self.tools["cl"].as_posix())
        self.assertEqual(metadata["TOOL_INPUTS"]["ml64"], "tools/ml64.exe")
        self.assertEqual(len(links), 4)
        self.assertIn({"source": str(self.tools["cl"]), "destination": "tools/cl.exe"}, links)
        self.assertFalse((generated / "toolchain/tools").exists())

    def test_missing_metadata_and_each_required_environment_value_fail_closed(self):
        for value in (None, {}, {"INCLUDE": ""}):
            with (
                self.subTest(value=value),
                self.assertRaisesRegex(workspace.BazelWorkspaceError, "compiler_environment"),
            ):
                workspace._windows_toolchain(self.root / "generated", value)
        for name in self.environment:
            with self.subTest(name=name):
                environment = dict(self.environment, **{name: ""})
                with self.assertRaisesRegex(workspace.BazelWorkspaceError, "nonempty"):
                    workspace._windows_toolchain(self.root / "generated", environment)

    def test_whole_sdk_parent_prefix_and_provider_headers_are_not_builtins(self):
        provider = self.root / "vcpkg/include"
        provider.mkdir(parents=True)
        for directory in (self.sdk, self.vc, self.sdk / "Include/10.0.selected", provider, self.root):
            with self.subTest(directory=directory):
                self.environment["INCLUDE"] = str(directory)
                with self.assertRaisesRegex(workspace.BazelWorkspaceError, "exact selected"):
                    self.render()

    def test_other_sdk_version_and_nested_sdk_headers_are_not_builtin_roots(self):
        for directory in (self.sdk / "Include/other/um", self.include[0] / "application"):
            directory.mkdir(parents=True)
            self.environment["INCLUDE"] = str(directory)
            with (
                self.subTest(directory=directory),
                self.assertRaisesRegex(workspace.BazelWorkspaceError, "exact selected"),
            ):
                self.render()

    def test_relative_missing_or_empty_include_interface_is_rejected(self):
        for value in ("relative/include", str(self.vc / "atlmfc/include"), ";;;"):
            self.environment["INCLUDE"] = value
            with self.subTest(value=value), self.assertRaisesRegex(workspace.BazelWorkspaceError, "INCLUDE"):
                self.render()

    def test_lib_rejects_parent_provider_other_version_architecture_and_unmodeled_directories(self):
        provider = self.root / "vcpkg/lib"
        nested = self.vc / "lib/x64/extra"
        other = self.sdk / "Lib/other/um/x64"
        x86 = self.vc / "lib/x86"
        for directory in (provider, nested, other, x86):
            directory.mkdir(parents=True)
        for directory in (self.vc, self.sdk, self.sdk / "Lib/10.0.selected", provider, nested, other, x86):
            self.environment["LIB"] = str(directory)
            with (
                self.subTest(directory=directory),
                self.assertRaisesRegex(workspace.BazelWorkspaceError, "LIB.*exact selected"),
            ):
                self.render()

    def test_lib_rejects_relative_missing_and_empty_interfaces(self):
        for value in ("relative/lib", str(self.vc / "atlmfc/lib/x64"), ";;;"):
            self.environment["LIB"] = value
            with self.subTest(value=value), self.assertRaisesRegex(workspace.BazelWorkspaceError, "LIB"):
                self.render()

    def test_optional_atlmfc_and_selected_sdk_lib_interfaces_are_accepted(self):
        directories = [self.vc / "atlmfc/include", self.vc / "atlmfc/lib/x64", self.sdk / "Lib/10.0.selected/ucrt/x64"]
        for directory in directories:
            directory.mkdir(parents=True)
        self.environment["INCLUDE"] += str(directories[0])
        self.environment["LIB"] += ";".join(map(str, directories[1:]))
        _, _, metadata = self.render()
        self.assertEqual(metadata["MSVC_ENVIRONMENT"]["LIB"], self.environment["LIB"])
        self.assertEqual(metadata["BUILTIN_INCLUDE_DIRECTORIES"][-1], directories[0].as_posix())

    @unittest.skipIf(os.name == "nt", "fixture requires POSIX symlink creation, not a native privilege grant")
    def test_selected_lib_symlink_cannot_admit_provider_libraries(self):
        provider = self.root / "provider/lib"
        provider.mkdir(parents=True)
        lib = self.vc / "atlmfc/lib/x64"
        lib.parent.mkdir(parents=True)
        lib.symlink_to(provider, target_is_directory=True)
        self.environment["LIB"] = str(lib)
        with self.assertRaisesRegex(workspace.BazelWorkspaceError, "LIB.*exact selected"):
            self.render()

    @unittest.skipIf(os.name == "nt", "fixture requires POSIX symlink creation, not a native privilege grant")
    def test_selected_include_symlink_cannot_admit_provider_headers(self):
        provider = self.root / "provider/include"
        provider.mkdir(parents=True)
        include = self.vc / "atlmfc/include"
        include.parent.mkdir()
        include.symlink_to(provider, target_is_directory=True)
        self.environment["INCLUDE"] = str(include)
        with self.assertRaisesRegex(workspace.BazelWorkspaceError, "exact selected"):
            self.render()

    def test_selected_tools_must_be_installed_under_selected_x64_toolset(self):
        for role in self.tools:
            original = self.tools[role]
            for source in (self.root / (role + ".exe"), self.vc / "bin/Hostx86/x64" / (role + ".exe")):
                source.parent.mkdir(parents=True, exist_ok=True)
                source.write_text("unselected tool fixture\n")
                self.tools[role] = source
                with (
                    self.subTest(role=role, source=source),
                    self.assertRaisesRegex(workspace.BazelWorkspaceError, "outside VCToolsInstallDir"),
                ):
                    self.render()
            self.tools[role] = original

    def test_missing_tool_and_destination_collision_are_rejected(self):
        original = self.tools["link"]
        self.tools["link"] = self.tools["cl"]
        with self.assertRaisesRegex(workspace.BazelWorkspaceError, "duplicate.*destination"):
            self.render()
        self.tools["link"] = original
        self.tools["ml64"].unlink()
        with self.assertRaisesRegex(workspace.BazelWorkspaceError, "outside VCToolsInstallDir"):
            self.render()

    def test_original_tool_and_include_selection_decisions_are_watched(self):
        observed = []
        with patch.object(workspace, "_INPUT_PROBE", observed.append):
            self.render()
        self.assertLessEqual(set(self.include + self.lib + list(self.tools.values())), set(observed))

    def test_identity_retains_sdk_markers_versions_and_all_selected_tool_inputs(self):
        identity = json.loads(self.identity().read_text())
        self.assertEqual(
            {item["path"] for item in identity["files"]},
            {str(path) for path in [*self.tools.values(), self.mt, *self.markers]},
        )
        self.assertEqual(identity["context"]["machine"], "AMD64")
        self.assertEqual(identity["context"]["VCToolsVersion"], "selected-vc")
        self.assertEqual(identity["context"]["UCRTVersion"], "selected-ucrt")
        self.assertEqual(identity["context"]["WindowsSDKVersion"], self.environment["WindowsSDKVersion"])
        manifest = next(item for item in identity["files"] if item["path"] == str(self.mt))
        self.assertEqual(manifest["sha256"], hashlib.sha256(self.mt.read_bytes()).hexdigest())

    def test_sdk_headers_libraries_and_compiler_runtime_invalidate_without_version_or_mtime_changes(self):
        inputs = [
            self.vc / "include/vector",
            self.include[1] / "other-crt.h",
            self.lib[0] / "runtime.lib",
            self.tools["cl"].with_name("c1xx.dll"),
            self.mt,
        ]
        for path in inputs[:-1]:
            path.write_bytes(b"original")
        for path in inputs:
            with self.subTest(path=path.relative_to(self.root)):
                baseline = self.identity().read_bytes()
                original, stat = path.read_bytes(), path.stat()
                path.write_bytes(b"modified")
                os.utime(path, ns=(stat.st_atime_ns, stat.st_mtime_ns))
                self.assertNotEqual(self.identity().read_bytes(), baseline)
                path.write_bytes(original)
                self.assertEqual(self.identity().read_bytes(), baseline)

    def test_metadata_has_no_compilation_mode_or_compiler_policy_channel(self):
        _, _, metadata = self.render()
        self.assertEqual(set(metadata), {"TOOLS", "TOOL_INPUTS", "BUILTIN_INCLUDE_DIRECTORIES", "MSVC_ENVIRONMENT"})
        for flag in ("/MD", "/MDd", "/O2", "/DNDEBUG", "/W4", "/std:c++latest"):
            self.assertNotIn(flag, json.dumps(metadata))

    def resolve(self, compiler_environment):
        prefix = self.root / "unrelated-provider/x64-windows-static-md"
        prefix.mkdir(parents=True, exist_ok=True)
        provider_status = prefix.parent / "vcpkg/status"
        provider_status.parent.mkdir(exist_ok=True)
        provider_status.write_text("unrelated provider metadata\n")
        request = self.root / "request.json"
        request.write_text(
            json.dumps(
                {
                    "schema": 1,
                    "host": "windows",
                    "config": "debug",
                    "prefix": str(prefix),
                    "compiler_environment": compiler_environment,
                }
            )
        )
        output = self.root / "repository/_resolution"
        arguments = [
            "resolver",
            "--request",
            str(request),
            "--group",
            "toolchain",
            "--output",
            str(output),
            "--environment-json",
            json.dumps({"PATH": self.environment["PATH"], "TMP": "repository-owned-scratch"}),
        ]

        def identity(generated, host, *, compiler_environment):
            self.assertEqual(host, "windows")
            self.assertEqual(compiler_environment, self.environment)
            (generated / "toolchain/identity.json").write_text(
                json.dumps({"files": [{"path": str(tool)} for tool in self.tools.values()]})
            )

        with (
            patch.object(sys, "argv", arguments),
            patch.object(sys, "addaudithook"),
            patch.dict(os.environ, {}, clear=True),
            patch.object(workspace, "native_platform", return_value="windows"),
            patch.object(workspace, "_INPUT_PROBE", None),
            patch.object(workspace, "_executable", side_effect=self.selected_tool),
            patch.object(workspace, "_toolchain_identity", side_effect=identity) as identify,
        ):
            status = resolver.main()
        return status, json.loads((output / "repository.json").read_text()), identify

    def test_actual_resolver_reports_missing_include_and_lib_without_running_identity(self):
        for name in ("INCLUDE", "LIB"):
            environment = dict(self.environment)
            del environment[name]
            with self.subTest(name=name):
                status, result, identify = self.resolve(environment)
                self.assertEqual(status, 1)
                self.assertIn("compiler_environment requires nonempty INCLUDE, LIB", result["error"])
                identify.assert_not_called()

    def test_resolver_publishes_identity_and_tool_inputs_without_a_compile_macro(self):
        status, result, identify = self.resolve(self.environment)
        self.assertEqual(status, 0, result.get("error"))
        identify.assert_called_once()
        self.assert_thin_build(result["files"]["BUILD.bazel"])
        self.assertNotIn("compile_identity", result["files"]["BUILD.bazel"])
        self.assertNotIn("AOBUS_BAZEL_TOOLCHAIN_ID", result["files"]["BUILD.bazel"])
        self.assertNotIn("cc_library", result["files"]["BUILD.bazel"])
        identity = json.loads(result["files"]["identity.json"])
        self.assertEqual({item["path"] for item in identity["files"]}, {str(tool) for tool in self.tools.values()})
        self.assertEqual(len(result["links"]), 4)
        self.assertLessEqual({str(tool) for tool in self.tools.values()}, set(result["watch_files"]))
        unrelated = self.root / "unrelated-provider"
        self.assertFalse(
            any(
                Path(path).is_relative_to(unrelated)
                for kind in ("watch_files", "watch_directories")
                for path in result[kind]
            )
        )
        metadata = {
            node.targets[0].id: ast.literal_eval(node.value)
            for node in ast.parse(result["files"]["native_paths.bzl"]).body
            if isinstance(node, ast.Assign)
        }
        self.assertEqual(
            metadata["MSVC_ENVIRONMENT"], {name: self.environment[name] for name in ("INCLUDE", "LIB", "PATH", "TMP")}
        )
        self.assertEqual(metadata["TOOL_INPUTS"], {role: "tools/" + role + ".exe" for role in self.tools})
        self.assertEqual(
            result["links"],
            [{"source": str(tool), "destination": metadata["TOOL_INPUTS"][role]} for role, tool in self.tools.items()],
        )
        self.assertNotIn("repository-owned-scratch", result["files"]["native_paths.bzl"])


class WindowsToolchainPreparationTest(unittest.TestCase):
    def test_case_insensitive_windows_capture_is_request_only_and_keeps_original_strings(self):
        selected = {
            "INCLUDE": "C:\\VS\\VC\\Tools\\MSVC\\selected\\include;C:\\SDK\\Include\\10.selected\\um;",
            "LIB": "C:\\VS\\lib;C:\\SDK\\lib;",
            "PATH": "C:\\VS\\bin\\Hostx64\\x64;C:\\Windows\\System32",
            "TMP": "C:\\Users\\fixture\\AppData\\Local\\Temp",
            "VCToolsInstallDir": "C:\\VS\\VC\\Tools\\MSVC\\selected\\",
            "WindowsSdkDir": "C:\\SDK\\",
            "WindowsSDKVersion": "10.selected\\",
            "NETFXSDKDir": "C:\\Windows Kits\\NETFXSDK\\selected-netfx\\",
        }
        with tempfile.TemporaryDirectory(prefix="aobus-msvc-request-") as temporary:
            root = Path(temporary)
            with (
                patch.dict(os.environ, {name.swapcase(): value for name, value in selected.items()}, clear=True),
                patch.object(workspace, "native_platform", return_value="windows"),
                patch.object(workspace, "vcpkg_prefix", return_value=root / "provider"),
                patch.object(workspace.builddir, "windows_state_root", return_value=root / "state"),
            ):
                prepared = workspace.prepare_workspace(root / "build", "debug")
            request = json.loads((prepared / "request.json").read_text())
            self.assertEqual({name: request["compiler_environment"][name] for name in selected}, selected)
            rc = (prepared / "aobus.bazelrc").read_text()
            self.assertNotIn("--repo_env=INCLUDE=", rc)
            self.assertNotIn("--repo_env=LIB=", rc)
            self.assertNotIn(selected["TMP"], rc)

    def test_prepare_only_missing_native_environment_is_captured_but_not_invented(self):
        with tempfile.TemporaryDirectory(prefix="aobus-msvc-empty-request-") as temporary:
            root = Path(temporary)
            with (
                patch.dict(os.environ, {}, clear=True),
                patch.object(workspace, "native_platform", return_value="windows"),
                patch.object(workspace, "vcpkg_prefix", return_value=root / "provider"),
                patch.object(workspace.builddir, "windows_state_root", return_value=root / "state"),
            ):
                prepared = workspace.prepare_workspace(root / "build", "release")
            request = json.loads((prepared / "request.json").read_text())
            self.assertEqual(set(request["compiler_environment"].values()), {""})

    def test_prepare_selects_git_for_windows_bash_unless_the_caller_names_a_shell(self):
        with tempfile.TemporaryDirectory(prefix="aobus-git-bash-") as temporary:
            root = Path(temporary)
            git = root / "Git/cmd/git.exe"
            bash = root / "Git/usr/bin/bash.exe"
            for path in (git, bash):
                path.parent.mkdir(parents=True)
                path.write_bytes(b"")

            def shell(environment, *, found=str(git)):
                with (
                    patch.dict(os.environ, environment, clear=True),
                    patch.object(workspace, "native_platform", return_value="windows"),
                    patch.object(workspace, "vcpkg_prefix", return_value=root / "provider"),
                    patch.object(workspace.builddir, "windows_state_root", return_value=root / "state"),
                    patch.object(workspace.shutil, "which", return_value=found) as which,
                ):
                    prepared = workspace.prepare_workspace(root / "build", "debug")
                rc = (prepared / "aobus.bazelrc").read_text(encoding="utf-8")
                selected = [line for line in rc.splitlines() if "--repo_env=BAZEL_SH=" in line]
                return [
                    json.loads(line.split(" ", 1)[1]).partition("=")[2].partition("=")[2] for line in selected
                ], which

            selected, which = shell({})
            self.assertEqual(selected, [str(bash.resolve())])
            which.assert_called_once_with("git")
            explicit = str(root / "msys64/usr/bin/bash.exe")
            selected, which = shell({"BAZEL_SH": explicit})
            self.assertEqual(selected, [explicit])
            which.assert_not_called()
            bash.unlink()
            self.assertEqual(shell({})[0], [])
            self.assertEqual(shell({}, found=None)[0], [])


class WindowsToolchainDefinitionTest(unittest.TestCase):
    def setUp(self):
        self.source = (workspace.PROJECT_ROOT / "bazel/windows_toolchain.bzl").read_text()
        self.tree = ast.parse(self.source)
        self.macro = next(node for node in self.tree.body if isinstance(node, ast.FunctionDef))
        self.calls = {}
        for node in self.macro.body:
            if isinstance(node, ast.Expr) and isinstance(node.value, ast.Call):
                fields = {keyword.arg: keyword.value for keyword in node.value.keywords}
                if "name" in fields:
                    self.calls[ast.literal_eval(fields["name"])] = (ast.unparse(node.value.func), fields)

    def test_warning_bridge_is_disabled_data_free_and_cpp_only(self):
        kind, config = self.calls["msvc_config"]
        self.assertEqual(kind, "cc_toolchain_config")
        self.assertEqual(ast.literal_eval(config["extra_known_features"]), [":aobus_warnings"])
        self.assertEqual(ast.literal_eval(config["extra_enabled_features"]), [":aobus_external_headers"])
        _, feature = self.calls["aobus_warnings"]
        self.assertEqual(ast.literal_eval(feature["feature_name"]), "aobus_warnings")
        self.assertEqual(ast.literal_eval(feature["args"]), [":warning_args"])
        _, arguments = self.calls["warning_args"]
        self.assertEqual(ast.literal_eval(arguments["args"]), ["/W4", "/WX"])
        self.assertEqual(
            ast.literal_eval(arguments["actions"]), ["@rules_cc//cc/toolchains/actions:cpp_compile_actions"]
        )
        self.assertNotIn("data", arguments)
        self.assertIn("pinned-private", self.source)
        self.assertIn("@rules_cc//cc/private/toolchain:windows_cc_toolchain_config.bzl", self.source)
        self.assertIn("@rules_cc//cc/toolchains:cc_toolchain.bzl", self.source)

    def test_fixed_cpp_policy_and_actual_mode_ownership_are_not_python_flags(self):
        _, config = self.calls["msvc_config"]
        self.assertEqual(ast.literal_eval(config["cpu"]), "x64_windows")
        self.assertEqual(ast.literal_eval(config["compiler"]), "msvc-cl")
        self.assertEqual(
            ast.literal_eval(config["cxx_flags"]),
            [
                "/std:c++latest",
                "/external:W0",
                "/utf-8",
                "/permissive-",
                "/Zc:__cplusplus",
                "/Zc:preprocessor",
                "/DWIN32_LEAN_AND_MEAN",
                "/DUNICODE",
                "/D_UNICODE",
                "/DWINVER=0x0A00",
            ],
        )
        self.assertEqual(ast.literal_eval(config["win32_winnt_flag"]), "/D_WIN32_WINNT=0x0A00")
        # Reuse the pinned defaults for NOMINMAX/CRT suppression, dynamic CRT,
        # mode features and the earlier C++17 baseline; do not duplicate them.
        for field in ("default_compile_flags", "all_compile_flags", "conly_flags"):
            self.assertNotIn(field, config)
        self.assertNotIn("/MDd", self.source)
        self.assertNotIn("/DNDEBUG", self.source)

    def test_archive_link_pdb_response_file_and_tool_inputs_remain_explicit(self):
        _, config = self.calls["msvc_config"]
        for name in ("archiver_flags", "default_link_flags"):
            self.assertEqual(ast.literal_eval(config[name]), ["/MACHINE:X64"])
        self.assertEqual(ast.literal_eval(config["dbg_mode_debug_flag"]), "/DEBUG:FULL")
        self.assertEqual(ast.literal_eval(config["fastbuild_mode_debug_flag"]), "/DEBUG:FASTLINK")
        self.assertTrue(ast.literal_eval(config["supports_parse_showincludes"]))
        self.assertTrue(ast.literal_eval(config["shorten_virtual_includes"]))
        _, toolchain = self.calls["msvc"]
        self.assertEqual(ast.literal_eval(toolchain["supports_param_files"]), 1)
        for field, target in (
            ("all_files", "all_inputs"),
            ("compiler_files", "compiler_inputs"),
            ("as_files", "assembler_inputs"),
            ("ar_files", "archiver_inputs"),
            ("linker_files", "linker_inputs"),
        ):
            self.assertEqual(ast.literal_eval(toolchain[field]), ":" + target)
        for name, role in (
            ("compiler_inputs", "cl"),
            ("assembler_inputs", "ml64"),
            ("archiver_inputs", "lib"),
            ("linker_inputs", "link"),
        ):
            _, files = self.calls[name]
            self.assertEqual(ast.unparse(files["srcs"]), f"[':metadata', tool_inputs['{role}']]")
        self.assertNotIn("msvc_nop", self.source)
        self.assertNotIn('"gcov":', self.source)
        self.assertNotIn('"cpp-module-deps-scanner":', self.source)
        for role in ("nm", "objdump", "strip"):
            self.assertIn(f'"{role}": "unsupported/{role}"', self.source)
        kind, registered = self.calls["registered_msvc"]
        self.assertEqual(kind, "native.toolchain")
        self.assertEqual(ast.literal_eval(registered["toolchain"]), ":msvc")
        for field in ("exec_compatible_with", "target_compatible_with"):
            self.assertEqual(ast.literal_eval(registered[field]), ["@platforms//cpu:x86_64", "@platforms//os:windows"])

    def test_macro_keeps_installed_execution_paths_distinct_from_caller_relative_inputs(self):
        self.assertEqual(self.macro.name, "windows_toolchain")
        self.assertEqual(
            [arg.arg for arg in self.macro.args.args],
            ["tools", "tool_inputs", "builtin_include_directories", "msvc_environment"],
        )
        self.assertFalse(
            any(isinstance(node, ast.Call) and ast.unparse(node.func) == "Label" for node in ast.walk(self.tree))
        )
        kind, metadata = self.calls["metadata"]
        self.assertEqual(kind, "native.filegroup")
        self.assertEqual(ast.literal_eval(metadata["srcs"]), ["identity.json", "native_paths.bzl"])
        _, config = self.calls["msvc_config"]
        for field, role in (
            ("msvc_cl_path", "cl"),
            ("msvc_link_path", "link"),
            ("msvc_lib_path", "lib"),
            ("msvc_ml_path", "ml64"),
        ):
            self.assertEqual(ast.unparse(config[field]), f"tools['{role}']")
        paths = {
            ast.literal_eval(key): ast.unparse(value)
            for key, value in zip(config["tool_paths"].keys, config["tool_paths"].values, strict=True)
        }
        for role, executable in (("ar", "lib"), ("cpp", "cl"), ("gcc", "cl"), ("ld", "link"), ("ml", "ml64")):
            self.assertEqual(paths[role], f"tools['{executable}']")
        self.assertEqual(ast.unparse(config["cxx_builtin_include_directories"]), "builtin_include_directories")
        for field, name in (
            ("msvc_env_include", "INCLUDE"),
            ("msvc_env_lib", "LIB"),
            ("msvc_env_path", "PATH"),
            ("msvc_env_tmp", "TMP"),
        ):
            self.assertEqual(ast.unparse(config[field]), f"msvc_environment['{name}']")


if __name__ == "__main__":
    unittest.main()
