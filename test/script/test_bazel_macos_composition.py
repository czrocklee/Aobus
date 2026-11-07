"""Functional composition of public Apple rules over admitted native SDK facts."""

import shlex
import unittest
from pathlib import Path
from types import SimpleNamespace

_BAZEL = Path(__file__).resolve().parents[2] / "bazel"
_SOURCE = "@apple_support//crosstool:wrapped_clang.cc"
_PUBLIC_MACRO = "@apple_support//toolchain:cc_toolchain.bzl"
_GENRULE = "@apple_support//rules:apple_genrule.bzl"


class _ToolLabels(dict):
    def values(self):
        # Starlark dict.values() returns a list, unlike Python's dictionary view.
        return list(super().values())


class _Composition:
    def __init__(self):
        self.rules = {}
        self.loads = []
        self.exports = []
        self.selection_errors = []

    def record(self, kind):
        def declare(**kwargs):
            name = kwargs["name"]
            if name in self.rules:
                raise AssertionError("duplicate declaration: " + name)
            self.rules[name] = (kind, kwargs)

        return declare

    def select(self, choices, *, no_match_error):
        self.selection_errors.append(no_match_error)
        return choices


def _facts():
    return {
        "llvm": "/Åsa/LLVM 22",
        "llvm_real": "/private/Åsa/LLVM 22",
        "llvm_version": "22.1.8",
        "sdk": "/Developer/Selected SDK/MacOSX.sdk",
        "shim": "/Åsa/expected shim",
        "linker": "/Developer/Selected Tools/ld",
        "developer_dir": "/Åsa/Xcode.app/Contents/Developer",
        "deployment": "15.0",
        "cpu": "arm64",
        "constraint_cpu": "aarch64",
        "tools": _ToolLabels(
            {
                "ar": "tools/llvm-libtool-darwin",
                "strip": "tools/llvm-strip",
                "clang++": "tools/Åsa clang++",
            }
        ),
    }


def _compose(facts):
    state = _Composition()
    namespace = {}

    def load(label, *symbols, **aliases):
        state.loads.append((label, symbols, aliases))
        if label == ":compiler_policy.bzl" and symbols == ("UNIX_WARNINGS",):
            policy = {}
            source = (_BAZEL / "compiler_policy.bzl").read_text(encoding="utf-8")
            exec(compile(source, "compiler_policy.bzl", "exec"), policy)
            namespace["UNIX_WARNINGS"] = policy["UNIX_WARNINGS"]
        elif (label, aliases) == (_PUBLIC_MACRO, {"apple_cc_toolchain": "cc_toolchain"}):
            namespace["apple_cc_toolchain"] = state.record("apple_cc_toolchain")
        elif not aliases and len(symbols) == 1 and (label.startswith("@rules_cc//cc/toolchains:") or label == _GENRULE):
            namespace[symbols[0]] = state.record(symbols[0])
        else:
            raise AssertionError("unexpected load: " + label)

    namespace.update(
        load=load,
        select=state.select,
        native=SimpleNamespace(
            exports_files=state.exports.extend,
            filegroup=state.record("filegroup"),
            platform=state.record("platform"),
            config_setting=state.record("config_setting"),
            toolchain=state.record("toolchain"),
        ),
    )
    source = (_BAZEL / "macos_cc_config.bzl").read_text(encoding="utf-8")
    exec(compile(source, "macos_cc_config.bzl", "exec"), namespace)
    namespace["macos_toolchain"](**facts)
    return state


def _bootstrap_argv(declaration):
    # Model only Bazel's known make substitutions, then parse POSIX quoting.
    command = declaration["cmd"].replace("$(execpath " + _SOURCE + ")", "external/apple_support+/wrapped_clang.cc")
    for output in declaration["outs"]:
        command = command.replace("$(execpath :" + output + ")", "out/external/sdk/" + output)
    return shlex.split(command.replace("$$", "$"))


def _patched_then_branch(platform_args, extra_include_directories):
    added = [
        line[1:]
        for line in (_BAZEL / "apple_support_llvm.patch").read_text(encoding="utf-8").splitlines()
        if line.startswith("+") and not line.startswith("+++")
    ]
    line = next(line for line in added if "if platform_args else" in line)
    expression = line.split("args = ", 1)[1].split(" if platform_args else", 1)[0]
    return eval(
        expression,
        {"platform_args": platform_args, "extra_include_directories": extra_include_directories},
    )


