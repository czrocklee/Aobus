"""The native WinUI naming gate stays explicit and requires a loaded desktop."""

import re
import sys
import unittest
from pathlib import Path
from unittest import mock

from ao.core import winui

WORKFLOW = Path(__file__).parents[2] / ".github/workflows/ci.yml"


def _windows_step(name: str) -> str:
    windows = WORKFLOW.read_text(encoding="utf-8").split("  windows:\n", 1)[1]
    return windows.split(f"      - name: {name}\n", 1)[1].split("      - name:", 1)[0]


class WinUiNamingGateTest(unittest.TestCase):
    def test_ci_requires_the_native_desktop_gate_without_ignoring_failure(self):
        windows = WORKFLOW.read_text(encoding="utf-8").split("  windows:\n", 1)[1]
        positions = [
            windows.index(f"      - name: {name}\n")
            for name in (
                "Set up Windows App Runtime",
                "Check Bazel completion",
                "Test direct Bazel entry",
                "WinUI naming gate",
                "Stop Bazel remote cache",
            )
        ]
        self.assertEqual(positions, sorted(positions))
        block = _windows_step("WinUI naming gate")
        self.assertNotIn("continue-on-error", block)
        self.assertNotIn("\n        if:", block)
        self.assertIn(r"tools\python\$($contract.python)\python.exe", block)
        self.assertIn("$python -I -S -B -X utf8 -c", block)
        self.assertNotIn(r".\ao.bat", block)
        self.assertIn("$version = (Get-Content .bazelversion -Raw).Trim()", block)
        self.assertIn(r"tools\bazel\$version\bazel.exe", block)
        self.assertIn("if (-not (Test-Path $bazel)) { throw", block)
        self.assertIn('& $bazel "--output_base=$bare\\state\\output" run', block)
        self.assertIn("//app/windows-winui:ao_winui_automation_probe", block)
        self.assertIn(r"--build_event_json_file=$bare\winui-naming.bep.json", block)
        self.assertEqual(block.count("if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }"), 2)
        self.assertLess(block.index("winui.require_interactive_session()"), block.index("& $bazel"))

    def test_desktop_gate_reuses_the_admitted_direct_native_environment_and_evidence_root(self):
        direct = _windows_step("Test direct Bazel entry")
        audit = _windows_step("WinUI naming gate")
        self.assertNotIn("continue-on-error", direct)
        self.assertNotIn("\n        if:", direct)
        self.assertIn("prepare_build_environment()", direct)
        self.assertIn("selected.update(windows_compiler_selection(os.environ))", direct)
        self.assertIn("foreach ($property in $selection.PSObject.Properties)", direct)
        self.assertIn('"$($property.Name)=$($property.Value)" >> $env:GITHUB_ENV', direct)
        self.assertIn('"AOBUS_BAZEL_VCPKG_PREFIX=$env:AOBUS_BAZEL_VCPKG_PREFIX" >> $env:GITHUB_ENV', direct)
        self.assertIn('"BAZEL_SH=$env:BAZEL_SH" >> $env:GITHUB_ENV', direct)
        self.assertIn(r'$env:AOBUS_BAZEL_VCPKG_PREFIX = "D:\aobus-bazel-fastbuild\vcpkg_installed\$triplet"', direct)
        self.assertIn('vcpkg_triplets(host="windows")', direct)
        # The inherited native selection must not be replaced with portal or
        # remote-cache bindings before the desktop launch.
        for step in (direct, audit):
            self.assertIn("$bare = 'D:\\aobus-bare'", step)
        self.assertNotIn("AOBUS_BAZEL_REMOTE_CACHE:", audit)
        evidence = _windows_step("Preserve direct Bazel evidence")
        self.assertIn(r"path: D:\aobus-bare\*.bep.json", evidence)
        self.assertIn("if: ${{ always() }}", evidence)

    def test_ci_desktop_admission_keeps_runtime_and_console_failures_fatal(self):
        block = _windows_step("WinUI naming gate")
        match = re.search(r"-c '([^']+)'", block)
        self.assertIsNotNone(match, "The desktop gate must admit its runtime and console before launch")
        script = compile(match.group(1), "<CI desktop admission>", "exec")
        calls = []
        with (
            mock.patch.object(sys, "path", list(sys.path)),
            mock.patch.object(winui, "require_runtime", side_effect=lambda: calls.append("runtime")),
            mock.patch.object(winui, "require_interactive_session", side_effect=lambda: calls.append("console")),
        ):
            exec(script, {})
        self.assertEqual(calls, ["runtime", "console"])
        for missing in ("require_runtime", "require_interactive_session"):
            with (
                self.subTest(missing=missing),
                mock.patch.object(sys, "path", list(sys.path)),
                mock.patch.object(winui, "require_runtime"),
                mock.patch.object(winui, "require_interactive_session"),
            ):
                with mock.patch.object(winui, missing, side_effect=RuntimeError("admission failed")):
                    with self.assertRaisesRegex(RuntimeError, "admission failed"):
                        exec(script, {})


if __name__ == "__main__":
    unittest.main()
