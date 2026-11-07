"""Mutable native SDK inputs must change the compiler's action identity."""

import hashlib
import os
import subprocess
import tempfile
import unittest
from contextlib import contextmanager
from pathlib import Path
from unittest.mock import patch

from ao.core import bazel_compiler_probe as compiler_probe
from ao.core import bazel_native as native


class MacosToolchainIdentityTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-macos-toolchain-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.llvm = self.root / "llvm"
        self.sdk = self.root / "sdk"
        self.shim = self.root / "Åsa/expected"
        self.resource = self.llvm / "lib/clang/selected"
        self.linker = self.root / "developer/usr/bin/ld"
        self.dsymutil = self.root / "developer/usr/bin/dsymutil"
        self.apple_strip = self.root / "developer/usr/bin/strip"
        self.xcrun = self.root / "selected/xcrun"
        self.developer = self.root / "Åsa/Developer"
        self.generated = self.root / "generated"
        self.version = "22.1.8"
        self.executables = [
            *[self.llvm / "bin" / name for name in ("clang", "clang++", "llvm-libtool-darwin", "llvm-strip")],
            self.linker,
            self.dsymutil,
            self.apple_strip,
            self.xcrun,
        ]
        self.inputs = [
            *self.executables,
            self.llvm / "bin/llvm-ar",
            self.llvm / "lib/libLTO.dylib",
            self.llvm / "include/c++/v1/vector",
            self.resource / "include/stddef.h",
            self.resource / "lib/darwin/libclang_rt.osx.a",
            self.sdk / "SDKSettings.json",
            self.sdk / "usr/include/stdio.h",
            self.sdk / "usr/lib/libSystem.tbd",
            self.sdk / "System/Library/Frameworks/Example.framework/Versions/A/Headers/Example.h",
            self.sdk / "System/Library/Frameworks/Example.framework/Versions/A/Example.tbd",
            self.shim / "__expected",
        ]
        for path in self.inputs:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"original")
        for path in self.executables:
            path.chmod(0o755)
        self.developer.mkdir(parents=True)
        self.environment = {
            "AOBUS_LLVM_ROOT": str(self.llvm),
            "AOBUS_LIBCXX_EXPECTED_SHIM": str(self.shim),
            "DEVELOPER_DIR": str(self.developer),
        }

    def selected_xcrun(self, name, *, nix=False, path=None):
        self.assertEqual((name, nix, path), ("xcrun", False, "/usr/bin"))
        return str(self.xcrun)

    def query(self, command, **kwargs):
        normalized = tuple(str(part) for part in command)
        answers = {
            (str(self.xcrun), "--show-sdk-path"): str(self.sdk),
            (str(self.xcrun), "--find", "ld"): str(self.linker),
            (str(self.xcrun), "--find", "dsymutil"): str(self.dsymutil),
            (str(self.xcrun), "--find", "strip"): str(self.apple_strip),
            (str(self.llvm / "bin/clang++"), "--no-default-config", "-print-resource-dir"): str(self.resource),
            (str(self.llvm / "bin/clang++"), "--no-default-config", "-dumpversion"): self.version,
        }
        self.assertIn(normalized, answers)
        self.assertEqual(kwargs["capture_output"], True)
        self.assertEqual(kwargs["text"], True)
        self.assertTrue(kwargs["check"])
        return subprocess.CompletedProcess(command, 0, answers[normalized] + "\n", "")

    @contextmanager
    def patched(self, machine):
        with (
            patch.dict(os.environ, self.environment),
            patch.object(native, "host_machine", return_value=machine),
            patch.object(compiler_probe.platform, "machine", return_value=machine),
            patch.object(native, "executable", side_effect=self.selected_xcrun),
            patch.object(compiler_probe.subprocess, "run", side_effect=self.query),
        ):
            yield

    def identity(self):
        with self.patched("arm64"):
            return compiler_probe.probe_compiler("macos")["identity"]

    def test_selected_sdk_is_raw_metadata_without_graph_policy_or_native_tool_relocation(self):
        with self.patched("ARM64"):
            data = compiler_probe.probe_compiler("macos")
        sdk = data["sdk"]
        self.assertEqual(set(data), {"platform", "sdk", "identity"})
        self.assertEqual(data["platform"], "macos")
        self.assertEqual(
            set(sdk),
            {
                "machine",
                "llvm",
                "llvm_real",
                "llvm_version",
                "sdk",
                "shim",
                "linker",
                "developer_dir",
                "deployment",
                "tools",
            },
        )
        self.assertIn("Åsa", sdk["shim"])
        self.assertEqual(sdk["shim"], str(self.shim))
        self.assertEqual(sdk["linker"], str(self.linker))
        self.assertEqual(sdk["sdk"], str(self.sdk))
        self.assertEqual(sdk["llvm_version"], "22.1.8")
        self.assertEqual(sdk["developer_dir"], str(self.developer))
        self.assertIn("Åsa", sdk["developer_dir"])
        self.assertEqual(sdk["machine"], "ARM64")
        self.assertEqual(
            sdk["tools"],
            {
                "clang": str(self.llvm / "bin/clang"),
                "clang++": str(self.llvm / "bin/clang++"),
                "ar": str(self.llvm / "bin/llvm-libtool-darwin"),
                "strip": str(self.llvm / "bin/llvm-strip"),
                "ld": str(self.linker),
                "dsymutil": str(self.dsymutil),
                "apple_strip": str(self.apple_strip),
                "xcrun": str(self.xcrun),
            },
        )
        self.assertNotIn("/usr/bin", sdk["tools"]["xcrun"])
        indexed = {item["path"]: item for item in data["identity"]["files"]}
        for path in (self.llvm / "bin/llvm-strip", self.dsymutil, self.apple_strip, self.xcrun):
            record = indexed[str(path)]
            self.assertEqual(record["realpath"], str(path.resolve()))
            self.assertEqual(record["sha256"], hashlib.sha256(b"original").hexdigest())
        self.assertFalse((self.generated / "toolchain/bin").exists())

    def test_developer_dir_and_clang_version_fail_before_sdk_facts_are_returned(self):
        for developer in ("Developer", str(self.root / "missing-developer")):
            with self.subTest(developer=developer):
                self.environment["DEVELOPER_DIR"] = developer
                with self.patched("arm64"), self.assertRaisesRegex(native.BazelWorkspaceError, "developer directory"):
                    compiler_probe.probe_compiler("macos")
        self.environment["DEVELOPER_DIR"] = str(self.developer)
        self.version = "22"
        with self.patched("arm64"), self.assertRaisesRegex(native.BazelWorkspaceError, "invalid version"):
            compiler_probe.probe_compiler("macos")

    def test_non_executable_selected_tool_is_rejected(self):
        tool = self.llvm / "bin/llvm-strip"
        actual_access = os.access

        def access(path, mode):
            return False if Path(path) == tool and mode == os.X_OK else actual_access(path, mode)

        # Native Windows file modes cannot model a POSIX executable bit.
        with (
            self.patched("arm64"),
            patch.object(compiler_probe.os, "access", side_effect=access),
            self.assertRaisesRegex(native.BazelWorkspaceError, "not executable"),
        ):
            compiler_probe.probe_compiler("macos")

    def test_in_place_native_changes_invalidate_even_with_unchanged_versions_and_mtimes(self):
        baseline = self.identity()
        for path in self.inputs:
            with self.subTest(path=path.relative_to(self.root)):
                stat = path.stat()
                path.write_bytes(b"modified")
                os.utime(path, ns=(stat.st_atime_ns, stat.st_mtime_ns))
                changed = self.identity()
                self.assertNotEqual(changed, baseline)
                record = next(item for item in changed["files"] if item["path"] == str(path))
                self.assertEqual(record["sha256"], hashlib.sha256(b"modified").hexdigest())
                path.write_bytes(b"original")
                self.assertEqual(self.identity(), baseline)

    def test_header_addition_and_removal_change_identity_and_unchanged_inputs_are_repeatable(self):
        baseline = self.identity()
        self.assertEqual(self.identity(), baseline)
        added = self.sdk / "usr/include/new.h"
        added.write_bytes(b"new header")
        self.assertNotEqual(self.identity(), baseline)
        added.unlink()
        self.assertEqual(self.identity(), baseline)

    def test_optional_sdk_search_directory_is_watched_before_installation(self):
        directory = self.sdk / "usr/local/include"
        observed = []
        with native.capture_probes(observed.append):
            baseline = self.identity()
        self.assertIn(directory, observed)
        directory.mkdir(parents=True)
        (directory / "new.h").write_bytes(b"new native SDK header")
        self.assertNotEqual(self.identity(), baseline)


if __name__ == "__main__":
    unittest.main()
