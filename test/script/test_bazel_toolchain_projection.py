"""Compiler SDK observations and repository-owned materialization contracts."""

import ast
import hashlib
import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from ao.core import bazel_compiler_probe as compiler_probe
from ao.core import bazel_native as native
from ao.core import bazel_providers as providers

_BAZEL = Path(__file__).resolve().parents[2] / "bazel"


class _StarlarkFailure(Exception):
    """A repository helper's fail() result."""


def _materializer():
    def fail(message):
        raise _StarlarkFailure(message)

    namespace = {
        "json": SimpleNamespace(encode=json.dumps),
        "fail": fail,
        "type": lambda value: {dict: "dict", str: "string"}.get(type(value), type(value).__name__),
    }
    source = (_BAZEL / "toolchain_repository.bzl").read_text(encoding="utf-8")
    exec(compile(ast.parse(source), "toolchain_repository.bzl", "exec"), namespace)
    return namespace["materialize_toolchain"]


class _RepositoryContext:
    """Record only repository_ctx's file and symlink boundary."""

    def __init__(self):
        self.files = {}
        self.links = {}

    def path(self, source):
        return source

    def symlink(self, source, destination):
        self.links[destination] = source

    def file(self, name, content, *, executable):
        if executable:
            raise AssertionError("toolchain metadata is not executable")
        self.files[name] = content


class ToolchainObservationTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-toolchain-facts-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.sdk = self.root / "compiler"
        self.include = self.sdk / "include/c++"
        self.include.mkdir(parents=True)
        tools = {}
        for role, name in (
            ("clang", "clang"),
            ("clang++", "clang++"),
            ("ar", "llvm-ar"),
            ("strip", "llvm-strip"),
            ("objcopy", "llvm-objcopy"),
        ):
            path = self.sdk / "bin" / name
            path.parent.mkdir(exist_ok=True)
            path.write_bytes(b"selected compiler input")
            path.chmod(0o755)
            tools[role] = path.as_posix()
        # The selected lexical spelling must survive observation and symlinking.
        (self.sdk / "bin/dispatch").mkdir()
        tools["clang++"] = (self.sdk / "bin/dispatch/../clang++").as_posix()
        self.tools = tools
        self.wrapper = {
            "suffixSalt": "fixture_target",
            "hardeningEnable": ["fortify", "stackprotector", "relro"],
            "tools": tools,
            "builtinRoots": [str(self.sdk)],
        }
        checkout = self.root / "checkout"
        checkout.mkdir()
        (checkout / "nixpkgs.json").write_text(json.dumps({"rev": "fixture-pin"}), encoding="utf-8")
        report = self.root / "nix-report.json"
        report.write_text(
            json.dumps({"schemaVersion": 1, "nixpkgsRevision": "fixture-pin", "ccWrapper": self.wrapper}),
            encoding="utf-8",
        )
        self.enterContext(patch.object(native, "PROJECT_ROOT", checkout))
        self.enterContext(patch.dict(os.environ, {"AOBUS_NIX_DEPENDENCY_REPORT": str(report)}))
        self.enterContext(patch.object(native, "host_machine", return_value="x86_64"))
        self.enterContext(patch.object(providers, "nix_root", side_effect=self.nix_root))
        self.query_error = None
        self.enterContext(patch.object(compiler_probe.subprocess, "run", side_effect=self.query))

    def nix_root(self, path):
        self.assertTrue(path.resolve().is_relative_to(self.sdk))
        return self.sdk

    def query(self, command, **kwargs):
        self.assertEqual(
            command,
            [self.tools["clang++"], "-E", "-x", "c++", "-std=c++26", "-no-canonical-prefixes", "-", "-v"],
        )
        self.assertEqual(kwargs["env"], {"PATH": ""})
        self.assertEqual(kwargs["input"], "")
        self.assertTrue(kwargs["check"])
        if self.query_error is not None:
            raise self.query_error
        stderr = f"#include <...> search starts here:\n {self.include}\nEnd of search list.\n"
        return subprocess.CompletedProcess(command, 0, "", stderr)

    def snapshot(self):
        return {
            str(path.relative_to(self.root)): (path.stat().st_mtime_ns, path.read_bytes() if path.is_file() else None)
            for path in self.root.rglob("*")
        }

    def test_probe_returns_only_json_sdk_facts_without_creating_or_rewriting_files(self):
        before = self.snapshot()
        with patch.object(native, "write_json", side_effect=AssertionError("observation must not write")):
            data = compiler_probe.probe_compiler("linux")
        self.assertEqual(self.snapshot(), before)
        self.assertEqual(set(data), {"platform", "sdk", "identity"})
        self.assertEqual(json.loads(json.dumps(data)), data)
        self.assertEqual(data["platform"], "linux")
        self.assertEqual(
            data["sdk"],
            {
                "machine": "x86_64",
                "tools": self.tools,
                "builtin_include_directories": [str(self.include)],
                "nix_cc_wrapper": {
                    "suffixSalt": "fixture_target",
                    "hardeningEnable": ["fortify", "stackprotector", "relro"],
                },
            },
        )
        identity = data["identity"]
        self.assertEqual(identity["schema"], 1)
        self.assertEqual(identity["context"]["wrapper_suffix"], "fixture_target")
        compiler = next(item for item in identity["files"] if item["path"] == str(Path(self.tools["clang++"])))
        self.assertEqual(compiler["realpath"], str(self.sdk / "bin/clang++"))
        self.assertEqual(compiler["sha256"], hashlib.sha256(b"selected compiler input").hexdigest())

    def test_projection_retains_selected_lexical_tools_and_unchanged_identity(self):
        before = self.snapshot()
        data = compiler_probe.probe_compiler("linux")
        context = _RepositoryContext()
        _materializer()(context, data)
        self.assertEqual(context.links["tools/clang++"], self.tools["clang++"])
        self.assertEqual(
            set(context.links),
            {"tools/clang", "tools/clang++", "tools/llvm-ar", "tools/llvm-strip", "tools/llvm-objcopy"},
        )
        self.assertEqual(json.loads(context.files["identity.json"]), data["identity"])
        self.assertEqual(self.snapshot(), before)

    def test_host_machine_spelling_remains_raw_until_repository_projection(self):
        with patch.object(native, "host_machine", return_value="AARCH64"):
            data = compiler_probe.probe_compiler("linux")
        self.assertEqual(data["sdk"]["machine"], "AARCH64")
        self.assertEqual(data["identity"]["context"]["machine"], "AARCH64")
        self.assertNotIn("CONSTRAINT_CPU", json.dumps(data))

    def test_selected_tool_byte_changes_invalidate_identity_without_changing_sdk_facts(self):
        before = compiler_probe.probe_compiler("linux")
        compiler = Path(self.tools["clang++"])
        stat = compiler.stat()
        compiler.write_bytes(b"changed in place")
        os.utime(compiler, ns=(stat.st_atime_ns, stat.st_mtime_ns))
        after = compiler_probe.probe_compiler("linux")
        self.assertEqual(after["sdk"], before["sdk"])
        self.assertNotEqual(after["identity"], before["identity"])
        record = next(item for item in after["identity"]["files"] if item["path"] == str(compiler))
        self.assertEqual(record["sha256"], hashlib.sha256(b"changed in place").hexdigest())

    def test_compiler_probe_failure_propagates_without_materializing_partial_output(self):
        before = self.snapshot()
        self.query_error = subprocess.CalledProcessError(9, [self.tools["clang++"]], stderr="compiler failed")
        with self.assertRaises(subprocess.CalledProcessError) as failure:
            compiler_probe.probe_compiler("linux")
        self.assertIs(failure.exception, self.query_error)
        self.assertEqual(self.snapshot(), before)

    def test_unknown_platform_is_rejected_before_compiler_observation(self):
        with (
            patch.object(native, "host_machine", side_effect=AssertionError("must not inspect the host")),
            self.assertRaisesRegex(native.BazelWorkspaceError, "unsupported.*platform: haiku"),
        ):
            compiler_probe.probe_compiler("haiku")


