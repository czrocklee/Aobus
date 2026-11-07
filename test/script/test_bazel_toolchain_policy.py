"""Starlark-owned compiler projections and Nix compilation-mode policy."""

import ast
import copy
import json
import os
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

_BAZEL = Path(__file__).resolve().parents[2] / "bazel"


class _StarlarkFailure(Exception):
    """The repository helper rejected an unsupported construction input."""


def _fail(message):
    raise _StarlarkFailure(message)


def _starlark_type(value):
    if isinstance(value, str):
        return "string"
    if isinstance(value, dict):
        return "dict"
    return type(value).__name__


def _execute(name, namespace):
    # These .bzl files use the common Python/Starlark subset; only external I/O
    # and declaration functions are replaced, never the policy being exercised.
    source = (_BAZEL / name).read_text(encoding="utf-8")
    exec(compile(source, name, "exec"), namespace)


class _LexicalPath:
    def __init__(self, source):
        self.source = source

    def __str__(self):
        return self.source

    @property
    def realpath(self):
        raise AssertionError("compiler dispatch must not resolve the selected lexical path")


class _RepositoryContext:
    def __init__(self):
        self.files = {}
        self.links = {}

    def path(self, source):
        return _LexicalPath(source)

    def symlink(self, source, destination):
        if destination in self.links:
            raise AssertionError("duplicate tool input: " + destination)
        self.links[destination] = str(source)

    def file(self, name, content, *, executable):
        if executable:
            raise AssertionError("compiler metadata must not be executable")
        self.files[name] = content

    def metadata(self):
        return {
            node.targets[0].id: ast.literal_eval(node.value) for node in ast.parse(self.files["native_paths.bzl"]).body
        }


class _ToolLabels(dict):
    def values(self):
        # Starlark dict.values() returns a list, unlike Python's dictionary view.
        return list(super().values())


class _LinuxDeclarations:
    def __init__(self, context):
        self.rules = {}
        self.exports = []
        self.namespace = {
            "native": SimpleNamespace(
                exports_files=self.exports.extend,
                filegroup=self.record("filegroup"),
                toolchain=self.record("toolchain"),
            ),
            "package": self.package,
            "load": self.load,
        }
        self.context = context
        exec(compile(context.files["BUILD.bazel"], "BUILD.bazel", "exec"), self.namespace)

    def package(self, **kwargs):
        if kwargs != {"default_visibility": ["//visibility:public"]}:
            raise AssertionError("unexpected toolchain visibility")

    def record(self, kind):
        def declare(**kwargs):
            name = kwargs["name"]
            if name in self.rules:
                raise AssertionError("duplicate declaration: " + name)
            self.rules[name] = (kind, kwargs)

        return declare

    def load(self, label, *symbols):
        if label == "@aobus//bazel:linux_toolchain.bzl" and symbols == ("linux_toolchain",):
            _execute("linux_toolchain.bzl", self.namespace)
        elif label == ":native_paths.bzl":
            metadata = self.context.metadata()
            if set(symbols) != set(metadata):
                raise AssertionError("BUILD must consume the complete native toolchain interface")
            self.namespace.update(metadata)
            self.namespace["TOOLS"] = _ToolLabels(metadata["TOOLS"])
        elif label == ":compiler_policy.bzl" and symbols == ("UNIX_WARNINGS",):
            _execute("compiler_policy.bzl", self.namespace)
        else:
            modules = {
                "cc_args": "args",
                "cc_feature": "feature",
                "cc_feature_constraint": "feature_constraint",
                "cc_tool": "tool",
                "cc_tool_map": "tool_map",
                "cc_toolchain": "toolchain",
            }
            if len(symbols) != 1 or symbols[0] not in modules:
                raise AssertionError("unexpected toolchain load: " + label)
            symbol = symbols[0]
            if label != "@rules_cc//cc/toolchains:" + modules[symbol] + ".bzl":
                raise AssertionError("unexpected rule module: " + label)
            self.namespace[symbol] = self.record(symbol)


