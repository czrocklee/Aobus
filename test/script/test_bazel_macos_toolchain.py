"""macOS trusts installed SDK selection, not a recursive native byte ledger."""

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
        self.linker = self.root / "developer/usr/bin/ld"
        self.dsymutil = self.root / "developer/usr/bin/dsymutil"
        self.apple_strip = self.root / "developer/usr/bin/strip"
        self.xcrun = self.root / "selected/xcrun"
        self.developer = self.root / "Åsa/Developer"
        self.version, self.sdk_version, self.sdk_build = "22.1.8", "26.5", "25F70"
        self.calls = []
        self.executables = [
            *[self.llvm / "bin" / name for name in ("clang", "clang++", "llvm-libtool-darwin", "llvm-strip")],
            self.linker,
            self.dsymutil,
            self.apple_strip,
            self.xcrun,
        ]
        self.liblto = self.llvm / "lib/libLTO.dylib"
        self.shim_header = self.shim / "__expected/expected.h"
        self.headers = [self.sdk / "usr/include/stdio.h", self.llvm / "include/c++/v1/vector"]
        for path in [*self.executables, self.liblto, self.shim_header, *self.headers, self.sdk / "SDKSettings.json"]:
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
        self.calls.append(normalized)
        answers = {
            (str(self.xcrun), "--show-sdk-path"): str(self.sdk),
            (str(self.xcrun), "--sdk", str(self.sdk), "--show-sdk-version"): self.sdk_version,
            (str(self.xcrun), "--sdk", str(self.sdk), "--show-sdk-build-version"): self.sdk_build,
            (str(self.xcrun), "--find", "ld"): str(self.linker),
            (str(self.xcrun), "--find", "dsymutil"): str(self.dsymutil),
            (str(self.xcrun), "--find", "strip"): str(self.apple_strip),
            (str(self.llvm / "bin/clang++"), "--no-default-config", "-dumpversion"): self.version,
        }
        self.assertIn(normalized, answers)
        self.assertEqual(kwargs["capture_output"], True)
        self.assertEqual(kwargs["text"], True)
        self.assertTrue(kwargs["check"])
        return subprocess.CompletedProcess(command, 0, answers[normalized] + "\n", "")

    @contextmanager
    def patched(self, machine="arm64"):
        with (
            patch.dict(os.environ, self.environment),
            patch.object(native, "host_machine", return_value=machine),
            patch.object(compiler_probe.platform, "machine", return_value=machine),
            patch.object(native, "executable", side_effect=self.selected_xcrun),
            patch.object(compiler_probe.subprocess, "run", side_effect=self.query),
        ):
            yield

    def probe(self):
        with self.patched():
            return compiler_probe.probe_compiler("macos")

    def test_selection_metadata_binds_versions_paths_and_deployment_without_a_byte_ledger(self):
        with self.patched("ARM64"):
            data = compiler_probe.probe_compiler("macos")
        sdk = data["sdk"]
        self.assertEqual(set(data), {"platform", "sdk", "identity"})
        self.assertEqual(data["identity"], {"schema": 2, "platform": "macos", "sdk": sdk})
        self.assertNotIn("files", data["identity"])
        self.assertEqual(sdk["machine"], "ARM64")
        self.assertEqual(sdk["llvm_version"], "22.1.8")
        self.assertEqual(sdk["sdk_version"], "26.5")
        self.assertEqual(sdk["sdk_build_version"], "25F70")
        self.assertEqual(sdk["sdk"], str(self.sdk))
        self.assertEqual(sdk["shim"], str(self.shim))
        self.assertEqual(sdk["liblto"], str(self.liblto))
        self.assertEqual(sdk["developer_dir"], str(self.developer))
        self.assertEqual(sdk["deployment"], "15.0")
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
        self.assertEqual(len(self.calls), len(set(self.calls)))

    def test_sdk_settings_are_watched_but_implicit_headers_are_not_enumerated(self):
        observed = []
        with native.capture_probes(observed.append):
            self.probe()
        for name in ("SDKSettings.json", "SDKSettings.plist"):
            self.assertIn(self.sdk / name, observed)
        self.assertNotIn(self.headers[0], observed)
        (self.sdk / "SDKSettings.json").unlink()
        with self.assertRaisesRegex(native.BazelWorkspaceError, "SDKSettings metadata"):
            self.probe()

    def test_llvm_sdk_version_and_sdk_build_changes_change_action_metadata(self):
        baseline = self.probe()["identity"]
        for field, changed in (("version", "22.1.9"), ("sdk_version", "26.6"), ("sdk_build", "25F71")):
            with self.subTest(field=field):
                original = getattr(self, field)
                setattr(self, field, changed)
                self.assertNotEqual(self.probe()["identity"], baseline)
                setattr(self, field, original)

    def test_missing_explicit_plugin_or_governed_shim_rejects_before_projection(self):
        for path in (self.liblto, self.shim_header):
            with self.subTest(path=path):
                path.unlink()
                with self.assertRaisesRegex(native.BazelWorkspaceError, "compiler input is missing"):
                    self.probe()
                path.write_bytes(b"original")


if __name__ == "__main__":
    unittest.main()
