"""Real CMake fixtures for UIModel organization root and layout checks."""

import subprocess
import tempfile
import unittest
from pathlib import Path

from cmake_support import cmake_executable

from ao.core.paths import PROJECT_ROOT


class UimodelOrganizationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cmake = cmake_executable()

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus organization ")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.roots = {name: self.root / name for name in ("PUBLIC_ROOT", "SOURCE_ROOT", "TEST_ROOT")}
        for root in self.roots.values():
            root.mkdir()

    def run_audit(self, roots):
        return subprocess.run(
            [
                self.cmake,
                *(f"-D{name}={path}" for name, path in roots.items()),
                "-P",
                str(PROJECT_ROOT / "cmake/AssertUimodelOrganization.cmake"),
            ],
            capture_output=True,
            text=True,
            check=False,
            cwd=self.root,
        )

    def test_existing_empty_roots_are_valid(self):
        result = self.run_audit(self.roots)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_falsey_and_notfound_named_roots_are_valid_directories(self):
        for name in self.roots:
            for dirname in ("OFF", "0", "ROOT-NOTFOUND"):
                with self.subTest(root=name, dirname=dirname):
                    root = self.root / dirname
                    (root / "field").mkdir(parents=True, exist_ok=True)
                    (root / "field/Example.h").write_text("namespace ao::uimodel {}\n", encoding="utf-8")
                    result = self.run_audit({**self.roots, name: Path(dirname)})
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

            absolute_root = (self.root / f"{name}-NOTFOUND").resolve()
            absolute_root.mkdir()
            result = self.run_audit({**self.roots, name: absolute_root})
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_every_root_must_be_a_directory(self):
        regular_file = self.root / "regular file"
        regular_file.write_text("not a directory", encoding="utf-8")
        for name in self.roots:
            for path in (self.root / "missing directory", regular_file):
                with self.subTest(root=name, path=path):
                    result = self.run_audit({**self.roots, name: path})
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn(f"{name} is not a directory", result.stderr)
                    self.assertIn(path.as_posix(), " ".join(result.stderr.replace("\\", "/").split()))

    def test_every_root_must_be_specified_and_nonempty(self):
        for name in self.roots:
            for omitted in (False, True):
                with self.subTest(root=name, omitted=omitted):
                    roots = {key: path for key, path in self.roots.items() if key != name}
                    if not omitted:
                        roots[name] = ""
                    result = self.run_audit(roots)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn(f"{name} not specified", result.stderr)

    def test_valid_roots_still_enforce_layout_and_namespaces(self):
        for root in self.roots.values():
            path = root / "field/Example.h"
            path.parent.mkdir()
            path.write_text("namespace ao::uimodel {}\n", encoding="utf-8")
        result = self.run_audit(self.roots)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        for name in ("PUBLIC_ROOT", "TEST_ROOT"):
            with self.subTest(root=name):
                path = self.roots[name] / "field/Example.h"
                path.write_text("namespace ao::uimodel::feature {}\n", encoding="utf-8")
                result = self.run_audit(self.roots)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("namespace", result.stderr)
                path.write_text("namespace ao::uimodel {}\n", encoding="utf-8")
        invalid = self.roots["SOURCE_ROOT"] / "Outside.cpp"
        invalid.write_text("", encoding="utf-8")
        result = self.run_audit(self.roots)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("outside the final UIModel folder layout", " ".join(result.stderr.split()))


if __name__ == "__main__":
    unittest.main()
