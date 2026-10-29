"""Shared Windows SDK manifest contracts, without downloading an SDK."""

import contextlib
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from cmake_support import cmake_executable

from ao.core import builddir, tidyengine
from ao.core.paths import PROJECT_ROOT


class WindowsLlvmSdkManifestTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus SDK manifest ")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        module = self.root / "cmake/LlvmSdk.cmake"
        module.parent.mkdir()
        shutil.copyfile(PROJECT_ROOT / "cmake/LlvmSdk.cmake", module)
        self.manifest = self.root / "script/ao/windows-llvm-sdk.json"
        self.manifest.parent.mkdir(parents=True)
        self.base = json.loads((PROJECT_ROOT / "script/ao/windows-llvm-sdk.json").read_text(encoding="utf-8"))
        self.manifest.write_text(json.dumps(self.base), encoding="utf-8")
        self.enterContext(mock.patch.object(tidyengine, "LLVM_SDK_MANIFEST", self.manifest))
        tidyengine.windows_llvm_sdk_manifest.cache_clear()
        self.addCleanup(tidyengine.windows_llvm_sdk_manifest.cache_clear)

    def cmake(self, body=""):
        script = self.root / "probe.cmake"
        script.write_text(f'include("{(self.root / "cmake/LlvmSdk.cmake").as_posix()}")\n' + body, encoding="utf-8")
        return subprocess.run(
            [cmake_executable(), "-P", str(script)],
            cwd=self.root,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            check=False,
        )

    def test_both_readers_evaluate_identity_and_derived_files(self):
        result = self.cmake(
            'string(REPLACE "\\n" "\\\\n" _marker "${_AOBUS_LLVM_SDK_COMPLETION_CONTENT}")\n'
            'file(WRITE "identity.txt" "version=${AOBUS_LLVM_SDK_VERSION}\\n"\n'
            '"sha256=${AOBUS_LLVM_SDK_SHA256}\\narchive=${AOBUS_LLVM_SDK_ARCHIVE}\\n"\n'
            '"url=${AOBUS_LLVM_SDK_URL}\\nrequired=${_AOBUS_LLVM_SDK_REQUIRED_FILES}\\nmarker=${_marker}\\n")\n'
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        evaluated = dict(line.split("=", 1) for line in (self.root / "identity.txt").read_text().splitlines())
        version, sha256, required = tidyengine.windows_llvm_sdk_manifest()
        self.assertEqual((version, sha256), (self.base["version"], self.base["archiveSha256"]))
        self.assertEqual(evaluated["version"], version)
        self.assertEqual(evaluated["sha256"], sha256)
        self.assertEqual(evaluated["archive"], f"clang+llvm-{version}-x86_64-pc-windows-msvc.tar.xz")
        self.assertEqual(
            evaluated["url"],
            f"https://github.com/llvm/llvm-project/releases/download/llvmorg-{version}"
            f"/clang%2Bllvm-{version}-x86_64-pc-windows-msvc.tar.xz",
        )
        expected = (*self.base["requiredFiles"], f"lib/clang/{version.split('.', 1)[0]}/include/stddef.h")
        self.assertEqual(required, expected)
        self.assertEqual(tuple(evaluated["required"].split(";")), expected)
        self.assertEqual(evaluated["marker"].replace("\\n", "\n"), f"version={version}\nsha256={sha256}\n")

    def test_readers_agree_on_manifest_shape_and_portable_path_contract(self):
        invalid = [
            [],
            *({key: value for key, value in self.base.items() if key != missing} for missing in self.base),
            *({**self.base, "version": value} for value in (5, True, "22", "22.1.x", "22.1.8\x00x")),
            *(
                {**self.base, "archiveSha256": value}
                for value in (5, True, "short", "g" * 64, "A" * 64, "a" * 31 + "\x00" + "b" * 32)
            ),
            *({**self.base, "requiredFiles": value} for value in ([], "bin/tool.exe", {"0": "bin/tool.exe"})),
            *(
                {**self.base, "requiredFiles": [value]}
                for value in (
                    5,
                    True,
                    False,
                    None,
                    {},
                    "",
                    "/root/file",
                    "C:/file",
                    "C:file",
                    "\\\\server\\file",
                    "bin\\tool.exe",
                    "../file",
                    "bin/../file",
                    "./file",
                    "bin/./file",
                    "a;b",
                    "a//b",
                    "bin/",
                    "a\nb",
                    "a\x00b",
                )
            ),
        ]
        valid = [self.base, {**self.base, "requiredFiles": ["OFF", "0", "FALSE", "x-NOTFOUND", "a..b/tool+name.h"]}]
        for accepted, payloads in ((False, invalid), (True, valid)):
            for payload in payloads:
                with self.subTest(accepted=accepted, payload=payload):
                    self.manifest.write_text(json.dumps(payload), encoding="utf-8")
                    tidyengine.windows_llvm_sdk_manifest.cache_clear()
                    if accepted:
                        _, _, required = tidyengine.windows_llvm_sdk_manifest()
                    else:
                        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                            tidyengine.windows_llvm_sdk_manifest()
                    result = self.cmake('file(WRITE "required.txt" "${_AOBUS_LLVM_SDK_REQUIRED_FILES}")\n')
                    self.assertEqual(result.returncode == 0, accepted, result.stderr)
                    if accepted:
                        self.assertEqual(tuple((self.root / "required.txt").read_text().split(";")), required)

    def test_missing_members_keep_field_specific_cmake_diagnostics(self):
        for missing in self.base:
            with self.subTest(missing=missing):
                self.manifest.write_text(json.dumps({key: value for key, value in self.base.items() if key != missing}))
                result = self.cmake()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(missing, result.stderr)
                self.assertIn("not found", result.stderr)

    def test_unreadable_or_malformed_manifest_fails_at_sdk_use(self):
        for content in (None, b"{ not json", b"\xff"):
            with self.subTest(content=content):
                if content is None:
                    self.manifest.unlink()
                else:
                    self.manifest.write_bytes(content)
                tidyengine.windows_llvm_sdk_manifest.cache_clear()
                with contextlib.redirect_stderr(io.StringIO()) as stderr, self.assertRaises(SystemExit):
                    tidyengine.windows_llvm_sdk_manifest()
                self.assertIn("Windows LLVM SDK manifest", stderr.getvalue())
                self.assertNotEqual(self.cmake().returncode, 0)

    def test_manifest_is_cached_as_an_immutable_snapshot(self):
        first = tidyengine.windows_llvm_sdk_manifest()
        self.manifest.unlink()
        self.assertIs(tidyengine.windows_llvm_sdk_manifest(), first)
        self.assertIsInstance(first, tuple)
        self.assertIsInstance(first[2], tuple)

    def test_non_windows_sdk_ensure_does_not_read_the_manifest(self):
        self.manifest.unlink()
        for profile in (builddir.LINUX_PROFILE, builddir.MACOS_PROFILE):
            with (
                self.subTest(profile=profile.name),
                mock.patch.object(builddir, "platform_profile", return_value=profile),
            ):
                tidyengine.ensure_windows_llvm_sdk(self.root / "unused-build")

    def test_help_startup_does_not_depend_on_a_windows_manifest(self):
        # Use a fresh dispatch process: importing a module already loaded by this
        # test interpreter would not catch accidental import-time manifest IO.
        shutil.copytree(
            PROJECT_ROOT / "script/ao",
            self.root / "script/ao",
            dirs_exist_ok=True,
            ignore=shutil.ignore_patterns("__pycache__"),
        )
        self.manifest.write_text('{"version":"22.1.8"}', encoding="utf-8")
        result = subprocess.run(
            [sys.executable, "-m", "ao", "help"],
            cwd=self.root,
            # This package copy has no private Git directory. Give the Windows
            # profile a fixture identity without copying real checkout metadata.
            env={**os.environ, "PYTHONPATH": str(self.root / "script"), "AOBUS_CHECKOUT_ID": "sdk-startup-fixture"},
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("usage:", result.stdout)

    def test_cmake_sdk_completeness_and_marker_validation(self):
        result = self.cmake(
            "foreach(_required IN LISTS _AOBUS_LLVM_SDK_REQUIRED_FILES)\n"
            '  file(WRITE "${CMAKE_CURRENT_LIST_DIR}/sdk/${_required}" "")\n'
            "endforeach()\n"
            '_aobus_validate_llvm_sdk("${CMAKE_CURRENT_LIST_DIR}/sdk" FALSE _valid _reason)\n'
            'file(WRITE "validation.txt" "complete=${_valid};${_reason}\\n")\n'
            '_aobus_validate_llvm_sdk("${CMAKE_CURRENT_LIST_DIR}/sdk" TRUE _valid _reason)\n'
            'file(APPEND "validation.txt" "marker_missing=${_valid};${_reason}\\n")\n'
            'file(WRITE "sdk/${_AOBUS_LLVM_SDK_COMPLETION_MARKER}" "${_AOBUS_LLVM_SDK_COMPLETION_CONTENT}")\n'
            '_aobus_validate_llvm_sdk("${CMAKE_CURRENT_LIST_DIR}/sdk" TRUE _valid _reason)\n'
            'file(APPEND "validation.txt" "marker_complete=${_valid};${_reason}\\n")\n'
            'file(WRITE "sdk/${_AOBUS_LLVM_SDK_COMPLETION_MARKER}" "incomplete\\n")\n'
            '_aobus_validate_llvm_sdk("${CMAKE_CURRENT_LIST_DIR}/sdk" TRUE _valid _reason)\n'
            'file(APPEND "validation.txt" "marker_stale=${_valid};${_reason}\\n")\n'
            "list(GET _AOBUS_LLVM_SDK_REQUIRED_FILES 0 _first)\n"
            'file(REMOVE "sdk/${_first}")\n'
            '_aobus_validate_llvm_sdk("${CMAKE_CURRENT_LIST_DIR}/sdk" FALSE _valid _reason)\n'
            'file(APPEND "validation.txt" "file_missing=${_valid};${_reason}\\n")\n'
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        results = dict(line.split("=", 1) for line in (self.root / "validation.txt").read_text().splitlines())
        self.assertEqual(results["complete"], "TRUE;")
        self.assertEqual(results["marker_complete"], "TRUE;")
        self.assertTrue(results["marker_missing"].startswith("FALSE;missing completion marker"))
        self.assertTrue(results["marker_stale"].startswith("FALSE;completion marker does not match"))
        self.assertTrue(results["file_missing"].startswith("FALSE;missing"))
