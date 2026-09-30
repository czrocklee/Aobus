"""Tests for Nix shell reuse and the Linux portal's Python isolation."""

import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[2]
SHELL_INPUTS = ("shell.nix", "nixpkgs.json", "script/ao/toolchain.json", "script/ao/compiler-cache.json")


@unittest.skipUnless(sys.platform == "linux", "Linux Nix portal boundary")
class NativeNixPortalTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="nix portal ")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.checkout = self.root / "checkout"
        self.package = self.checkout / "script" / "ao"
        self.package.mkdir(parents=True)
        self.portal = self.checkout / "ao"
        shutil.copy2(PROJECT_ROOT / "ao", self.portal)
        for name in SHELL_INPUTS:
            shutil.copy2(PROJECT_ROOT / name, self.checkout / name)
        (self.package / "__init__.py").touch()
        (self.package / "__main__.py").write_text("print('portal entered')", encoding="utf-8")
        self.stubs = self.root / "bin"
        self.stubs.mkdir()
        self._stub("nix-shell", 'echo "nix-shell re-entered"')
        self.bash = shutil.which("bash")
        self.assertIsNotNone(self.bash)
        self.environment = {
            **os.environ,
            "PATH": f"{self.stubs}{os.pathsep}{os.environ['PATH']}",
            "NIX_BUILD_SHELL": self.bash,
            "AO_NIX_PORTAL_PYTHON": sys.executable,
        }
        for name in ("AO_IN_NIX_PORTAL", "AO_NIX_SHELL_FINGERPRINT", "AOBUS_NIX_UNSTRIPPED_GTK"):
            self.environment.pop(name, None)

    def _stub(self, name, body):
        path = self.stubs / name
        path.write_text(f"#!/bin/sh\n{body}\n", encoding="utf-8")
        path.chmod(0o755)

    def _fingerprint(self, gtk_mode="0"):
        digests = "".join(
            hashlib.sha256((self.checkout / name).read_bytes()).hexdigest() + "\n" for name in SHELL_INPUTS
        )
        return hashlib.sha256((digests + f"gtk-unstripped={gtk_mode}\n").encode("ascii")).hexdigest()

    def _run_portal(self, **environment):
        return subprocess.run(
            [self.bash, str(self.portal), "help"],
            env={**self.environment, **environment},
            capture_output=True,
            text=True,
            timeout=30,
        )

    def test_nix_reentry_selects_bash_without_a_nix_search_path(self):
        shell_package = self.root / "pinned-bash"
        self._stub("nix-build", f'printf "%s\\n" "{shell_package}"')
        self._stub("nix-shell", 'printf "%s\\n" "$NIX_BUILD_SHELL" "$@"')
        self.environment.pop("NIX_BUILD_SHELL")
        for selected_shell in (None, "/explicit/bash"):
            with self.subTest(selected_shell=selected_shell):
                environment = {"NIX_PATH": ""}
                if selected_shell is not None:
                    environment["NIX_BUILD_SHELL"] = selected_shell
                result = self._run_portal(**environment)

                self.assertEqual(result.returncode, 0, result.stderr)
                lines = result.stdout.splitlines()
                self.assertEqual(lines[0], selected_shell or str(shell_package / "bin/bash"))
                self.assertEqual(lines[1:3], [str(self.checkout / "shell.nix"), "--run"])
                self.assertIn("AO_IN_NIX_PORTAL=1", lines[3])

    def test_nix_reentry_reuses_only_a_shell_built_from_current_inputs(self):
        for fingerprint, expected in ((self._fingerprint(), "portal entered"), ("0" * 64, "nix-shell re-entered")):
            with self.subTest(expected=expected):
                result = self._run_portal(AO_NIX_SHELL_FINGERPRINT=fingerprint)

                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout.strip(), expected)

    def test_gtk_debug_mode_changes_invalidate_the_entered_shell(self):
        for entered, requested in (("0", "1"), ("1", "0"), ("1", "")):
            with self.subTest(entered=entered, requested=requested):
                result = self._run_portal(
                    AO_NIX_SHELL_FINGERPRINT=self._fingerprint(entered), AOBUS_NIX_UNSTRIPPED_GTK=requested
                )

                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout.strip(), "nix-shell re-entered")

    def test_matching_fingerprint_without_pinned_python_reenters_the_shell(self):
        self.environment.pop("AO_NIX_PORTAL_PYTHON")
        result = self._run_portal(AO_NIX_SHELL_FINGERPRINT=self._fingerprint())

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), "nix-shell re-entered")

    def test_reused_shell_ignores_ambient_python_home_path_and_interpreter(self):
        ambient = self.root / "ambient"
        ambient.mkdir()
        (ambient / "json.py").write_text("raise RuntimeError('ambient json was imported')", encoding="utf-8")
        self._stub("python3", 'echo "ambient interpreter was used"; exit 99')
        (self.package / "__main__.py").write_text(
            "import json, os, sys\n"
            "print(json.dumps({'python': sys.executable, 'home': os.environ.get('PYTHONHOME'), "
            "'path': os.environ.get('PYTHONPATH')}))\n",
            encoding="utf-8",
        )
        result = self._run_portal(
            AO_NIX_SHELL_FINGERPRINT=self._fingerprint(), PYTHONPATH=str(ambient), PYTHONHOME=str(ambient)
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            json.loads(result.stdout), {"python": sys.executable, "home": None, "path": str(self.checkout / "script")}
        )

    def test_nix_reentry_discards_ambient_python_paths_before_entering_the_shell(self):
        self._stub("nix-shell", 'printf "%s\\n" "${PYTHONHOME-unset}" "${PYTHONPATH-unset}"')
        result = self._run_portal(PYTHONHOME="/ambient/home", PYTHONPATH="/ambient/packages")

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.splitlines(), ["unset", "unset"])

    def test_bash_reuse_check_matches_nix_fingerprints_and_normalizes_gtk_mode(self):
        nix = shutil.which("nix-instantiate")
        self.assertIsNotNone(nix, "the Linux portal requires Nix")
        fingerprints = {}
        for mode in ("0", "1", "other"):
            with self.subTest(mode=mode):
                # Evaluation checks the actual Nix hash without building unstripped GTK.
                evaluated = subprocess.run(
                    [
                        nix,
                        str(PROJECT_ROOT / "shell.nix"),
                        "--eval",
                        "--strict",
                        "--json",
                        "-A",
                        "AO_NIX_SHELL_FINGERPRINT",
                    ],
                    env={**self.environment, "AOBUS_NIX_UNSTRIPPED_GTK": mode},
                    capture_output=True,
                    text=True,
                    timeout=30,
                )
                self.assertEqual(evaluated.returncode, 0, evaluated.stderr)
                fingerprints[mode] = json.loads(evaluated.stdout)
                result = self._run_portal(AO_NIX_SHELL_FINGERPRINT=fingerprints[mode], AOBUS_NIX_UNSTRIPPED_GTK=mode)

                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout.strip(), "portal entered")
        self.assertNotEqual(fingerprints["0"], fingerprints["1"])
        self.assertEqual(fingerprints["0"], fingerprints["other"])


