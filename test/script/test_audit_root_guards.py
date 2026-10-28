"""Real CMake fixtures for the audit root guard contract shared by the leaf scripts."""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from ao.core.paths import PROJECT_ROOT

ROOT_SCRIPTS = {
    "AssertGtkLeafCapabilities.cmake": "reaches a high-authority dependency",
    "AssertWinUiLeafCapabilities.cmake": "reaches a high-authority dependency",
    "AssertWinUiEventRevokers.cmake": "holds a raw event_token",
    "AssertWinUiStateSubscriptions.cmake": "restores the old 'onShellStateChanged' push contract",
}
# One content violation per script proves an accepted root is actually scanned.
ROOT_VIOLATIONS = {
    "AssertGtkLeafCapabilities.cmake": ("app/Leaf.h", "AppRuntime* reach;\n"),
    "AssertWinUiLeafCapabilities.cmake": ("app/Leaf.h", "AppRuntime* reach;\n"),
    "AssertWinUiEventRevokers.cmake": ("app/Leaf.h", "winrt::event_token token;\n"),
    "AssertWinUiStateSubscriptions.cmake": ("app/Leaf.h", "onShellStateChanged();\n"),
}
FRONTEND_VIOLATION = ("field/Schema.h", "Gtk::Widget* widget;\n", "speaks a frontend's vocabulary")


def normalized(output):
    return " ".join(output.replace("\\", "/").split())


class AuditRootGuardTest(unittest.TestCase):
    def run_root_script(self, cwd, script, root=None):
        cmake = shutil.which("cmake")
        if cmake is None:
            self.skipTest("cmake is not on PATH")
        arguments = [cmake]
        if root is not None:
            arguments.append(f"-DROOT={root}")
        arguments += ["-P", str(PROJECT_ROOT / "cmake" / script)]
        return subprocess.run(arguments, capture_output=True, text=True, check=False, cwd=cwd)

    def test_existing_empty_roots_are_valid(self):
        for script in ROOT_SCRIPTS:
            with self.subTest(script=script), tempfile.TemporaryDirectory(prefix="aobus audit roots ") as temporary:
                root = Path(temporary).resolve()
                empty = root / "empty"
                empty.mkdir()
                result = self.run_root_script(root, script, empty)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_falsey_named_roots_are_accepted_and_scanned(self):
        for script in ROOT_SCRIPTS:
            with self.subTest(script=script), tempfile.TemporaryDirectory(prefix="aobus audit roots ") as temporary:
                root = Path(temporary).resolve()
                for dirname in ("OFF", "0", "ROOT-NOTFOUND"):
                    with self.subTest(script=script, dirname=dirname):
                        directory = root / dirname
                        relative, source = ROOT_VIOLATIONS[script]
                        path = directory / relative
                        path.parent.mkdir(parents=True, exist_ok=True)
                        path.write_text(source, encoding="utf-8")
                        # A relative root resolves against the script's working directory.
                        result = self.run_root_script(root, script, dirname)
                        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                        self.assertIn(ROOT_SCRIPTS[script], normalized(result.stderr))
                absolute = (root / f"{script.split('.')[0]}-NOTFOUND").resolve()
                relative, source = ROOT_VIOLATIONS[script]
                path = absolute / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(source, encoding="utf-8")
                result = self.run_root_script(root, script, absolute)
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn(ROOT_SCRIPTS[script], normalized(result.stderr))

    def test_every_root_must_be_a_directory(self):
        for script in ROOT_SCRIPTS:
            with self.subTest(script=script), tempfile.TemporaryDirectory(prefix="aobus audit roots ") as temporary:
                root = Path(temporary).resolve()
                regular_file = root / "regular file"
                regular_file.write_text("not a directory", encoding="utf-8")
                for path in (root / "missing directory", regular_file):
                    with self.subTest(script=script, path=path):
                        result = self.run_root_script(root, script, path)
                        self.assertNotEqual(result.returncode, 0)
                        self.assertIn("is not a directory", result.stderr)
                        self.assertIn(path.as_posix(), normalized(result.stderr))

    def test_every_root_must_be_specified_and_nonempty(self):
        for script in ROOT_SCRIPTS:
            for omitted in (False, True):
                with self.subTest(script=script, omitted=omitted), tempfile.TemporaryDirectory() as temporary:
                    root = Path(temporary)
                    result = self.run_root_script(root, script, None if omitted else "")
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("ROOT not specified", result.stderr)

    def test_valid_roots_still_enforce_their_rules(self):
        for script, (relative, source) in ROOT_VIOLATIONS.items():
            with self.subTest(script=script), tempfile.TemporaryDirectory(prefix="aobus audit roots ") as temporary:
                root = Path(temporary).resolve()
                path = root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(source, encoding="utf-8")
                result = self.run_root_script(root, script, root)
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn(ROOT_SCRIPTS[script], normalized(result.stderr))


class FrontendNeutralityRootGuardTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus neutrality roots ")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.roots = {name: self.root / name for name in ("PUBLIC_ROOT", "SOURCE_ROOT", "TEST_ROOT")}
        for root in self.roots.values():
            root.mkdir()

    def run_audit(self, roots):
        cmake = shutil.which("cmake")
        if cmake is None:
            self.skipTest("cmake is not on PATH")
        return subprocess.run(
            [
                cmake,
                *(f"-D{name}={path}" for name, path in roots.items()),
                "-P",
                str(PROJECT_ROOT / "cmake/AssertUimodelFrontendNeutrality.cmake"),
            ],
            capture_output=True,
            text=True,
            check=False,
            cwd=self.root,
        )

    def test_existing_empty_roots_are_valid(self):
        result = self.run_audit(self.roots)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_falsey_named_roots_are_accepted_and_scanned(self):
        for name in self.roots:
            for dirname in ("OFF", "0", "ROOT-NOTFOUND"):
                with self.subTest(root=name, dirname=dirname):
                    directory = self.root / dirname
                    relative, source, message = FRONTEND_VIOLATION
                    path = directory / relative
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_text(source, encoding="utf-8")
                    result = self.run_audit({**self.roots, name: Path(dirname)})
                    self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertIn(message, normalized(result.stderr))

            absolute_root = (self.root / f"{name}-NOTFOUND").resolve()
            absolute_root.mkdir(exist_ok=True)
            relative, source, message = FRONTEND_VIOLATION
            path = absolute_root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(source, encoding="utf-8")
            result = self.run_audit({**self.roots, name: absolute_root})
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn(message, normalized(result.stderr))

    def test_every_root_must_be_a_directory(self):
        regular_file = self.root / "regular file"
        regular_file.write_text("not a directory", encoding="utf-8")
        for name in self.roots:
            for path in (self.root / "missing directory", regular_file):
                with self.subTest(root=name, path=path):
                    result = self.run_audit({**self.roots, name: path})
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("is not a directory", result.stderr)
                    self.assertIn(path.as_posix(), normalized(result.stderr))

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

    def test_valid_roots_still_enforce_frontend_neutrality(self):
        path = self.roots["SOURCE_ROOT"] / FRONTEND_VIOLATION[0]
        path.parent.mkdir()
        path.write_text(FRONTEND_VIOLATION[1], encoding="utf-8")
        result = self.run_audit(self.roots)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(FRONTEND_VIOLATION[2], normalized(result.stderr))


if __name__ == "__main__":
    unittest.main()
