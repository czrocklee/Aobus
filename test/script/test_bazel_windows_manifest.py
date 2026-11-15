"""Static Bazel graph contracts and native Windows manifest-verifier regressions.

Source inspection does not execute Bazel or prove action-cache behavior.
"""

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from ao.core.paths import PROJECT_ROOT


@unittest.skipUnless(sys.platform == "win32", "Requires native Windows PowerShell and cmd.exe")
class WindowsManifestVerifierTests(unittest.TestCase):
    def setUp(self):
        base = Path(tempfile.gettempdir()).resolve()
        if (
            not base.drive
            or str(base).startswith("\\\\")
            or len(str(base)) > 120
            or base.is_relative_to(PROJECT_ROOT.resolve())
        ):
            self.skipTest("Verifier fixtures require a short local ordinary TEMP outside the checkout")
        self.powershell = Path(os.environ["SYSTEMROOT"]) / "System32/WindowsPowerShell/v1.0/powershell.exe"
        self.assertTrue(self.powershell.is_file(), f"Missing system PowerShell: {self.powershell}")
        temporary = tempfile.TemporaryDirectory(prefix="aobus manifest ", dir=base)
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.binary = self.root / "dummy program.exe"
        self.binary.write_bytes(b"owned dummy executable; never loaded")
        self.mt = self.root / "fake mt.cmd"
        self.fixture = self.root / "fixture.xml"
        self.manifest = self.root / "extracted manifest.xml"
        self.stamp = self.root / "verified manifest.ok"
        self.write_mt()

    def write_mt(self, status=0):
        self.mt.write_text(
            "@echo off\n"
            "setlocal\n"
            'set "output=%~3"\n'
            'echo %* > "%~dp0mt arguments.txt"\n'
            f"if not {status}==0 exit /b {status}\n"
            'copy /b "%~dp0fixture.xml" "%output:~5%" >nul\n'
            "exit /b %errorlevel%\n",
            encoding="ascii",
        )

    def verify(self, xml):
        self.fixture.write_text(xml, encoding="utf-8")
        return subprocess.run(
            [
                str(self.powershell),
                "-NoLogo",
                "-NoProfile",
                "-NonInteractive",
                "-ExecutionPolicy",
                "Bypass",
                "-File",
                str(PROJECT_ROOT / "bazel/verify_windows_utf8_manifest.ps1"),
                "-Mt",
                str(self.mt),
                "-Binary",
                str(self.binary),
                "-Manifest",
                str(self.manifest),
                "-Stamp",
                str(self.stamp),
            ],
            cwd=self.root,
            capture_output=True,
            timeout=30,
            check=False,
        )

    def test_resource_one_accepts_single_utf8_and_writes_deterministic_stamp_with_spaced_paths(self):
        xml = (
            '<assembly><activeCodePage xmlns="http://schemas.microsoft.com/SMI/2019/WindowsSettings">'
            "UTF-8</activeCodePage></assembly>"
        )
        result = self.verify(xml)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.stamp.read_bytes(), b"UTF-8\n")
        self.assertEqual(self.manifest.read_bytes(), self.fixture.read_bytes())
        arguments = (self.root / "mt arguments.txt").read_text()
        self.assertIn(f"-inputresource:{self.binary};#1", arguments)
        self.assertIn(f"-out:{self.manifest}", arguments)
        self.assertEqual(self.binary.read_bytes(), b"owned dummy executable; never loaded")

    def test_invalid_xml_or_nonunique_exact_utf8_value_never_writes_success_stamp(self):
        namespace = "http://schemas.microsoft.com/SMI/2019/WindowsSettings"
        fixtures = {
            "missing": "<assembly/>",
            "wrong_namespace": '<assembly><activeCodePage xmlns="urn:wrong">UTF-8</activeCodePage></assembly>',
            "wrong_element_case": f'<assembly xmlns="{namespace}"><activecodepage>UTF-8</activecodepage></assembly>',
            "wrong_value": f'<assembly xmlns="{namespace}"><activeCodePage>1252</activeCodePage></assembly>',
            "wrong_value_case": f'<assembly xmlns="{namespace}"><activeCodePage>utf-8</activeCodePage></assembly>',
            "duplicate": (
                f'<assembly xmlns="{namespace}"><activeCodePage>UTF-8</activeCodePage>'
                "<activeCodePage>UTF-8</activeCodePage></assembly>"
            ),
            "malformed": "<assembly>",
            "dtd": (
                '<!DOCTYPE assembly [<!ENTITY codepage "UTF-8">]>'
                f'<assembly xmlns="{namespace}"><activeCodePage>&codepage;</activeCodePage></assembly>'
            ),
        }
        for name, xml in fixtures.items():
            with self.subTest(fixture=name):
                result = self.verify(xml)
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertFalse(self.stamp.exists())

    def test_sdk_failure_is_nonzero_and_never_writes_success_stamp(self):
        self.write_mt(status=7)
        result = self.verify("<assembly/>")
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertFalse(self.stamp.exists())
        self.assertFalse(self.manifest.exists())
        self.assertIn(b"mt.exe failed with exit 7", result.stderr)


if __name__ == "__main__":
    unittest.main()
