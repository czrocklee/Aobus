"""Regression coverage for explicit validation scopes at the portal boundary."""

import contextlib
import io
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from ao.__main__ import make_parser
from ao.core.paths import PROJECT_ROOT


class ValidationScopeTest(unittest.TestCase):
    def run_portal(self, arguments, *, module="ao"):
        env = {**os.environ, "PYTHONPATH": str(PROJECT_ROOT / "script")}
        env.pop("AOBUS_PREFLIGHT_SCOPE", None)
        return subprocess.run(
            [sys.executable, "-m", module, *arguments],
            cwd=PROJECT_ROOT,
            env=env,
            capture_output=True,
            text=True,
            timeout=30,
        )

    def test_missing_explicit_paths_fail_even_without_applicable_suffixes(self):
        with tempfile.TemporaryDirectory() as temporary:
            for command in ("format", "hygiene", "test-audit", "name-audit"):
                for filename in ("MissingTest.cpp", "missing.txt", "missing-folder"):
                    missing = str(Path(temporary) / filename)
                    with self.subTest(command=command, filename=filename):
                        result = self.run_portal([command, missing])
                        self.assertNotEqual(result.returncode, 0, result.stdout)
                        self.assertIn(missing, result.stderr)

    def test_missing_folder_fails_before_native_preflight_or_validation(self):
        with tempfile.TemporaryDirectory() as temporary:
            missing = str(Path(temporary) / "missing-folder")
            for command in ("format", "hygiene"):
                for module in ("ao", "ao.core.buildenv"):
                    with self.subTest(command=command, module=module):
                        result = self.run_portal([command, "--folder", missing], module=module)
                        self.assertNotEqual(result.returncode, 0, result.stdout)
                        self.assertIn(missing, result.stderr)

    def test_mixed_valid_and_missing_paths_fail_before_formatting(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "Example.cpp"
            original = "int   answer=42;\n"
            source.write_text(original, encoding="utf-8")
            missing = str(Path(temporary) / "missing.txt")
            for command in ("format", "hygiene", "test-audit", "name-audit"):
                with self.subTest(command=command):
                    source.write_text(original, encoding="utf-8")
                    result = self.run_portal([command, str(source), missing])
                    self.assertNotEqual(result.returncode, 0, result.stdout)
                    self.assertIn(missing, result.stderr)
                    self.assertEqual(source.read_text(encoding="utf-8"), original)

    def test_existing_empty_and_irrelevant_scopes_succeed(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            empty = root / "empty"
            empty.mkdir()
            irrelevant = root / "notes.txt"
            irrelevant.write_text("No source files.\n", encoding="utf-8")
            for command in ("format", "hygiene", "test-audit", "name-audit"):
                scopes = [[str(irrelevant)]]
                scopes.append(["--folder", str(empty)] if command in ("format", "hygiene") else [str(empty)])
                for scope in scopes:
                    with self.subTest(command=command, scope=scope):
                        result = self.run_portal([command, *scope])
                        self.assertEqual(result.returncode, 0, result.stderr)

    def test_prepared_empty_scope_does_not_hide_removed_explicit_paths(self):
        with tempfile.TemporaryDirectory() as temporary:
            missing = str(Path(temporary) / "removed.py")
            for command in ("format", "hygiene"):
                with self.subTest(command=command):
                    args = make_parser().parse_args([command, missing])
                    args._resolved_sources = []
                    stderr = io.StringIO()
                    with contextlib.redirect_stderr(stderr), self.assertRaises(SystemExit) as raised:
                        args.func(args)
                    self.assertNotEqual(raised.exception.code, 0)
                    self.assertIn(missing, stderr.getvalue())


if __name__ == "__main__":
    unittest.main()