class MacosAppleSupportCompositionTest(unittest.TestCase):
    def setUp(self):
        self.facts = _facts()
        self.state = _compose(self.facts)

    def rule(self, name):
        return self.state.rules[name]

    def test_public_macro_and_genrule_load_external_source_without_vendoring(self):
        self.assertIn((_PUBLIC_MACRO, (), {"apple_cc_toolchain": "cc_toolchain"}), self.state.loads)
        self.assertIn((_GENRULE, ("apple_genrule",), {}), self.state.loads)
        self.assertFalse(any("cc_toolchain_config.bzl" in label for label, _, _ in self.state.loads))
        self.assertFalse(any(_BAZEL.rglob("wrapped_clang.cc")))
        self.assertEqual(self.rule("llvm22")[0], "apple_cc_toolchain")
        call = self.rule("llvm22")[1]
        self.assertIs(call["flags_from_env"], False)
        self.assertEqual(call["module_map"], "@apple_support//crosstool:module.modulemap")
        self.assertNotIn("extra_include_directories", call)
        self.assertNotIn("apple_environment", call)

    def test_bootstrap_declares_live_identity_and_tools_without_using_them_as_sources(self):
        kind, entry = self.rule("compiler_entry")
        self.assertEqual(kind, "apple_genrule")
        self.assertEqual(set(entry["srcs"]), {":identity", _SOURCE})
        self.assertEqual(set(entry["tools"]), set(self.facts["tools"].values()))
        self.assertEqual(entry["outs"], ["entry/wrapped_clang", "entry/wrapped_clang_pp"])
        self.assertNotIn(":llvm22", entry["srcs"] + entry["tools"])
        argv = _bootstrap_argv(entry)
        self.assertEqual(
            argv[:4],
            [
                "env",
                "DEVELOPER_DIR=" + self.facts["developer_dir"],
                "SDKROOT=" + self.facts["sdk"],
                self.facts["llvm"] + "/bin/clang++",
            ],
        )
        self.assertIn("--no-default-config", argv)
        self.assertIn("-std=c++17", argv)
        self.assertIn("arm64-apple-macosx15.0", argv)
        self.assertIn(self.facts["sdk"], argv)
        self.assertIn("--ld-path=" + self.facts["linker"], argv)
        self.assertEqual(
            argv[-8:],
            [
                "external/apple_support+/wrapped_clang.cc",
                "-o",
                "out/external/sdk/entry/wrapped_clang",
                "&&",
                "ln",
                "-s",
                "wrapped_clang",
                "out/external/sdk/entry/wrapped_clang_pp",
            ],
        )
        self.assertNotIn(":identity", argv)
        self.assertFalse(any(tool in argv for tool in self.facts["tools"].values()))

    def test_bootstrap_preserves_literal_dollars_quotes_spaces_and_unicode_in_sdk_paths(self):
        facts = _facts()
        for key in ("llvm", "sdk", "linker", "developer_dir"):
            facts[key] = "/Åsa/$literal 'selected'/" + key
        entry = _compose(facts).rules["compiler_entry"][1]
        argv = _bootstrap_argv(entry)
        self.assertIn(facts["llvm"] + "/bin/clang++", argv)
        self.assertIn("SDKROOT=" + facts["sdk"], argv)
        self.assertIn("DEVELOPER_DIR=" + facts["developer_dir"], argv)
        self.assertIn("--ld-path=" + facts["linker"], argv)

    def test_platform_projects_target_and_exec_minima_without_fallback_sdk_inputs(self):
        platform = self.rule("platform")[1]
        self.assertEqual(platform["constraint_values"], ["@platforms//os:macos", "@platforms//cpu:aarch64"])
        self.assertEqual(platform["flags"], ["--macos_minimum_os=15.0", "--host_macos_minimum_os=15.0"])
        admitted = self.rule("admitted_deployment")[1]
        self.assertEqual(admitted["values"], {"macos_minimum_os": "15.0", "host_macos_minimum_os": "15.0"})
        self.assertEqual(self.rule("native_inputs")[1]["data"], {":admitted_deployment": [":identity"]})
        self.assertEqual(len(self.state.selection_errors), 1)
        self.assertIn("15.0", self.state.selection_errors[0])
        rc = (_BAZEL.parent / ".bazelrc").read_text(encoding="utf-8")
        self.assertIn("build:macos --platforms=@aobus_toolchain//:platform", rc.splitlines())

    def test_compiler_map_is_file_backed_and_declares_original_bytes(self):
        closure = {":identity", *self.facts["tools"].values()}
        for name, source in (("clang", "entry/wrapped_clang"), ("clang++", "entry/wrapped_clang_pp")):
            kind, tool = self.rule(name)
            self.assertEqual(kind, "cc_tool")
            self.assertEqual(tool["src"], source)
            self.assertEqual(set(tool["data"]), closure)
            self.assertEqual(tool["env"], {"APPLE_SUPPORT_LLVM_ROOT": self.facts["llvm"]})
        self.assertEqual(self.rule("libtool")[1]["src"], self.facts["tools"]["ar"])
        self.assertEqual(self.rule("strip")[1]["src"], self.facts["tools"]["strip"])
        mapped = self.rule("tools")[1]["tools"]
        self.assertEqual(mapped["@rules_cc//cc/toolchains/actions:cpp_compile"], ":clang++")
        self.assertEqual(mapped["@rules_cc//cc/toolchains/actions:cpp_link_executable"], ":clang++")
        self.assertEqual(set(self.state.exports), {"identity.json", "native_paths.bzl", *self.facts["tools"].values()})

    def test_warnings_are_gated_inside_the_enabled_policy_before_user_flags(self):
        marker = self.rule("aobus_warnings")[1]
        self.assertEqual(marker["feature_name"], "aobus_warnings")
        self.assertNotIn("enabled", marker)
        self.assertNotIn("implies", marker)
        policy = self.rule("warning_policy")[1]
        self.assertEqual(policy["requires_any_of"], [":aobus_warnings"])
        self.assertIn("-Wall", policy["args"])
        self.assertIn("-Werror", policy["args"])
        native_policy = self.rule("native_policy")[1]["args"]
        self.assertLess(native_policy.index(":warning_policy"), native_policy.index(":cpp_policy"))
        call = self.rule("llvm22")[1]
        self.assertEqual(call["extra_enabled_features"], ":enabled_policy")
        self.assertIn(":native_policy", self.rule("enabled_policy")[1]["all_of"])
        self.assertEqual(call["extra_known_features"], ":known_policy")
        self.assertEqual(self.rule("known_policy")[1]["all_of"], [":aobus_warnings"])
        self.assertNotIn(":aobus_warnings", self.rule("enabled_policy")[1]["all_of"])

    def test_enabled_policy_retains_language_sdk_linker_but_leaves_arc_and_target_to_standard_rules(self):
        cpp = self.rule("cpp_policy")[1]
        self.assertIn("-std=c++26", cpp["args"])
        self.assertIn(self.facts["shim"], cpp["args"])
        self.assertIn(self.facts["sdk"], self.rule("sysroot_args")[1]["args"])
        linker = self.rule("selected_linker")[1]
        self.assertIn("--ld-path=" + self.facts["linker"], linker["args"])
        self.assertIn(self.facts["sdk"], linker["args"])
        self.assertIn("@rules_cc//cc/toolchains/actions:objc_executable", linker["actions"])
        local_args = [
            argument
            for kind, rule in self.state.rules.values()
            if kind == "cc_args"
            for argument in rule.get("args", [])
        ]
        self.assertNotIn("-target", local_args)
        self.assertNotIn("-fobjc-arc", local_args)
        self.assertIn(":selected_linker", self.rule("native_policy")[1]["args"])
        self.assertIn("@apple_support//toolchain:external_include_paths", self.rule("enabled_policy")[1]["all_of"])

    def test_platform_args_replace_automatic_apple_includes_and_environment(self):
        inputs = self.rule("native_inputs")[1]
        self.assertEqual(
            inputs["allowlist_absolute_include_directories"],
            [self.facts["llvm"], self.facts["llvm_real"], self.facts["sdk"], self.facts["shim"]],
        )
        self.assertEqual(
            inputs["env"],
            {"DEVELOPER_DIR": self.facts["developer_dir"], "SDKROOT": self.facts["sdk"], "ZERO_AR_DATE": "1"},
        )
        self.assertEqual(self.rule("llvm22")[1]["platform_args"], ":native_inputs")
        self.assertEqual(_patched_then_branch(":native_inputs", []), [":native_inputs"])
        self.assertNotIn("include_directories_from_xcode", _patched_then_branch(":native_inputs", []))
        patch = (_BAZEL / "apple_support_llvm.patch").read_text(encoding="utf-8")
        added = [line[1:] for line in patch.splitlines() if line.startswith("+") and not line.startswith("+++")]
        self.assertTrue(any(line.strip() == "platform_args = None," for line in added))
        self.assertFalse(any("apple_environment" in line for line in added))
        self.assertFalse(any("include_directories_from_xcode" in line for line in added))
        self.assertIn("include_directories_from_xcode", patch)


if __name__ == "__main__":
    unittest.main()
