"""Shared Windows SDK manifest, cache and marker contracts, without downloading an SDK."""

import contextlib
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao.command import format as format_command
from ao.core import builddir, tidyengine, windows_llvm_sdk, winui
from ao.core.paths import PROJECT_ROOT


class WindowsLlvmSdkManifestTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus SDK manifest ")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.manifest = self.root / "script/ao/windows-llvm-sdk.json"
        self.manifest.parent.mkdir(parents=True)
        self.base = json.loads((PROJECT_ROOT / "script/ao/windows-llvm-sdk.json").read_text(encoding="utf-8"))
        self.manifest.write_text(json.dumps(self.base), encoding="utf-8")
        self.enterContext(mock.patch.object(tidyengine, "LLVM_SDK_MANIFEST", self.manifest))
        tidyengine.windows_llvm_sdk_manifest.cache_clear()
        self.addCleanup(tidyengine.windows_llvm_sdk_manifest.cache_clear)

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


class WindowsFormatReadySdkTest(unittest.TestCase):
    def test_cpp_format_uses_only_the_ready_pinned_sdk_without_visual_studio(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest = windows_llvm_sdk.Manifest("31.2.3", "b" * 64, ("bin/clang-format.exe",))
            with (
                mock.patch.object(builddir, "platform_profile", return_value=builddir.WINDOWS_PROFILE),
                mock.patch.object(windows_llvm_sdk, "read_manifest", return_value=manifest),
                mock.patch.object(windows_llvm_sdk, "ready_root", return_value=(root, False)) as ready,
                mock.patch.object(winui, "visual_studio_installation", side_effect=AssertionError("unexpected VS")),
                mock.patch.object(
                    format_command.subprocess, "run", return_value=subprocess.CompletedProcess([], 0)
                ) as run,
            ):
                self.assertEqual(format_command.run_clang_format(["lib/Foo.cpp"], check=True), 0)
            ready.assert_called_once_with(os.environ, manifest)
            run.assert_called_once_with(
                [str(root / "bin/clang-format.exe"), "--dry-run", "-Werror", "lib/Foo.cpp"], cwd=PROJECT_ROOT
            )

    def test_missing_ready_sdk_fails_without_launching_a_setup_or_formatter_process(self):
        with (
            mock.patch.object(builddir, "platform_profile", return_value=builddir.WINDOWS_PROFILE),
            mock.patch.object(windows_llvm_sdk, "read_manifest", return_value=object()),
            mock.patch.object(
                windows_llvm_sdk, "ready_root", side_effect=windows_llvm_sdk.WindowsLlvmSdkError("absent")
            ),
            mock.patch.object(winui, "visual_studio_installation", side_effect=AssertionError("unexpected VS")),
            mock.patch.object(format_command.subprocess, "run") as run,
            contextlib.redirect_stderr(io.StringIO()) as stderr,
            self.assertRaises(SystemExit),
        ):
            format_command.run_clang_format(["lib/Foo.cpp"], check=False)
        run.assert_not_called()
        self.assertIn("run ao setup llvm-sdk explicitly", stderr.getvalue())


class ReadySdkRootTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-ready-sdk-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.manifest = windows_llvm_sdk.Manifest(
            "31.2.3", "b" * 64, ("bin/clang-cl.exe", "lib/clangTidyMain.lib", "lib/clang/31/include/stddef.h")
        )

    def ready(self, root, *, marker=True):
        for relative in self.manifest.required_files:
            (root / relative).parent.mkdir(parents=True, exist_ok=True)
            (root / relative).write_text("fixture\n", encoding="utf-8")
        if marker:
            marker = "version=31.2.3\nsha256=" + "b" * 64 + "\n"
            (root / windows_llvm_sdk.MARKER).write_text(marker, encoding="utf-8", newline="\n")
        return root

    def test_default_cache_uses_governed_precedence_and_shared_completion_identity(self):
        for variable, prefix in (
            ("AOBUS_LLVM_SDK_CACHE_ROOT", ""),
            ("AOBUS_STATE_ROOT", "cache/llvm"),
            ("LOCALAPPDATA", "Aobus/cache/llvm"),
        ):
            with self.subTest(variable=variable):
                base = self.root / variable
                selected = self.ready(base / prefix / "toolchains/llvm-31.2.3-x86_64-windows-msvc")
                environment = {"LOCALAPPDATA": str(self.root / "not-selected"), variable: str(base)}
                self.assertEqual(windows_llvm_sdk.cache_root(environment, self.manifest), selected)
                self.assertEqual(windows_llvm_sdk.ready_root(environment, self.manifest), (selected, True))
                stale = "version=31.2.2\nsha256=" + "b" * 64 + "\n"
                (selected / windows_llvm_sdk.MARKER).write_text(stale, encoding="utf-8")
                with self.assertRaisesRegex(windows_llvm_sdk.WindowsLlvmSdkError, "completion marker"):
                    windows_llvm_sdk.ready_root(environment, self.manifest)

    def test_cached_marker_accepts_windows_newlines_without_rewriting_its_identity(self):
        base = self.root / "cached"
        root = self.ready(base / "toolchains/llvm-31.2.3-x86_64-windows-msvc")
        marker = root / windows_llvm_sdk.MARKER
        content = ("version=31.2.3\r\nsha256=" + "b" * 64 + "\r\n").encode("utf-8")
        marker.write_bytes(content)
        environment = {"AOBUS_LLVM_SDK_CACHE_ROOT": str(base)}
        self.assertEqual(windows_llvm_sdk.ready_root(environment, self.manifest), (root, True))
        self.assertEqual(marker.read_bytes(), content)
        marker.write_bytes(content.replace(b"sha256=b", b"sha256=c"))
        with self.assertRaisesRegex(windows_llvm_sdk.WindowsLlvmSdkError, "completion marker"):
            windows_llvm_sdk.ready_root(environment, self.manifest)

    def test_cached_extraction_without_its_marker_is_not_ready(self):
        base = self.root / "unmarked"
        root = self.ready(base / "toolchains/llvm-31.2.3-x86_64-windows-msvc", marker=False)
        with self.assertRaisesRegex(windows_llvm_sdk.WindowsLlvmSdkError, re.escape(windows_llvm_sdk.MARKER)):
            windows_llvm_sdk.ready_root({"AOBUS_LLVM_SDK_CACHE_ROOT": str(base)}, self.manifest)
        self.assertFalse((root / windows_llvm_sdk.MARKER).exists())

    def test_preprovisioned_sdk_needs_files_but_not_an_automatic_cache_marker(self):
        root = self.ready(self.root / "preprovisioned", marker=False)
        environment = {"AOBUS_LLVM_SDK_ROOT": str(root), "AOBUS_LLVM_SDK_CACHE_ROOT": str(self.root / "missing")}
        self.assertEqual(windows_llvm_sdk.ready_root(environment, self.manifest), (root, False))
        (root / "lib/clang/31/include/stddef.h").unlink()
        with self.assertRaisesRegex(windows_llvm_sdk.WindowsLlvmSdkError, "stddef.h"):
            windows_llvm_sdk.ready_root(environment, self.manifest)
        self.assertFalse((self.root / "missing").exists())

    def test_a_missing_cache_marker_is_typed_with_its_exact_negative(self):
        base = self.root / "unmarked-cache"
        root = self.ready(base / "toolchains/llvm-31.2.3-x86_64-windows-msvc", marker=False)
        with self.assertRaises(windows_llvm_sdk.WindowsLlvmSdkSelectedFileMissing) as caught:
            windows_llvm_sdk.ready_root({"AOBUS_LLVM_SDK_CACHE_ROOT": str(base)}, self.manifest)
        self.assertEqual(caught.exception.path, root / windows_llvm_sdk.MARKER)
        self.assertIn(windows_llvm_sdk.MARKER, str(caught.exception))

    def test_relative_or_unselected_roots_are_rejected_without_creating_files(self):
        for environment, message in (
            ({}, "No ready Windows LLVM SDK selected"),
            ({"AOBUS_LLVM_SDK_ROOT": "relative/sdk"}, "AOBUS_LLVM_SDK_ROOT must select an absolute"),
            ({"AOBUS_LLVM_SDK_CACHE_ROOT": "relative/cache"}, "LLVM SDK cache root must select an absolute"),
            ({"AOBUS_LLVM_SDK_CACHE_ROOT": str(self.root / "missing")}, "Windows LLVM SDK file is missing"),
        ):
            with self.subTest(environment=environment):
                with self.assertRaisesRegex(windows_llvm_sdk.WindowsLlvmSdkError, message):
                    windows_llvm_sdk.ready_root(environment, self.manifest)
        self.assertEqual(list(self.root.iterdir()), [])
