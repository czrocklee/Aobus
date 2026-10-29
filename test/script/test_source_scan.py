"""Raw source inventory shared by the two CMake guardrail entry points."""

import json
import subprocess
import tempfile
import unittest
from pathlib import Path

from cmake_support import cmake_executable

from ao.core.paths import PROJECT_ROOT


class SourceScanTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cmake = cmake_executable()

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus source inventory ")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.script = self.root / "scan.cmake"
        self.script.write_text(
            f'include("{(PROJECT_ROOT / "cmake/AoSourceCode.cmake").as_posix()}")\n'
            'ao_raw_source_files(_files "${ROOT}" "Fixture root is missing: ${ROOT}")\n'
            'file(WRITE "inventory.txt" "${_files}")\n',
            encoding="utf-8",
        )

    def run_cmake(self, arguments):
        return subprocess.run(
            [self.cmake, *arguments],
            cwd=self.root,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            check=False,
        )

    def scan(self, root):
        return self.run_cmake([f"-DROOT={root}", "-P", str(self.script)])

    def test_directory_inventory_keeps_broad_suffixes_and_ignores_other_files(self):
        directory = self.root / "source space"
        nested = directory / "nested"
        header_lookalike = directory / "LooksLikeHeader.h"
        nested.mkdir(parents=True)
        header_lookalike.mkdir()
        supported = ("h", "hh", "hpp", "hxx", "c", "cc", "cpp", "cxx", "mm", "inl", "ipp", "def")
        for suffix in (*supported, "c++", "m", "txt"):
            (directory / f"File.{suffix}").touch()
        (nested / "Nested.cpp").touch()
        (header_lookalike / "Inside.cpp").touch()
        result = self.scan(directory)
        self.assertEqual(result.returncode, 0, result.stderr)
        paths = [Path(path) for path in (self.root / "inventory.txt").read_text().split(";")]
        expected = [directory / f"File.{suffix}" for suffix in supported]
        expected += [header_lookalike / "Inside.cpp", nested / "Nested.cpp"]
        self.assertEqual(paths, sorted(expected))

    def test_explicit_file_root_is_kept_regardless_of_suffix(self):
        path = self.root / "Explicit File.txt"
        path.touch()
        result = self.scan(path)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(Path((self.root / "inventory.txt").read_text()), path)

    def test_empty_falsey_named_directories_and_missing_root_diagnostic(self):
        for dirname in ("OFF", "0", "ROOT-NOTFOUND"):
            with self.subTest(dirname=dirname):
                root = self.root / dirname
                root.mkdir()
                result = self.scan(root)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual((self.root / "inventory.txt").read_text(), "")
        missing = self.root / "missing-root"
        result = self.scan(missing)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Fixture root is missing:", result.stderr)
        self.assertIn(" ".join(missing.as_posix().split()), " ".join(result.stderr.replace("\\", "/").split()))

    def test_architecture_configuration_does_not_load_raw_scan_module(self):
        project = self.root / "project"
        (project / "build").mkdir(parents=True)
        (project / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.30)\n"
            "project(SourceScanFixture LANGUAGES NONE)\n"
            f'include("{(PROJECT_ROOT / "app/cmake/ArchitectureAudit.cmake").as_posix()}")\n'
            "aobus_add_architecture_audit()\n"
            "if(NOT TARGET ao_application_architecture_audit)\n"
            '  message(FATAL_ERROR "Architecture audit target is not registered")\n'
            "endif()\n",
            encoding="utf-8",
        )
        result = self.run_cmake(
            [
                "--trace",
                "--trace-format=json-v1",
                f"--trace-source={PROJECT_ROOT / 'cmake/AoSourceCode.cmake'}",
                "-S",
                str(project),
                "-B",
                str(project / "build"),
            ]
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        # Includes become implicit configure dependencies, not entries in the
        # explicit CMAKE_CONFIGURE_DEPENDS directory property. Observe loading.
        events = [json.loads(line) for line in result.stderr.splitlines() if line.startswith("{")]
        self.assertFalse(any("cmd" in event for event in events), result.stderr)