class ToolchainConstructionPolicyTest(unittest.TestCase):
    def setUp(self):
        namespace = {
            "json": SimpleNamespace(encode=json.dumps),
            "fail": _fail,
            "type": _starlark_type,
        }
        _execute("toolchain_repository.bzl", namespace)
        self.materialize = namespace["materialize_toolchain"]

    def data(self, platform="linux"):
        if platform == "linux":
            sdk = {
                "machine": "X86_64",
                "tools": {
                    "clang": "/nix/store/wrapper/bin/clang",
                    "clang++": "/nix/store/wrapper/bin/dispatch/../clang++",
                    "ar": "/nix/store/llvm/bin/llvm-ar",
                    "strip": "/nix/store/llvm/bin/strip-22",
                    "objcopy": "/nix/store/llvm/bin/llvm-objcopy",
                },
                "builtin_include_directories": ["/nix/store/compiler-headers/include/c++"],
                "nix_cc_wrapper": {"suffixSalt": "fixture_target", "hardeningEnable": ["fortify", "relro"]},
            }
        elif platform == "macos":
            sdk = {
                "machine": "ARM64",
                "llvm": "/opt/llvm",
                "llvm_real": "/installed/llvm",
                "llvm_version": "22.1.8",
                "sdk": "/Developer/Selected.sdk",
                "shim": "/Åsa/expected",
                "linker": "/Developer/bin/ld",
                "developer_dir": "/Åsa/Developer",
                "deployment": "fixture-deployment",
                "tools": {
                    "clang": "/opt/llvm/bin/clang",
                    "clang++": "/opt/llvm/bin/dispatch/../clang++",
                    "ar": "/opt/llvm/bin/llvm-libtool-darwin",
                    "strip": "/opt/llvm/bin/llvm-strip",
                    "ld": "/Developer/bin/ld",
                    "dsymutil": "/Developer/bin/dsymutil",
                    "apple_strip": "/Developer/bin/strip",
                    "xcrun": "/usr/bin/xcrun",
                },
            }
        else:
            raise ValueError("unsupported fixture platform: " + platform)
        return {
            "platform": platform,
            "sdk": sdk,
            "identity": {"schema": 1, "context": {"platform": platform}, "files": [{"sha256": "fixture-digest"}]},
        }

    def project(self, data):
        context = _RepositoryContext()
        self.materialize(context, data)
        self.assertEqual(set(context.files), {"BUILD.bazel", "native_paths.bzl", "identity.json"})
        self.assertEqual(json.loads(context.files["identity.json"]), data["identity"])
        return context

    def forwarded(self, context, module, macro):
        calls = []
        namespace = {
            "package": lambda **kwargs: self.assertEqual(kwargs, {"default_visibility": ["//visibility:public"]})
        }

        def load(label, *symbols):
            if label == ":native_paths.bzl":
                metadata = context.metadata()
                self.assertEqual(set(symbols), set(metadata))
                namespace.update(metadata)
            else:
                self.assertEqual((label, symbols), ("@aobus//bazel:" + module, (macro,)))
                namespace[macro] = lambda **kwargs: calls.append(kwargs)

        namespace["load"] = load
        exec(compile(context.files["BUILD.bazel"], "BUILD.bazel", "exec"), namespace)
        self.assertEqual(len(calls), 1)
        return calls[0]

    def test_linux_projects_lexical_dispatch_basenames_and_unmodified_wrapper_facts(self):
        data = self.data()
        before = copy.deepcopy(data)
        context = self.project(data)
        self.assertEqual(
            context.metadata(),
            {
                "TOOLS": {
                    "clang": "tools/clang",
                    "clang++": "tools/clang++",
                    "ar": "tools/llvm-ar",
                    "strip": "tools/strip-22",
                    "objcopy": "tools/llvm-objcopy",
                },
                "BUILTIN_INCLUDE_DIRECTORIES": ["/nix/store/compiler-headers/include/c++"],
                "NIX_CC_WRAPPER": {"suffixSalt": "fixture_target", "hardeningEnable": ["fortify", "relro"]},
                "CONSTRAINT_CPU": "x86_64",
            },
        )
        self.assertEqual(
            context.links,
            {
                "tools/clang": "/nix/store/wrapper/bin/clang",
                "tools/clang++": "/nix/store/wrapper/bin/dispatch/../clang++",
                "tools/llvm-ar": "/nix/store/llvm/bin/llvm-ar",
                "tools/strip-22": "/nix/store/llvm/bin/strip-22",
                "tools/llvm-objcopy": "/nix/store/llvm/bin/llvm-objcopy",
            },
        )
        self.assertEqual(data, before)

    def test_unix_cpu_constraints_are_derived_from_raw_machine_spelling(self):
        for platform, machine, cpu, constraint in (
            ("linux", "X86_64", None, "x86_64"),
            ("linux", "AARCH64", None, "aarch64"),
            ("macos", "X86_64", "x86_64", "x86_64"),
            ("macos", "ARM64", "arm64", "aarch64"),
        ):
            with self.subTest(platform=platform, machine=machine):
                data = self.data(platform)
                data["sdk"]["machine"] = machine
                metadata = self.project(data).metadata()
                self.assertEqual(metadata["CONSTRAINT_CPU"], constraint)
                if cpu is not None:
                    self.assertEqual(metadata["CPU"], cpu)

    def test_macos_forwards_selected_sdk_and_projects_lexical_tool_basenames(self):
        data = self.data("macos")
        context = self.project(data)
        self.assertEqual(
            context.links,
            {
                "tools/clang": "/opt/llvm/bin/clang",
                "tools/clang++": "/opt/llvm/bin/dispatch/../clang++",
                "tools/llvm-libtool-darwin": "/opt/llvm/bin/llvm-libtool-darwin",
                "tools/llvm-strip": "/opt/llvm/bin/llvm-strip",
                "tools/ld": "/Developer/bin/ld",
                "tools/dsymutil": "/Developer/bin/dsymutil",
                "tools/strip": "/Developer/bin/strip",
                "tools/xcrun": "/usr/bin/xcrun",
            },
        )
        self.assertEqual(
            self.forwarded(context, "macos_cc_config.bzl", "macos_toolchain"),
            {
                "llvm": "/opt/llvm",
                "llvm_real": "/installed/llvm",
                "llvm_version": "22.1.8",
                "sdk": "/Developer/Selected.sdk",
                "shim": "/Åsa/expected",
                "linker": "/Developer/bin/ld",
                "developer_dir": "/Åsa/Developer",
                "deployment": "fixture-deployment",
                "tools": {
                    "clang": "tools/clang",
                    "clang++": "tools/clang++",
                    "ar": "tools/llvm-libtool-darwin",
                    "strip": "tools/llvm-strip",
                    "ld": "tools/ld",
                    "dsymutil": "tools/dsymutil",
                    "apple_strip": "tools/strip",
                    "xcrun": "tools/xcrun",
                },
                "cpu": "arm64",
                "constraint_cpu": "aarch64",
            },
        )
        self.assertEqual(context.links["tools/clang++"], data["sdk"]["tools"]["clang++"])

    def assert_rejected_without_output(self, data, message):
        context = _RepositoryContext()
        with self.assertRaisesRegex(_StarlarkFailure, message):
            self.materialize(context, data)
        self.assertEqual(context.files, {})
        self.assertEqual(context.links, {})

    def test_unknown_platform_fails_before_reading_sdk_or_publishing_outputs(self):
        self.assert_rejected_without_output({"platform": "haiku"}, "unsupported.*platform: haiku")

    def test_unsupported_cpus_fail_before_linking_or_publishing_outputs(self):
        for platform, field, invalid in (
            ("linux", "machine", "arm64"),
            ("linux", "machine", None),
            ("macos", "machine", "aarch64"),
            ("macos", "machine", ""),
        ):
            with self.subTest(platform=platform, invalid=invalid):
                data = self.data(platform)
                data["sdk"][field] = invalid
                self.assert_rejected_without_output(data, "unsupported.*CPU")

    def test_unicode_and_space_tool_basenames_preserve_the_selected_dispatch_name(self):
        for basename in ("clång++", "clang tool++"):
            with self.subTest(basename=basename):
                data = self.data()
                source = "/nix/store/wrapper/bin/" + basename
                data["sdk"]["tools"]["clang++"] = source
                context = self.project(data)
                label = "tools/" + basename
                self.assertEqual(context.links[label], source)
                self.assertEqual(context.metadata()["TOOLS"]["clang++"], label)
                self.assertEqual(_LinuxDeclarations(context).rules["clang++"][1]["src"], label)

    def test_invalid_tool_basenames_cannot_become_bazel_labels(self):
        for basename in ("", ".", "..", "a:b", "a\\b", "a\nb", "a\tb", "a\x00b", "a\x7fb"):
            with self.subTest(basename=basename):
                data = self.data()
                data["sdk"]["tools"]["clang"] = "/selected/bin/" + basename
                self.assert_rejected_without_output(data, "invalid.*tool basename")

    def test_duplicate_tool_basenames_fail_closed_on_every_host(self):
        for platform, role, source in (
            ("linux", "ar", "/another/bin/clang"),
            ("macos", "ld", "/another/bin/clang"),
        ):
            with self.subTest(platform=platform):
                data = self.data(platform)
                data["sdk"]["tools"][role] = source
                self.assert_rejected_without_output(data, "duplicate.*tool basename")

    def test_linux_projection_does_not_collapse_case_distinct_basenames(self):
        data = self.data()
        data["sdk"]["tools"]["ar"] = "/selected/bin/CLANG"
        context = self.project(data)
        self.assertIn("tools/CLANG", context.links)
        self.assertIn("tools/clang", context.links)

    def test_missing_or_unexpected_tool_roles_fail_before_partial_projection(self):
        for platform, role in (("linux", "clang"), ("macos", "dsymutil")):
            with self.subTest(platform=platform):
                data = self.data(platform)
                del data["sdk"]["tools"][role]
                self.assert_rejected_without_output(data, "invalid.*tool roles")
                data["sdk"]["tools"]["unexpected"] = "/selected/bin/unexpected"
                self.assert_rejected_without_output(data, "invalid.*tool roles")

    def test_link_failure_propagates_without_publishing_a_consumable_build(self):
        context = _RepositoryContext()
        error = OSError("selected compiler input is unavailable")
        with patch.object(context, "symlink", side_effect=error), self.assertRaises(OSError) as failure:
            self.materialize(context, self.data())
        self.assertIs(failure.exception, error)
        self.assertEqual(context.files, {})

    def test_linux_observation_order_does_not_change_generated_tool_inputs(self):
        data = self.data()
        first = self.project(data)
        data["sdk"]["tools"] = dict(reversed(list(data["sdk"]["tools"].items())))
        second = self.project(data)
        self.assertEqual(first.files, second.files)
        self.assertEqual(list(first.links.items()), list(second.links.items()))

    def assert_hardening(self, declarations, expected):
        for mode, environment in expected.items():
            with self.subTest(mode=mode):
                kind, args = declarations.rules[mode + "_hardening"]
                self.assertEqual(kind, "cc_args")
                self.assertEqual(args["env"], environment)
                self.assertEqual(
                    args["actions"],
                    [
                        "@rules_cc//cc/toolchains/actions:compile_actions",
                        "@rules_cc//cc/toolchains/actions:link_actions",
                    ],
                )
                self.assertEqual(args["requires_any_of"], [":is_" + mode])
                self.assertEqual(args["data"], [":identity"])
                self.assertEqual(declarations.rules["is_" + mode][1]["all_of"], [":" + mode])
                feature = declarations.rules[mode][1]
                self.assertEqual(feature["overrides"], "@rules_cc//cc/toolchains/features:" + mode)
                toolchain = declarations.rules["nix_clang"][1]
                self.assertIn(":" + mode, toolchain["known_features"])
                self.assertNotIn(":" + mode, toolchain["enabled_features"])
                self.assertIn(":" + mode + "_hardening", toolchain["args"])

    def test_real_linux_macro_selects_only_fortify_by_mode_for_compiler_and_link_actions(self):
        data = self.data()
        data["sdk"]["nix_cc_wrapper"]["hardeningEnable"] = [
            "fortify",
            "futurehardening2",
            "stackprotector",
            "fortify3",
            "relro",
            "fortifyextra",
            "relro",
        ]
        before = copy.deepcopy(data)
        with patch.dict(os.environ, {"NIX_HARDENING_ENABLE": "ambient", "NIX_CFLAGS_COMPILE": "-O3"}):
            context = self.project(data)
            declarations = _LinuxDeclarations(context)
        self.assert_hardening(
            declarations,
            {
                "dbg": {
                    "NIX_HARDENING_ENABLE_fixture_target": "futurehardening2 stackprotector relro fortifyextra relro"
                },
                "opt": {
                    "NIX_HARDENING_ENABLE_fixture_target": (
                        "fortify futurehardening2 stackprotector fortify3 relro fortifyextra relro"
                    )
                },
                "fastbuild": {
                    "NIX_HARDENING_ENABLE_fixture_target": "futurehardening2 stackprotector relro fortifyextra relro"
                },
            },
        )
        self.assertEqual(data, before)
        self.assertEqual(context.metadata()["NIX_CC_WRAPPER"], before["sdk"]["nix_cc_wrapper"])
        self.assertEqual(declarations.rules["clang++"][1]["src"], "tools/clang++")
        self.assertEqual(declarations.rules["clang++"][1]["data"], [":identity"])
        self.assertEqual(declarations.rules["identity"][1]["srcs"], ["identity.json", "native_paths.bzl"])

    def test_real_linux_macro_preserves_explicit_empty_policy_and_observed_salt_in_every_mode(self):
        data = self.data()
        data["sdk"]["nix_cc_wrapper"] = {"suffixSalt": "empty_policy_7", "hardeningEnable": []}
        with patch.dict(os.environ, {"NIX_HARDENING_ENABLE_empty_policy_7": "must-not-leak"}):
            declarations = _LinuxDeclarations(self.project(data))
        self.assert_hardening(
            declarations,
            {
                "dbg": {"NIX_HARDENING_ENABLE_empty_policy_7": ""},
                "opt": {"NIX_HARDENING_ENABLE_empty_policy_7": ""},
                "fastbuild": {"NIX_HARDENING_ENABLE_empty_policy_7": ""},
            },
        )
        self.assertEqual(data["sdk"]["nix_cc_wrapper"]["hardeningEnable"], [])


if __name__ == "__main__":
    unittest.main()
