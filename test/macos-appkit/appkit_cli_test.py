"""Exercise AppKit's public path arguments before starting a GUI."""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from typing import ClassVar

from python.runfiles import runfiles


class AppKitPathArgumentsTest(unittest.TestCase):
    executables: ClassVar[dict[str, Path]]

    @classmethod
    def setUpClass(cls) -> None:
        resolver = runfiles.Create()
        assert resolver is not None
        cls.executables = {}
        for entrypoint, label in zip(("product", "smoke"), sys.argv[1:], strict=True):
            path = resolver.Rlocation(label)
            assert path is not None, f"No runfile for {label}"
            cls.executables[entrypoint] = Path(path).absolute()

    def run_in_fixture(self, root: Path, *command: str | Path) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            command,
            cwd=root,
            env={**os.environ, "HOME": str(root), "CFFIXED_USER_HOME": str(root)},
            capture_output=True,
            encoding="utf-8",
            errors="replace",
            timeout=10,
            check=False,
        )

    def run_from_removed_cwd(self, root: Path, executable: Path, *arguments: str) -> subprocess.CompletedProcess[str]:
        # Remove only the child's working directory; the absolute executable remains available.
        script = (
            "import os, sys\n"
            "os.mkdir('deleted')\n"
            "os.chdir('deleted')\n"
            "os.rmdir('../deleted')\n"
            "os.execv(sys.argv[1], sys.argv[1:])\n"
        )
        return self.run_in_fixture(root, sys.executable, "-c", script, executable, *arguments)

    def require_usage_error(self, result: subprocess.CompletedProcess[str], diagnostic: str) -> None:
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn(diagnostic, result.stderr)

    def test_path_options_reject_missing_empty_and_dash_prefixed_values(self) -> None:
        for entrypoint in self.executables:
            for option, other in (("--library", "--state-root"), ("--state-root", "--library")):
                option_values = ["", other, "--typo", "-x", "-"]
                if entrypoint == "smoke":
                    option_values.append("--scenario")
                # The final bad option guards against GUI startup if a parser swallows the value.
                cases = [[other, "unused", option]] + [
                    [option, value, "--unexpected-argument"] for value in option_values
                ]

                for arguments in cases:
                    with (
                        self.subTest(entrypoint=entrypoint, arguments=arguments),
                        tempfile.TemporaryDirectory(dir=os.environ["TEST_TMPDIR"]) as scratch,
                    ):
                        root = Path(scratch)
                        result = self.run_in_fixture(root, self.executables[entrypoint], *arguments)
                        self.require_usage_error(result, f"Startup argument '{option}' requires a path")
                        self.assertEqual(list(root.iterdir()), [], "Malformed arguments must not create fixture paths")

    def test_private_scan_flag_cannot_be_removed_from_public_path_slot(self) -> None:
        for entrypoint, executable in self.executables.items():
            with (
                self.subTest(entrypoint=entrypoint),
                tempfile.TemporaryDirectory(dir=os.environ["TEST_TMPDIR"]) as scratch,
            ):
                root = Path(scratch)
                # Otherwise valid successor flags could hide the bad operand by consuming it.
                result = self.run_in_fixture(
                    root,
                    executable,
                    "--state-root",
                    "--scan-after-open",
                    "unused",
                    "--aobus-successor",
                    "--library-root",
                    str(root / "successor"),
                    "--unexpected-argument",
                )
                self.require_usage_error(result, "Startup argument '--state-root' requires a path")
                self.assertEqual(list(root.iterdir()), [])

    def test_library_option_rejects_duplicate_and_successor_conflict(self) -> None:
        for entrypoint, executable in self.executables.items():
            with tempfile.TemporaryDirectory(dir=os.environ["TEST_TMPDIR"]) as scratch:
                root = Path(scratch)
                for arguments, reason in (
                    (["--library", "other"], "may only be specified once"),
                    (
                        ["--aobus-successor", "--library-root", str(root / "successor")],
                        "cannot be combined with a successor request",
                    ),
                ):
                    with self.subTest(entrypoint=entrypoint, reason=reason):
                        result = self.run_in_fixture(
                            root, executable, "--library", "music", *arguments, "--unexpected-argument"
                        )
                        self.require_usage_error(result, f"Startup argument '--library' {reason}")
                        self.assertEqual(list(root.iterdir()), [])

    def test_smoke_scenario_requires_an_operand(self) -> None:
        with tempfile.TemporaryDirectory(dir=os.environ["TEST_TMPDIR"]) as scratch:
            root = Path(scratch)
            # Overlap keeps this headless even if a regression silently applies the default scenario.
            result = self.run_in_fixture(
                root, self.executables["smoke"], "--library", "unused", "--state-root", "unused", "--scenario"
            )
            self.require_usage_error(result, "Startup argument '--scenario' requires a value")
            self.assertEqual(list(root.iterdir()), [])

    def test_rejects_unknown_option_and_reserves_scenario_for_smoke(self) -> None:
        for entrypoint, arguments in (
            ("product", ["--unknown"]),
            ("smoke", ["--unknown"]),
            ("product", ["--scenario", "desktop"]),
        ):
            with (
                self.subTest(entrypoint=entrypoint, arguments=arguments),
                tempfile.TemporaryDirectory(dir=os.environ["TEST_TMPDIR"]) as scratch,
            ):
                root = Path(scratch)
                # If the unexpected option were accepted, resolving the relative state path would
                # still fail before GUI startup, with a different diagnostic.
                result = self.run_from_removed_cwd(
                    root, self.executables[entrypoint], *arguments, "--state-root", "relative"
                )
                self.require_usage_error(result, "Invalid arguments")
                self.assertEqual(list(root.iterdir()), [])

    def test_relative_path_reports_missing_working_directory(self) -> None:
        for entrypoint, executable in self.executables.items():
            for option in ("--library", "--state-root"):
                with (
                    self.subTest(entrypoint=entrypoint, option=option),
                    tempfile.TemporaryDirectory(dir=os.environ["TEST_TMPDIR"]) as scratch,
                ):
                    root = Path(scratch)
                    # './' makes a dash-prefixed name a path operand on both entrypoints.
                    operand = "./--typo" if option == "--library" else "./-foo"
                    result = self.run_from_removed_cwd(root, executable, option, operand, "--unexpected-argument")
                    self.require_usage_error(result, f"Cannot resolve path for startup argument '{option}'")
                    self.assertEqual(list(root.iterdir()), [])

    def test_smoke_resolves_relative_paths_before_checking_fixture_overlap(self) -> None:
        for name in ("--typo", "-foo"):
            with self.subTest(name=name), tempfile.TemporaryDirectory(dir=os.environ["TEST_TMPDIR"]) as scratch:
                root = Path(scratch)
                (root / "subdir").mkdir()
                (root / "not-a-directory").touch()
                result = self.run_in_fixture(
                    root,
                    self.executables["smoke"],
                    "--library",
                    f"./{name}",
                    "--state-root",
                    "not-a-directory",
                    "--state-root",
                    f"subdir/../{name}",
                )
                # The last state root wins; both path operands are valid, but the fixtures overlap.
                self.require_usage_error(result, "--library and --state-root must not equal or contain one another")
                self.assertFalse((root / name).exists())


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
