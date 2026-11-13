"""Compiler SDK observations and repository-owned materialization contracts."""

import hashlib
import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from ao.core import bazel_compiler_probe as compiler_probe
from ao.core import bazel_native as native
from ao.core import bazel_providers as providers


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
            ("gcov", "llvm-cov"),
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
        self.resource = self.sdk / "resource-root"
        share = self.resource / "share"
        share.mkdir(parents=True)
        (share / "asan_ignorelist.txt").write_text("fun:*_aobus_sanitizer_fixture*\n")
        self.sanitizer_include = (self.sdk / "include").as_posix()
        sanitizer = self.sdk / "include/sanitizer"
        sanitizer.mkdir(parents=True)
        for header in ("tsan_interface.h", "common_interface_defs.h"):
            (sanitizer / header).write_text("/* compiler-rt fixture interface */\n")
        self.wrapper = {
            "suffixSalt": "fixture_target",
            "hardeningEnable": ["fortify", "stackprotector", "relro"],
            "tools": tools,
            "builtinRoots": [str(self.sdk)],
            "sanitizerIncludeDirectory": self.sanitizer_include,
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
        self.assertEqual(command[0], self.tools["clang++"])
        self.assertEqual(kwargs["env"], {"PATH": ""})
        self.assertTrue(kwargs["check"])
        if command[1:] == ["-print-resource-dir"]:
            self.assertNotIn("input", kwargs)
            return subprocess.CompletedProcess(command, 0, str(self.resource), "")
        self.assertEqual(
            command,
            [self.tools["clang++"], "-E", "-x", "c++", "-std=c++26", "-no-canonical-prefixes", "-", "-v"],
        )
        self.assertEqual(kwargs["input"], "")
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
                "compiler_resource_directory": str(self.resource),
                "sanitizer_include_directory": self.sanitizer_include,
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


if __name__ == "__main__":
    unittest.main()
