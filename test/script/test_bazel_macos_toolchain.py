"""Mutable native SDK inputs must change the compiler's action identity."""

import ast
import hashlib
import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from ao.core import bazel_workspace as workspace


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
            patch.object(workspace, "_host_machine", return_value="arm64"),
            patch.object(workspace.subprocess, "run", side_effect=self.query),
        ):
            workspace._toolchain_identity(self.generated, "macos")
        return self.generated / "toolchain/identity.json"

    def test_selected_linker_is_exported_without_relocating_native_tools(self):
        with (
            patch.dict(os.environ, self.environment),
            patch.object(workspace.platform, "machine", return_value="arm64"),
            patch.object(workspace.subprocess, "run", side_effect=self.query),
        ):
            workspace._macos_toolchain(self.generated)
        # These assignments are the emitted Starlark data interface, not the
        # implementation spelling or a second product-graph inventory.
        source = (self.generated / "toolchain/native_paths.bzl").read_text(encoding="utf-8")
        metadata = {node.targets[0].id: ast.literal_eval(node.value) for node in ast.parse(source).body}
        self.assertIn("Åsa", source)
        self.assertEqual(metadata["SHIM"], str(self.shim))
        self.assertEqual(metadata["LINKER"], str(self.linker))
        self.assertEqual(metadata["SDK"], str(self.sdk))
        self.assertEqual(metadata["CONSTRAINT_CPU"], "aarch64")
        self.assertFalse((self.generated / "toolchain/bin").exists())

    def test_in_place_native_changes_invalidate_even_with_unchanged_versions_and_mtimes(self):
        baseline = self.identity().read_bytes()
        for path in self.inputs:
            with self.subTest(path=path.relative_to(self.root)):
                stat = path.stat()
                path.write_bytes(b"modified")
                os.utime(path, ns=(stat.st_atime_ns, stat.st_mtime_ns))
                changed = self.identity()
                self.assertNotEqual(changed.read_bytes(), baseline)
                record = next(item for item in json.loads(changed.read_text())["files"] if item["path"] == str(path))
                self.assertEqual(record["sha256"], hashlib.sha256(b"modified").hexdigest())
                path.write_bytes(b"original")
                self.assertEqual(self.identity().read_bytes(), baseline)

    def test_header_addition_and_removal_change_identity_without_rewriting_unchanged_output(self):
        identity = self.identity()
        baseline = identity.read_bytes()
        sentinel = 1_000_000_000_000_000_000
        os.utime(identity, ns=(sentinel, sentinel))
        self.identity()
        self.assertEqual(identity.stat().st_mtime_ns, sentinel)
        added = self.sdk / "usr/include/new.h"
        added.write_bytes(b"new header")
        self.assertNotEqual(self.identity().read_bytes(), baseline)
        added.unlink()
        self.assertEqual(self.identity().read_bytes(), baseline)

    def test_optional_sdk_search_directory_is_watched_before_installation(self):
        directory = self.sdk / "usr/local/include"
        observed = []
        with patch.object(workspace, "_INPUT_PROBE", observed.append):
            baseline = self.identity().read_bytes()
        self.assertIn(directory, observed)
        directory.mkdir(parents=True)
        (directory / "new.h").write_bytes(b"new native SDK header")
        self.assertNotEqual(self.identity().read_bytes(), baseline)


if __name__ == "__main__":
    unittest.main()
