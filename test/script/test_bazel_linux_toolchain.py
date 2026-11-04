"""Nix SDK metadata, bounded compiler built-ins and repository-owned tool links."""

import ast
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from ao.core import bazel_workspace as workspace


class LinuxToolchainProjectionTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-linux-toolchain-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.sdk = self.root / "compiler"
        include = self.sdk / "include/c++"
        include.mkdir(parents=True)
        self.include = include
        tools = {}
        for role, name in (
            ("clang", "clang"),
            ("clang++", "clang++"),
            ("ar", "llvm-ar"),
            ("strip", "llvm-strip"),
            ("objcopy", "llvm-objcopy"),
        ):
            file = self.sdk / "bin" / name
            file.parent.mkdir(exist_ok=True)
            file.write_text("owned tool fixture\n")
            file.chmod(0o755)
            tools[role] = str(file)
        self.wrapper = {
            "suffixSalt": "fixture_target",
            "hardeningEnable": ["fortify", "fortify3", "stackprotector", "relro"],
            "tools": tools,
            "builtinRoots": [str(self.sdk)],
        }
        self.stderr = f"#include <...> search starts here:\n {include}\nEnd of search list.\n"

    def render(self):
        with (
            patch.object(workspace, "_linux_cc_wrapper", return_value=self.wrapper),
            patch.object(workspace, "_nix_root", return_value=self.sdk.resolve()),
            patch.object(workspace, "_host_machine", return_value="x86_64"),
            patch.object(
                workspace.subprocess,
                "run",
                return_value=subprocess.CompletedProcess([], 0, "", self.stderr),
            ) as run,
        ):
            generated = self.root / "generated"
            links = workspace._linux_toolchain(generated)
        metadata = {}
        for node in ast.parse((generated / "toolchain/native_paths.bzl").read_text()).body:
            if isinstance(node, ast.Assign):
                metadata[node.targets[0].id] = ast.literal_eval(node.value)
        return generated, links, metadata, run

    def test_toolchain_projects_tools_without_copying_the_provider(self):
        generated, links, metadata, run = self.render()
        self.assertEqual(len(links), 5)
        self.assertEqual(metadata["TOOLS"]["clang++"], "tools/clang++")
        self.assertEqual(metadata["TOOLS"]["strip"], "tools/llvm-strip")
        self.assertFalse((generated / "toolchain/tools").exists())
        self.assertEqual(metadata["BUILTIN_INCLUDE_DIRECTORIES"], [str(self.include)])
        self.assertEqual(metadata["CONSTRAINT_CPU"], "x86_64")
        self.assertEqual(run.call_args.kwargs["env"], {"PATH": ""})
        self.assertEqual(run.call_args.kwargs["input"], "")
        self.assertIn("-std=c++26", run.call_args.args[0])

    def test_generated_build_loads_only_the_linux_macro_and_passes_local_metadata(self):
        with patch.object(workspace, "PROJECT_ROOT", self.root / "no-policy-templates"):
            generated, _, metadata, _ = self.render()
        tree = ast.parse((generated / "toolchain/BUILD.bazel").read_text())
        calls = [node.value for node in tree.body]
        self.assertEqual([ast.unparse(call.func) for call in calls], ["load", "load", "package", "linux_toolchain"])
        self.assertEqual(
            [ast.literal_eval(arg) for arg in calls[0].args],
            ["@aobus//bazel:linux_toolchain.bzl", "linux_toolchain"],
        )
        self.assertEqual(ast.literal_eval(calls[1].args[0]), ":native_paths.bzl")
        self.assertEqual({ast.literal_eval(arg) for arg in calls[1].args[1:]}, set(metadata))
        self.assertEqual(ast.literal_eval(calls[2].keywords[0].value), ["//visibility:public"])
        self.assertEqual(
            {field.arg: ast.unparse(field.value) for field in calls[3].keywords},
            {
                "tools": "TOOLS",
                "builtin_include_directories": "BUILTIN_INCLUDE_DIRECTORIES",
                "hardening_environment": "HARDENING_ENVIRONMENT",
                "constraint_cpu": "CONSTRAINT_CPU",
            },
        )

    def test_all_modes_are_metadata_not_preselected_compiler_options(self):
        generated, _, metadata, _ = self.render()
        policies = metadata["HARDENING_ENVIRONMENT"]
        self.assertEqual(set(policies), {"dbg", "opt", "fastbuild"})
        self.assertEqual(policies["dbg"], {"NIX_HARDENING_ENABLE_fixture_target": "stackprotector relro"})
        self.assertEqual(policies["fastbuild"], policies["dbg"])
        self.assertEqual(
            policies["opt"], {"NIX_HARDENING_ENABLE_fixture_target": "fortify fortify3 stackprotector relro"}
        )
        source = (generated / "toolchain/native_paths.bzl").read_text()
        self.assertNotIn("-O0", source)
        self.assertNotIn("-O2", source)
        self.assertNotIn("-DNDEBUG", source)

    def test_lexical_tool_paths_are_not_canonicalized_for_multicall_dispatch(self):
        (self.sdk / "bin/alias").mkdir()
        lexical = str(self.sdk / "bin/alias/../llvm-strip")
        self.wrapper["tools"]["strip"] = lexical
        _, links, _, _ = self.render()
        self.assertIn({"source": lexical, "destination": "tools/llvm-strip"}, links)

    def test_application_headers_are_not_admitted_as_compiler_builtins(self):
        application = self.root / "application/include"
        application.mkdir(parents=True)
        self.stderr = f"#include <...> search starts here:\n {application}\nEnd of search list.\n"
        with self.assertRaisesRegex(workspace.BazelWorkspaceError, "outside its declared Nix SDK roots"):
            self.render()

    def test_missing_or_empty_builtin_interface_fails_closed(self):
        for output in ("no search metadata", "#include <...> search starts here:\nEnd of search list.\n"):
            with self.subTest(output=output):
                self.stderr = output
                with self.assertRaisesRegex(workspace.BazelWorkspaceError, "include"):
                    self.render()

    def test_missing_or_ambiguous_tools_are_rejected(self):
        del self.wrapper["tools"]["ar"]
        with self.assertRaisesRegex(workspace.BazelWorkspaceError, "SDK metadata"):
            self.render()
        self.wrapper["tools"]["ar"] = self.wrapper["tools"]["clang"]
        with self.assertRaisesRegex(workspace.BazelWorkspaceError, "duplicate.*basename"):
            self.render()

    def test_whole_store_or_nonroot_compiler_allowlists_are_rejected(self):
        self.wrapper["builtinRoots"] = [str(self.include)]
        with self.assertRaisesRegex(workspace.BazelWorkspaceError, "built-in root"):
            self.render()