class ToolchainMaterializationTest(unittest.TestCase):
    def setUp(self):
        self.materialize = _materializer()
        self.context = _RepositoryContext()

    def data(self, platform, sdk=None):
        return {
            "platform": platform,
            "sdk": sdk if sdk is not None else self.linux_sdk(),
            "identity": {"schema": 1, "context": {"platform": platform}, "files": []},
        }

    def linux_sdk(self):
        return {
            "machine": "x86_64",
            "tools": {
                "clang": "/selected/bin/clang",
                "clang++": "/selected/bin/clang++",
                "ar": "/selected/bin/llvm-ar",
                "strip": "/selected/bin/llvm-strip",
                "objcopy": "/selected/bin/llvm-objcopy",
            },
            "builtin_include_directories": ["/selected/include"],
            "nix_cc_wrapper": {"suffixSalt": "selected", "hardeningEnable": ["fortify", "relro"]},
        }

    def macos_sdk(self):
        return {
            "machine": "arm64",
            "llvm": "/selected/llvm",
            "llvm_real": "/installed/llvm",
            "llvm_version": "22.1.8",
            "sdk": "/selected/sdk",
            "shim": "/Åsa/expected",
            "linker": "/developer/bin/ld",
            "developer_dir": "/Åsa/Developer",
            "deployment": "fixture-deployment",
            "tools": {
                "clang": "/selected/llvm/bin/clang",
                "clang++": "/selected/llvm/bin/dispatch/../clang++",
                "ar": "/selected/llvm/bin/llvm-libtool-darwin",
                "strip": "/selected/llvm/bin/llvm-strip",
                "ld": "/developer/bin/ld",
                "dsymutil": "/developer/bin/dsymutil",
                "apple_strip": "/developer/bin/strip",
                "xcrun": "/selected/bin/xcrun",
            },
        }

    def test_generated_builds_forward_observations_to_existing_toolchain_macros(self):
        cases = [
            (
                "linux",
                "linux_toolchain.bzl",
                "linux_toolchain",
                self.linux_sdk(),
                {
                    "TOOLS": {
                        "clang": "tools/clang",
                        "clang++": "tools/clang++",
                        "ar": "tools/llvm-ar",
                        "strip": "tools/llvm-strip",
                        "objcopy": "tools/llvm-objcopy",
                    },
                    "BUILTIN_INCLUDE_DIRECTORIES": ["/selected/include"],
                    "NIX_CC_WRAPPER": {"suffixSalt": "selected", "hardeningEnable": ["fortify", "relro"]},
                    "CONSTRAINT_CPU": "x86_64",
                },
                {
                    "tools/clang": "/selected/bin/clang",
                    "tools/clang++": "/selected/bin/clang++",
                    "tools/llvm-ar": "/selected/bin/llvm-ar",
                    "tools/llvm-strip": "/selected/bin/llvm-strip",
                    "tools/llvm-objcopy": "/selected/bin/llvm-objcopy",
                },
            ),
            (
                "macos",
                "macos_cc_config.bzl",
                "macos_toolchain",
                self.macos_sdk(),
                {
                    "LLVM": "/selected/llvm",
                    "LLVM_REAL": "/installed/llvm",
                    "LLVM_VERSION": "22.1.8",
                    "SDK": "/selected/sdk",
                    "SHIM": "/Åsa/expected",
                    "LINKER": "/developer/bin/ld",
                    "DEVELOPER_DIR": "/Åsa/Developer",
                    "DEPLOYMENT": "fixture-deployment",
                    "TOOLS": {
                        "clang": "tools/clang",
                        "clang++": "tools/clang++",
                        "ar": "tools/llvm-libtool-darwin",
                        "strip": "tools/llvm-strip",
                        "ld": "tools/ld",
                        "dsymutil": "tools/dsymutil",
                        "apple_strip": "tools/strip",
                        "xcrun": "tools/xcrun",
                    },
                    "CPU": "arm64",
                    "CONSTRAINT_CPU": "aarch64",
                },
                {
                    "tools/clang": "/selected/llvm/bin/clang",
                    "tools/clang++": "/selected/llvm/bin/dispatch/../clang++",
                    "tools/llvm-libtool-darwin": "/selected/llvm/bin/llvm-libtool-darwin",
                    "tools/llvm-strip": "/selected/llvm/bin/llvm-strip",
                    "tools/ld": "/developer/bin/ld",
                    "tools/dsymutil": "/developer/bin/dsymutil",
                    "tools/strip": "/developer/bin/strip",
                    "tools/xcrun": "/selected/bin/xcrun",
                },
            ),
        ]
        for platform, module, macro, sdk, paths, links in cases:
            with self.subTest(platform=platform):
                self.assert_generated_build(platform, module, macro, sdk, paths, links)

    def assert_generated_build(self, platform, module, macro, sdk, paths, links):
        context = _RepositoryContext()
        data = self.data(platform, sdk)
        self.materialize(context, data)
        self.assertEqual(set(context.files), {"BUILD.bazel", "native_paths.bzl", "identity.json"})
        self.assertEqual(context.links, links)
        nodes = ast.parse(context.files["native_paths.bzl"]).body
        self.assertTrue(all(isinstance(node, ast.Assign) for node in nodes))
        self.assertEqual([node.targets[0].id for node in nodes], sorted(paths))
        metadata = {node.targets[0].id: ast.literal_eval(node.value) for node in nodes}
        self.assertEqual(metadata, paths)
        self.assertEqual(json.loads(context.files["identity.json"]), data["identity"])
        forwarded = []
        packages = []
        namespace = {"package": lambda **kwargs: packages.append(kwargs)}

        def load(label, *names):
            if label == ":native_paths.bzl":
                self.assertEqual(set(names), set(paths))
                namespace.update({name: metadata[name] for name in names})
            else:
                self.assertEqual(label, "@aobus//bazel:" + module)
                self.assertEqual(names, (macro,))
                namespace[macro] = lambda **kwargs: forwarded.append(kwargs)

        namespace["load"] = load
        exec(context.files["BUILD.bazel"], namespace)
        self.assertEqual(packages, [{"default_visibility": ["//visibility:public"]}])
        self.assertEqual(forwarded, [{name.lower(): value for name, value in paths.items()}])
        graph = ast.parse((_BAZEL / module).read_text(encoding="utf-8"))
        declaration = next(node for node in graph.body if isinstance(node, ast.FunctionDef) and node.name == macro)
        self.assertEqual(set(forwarded[0]), {arg.arg for arg in declaration.args.args})

    def test_data_assignments_are_deterministic_when_observation_order_changes(self):
        sdk = self.linux_sdk()
        self.materialize(self.context, self.data("linux", sdk))
        reordered = _RepositoryContext()
        self.materialize(reordered, self.data("linux", dict(reversed(list(sdk.items())))))
        self.assertEqual(reordered.files["native_paths.bzl"], self.context.files["native_paths.bzl"])

    def test_cpu_constraints_are_selected_from_raw_host_machine_spelling(self):
        for platform, machine, expected in (
            ("linux", "AARCH64", {"CONSTRAINT_CPU": "aarch64"}),
            ("linux", "X86_64", {"CONSTRAINT_CPU": "x86_64"}),
            ("macos", "ARM64", {"CPU": "arm64", "CONSTRAINT_CPU": "aarch64"}),
            ("macos", "X86_64", {"CPU": "x86_64", "CONSTRAINT_CPU": "x86_64"}),
        ):
            with self.subTest(platform=platform, machine=machine):
                sdk = self.linux_sdk() if platform == "linux" else self.macos_sdk()
                sdk["machine"] = machine
                context = _RepositoryContext()
                self.materialize(context, self.data(platform, sdk))
                metadata = {}
                exec(context.files["native_paths.bzl"], metadata)
                self.assertEqual({name: metadata[name] for name in expected}, expected)

    def test_unsupported_cpu_fails_before_publishing_files_or_links(self):
        for platform, sdk in (
            ("linux", {"machine": "arm64"}),
            ("macos", {"machine": "aarch64"}),
        ):
            with self.subTest(platform=platform), self.assertRaisesRegex(_StarlarkFailure, "unsupported.*CPU"):
                self.materialize(self.context, self.data(platform, sdk))
            self.assertEqual(self.context.files, {})
            self.assertEqual(self.context.links, {})

    def test_required_roles_and_colliding_basenames_fail_at_the_repository_seam(self):
        cases = [
            ("linux", dict(self.linux_sdk(), tools={"clang++": "/selected/clang++"})),
            ("linux", dict(self.linux_sdk(), tools=dict(self.linux_sdk()["tools"], ar="/other/clang"))),
            ("macos", dict(self.macos_sdk(), tools={"clang": "/selected/clang"})),
            (
                "macos",
                dict(self.macos_sdk(), tools=dict(self.macos_sdk()["tools"], ld="/other/bin/clang")),
            ),
        ]
        for platform, sdk in cases:
            with (
                self.subTest(platform=platform, tools=sdk.get("tools")),
                self.assertRaisesRegex(_StarlarkFailure, "compiler tool (roles|basename)"),
            ):
                self.materialize(self.context, self.data(platform, sdk))
            self.assertEqual(self.context.files, {})
            self.assertEqual(self.context.links, {})

    def test_unknown_platform_fails_before_writing_files_or_linking_tools(self):
        with self.assertRaisesRegex(_StarlarkFailure, "unsupported.*platform: haiku"):
            self.materialize(self.context, self.data("haiku", {}))
        self.assertEqual(self.context.files, {})
        self.assertEqual(self.context.links, {})

    def test_windows_projection_is_retired_at_the_repository_seam(self):
        # The official rules_cc auto-config owns the Windows toolchain; the
        # repository materializer no longer accepts a Windows projection.
        with self.assertRaisesRegex(_StarlarkFailure, "unsupported.*platform: windows"):
            self.materialize(
                self.context,
                self.data("windows", {"target_architecture": "x64", "tools": {"cl": "C:/selected/cl.exe"}}),
            )
        self.assertEqual(self.context.files, {})
        self.assertEqual(self.context.links, {})

    def test_link_failure_is_not_hidden_by_publishing_a_consumable_build(self):
        error = OSError("selected compiler input is unavailable")
        with (
            patch.object(self.context, "symlink", side_effect=error),
            self.assertRaises(OSError) as failure,
        ):
            self.materialize(self.context, self.data("linux"))
        self.assertIs(failure.exception, error)
        self.assertEqual(self.context.files, {})


if __name__ == "__main__":
    unittest.main()
