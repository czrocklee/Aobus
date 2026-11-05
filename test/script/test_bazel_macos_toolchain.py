"""Mutable native SDK inputs must change the compiler's action identity."""

import hashlib
import os
import subprocess
import tempfile
import unittest
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
        self.generated = self.root / "generated"
        self.inputs = [
            *[self.llvm / "bin" / name for name in ("clang", "clang++", "llvm-ar", "llvm-libtool-darwin")],
            self.linker,
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
        self.environment = {"AOBUS_LLVM_ROOT": str(self.llvm), "AOBUS_LIBCXX_EXPECTED_SHIM": str(self.shim)}

    def query(self, command, **kwargs):
        answers = {
            ("xcrun", "--show-sdk-path"): self.sdk,
            ("xcrun", "--find", "ld"): self.linker,
            (str(self.llvm / "bin/clang++"), "--no-default-config", "-print-resource-dir"): self.resource,
        }
        self.assertIn(tuple(command), answers)
        self.assertTrue(kwargs["check"])
        return subprocess.CompletedProcess(command, 0, str(answers[tuple(command)]) + "\n", "")

    def identity(self):
        with (
            patch.dict(os.environ, self.environment),
            patch.object(native, "host_machine", return_value="arm64"),
            patch.object(compiler_probe.platform, "machine", return_value="arm64"),
            patch.object(compiler_probe.subprocess, "run", side_effect=self.query),
        ):
            return compiler_probe.probe_compiler("macos")["identity"]

    def test_selected_sdk_is_raw_metadata_without_graph_policy_or_native_tool_relocation(self):
        with (
            patch.dict(os.environ, self.environment),
            patch.object(native, "host_machine", return_value="ARM64"),
            patch.object(compiler_probe.platform, "machine", return_value="ARM64"),
            patch.object(compiler_probe.subprocess, "run", side_effect=self.query),
        ):
            data = compiler_probe.probe_compiler("macos")
        sdk = data["sdk"]
        self.assertEqual(set(data), {"platform", "sdk", "identity"})
        self.assertEqual(data["platform"], "macos")
        self.assertEqual(set(sdk), {"machine", "llvm", "llvm_real", "sdk", "shim", "linker", "deployment"})
        self.assertIn("Åsa", sdk["shim"])
        self.assertEqual(sdk["shim"], str(self.shim))
        self.assertEqual(sdk["linker"], str(self.linker))
        self.assertEqual(sdk["sdk"], str(self.sdk))
        self.assertEqual(sdk["machine"], "ARM64")
        self.assertFalse((self.generated / "toolchain/bin").exists())

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