class LinuxToolchainModesTest(unittest.TestCase):
    def setUp(self):
        self.tree = ast.parse((workspace.PROJECT_ROOT / "bazel/linux_toolchain.bzl").read_text())
        self.macro = next(node for node in self.tree.body if isinstance(node, ast.FunctionDef))
        self.calls = {}
        for node in self.macro.body:
            if isinstance(node, ast.Expr) and isinstance(node.value, ast.Call):
                fields = {keyword.arg: keyword.value for keyword in node.value.keywords}
                if "name" in fields:
                    self.calls[ast.literal_eval(fields["name"])] = (ast.unparse(node.value.func), fields)

    def test_shared_libraries_retain_bazels_standard_solib_link_names(self):
        _, fields = self.calls["nix_clang"]
        arguments = ast.literal_eval(fields["args"]) + ast.literal_eval(fields["enabled_features"])
        # The pinned API's runtime_solib_name differs from the external solib alias.
        self.assertNotIn("@rules_cc//cc/toolchains/args/soname_flags:set_soname", arguments)
        self.assertIn("@rules_cc//cc/toolchains/args/runtime_library_search_directories:feature", arguments)

    def test_owned_warning_feature_is_opt_in_and_precedes_user_compile_flags(self):
        calls = {name: fields for name, (_, fields) in self.calls.items()}
        toolchain = calls["nix_clang"]
        self.assertIn(":aobus_warnings", ast.literal_eval(toolchain["known_features"]))
        self.assertNotIn(":aobus_warnings", ast.literal_eval(toolchain["enabled_features"]))
        self.assertEqual(ast.literal_eval(calls["aobus_warnings"]["feature_name"]), "aobus_warnings")
        arguments = calls["warning_policy"]
        self.assertEqual(ast.literal_eval(arguments["requires_any_of"]), [":aobus_warnings"])
        self.assertEqual(
            ast.literal_eval(arguments["actions"]), ["@rules_cc//cc/toolchains/actions:cpp_compile_actions"]
        )
        self.assertEqual(
            set(ast.literal_eval(arguments["args"].left)),
            {"-Wthread-safety", "-Wself-assign", "-Wunused-but-set-parameter", "-Wno-free-nonheap-object"},
        )
        self.assertEqual(ast.unparse(arguments["args"].right), "UNIX_WARNINGS")
        self.assertIn(":warning_policy", ast.literal_eval(toolchain["args"]))
        self.assertEqual(
            ast.literal_eval(toolchain["enabled_features"])[-1],
            "@rules_cc//cc/toolchains/args/compile_flags:feature",
        )

    def test_macro_keeps_tool_identity_inputs_and_registration_caller_relative(self):
        self.assertEqual(self.macro.name, "linux_toolchain")
        self.assertEqual(
            [arg.arg for arg in self.macro.args.args],
            ["tools", "builtin_include_directories", "hardening_environment", "constraint_cpu"],
        )
        self.assertFalse(
            any(isinstance(node, ast.Call) and ast.unparse(node.func) == "Label" for node in ast.walk(self.tree))
        )
        kind, identity = self.calls["identity"]
        self.assertEqual(kind, "native.filegroup")
        self.assertEqual(ast.literal_eval(identity["srcs"]), ["identity.json", "native_paths.bzl"])
        for name, role in (
            ("clang", "clang"),
            ("clang++", "clang++"),
            ("llvm-ar", "ar"),
            ("llvm-strip", "strip"),
            ("llvm-objcopy", "objcopy"),
        ):
            with self.subTest(tool=name):
                kind, tool = self.calls[name]
                self.assertEqual(kind, "cc_tool")
                self.assertEqual(ast.unparse(tool["src"]), f"tools['{role}']")
                self.assertEqual(ast.literal_eval(tool["data"]), [":identity"])
        for name in ("compile_policy", "dbg_hardening", "opt_hardening", "fastbuild_hardening"):
            self.assertEqual(ast.literal_eval(self.calls[name][1]["data"]), [":identity"])
        self.assertEqual(
            ast.unparse(self.calls["compile_policy"][1]["allowlist_absolute_include_directories"]),
            "builtin_include_directories",
        )
        kind, registration = self.calls["registered_nix_clang"]
        self.assertEqual(kind, "native.toolchain")
        self.assertEqual(ast.literal_eval(registration["toolchain"]), ":nix_clang")
        for field in ("exec_compatible_with", "target_compatible_with"):
            self.assertEqual(ast.literal_eval(registration[field].elts[0]), "@platforms//os:linux")
            self.assertEqual(ast.unparse(registration[field].elts[1]), "'@platforms//cpu:' + constraint_cpu")

    def test_bazel_mode_features_select_arguments_without_default_enabling_modes(self):
        calls = self.calls
        kind, toolchain = calls["nix_clang"]
        self.assertEqual(kind, "cc_toolchain")
        known = ast.literal_eval(toolchain["known_features"])
        enabled = ast.literal_eval(toolchain["enabled_features"])
        for mode, expected in (
            ("dbg", ["-O0", "-g"]),
            ("opt", ["-O2", "-DNDEBUG", "-ffunction-sections", "-fdata-sections", "-g0"]),
            ("fastbuild", ["-O0", "-g0"]),
        ):
            with self.subTest(mode=mode):
                kind, feature = calls[mode]
                self.assertEqual(kind, "cc_feature")
                self.assertEqual(ast.literal_eval(feature["overrides"]), "@rules_cc//cc/toolchains/features:" + mode)
                self.assertIn(":" + mode, known)
                self.assertNotIn(":" + mode, enabled)
                _, constraint = calls["is_" + mode]
                self.assertEqual(ast.literal_eval(constraint["all_of"]), [":" + mode])
                _, arguments = calls[mode + "_compile"]
                self.assertEqual(ast.literal_eval(arguments["args"]), expected)
                self.assertEqual(ast.literal_eval(arguments["requires_any_of"]), [":is_" + mode])
                _, policy = calls[mode + "_hardening"]
                self.assertEqual(ast.literal_eval(policy["requires_any_of"]), [":is_" + mode])
                self.assertEqual(ast.unparse(policy["env"]), f"hardening_environment['{mode}']")
                self.assertIn("@rules_cc//cc/toolchains/actions:link_actions", ast.literal_eval(policy["actions"]))
        _, optimized_link = calls["opt_link"]
        self.assertEqual(ast.literal_eval(optimized_link["actions"]), ["@rules_cc//cc/toolchains/actions:link_actions"])
        self.assertEqual(ast.literal_eval(optimized_link["args"]), ["-Wl,--gc-sections"])
        self.assertEqual(ast.literal_eval(optimized_link["requires_any_of"]), [":is_opt"])
        self.assertIn(":opt_link", ast.literal_eval(toolchain["args"]))
        _, standard = calls["cpp_standard"]
        self.assertEqual(
            ast.literal_eval(standard["actions"]), ["@rules_cc//cc/toolchains/actions:cpp_compile_actions"]
        )
        self.assertEqual(ast.literal_eval(standard["args"]), ["-std=c++26"])


if __name__ == "__main__":
    unittest.main()
