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

    def run_in_fixture(self, root: Path, *command: str | bytes | Path) -> subprocess.CompletedProcess[str]:
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

    def run_smoke_fixture_preparation(
        self, root: Path, library: Path | bytes, state: Path | bytes
    ) -> subprocess.CompletedProcess[str]:
        # Stop admitted inputs at the first WAV write, before the raw smoke can open a GUI.
        # Directory creation and the creation of an empty WAV are still observable.
        script = (
            "import os, resource, signal, sys\n"
            "signal.signal(signal.SIGXFSZ, signal.SIG_IGN)\n"
            "resource.setrlimit(resource.RLIMIT_FSIZE, (0, 0))\n"
            "os.execv(sys.argv[1], sys.argv[1:])\n"
        )
        return self.run_in_fixture(
            root,
            sys.executable,
            "-c",
            script,
            self.executables["smoke"],
            "--scenario",
            "initial-index",
            "--library",
            library,
            "--state-root",
            state,
        )

    def require_fixture_rejected_before_writes(
        self,
        root: Path,
        library: Path | bytes,
        state: Path | bytes,
        diagnostic: str = "--library and --state-root must not equal or contain one another",
    ) -> None:
        before = sorted(root.rglob("*"))
        result = self.run_smoke_fixture_preparation(root, library, state)
        self.assertEqual(sorted(root.rglob("*")), before, result.stdout + result.stderr)
        self.require_usage_error(result, diagnostic)

    def require_fixture_roots_admitted(self, root: Path, library: Path, state: Path) -> None:
        result = self.run_smoke_fixture_preparation(root, library, state)
        self.require_usage_error(result, "Cannot write AppKit fixture track")
        self.assertEqual(len(list(library.glob("*.wav"))), 1)
        self.assertEqual(list(state.iterdir()), [])

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

    def test_smoke_distinguishes_case_aliases_from_distinct_fixture_roots(self) -> None:
        for first_path, second_path in (
            ("Fixture", "FIXTURE"),
            ("Fixture", "FIXTURE/pending"),
            ("Fixture/new", "FIXTURE/new/pending"),
            # The spellings differ in a component that does not exist in either path.
            ("Fixture/future", "Fixture/FUTURE"),
            ("Fixture/future", "Fixture/FUTURE/pending"),
        ):
            for reverse_roles in (False, True):
                with tempfile.TemporaryDirectory(dir=os.environ["TEST_TMPDIR"]) as scratch:
                    root = Path(scratch)
                    first = root / "Fixture"
                    alternate = root / "FIXTURE"
                    first.mkdir()
                    if not alternate.exists():
                        alternate.mkdir()
                    case_aliased = first.samefile(alternate)
                    library, state = root / first_path, root / second_path
                    if reverse_roles:
                        library, state = state, library
                    with self.subTest(library=library, state=state, case_aliased=case_aliased):
                        if case_aliased:
                            self.require_fixture_rejected_before_writes(root, library, state)
                        else:
                            self.require_fixture_roots_admitted(root, library, state)

    def test_smoke_requires_existing_identities_for_ambiguous_non_ascii_names(self) -> None:
        for first_name, second_name in (("Caf\u00e9", "Cafe\u0301"), ("\U00010570", "\U00010597")):
            with (
                self.subTest(first_name=first_name, second_name=second_name),
                tempfile.TemporaryDirectory(dir=os.environ["TEST_TMPDIR"]) as scratch,
            ):
                root = Path(scratch)
                probe = root / "identity-probe"
                first, second = probe / first_name, probe / second_name
                first.mkdir(parents=True)
                if second.exists() and first.samefile(second):
                    self.require_fixture_rejected_before_writes(probe, first, second)
                else:
                    second.mkdir()
                    self.require_fixture_roots_admitted(probe, first, second)

                for nested in (False, True):
                    for reverse_roles in (False, True):
                        with (
                            self.subTest(nested=nested, reverse_roles=reverse_roles),
                            tempfile.TemporaryDirectory(dir=root) as scratch_fixture,
                        ):
                            fixture = Path(scratch_fixture)
                            library, state = fixture / first_name, fixture / second_name
                            if nested:
                                state /= "pending"
                            if reverse_roles:
                                library, state = state, library
                            self.require_fixture_rejected_before_writes(
                                fixture, library, state, "precreate distinct empty fixture directories"
                            )

    def test_smoke_admits_missing_non_ascii_paths_with_an_ascii_discriminator(self) -> None:
        with tempfile.TemporaryDirectory(dir=os.environ["TEST_TMPDIR"]) as scratch:
            root = Path(scratch)
            self.require_fixture_roots_admitted(root, root / "Caf\u00e9" / "music", root / "Cafe\u0301" / "state")

    def test_smoke_rejects_invalid_utf8_in_either_fixture_operand(self) -> None:
        for invalid_library in (False, True):
            with (
                self.subTest(invalid_library=invalid_library),
                tempfile.TemporaryDirectory(dir=os.environ["TEST_TMPDIR"]) as scratch,
            ):
                root = Path(scratch)
                (root / "left").mkdir()
                (root / "right").mkdir()
                invalid_path = os.fsencode(root / "left") + b"/bad-\xff"
                valid_path = root / "right" / "valid"
                if invalid_library:
                    self.require_fixture_rejected_before_writes(
                        root, invalid_path, valid_path, "--library is not UTF-8"
                    )
                else:
                    self.require_fixture_rejected_before_writes(
                        root, valid_path, invalid_path, "--state-root is not UTF-8"
                    )

    def test_smoke_rejects_parent_traversal_after_a_missing_or_dangling_prefix(self) -> None:
        for prefix in ("missing", "dangling"):
            for reverse_roles in (False, True):
                with (
                    self.subTest(prefix=prefix, reverse_roles=reverse_roles),
                    tempfile.TemporaryDirectory(dir=os.environ["TEST_TMPDIR"]) as scratch,
                ):
                    root = Path(scratch)
                    (root / "dangling").symlink_to(root / "missing", target_is_directory=True)
                    library, state = root / prefix / ".." / "library", root / "state"
                    if reverse_roles:
                        library, state = state, library
                    self.require_fixture_rejected_before_writes(
                        root, library, state, "'..' requires an existing directory"
                    )

    def test_smoke_rejects_parent_traversal_through_an_unsearchable_directory(self) -> None:
        with tempfile.TemporaryDirectory(dir=os.environ["TEST_TMPDIR"]) as scratch:
            root = Path(scratch)
            restricted = root / "restricted"
            restricted.mkdir(mode=0o000)
            try:
                try:
                    (restricted / "..").stat()
                except PermissionError:
                    pass
                else:
                    self.skipTest("The test process can search the restricted directory")

                self.require_fixture_rejected_before_writes(
                    root, restricted / ".." / "library", root / "state", "'..' requires an existing directory"
                )
            finally:
                restricted.chmod(0o700)

    def test_smoke_checks_case_aliases_below_symlinked_fixture_ancestor(self) -> None:
        for reverse_roles in (False, True):
            with (
                self.subTest(reverse_roles=reverse_roles),
                tempfile.TemporaryDirectory(dir=os.environ["TEST_TMPDIR"]) as scratch,
            ):
                root = Path(scratch)
                directory = root / "fixture"
                alias = root / "alias"
                directory.mkdir()
                alias.symlink_to(directory, target_is_directory=True)
                alternate = root / "FIXTURE"
                case_aliased = alternate.exists() and directory.samefile(alternate)

                # Resolving the live symlink alone leaves two differently spelled missing paths.
                library, state = directory / "new", alias / "NEW" / "pending"
                if reverse_roles:
                    library, state = state, library
                if case_aliased:
                    self.require_fixture_rejected_before_writes(root, library, state)
                else:
                    self.require_fixture_roots_admitted(root, library, state)

    def test_smoke_rejects_dangling_library_alias_before_creating_state(self) -> None:
        for target in ("state", "missing/../media"):
            with (
                self.subTest(target=target),
                tempfile.TemporaryDirectory(dir=os.environ["TEST_TMPDIR"]) as scratch,
            ):
                root = Path(scratch)
                library, state = root / "library", root / "state"
                # A dangling target must not become valid by collapsing its missing component.
                (root / "media").mkdir()
                library.symlink_to(target, target_is_directory=True)
                self.assertFalse(library.exists())
                self.require_fixture_rejected_before_writes(root, library, state, "AppKit fixture preparation failed:")

    def test_smoke_admits_distinct_empty_sibling_fixture_roots(self) -> None:
        with tempfile.TemporaryDirectory(dir=os.environ["TEST_TMPDIR"]) as scratch:
            root = Path(scratch)
            library, state = root / "fixture", root / "fixture-state"
            library.mkdir()
            state.mkdir()
            links = root / "links"
            links.mkdir()
            alias = links / "state-link"
            alias.symlink_to(state, target_is_directory=True)
            self.assertFalse(library.samefile(state))
            self.require_fixture_roots_admitted(root, alias / ".." / library.name, state)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