class NixShellInputTest(unittest.TestCase):
    def test_nix_shell_fingerprint_covers_every_local_shell_input(self):
        shell = (PROJECT_ROOT / "shell.nix").read_text(encoding="utf-8")
        portal = (PROJECT_ROOT / "ao").read_text(encoding="utf-8")

        # Nix path literals are unquoted; drop comments and strings that mention paths.
        code = re.sub(r"(^|\s)#.*$", "", shell, flags=re.MULTILINE)
        interpolated = set(re.findall(r"\$\{\s*\./([\w./-]+)", code))
        code = re.sub(r"''.*?''|\"(?:[^\"\\]|\\.)*\"", "", code, flags=re.DOTALL)
        referenced = set(re.findall(r"(?<![\w/.])\./([\w./-]+)", code)) | interpolated
        declared = re.search(r"portalInputs = \[(.*?)\];", shell, re.DOTALL)
        hashed = re.search(r"for input in ([^;]+);", portal)

        self.assertIsNotNone(declared)
        self.assertIsNotNone(hashed)
        declared_inputs = [path.removeprefix("./") for path in declared.group(1).split()]
        self.assertEqual(set(declared_inputs), referenced)
        self.assertEqual(hashed.group(1).split(), declared_inputs)
        self.assertEqual(declared_inputs, list(SHELL_INPUTS))


if __name__ == "__main__":
    unittest.main()
