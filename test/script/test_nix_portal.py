"""Tests for Nix shell reuse and the Linux portal's Python isolation."""

import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[2]
SHELL_INPUTS = (
    "shell.nix",
    "nixpkgs.json",
    "script/ao/toolchain.json",
    "script/ao/bazel-toolchain.json",
    "script/ao/buildifier.json",
)
# Flags and search paths that nix-shell setup hooks extend for each target role rather than replace.
INHERITED_SEARCH_STATE = (
    "NIX_CFLAGS_COMPILE",
    "NIX_CFLAGS_COMPILE_FOR_TARGET",
    "NIX_CFLAGS_LINK",
    "NIX_LDFLAGS",
    "NIX_LDFLAGS_BEFORE",
    "NIX_LDFLAGS_FOR_BUILD",
    "NIX_LDFLAGS_FOR_TARGET",
    "PKG_CONFIG_PATH",
    "PKG_CONFIG_PATH_FOR_TARGET",
    "CMAKE_PREFIX_PATH",
    "NIXPKGS_CMAKE_PREFIX_PATH",
    "CMAKE_INCLUDE_PATH",
    "CMAKE_LIBRARY_PATH",
    "CPATH",
    "C_INCLUDE_PATH",
    "CPLUS_INCLUDE_PATH",
    "LIBRARY_PATH",
)


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
            "AO_NIX_SHELL_INPUTS": " ".join(SHELL_INPUTS),
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

    def _eval_shell_fingerprint(self, checkout):
        # Evaluation forces the shell's tool contracts without building anything.
        nix = shutil.which("nix-instantiate")
        self.assertIsNotNone(nix, "the Linux portal requires Nix")
        return subprocess.run(
            [
                nix,
                str(checkout / "shell.nix"),
                "--eval",
                "--strict",
                "--json",
                "-A",
                "AO_NIX_SHELL_FINGERPRINT",
            ],
            env={**self.environment, "AOBUS_NIX_UNSTRIPPED_GTK": "0"},
            capture_output=True,
            text=True,
            timeout=30,
        )

    def _run_portal(self, *, without_compgen=False, **environment):
        if without_compgen:
            # Disable only this child's optional builtin, even when the host's Bash has it.
            command = [
                self.bash,
                "-c",
                'enable -n compgen 2>/dev/null || :; source "$1" "${@:2}"',
                "bash-without-compgen",
                str(self.portal),
                "help",
            ]
        else:
            command = [self.bash, str(self.portal), "help"]
        return subprocess.run(
            command,
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

    def test_buildifier_pin_changes_invalidate_the_entered_shell(self):
        fingerprint = self._fingerprint()
        manifest = self.checkout / "script/ao/buildifier.json"
        manifest.write_text(manifest.read_text(encoding="utf-8") + "\n", encoding="utf-8")
        result = self._run_portal(AO_NIX_SHELL_FINGERPRINT=fingerprint)

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), "nix-shell re-entered")

    def test_native_pin_change_invalidates_the_entered_shell(self):
        fingerprint = self._fingerprint()
        manifest = self.checkout / "nixpkgs.json"
        pin = json.loads(manifest.read_text(encoding="utf-8"))
        for key in ("owner", "repo", "ref", "rev", "sha256"):
            self.assertIn(key, pin, "the native pin must carry a complete source identity")
        pin["rev"] = "0123456789abcdef0123456789abcdef01234567"
        pin["sha256"] = "1" * 52
        manifest.write_text(json.dumps(pin, indent=2) + "\n", encoding="utf-8")
        result = self._run_portal(AO_NIX_SHELL_FINGERPRINT=fingerprint)

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), "nix-shell re-entered")

    def test_gtk_debug_mode_changes_invalidate_the_entered_shell(self):
        # The fingerprint-mismatch mechanism is direction-independent, and
        # gtk-mode normalization is proven by the nix-fingerprint test below.
        result = self._run_portal(AO_NIX_SHELL_FINGERPRINT=self._fingerprint("0"), AOBUS_NIX_UNSTRIPPED_GTK="1")

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

    def test_nix_reentry_discards_inherited_dependency_search_state(self):
        inherited = {name: "/stale" for name in INHERITED_SEARCH_STATE}
        self._stub("nix-shell", "env -0")
        kept = {
            "AOBUS_UNRELATED_SETTING": "kept",
            "CMAKE_BUILD_PARALLEL_LEVEL": "2",
            "CMAKE_LIBRARY_PATH_SUFFIX": "kept",
            "NIX_CFLAGS": "kept",
            "NIXPKGS_UNRELATED": "kept",
            "PKG_CONFIG_LIBDIR": "kept",
        }
        # A host shell carries no fingerprint; a shell entered under an earlier pin carries a stale one.
        for without_compgen in (False, True):
            for fingerprint in (None, "0" * 64):
                with self.subTest(without_compgen=without_compgen, fingerprint=fingerprint):
                    environment = {**inherited, **kept}
                    if fingerprint is not None:
                        environment["AO_NIX_SHELL_FINGERPRINT"] = fingerprint
                    result = self._run_portal(without_compgen=without_compgen, **environment)

                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(result.stderr, "")
                    entered = dict(entry.split("=", 1) for entry in result.stdout.split("\0") if entry)
                    self.assertEqual(set(inherited) & set(entered), set())
                    self.assertEqual({name: entered[name] for name in kept}, kept)

    def test_nix_reentry_spares_same_named_shell_functions_of_absent_variables(self):
        # Inspect the functions in Bash: a /bin/sh probe may be dash, which
        # discards BASH_FUNC_* entries before env can observe them.
        probe = self.stubs / "nix-shell"
        probe.write_text(
            f"#!{self.bash}\n"
            "declare -F CPATH CMAKE_PREFIX_PATH >/dev/null || exit 1\n"
            "CPATH\nCMAKE_PREFIX_PATH\nenv -0\n",
            encoding="utf-8",
        )
        probe.chmod(0o755)
        # Bash exports a function as a BASH_FUNC_<name>%% environment entry; an
        # absent search-path variable with a same-named exported function must
        # not lose the function to the stale-variable cleanup.
        function_names = ("CPATH", "CMAKE_PREFIX_PATH")
        for name in function_names:
            self.environment.pop(name, None)
        exported_functions = {f"BASH_FUNC_{name}%%": "() { echo portal-function\n}\n" for name in function_names}
        stale = {"NIX_CFLAGS_COMPILE": "/stale", "LIBRARY_PATH": "/stale"}

        for without_compgen in (False, True):
            with self.subTest(without_compgen=without_compgen):
                result = self._run_portal(without_compgen=without_compgen, **exported_functions, **stale)

                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stderr, "")
                function_output = "portal-function\nportal-function\n"
                self.assertTrue(result.stdout.startswith(function_output), result.stdout)
                entered = dict(
                    entry.split("=", 1) for entry in result.stdout.removeprefix(function_output).split("\0") if entry
                )
                self.assertEqual({name: entered.get(name) for name in stale}, {name: None for name in stale})
                self.assertTrue(set(function_names).isdisjoint(entered))

    def test_reused_shell_keeps_its_dependency_search_state(self):
        (self.package / "__main__.py").write_text(
            "import json, os\n"
            f"print(json.dumps({{name: os.environ.get(name) for name in {list(INHERITED_SEARCH_STATE)!r}}}))\n",
            encoding="utf-8",
        )
        entered = {name: f"/pinned/{name}" for name in INHERITED_SEARCH_STATE}
        result = self._run_portal(AO_NIX_SHELL_FINGERPRINT=self._fingerprint(), **entered)

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout), entered)

    def test_bash_reuse_check_matches_nix_fingerprints_and_normalizes_gtk_mode(self):
        nix = shutil.which("nix-instantiate")
        self.assertIsNotNone(nix, "the Linux portal requires Nix")
        fingerprints = {}
        for mode in ("0", "1", "other"):
            with self.subTest(mode=mode):
                # Evaluation checks the actual Nix hash and its exported input
                # list without building unstripped GTK.
                exported = {}
                for name in ("AO_NIX_SHELL_FINGERPRINT", "AO_NIX_SHELL_INPUTS"):
                    evaluated = subprocess.run(
                        [nix, str(PROJECT_ROOT / "shell.nix"), "--eval", "--strict", "--json", "-A", name],
                        env={**self.environment, "AOBUS_NIX_UNSTRIPPED_GTK": mode},
                        capture_output=True,
                        text=True,
                        timeout=30,
                    )
                    self.assertEqual(evaluated.returncode, 0, evaluated.stderr)
                    exported[name] = json.loads(evaluated.stdout)
                fingerprints[mode] = exported["AO_NIX_SHELL_FINGERPRINT"]
                result = self._run_portal(**exported, AOBUS_NIX_UNSTRIPPED_GTK=mode)

                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout.strip(), "portal entered")
        self.assertNotEqual(fingerprints["0"], fingerprints["1"])
        self.assertEqual(fingerprints["0"], fingerprints["other"])

    def test_toolchain_contract_mismatch_fails_shell_evaluation(self):
        # A fixture checkout proves the exact tool version assertions stay
        # load-bearing: the same inputs evaluate, then fail once the pinned
        # Python contract no longer matches the pinned package set.
        fixture = self.root / "toolchain-contract"
        fixture.mkdir()
        for name in SHELL_INPUTS:
            destination = fixture / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(PROJECT_ROOT / name, destination)
        intact = self._eval_shell_fingerprint(fixture)
        self.assertEqual(intact.returncode, 0, intact.stderr)

        toolchain = fixture / "script/ao/toolchain.json"
        contract = json.loads(toolchain.read_text(encoding="utf-8"))
        contract["python"] = "0.0.0"
        toolchain.write_text(json.dumps(contract, indent=2) + "\n", encoding="utf-8")

        evaluated = self._eval_shell_fingerprint(fixture)
        self.assertNotEqual(evaluated.returncode, 0)
        self.assertIn("Aobus requires Python 0.0.0", evaluated.stderr)
        self.assertIn("but pinned Nixpkgs resolves", evaluated.stderr)


if __name__ == "__main__":
    unittest.main()
