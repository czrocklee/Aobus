"""Compiler SDK observations and repository-owned materialization contracts."""

import hashlib
import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from ao.core import bazel_native as native
from ao.portal import bazel_compiler_probe as compiler_probe
from ao.portal import bazel_providers as providers


class ToolchainObservationTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-toolchain-facts-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.sdk = self.root / "compiler"
        self.include = self.sdk / "include/c++"
        self.include.mkdir(parents=True)
        self.tools = {}
        for role, name in (
            ("cc", "gcc"),
            ("cxx", "g++"),
            ("ar", "gcc-ar"),
            ("strip", "strip"),
            ("objcopy", "objcopy"),
            ("gcov", "gcov"),
            ("cc", "clang"),
            ("cxx", "clang++"),
            ("ar", "llvm-ar"),
            ("strip", "llvm-strip"),
            ("objcopy", "llvm-objcopy"),
            ("gcov", "llvm-cov"),
        ):
            path = self.sdk / "bin" / name
            path.parent.mkdir(exist_ok=True)
            path.write_bytes(b"selected compiler input")
            path.chmod(0o755)
            self.tools.setdefault(role, {})[name] = path.as_posix()
        # The selected lexical spelling must survive observation and symlinking.
        (self.sdk / "bin/dispatch").mkdir()
        for _compiler, driver in (("gcc", "g++"), ("clang", "clang++")):
            self.tools["cxx"][driver] = (self.sdk / "bin" / f"dispatch/../{driver}").as_posix()
        # GCC's parallel LTO link re-execs this declared make helper.
        self.make_helper = self.sdk / "bin/make"
        self.make_helper.write_bytes(b"selected lto make helper")
        self.make_helper.chmod(0o755)
        self.resource = self.sdk / "resource-root"
        share = self.resource / "share"
        share.mkdir(parents=True)
        (share / "asan_ignorelist.txt").write_text("fun:*_aobus_sanitizer_fixture*\n")
        self.gcc_resource = self.sdk / "gcc-include-root"
        (self.gcc_resource / "sanitizer").mkdir(parents=True)
        (self.gcc_resource / "sanitizer/tsan_interface.h").write_text("/* GCC fixture interface */\n")
        self.sanitizer_include = (self.sdk / "include").as_posix()
        sanitizer = self.sdk / "include/sanitizer"
        sanitizer.mkdir(parents=True)
        for header in ("tsan_interface.h", "common_interface_defs.h"):
            (sanitizer / header).write_text("/* compiler-rt fixture interface */\n")
        self.wrappers = {
            "gcc": {
                "suffixSalt": "fixture_target_gcc",
                "hardeningEnable": ["fortify", "fortify3", "stackprotector", "relro"],
                "version": "16.2.0",
                "tools": {
                    role: self.tools[role][name]
                    for role, name in (
                        ("cc", "gcc"),
                        ("cxx", "g++"),
                        ("ar", "gcc-ar"),
                        ("strip", "strip"),
                        ("objcopy", "objcopy"),
                        ("gcov", "gcov"),
                    )
                },
                "builtinRoots": [str(self.sdk / "gcc-root")],
                "sanitizerIncludeDirectory": self.sanitizer_include,
                "ltoMake": self.make_helper.as_posix(),
            },
            "clang": {
                "suffixSalt": "fixture_target",
                "hardeningEnable": ["fortify", "stackprotector", "relro"],
                "version": "22.1.8",
                "tools": {
                    role: self.tools[role][name]
                    for role, name in (
                        ("cc", "clang"),
                        ("cxx", "clang++"),
                        ("ar", "llvm-ar"),
                        ("strip", "llvm-strip"),
                        ("objcopy", "llvm-objcopy"),
                        ("gcov", "llvm-cov"),
                    )
                },
                "builtinRoots": [str(self.sdk)],
                "sanitizerIncludeDirectory": self.sanitizer_include,
            },
        }
        checkout = self.root / "checkout"
        (checkout / "config").mkdir(parents=True)
        (checkout / "config/nixpkgs.json").write_text(json.dumps({"rev": "fixture-pin"}), encoding="utf-8")
        self.report = self.root / "nix-report.json"
        self.report_data = {
            "schemaVersion": 1,
            "nixpkgsRevision": "fixture-pin",
            "llvmTools": {"symbolizer": "/nix/store/fixture-llvm/bin/llvm-symbolizer"},
            "ccWrappers": self.wrappers,
        }
        self.write_report()
        self.enterContext(patch.object(native, "PROJECT_ROOT", checkout))
        self.enterContext(patch.dict(os.environ, {"AOBUS_NIX_DEPENDENCY_REPORT": str(self.report)}))
        self.enterContext(patch.object(native, "host_machine", return_value="x86_64"))
        self.enterContext(patch.object(providers, "nix_root", side_effect=self.nix_root))
        self.query_error = None
        self.enterContext(patch.object(compiler_probe.subprocess, "run", side_effect=self.query))

    def write_report(self):
        self.report.write_text(json.dumps(self.report_data), encoding="utf-8")

    def nix_root(self, path):
        # Compare the fixture spelling lexically: on Windows resolve() would
        # reinterpret the drive-relative Nix path against the current drive.
        if path == Path("/nix/store/fixture-llvm/bin/llvm-symbolizer"):
            return Path("/nix/store/fixture-llvm")
        resolved = path.resolve()
        if not resolved.is_relative_to(self.sdk):
            raise native.BazelWorkspaceError(f"provider path is outside the pinned Nix store: {path}")
        return self.sdk

    def query(self, command, **kwargs):
        driver = Path(command[0]).name
        compiler = "gcc" if driver == "g++" else "clang"
        self.assertEqual(command[0], self.wrappers[compiler]["tools"]["cxx"])
        self.assertEqual(kwargs["env"], {"PATH": ""})
        self.assertTrue(kwargs["check"])
        if command[1:] == ["-print-resource-dir"]:
            self.assertEqual(compiler, "clang")
            self.assertNotIn("input", kwargs)
            return subprocess.CompletedProcess(command, 0, str(self.resource), "")
        if command[1:] == ["-print-file-name=include"]:
            self.assertEqual(compiler, "gcc")
            self.assertNotIn("input", kwargs)
            return subprocess.CompletedProcess(command, 0, str(self.gcc_resource), "")
        self.assertEqual(
            command,
            [
                self.wrappers[compiler]["tools"]["cxx"],
                "-E",
                "-x",
                "c++",
                "-std=c++26",
                "-no-canonical-prefixes",
                "-",
                "-v",
            ],
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
        self.assertEqual(set(data["sdk"]), {"machine", "compilers"})
        self.assertEqual(set(data["sdk"]["compilers"]), {"gcc", "clang"})
        self.assertEqual(data["sdk"]["machine"], "x86_64")
        self.assertEqual(
            data["sdk"]["compilers"]["gcc"],
            {
                "version": "16.2.0",
                "tools": self.wrappers["gcc"]["tools"],
                "builtin_include_directories": [str(self.include)],
                "compiler_resource_directory": str(self.gcc_resource),
                "sanitizer_include_directory": self.sanitizer_include,
                "nix_cc_wrapper": {
                    "suffixSalt": "fixture_target_gcc",
                    "hardeningEnable": ["fortify", "fortify3", "stackprotector", "relro"],
                },
                "lto_make": str(self.make_helper),
            },
        )
        self.assertEqual(
            data["sdk"]["compilers"]["clang"],
            {
                "version": "22.1.8",
                "tools": self.wrappers["clang"]["tools"],
                "builtin_include_directories": [str(self.include)],
                "compiler_resource_directory": str(self.resource),
                "sanitizer_include_directory": self.sanitizer_include,
                "nix_cc_wrapper": {
                    "suffixSalt": "fixture_target",
                    "hardeningEnable": ["fortify", "stackprotector", "relro"],
                },
            },
        )
        self.assertEqual(set(data["identity"]), {"gcc", "clang"})
        for compiler, wrapper in self.wrappers.items():
            identity = data["identity"][compiler]
            context = identity["context"]
            self.assertEqual(identity["schema"], 1)
            self.assertEqual(context["compiler"], compiler)
            self.assertEqual(context["wrapper_suffix"], wrapper["suffixSalt"])
            self.assertEqual(context["version"], wrapper["version"])
            self.assertEqual(json.loads(context["tools"]), wrapper["tools"])
            self.assertEqual(json.loads(context["builtin_roots"]), wrapper["builtinRoots"])
            # The make helper is a link-only input; it stays out of the identity.
            # str(Path(value)) mirrors the recorded spelling on every host.
            self.assertEqual(
                {item["path"] for item in identity["files"]},
                {str(Path(value)) for value in wrapper["tools"].values()},
            )
            driver = Path(wrapper["tools"]["cxx"])
            record = next(item for item in identity["files"] if item["path"] == str(driver))
            self.assertEqual(record["realpath"], str(self.sdk / "bin" / driver.name))
            self.assertEqual(record["sha256"], hashlib.sha256(b"selected compiler input").hexdigest())

    def test_host_machine_spelling_remains_raw_until_repository_projection(self):
        with patch.object(native, "host_machine", return_value="AARCH64"):
            data = compiler_probe.probe_compiler("linux")
        self.assertEqual(data["sdk"]["machine"], "AARCH64")
        for identity in data["identity"].values():
            self.assertEqual(identity["context"]["machine"], "AARCH64")
        self.assertNotIn("CONSTRAINT_CPU", json.dumps(data))

    def test_selected_tool_byte_changes_invalidate_one_identity_without_changing_sdk_facts(self):
        before = compiler_probe.probe_compiler("linux")
        compiler = Path(self.wrappers["gcc"]["tools"]["cxx"])
        stat = compiler.stat()
        compiler.write_bytes(b"changed in place")
        os.utime(compiler, ns=(stat.st_atime_ns, stat.st_mtime_ns))
        after = compiler_probe.probe_compiler("linux")
        self.assertEqual(after["sdk"], before["sdk"])
        self.assertNotEqual(after["identity"]["gcc"], before["identity"]["gcc"])
        self.assertEqual(after["identity"]["clang"], before["identity"]["clang"])
        record = next(item for item in after["identity"]["gcc"]["files"] if item["path"] == str(compiler))
        self.assertEqual(record["sha256"], hashlib.sha256(b"changed in place").hexdigest())

    def test_compiler_probe_failure_propagates_without_materializing_partial_output(self):
        before = self.snapshot()
        self.query_error = subprocess.CalledProcessError(
            9, [self.wrappers["gcc"]["tools"]["cxx"]], stderr="compiler failed"
        )
        with self.assertRaises(subprocess.CalledProcessError) as failure:
            compiler_probe.probe_compiler("linux")
        self.assertIs(failure.exception, self.query_error)
        self.assertEqual(self.snapshot(), before)

    def test_gcc_lto_make_helper_admission_fails_closed(self):
        original = self.wrappers["gcc"]
        helper = Path(original["ltoMake"])
        foreign = self.root / "foreign-make"
        foreign.write_bytes(b"ambient make")
        foreign.chmod(0o755)
        helper.chmod(0o644)
        self.addCleanup(helper.chmod, 0o755)
        cases = (
            ("report without the helper field", {k: v for k, v in original.items() if k != "ltoMake"}, "pinned shell"),
            (
                "helper file missing",
                {**original, "ltoMake": str(self.sdk / "bin/absent-make")},
                "missing or not executable",
            ),
            ("helper not executable", {**original, "ltoMake": str(helper)}, "missing or not executable"),
            (
                "helper outside the pinned Nix store",
                {**original, "ltoMake": str(foreign)},
                "outside the pinned Nix store",
            ),
        )
        for name, wrapper, message in cases:
            if os.name == "nt" and name == "helper not executable":
                # Windows reports X_OK for any existing file, so the mode-bit
                # branch is observable only where POSIX modes apply.
                continue
            with self.subTest(name=name):
                self.wrappers["gcc"] = wrapper
                self.write_report()
                with self.assertRaisesRegex(native.BazelWorkspaceError, message):
                    compiler_probe.probe_compiler("linux")
        self.wrappers["gcc"] = original
        self.write_report()

    def test_unknown_platform_is_rejected_before_compiler_observation(self):
        with (
            patch.object(native, "host_machine", side_effect=AssertionError("must not inspect the host")),
            self.assertRaisesRegex(native.BazelWorkspaceError, "unsupported.*platform: haiku"),
        ):
            compiler_probe.probe_compiler("haiku")

    def test_wrapper_selection_reads_one_compiler_without_fallback(self):
        self.assertEqual(compiler_probe.nix_cc_wrapper("gcc"), self.wrappers["gcc"])
        self.assertEqual(compiler_probe.nix_cc_wrapper("clang"), self.wrappers["clang"])
        self.assertEqual(compiler_probe.nix_cc_wrapper(), self.wrappers["gcc"])
        with self.assertRaisesRegex(native.BazelWorkspaceError, "unsupported Linux Nix compiler wrapper: icc"):
            compiler_probe.nix_cc_wrapper("icc")

    def test_symbolizer_reads_the_explicit_report_field(self):
        self.assertEqual(compiler_probe.nix_symbolizer(), "/nix/store/fixture-llvm/bin/llvm-symbolizer")


if __name__ == "__main__":
    unittest.main()
