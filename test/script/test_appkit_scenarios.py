"""Disposable fixtures and native dispatch for opt-in AppKit smoke scenarios."""

import contextlib
import io
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao.graph import appkit_process


class AppKitScenarioTest(unittest.TestCase):
    def test_component_dispatch_preserves_fixtures_inherited_runtime_and_exit_status(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            binary = root / "smoke.app/Contents/MacOS/smoke"
            library, state_root = root / "music", root / "state"
            run_id = "0123456789abcdef0123456789abcdef"
            inherited = {"UBSAN_OPTIONS": "halt_on_error=1", appkit_process.RUN_ID_ENV: "old"}
            with (
                mock.patch.dict(os.environ, inherited, clear=True),
                mock.patch.object(appkit_process.uuid, "uuid4", return_value=mock.Mock(hex=run_id)),
                mock.patch.object(appkit_process, "run_desktop") as desktop,
                mock.patch.object(appkit_process, "_require_marker") as marker,
            ):
                for status in (0, 7):
                    with self.subTest(status=status):
                        parent = mock.Mock()
                        parent.wait.return_value = status
                        command = [
                            str(binary.resolve()),
                            "--scenario",
                            "credits-controls",
                            "--library",
                            str(library.resolve()),
                            "--state-root",
                            str(state_root.resolve()),
                        ]
                        with mock.patch.object(appkit_process.subprocess, "Popen", return_value=parent) as popen:
                            self.assertEqual(
                                appkit_process.main(
                                    [
                                        str(binary),
                                        "--scenario",
                                        "credits-controls",
                                        "--library",
                                        str(library),
                                        "--state-root",
                                        str(state_root),
                                    ]
                                ),
                                status,
                            )
                        popen.assert_called_once_with(command, env=inherited | {appkit_process.RUN_ID_ENV: run_id})
                        parent.wait.assert_called_once_with(timeout=120.0)
                        parent.terminate.assert_not_called()
                        parent.kill.assert_not_called()
                desktop.assert_not_called()
                marker.assert_not_called()

    def test_component_interrupt_reaps_parent_without_waiting_for_desktop_markers(self):
        parent = mock.Mock()
        parent.wait.side_effect = [KeyboardInterrupt, KeyboardInterrupt, -9]
        with (
            mock.patch.object(appkit_process.subprocess, "Popen", return_value=parent),
            self.assertRaises(KeyboardInterrupt),
        ):
            appkit_process._run_parent(["configured-smoke", "--scenario", "credits-controls"], {})
        parent.kill.assert_called_once_with()
        self.assertEqual(parent.wait.call_args_list, [mock.call(timeout=120.0), mock.call(), mock.call()])

    def test_component_timeout_forces_a_stalled_parent_to_exit(self):
        parent = mock.Mock()
        parent.poll.return_value = None
        parent.wait.side_effect = [subprocess.TimeoutExpired("smoke", 120), subprocess.TimeoutExpired("smoke", 5), -9]
        with (
            mock.patch.object(appkit_process.subprocess, "Popen", return_value=parent),
            contextlib.redirect_stderr(io.StringIO()),
        ):
            self.assertEqual(appkit_process._run_parent(["configured-smoke"], {}), 1)
        parent.terminate.assert_called_once_with()
        parent.kill.assert_called_once_with()
        self.assertEqual(parent.wait.call_args_list, [mock.call(timeout=120.0), mock.call(timeout=5.0), mock.call()])

    def test_rejects_nonempty_invalid_or_nested_fixtures_before_starting_parent(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            nonempty_state, nonempty_library = root / "nonempty-state", root / "nonempty-library"
            nonempty_state.mkdir()
            (nonempty_state / "desktop.plist").touch()
            nonempty_library.mkdir()
            (nonempty_library / "user-track.wav").touch()
            library_file, state_file = root / "library-file", root / "state-file"
            library_file.touch()
            state_file.touch()
            cases = (
                (nonempty_state, root / "music"),
                (state_file, root / "music-file-state"),
                (root / "state-a", nonempty_library),
                (root / "state-b", library_file),
            )
            with mock.patch.object(appkit_process.subprocess, "Popen") as popen:
                for state_root, library in cases:
                    with (
                        self.subTest(state_root=state_root, library=library),
                        contextlib.redirect_stderr(io.StringIO()) as errors,
                    ):
                        self.assertEqual(
                            appkit_process.main(
                                [str(root / "smoke"), "--library", str(library), "--state-root", str(state_root)]
                            ),
                            1,
                        )
                        self.assertNotIn("AppKit AppKit", errors.getvalue())
            popen.assert_not_called()
            for library, state_root in (
                (root / "same", root / "same"),
                (root / "music", root / "music/state"),
                (root / "state/music", root / "state"),
            ):
                with self.subTest(library=library, state_root=state_root):
                    self._assert_fixture_overlap_rejected(library, state_root)

    def _assert_fixture_dispatch(self, library, state_root, *, accepted, resolved_library=None):
        errors = io.StringIO()
        parent = mock.Mock()
        parent.wait.return_value = 0
        with (
            mock.patch.object(appkit_process.subprocess, "Popen", return_value=parent) as popen,
            contextlib.redirect_stderr(errors),
        ):
            status = appkit_process.main(
                ["smoke", "--scenario", "authoring", "--library", str(library), "--state-root", str(state_root)]
            )
        self.assertEqual((status, popen.call_count), (0, 1) if accepted else (1, 0), errors.getvalue())
        if resolved_library is not None:
            self.assertEqual(popen.call_args.args[0][3:5], ["--library", str(resolved_library)])
        return errors.getvalue()

    def _assert_fixture_overlap_rejected(self, library, state_root):
        errors = self._assert_fixture_dispatch(library, state_root, accepted=False)
        self.assertIn("Error: AppKit --library and --state-root must not equal or contain one another", errors)

    @unittest.skipUnless(sys.platform == "darwin", "fixture volume query is macOS-only")
    def test_unknown_fixture_case_sensitivity_reports_reason_and_rejects_before_dispatch(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            for result, reason in (
                (OSError("volume lacks case-sensitivity query support"), "volume lacks case-sensitivity query support"),
                (-1, "indeterminate pathconf result (-1)"),
            ):
                with (
                    self.subTest(result=result),
                    mock.patch.dict(os.pathconf_names, PC_CASE_SENSITIVE=123),
                    mock.patch.object(os, "pathconf", side_effect=[result]) as pathconf,
                ):
                    errors = self._assert_fixture_dispatch(root / "missing", root / "MISSING", accepted=False)
                    self.assertIn("Error: AppKit cannot determine fixture volume case sensitivity", errors)
                    self.assertIn(str(root), errors)
                    self.assertIn(reason, errors)
                    pathconf.assert_called_once_with(root, 123)

    @unittest.skipUnless(sys.platform == "darwin", "missing fixture case aliases are handled on macOS")
    def test_case_insensitive_fixture_aliases_are_rejected_before_dispatch(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            existing = root / "existing"
            existing.mkdir()
            if not (root / "EXISTING").is_dir():
                self.skipTest("fixture volume is case-sensitive")
            cases = (
                (existing, root / "EXISTING"),
                (existing, root / "EXISTING/child"),
                (root / "missing", root / "MISSING"),
                (root / "missing", root / "MISSING/child"),
                (root / "EXISTING/missing", existing / "missing"),
                (root / "EXISTING/missing", existing / "MISSING/child"),
            )
            for first, second in cases:
                for library, state_root in ((first, second), (second, first)):
                    with self.subTest(library=library, state_root=state_root):
                        self._assert_fixture_overlap_rejected(library, state_root)
            self.assertEqual(list(root.iterdir()), [existing])
            self.assertEqual(list(existing.iterdir()), [])

    @unittest.skipUnless(sys.platform == "darwin", "missing non-ASCII names are checked on macOS")
    def test_missing_non_ascii_names_require_proof_of_separation(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            nfc, nfd, distinct = root / "caf\u00e9", root / "cafe\u0301", root / "state"
            with mock.patch.object(os, "pathconf") as pathconf:
                for second in (nfd, nfd / "child", distinct):
                    for library, state_root in ((nfc, second), (second, nfc)):
                        with self.subTest(library=library, state_root=state_root):
                            errors = self._assert_fixture_dispatch(library, state_root, accepted=False)
                            self.assertIn("missing non-ASCII", errors)
                            self.assertIn("precreate distinct empty fixture directories", errors)
                self._assert_fixture_dispatch(nfc / "media", distinct / "snapshot", accepted=True)
                pathconf.assert_not_called()
            self.assertEqual(list(root.iterdir()), [])
            nfc.mkdir()
            distinct.mkdir()
            self._assert_fixture_dispatch(nfc, distinct, accepted=True)

    @unittest.skipUnless(sys.platform == "darwin", "fixture roots must be valid UTF-8 on macOS")
    def test_rejects_invalid_utf8_in_either_fixture_root_before_dispatch(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            invalid, distinct = root / "bad\udcff", root / "other"
            for library, state_root, option in ((invalid, distinct, "--library"), (distinct, invalid, "--state-root")):
                with self.subTest(option=option):
                    errors = self._assert_fixture_dispatch(library, state_root, accepted=False)
                    self.assertIn(f"{option} path is not valid UTF-8", errors)
            self.assertEqual(list(root.iterdir()), [])

    def test_rejects_unavailable_fixture_ancestor_with_option_and_path(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            obstruction = root / "file"
            obstruction.touch()
            invalid, distinct = obstruction / "child", root / "other"
            for library, state_root, option in ((invalid, distinct, "--library"), (distinct, invalid, "--state-root")):
                with self.subTest(option=option):
                    errors = self._assert_fixture_dispatch(library, state_root, accepted=False)
                    self.assertIn(f"cannot inspect {option} fixture ancestor", errors)
                    self.assertIn(str(obstruction), errors)
            self.assertEqual(list(root.iterdir()), [obstruction])

    @unittest.skipUnless(os.name == "posix", "Win32 collapses '..' lexically before the filesystem resolves it")
    def test_parent_traversal_requires_an_existing_directory_prefix(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            missing = root / "absent"
            invalid, distinct = missing / ".." / "media", root / "state"
            for library, state_root, option in ((invalid, distinct, "--library"), (distinct, invalid, "--state-root")):
                with self.subTest(option=option):
                    errors = self._assert_fixture_dispatch(library, state_root, accepted=False)
                    self.assertIn(f"cannot resolve {option} path", errors)
                    self.assertIn("'..' follows an unavailable directory", errors)
                    self.assertIn(str(missing), errors)
            self.assertEqual(list(root.iterdir()), [])
            missing.mkdir()
            self._assert_fixture_dispatch(invalid, distinct, accepted=True, resolved_library=root / "media")

    def test_case_sensitive_fixture_names_that_differ_in_case_stay_distinct(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            existing = root / "existing"
            existing.mkdir()
            if (root / "EXISTING").is_dir():
                self.skipTest("fixture volume is case-insensitive")
            (root / "EXISTING").mkdir()
            cases = (
                (existing, root / "EXISTING"),
                (existing, root / "EXISTING/child"),
                (root / "missing", root / "MISSING"),
                (root / "missing", root / "MISSING/child"),
            )
            for first, second in cases:
                for library, state_root in ((first, second), (second, first)):
                    with self.subTest(library=library, state_root=state_root):
                        self._assert_fixture_dispatch(library, state_root, accepted=True)

    def test_distinct_sibling_fixtures_are_accepted(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            parent = root / "fixtures"
            parent.mkdir()
            alternate = root / "FIXTURES"
            if not alternate.is_dir():
                alternate = parent
            self._assert_fixture_dispatch(parent / "media", alternate / "media-state", accepted=True)
            self._assert_fixture_dispatch(parent / "missing/media", alternate / "MISSING/media-state", accepted=True)

    @unittest.skipIf(os.name == "nt", "directory symlinks can require Windows privileges")
    def test_symlinked_fixture_paths_are_checked_at_their_targets(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            target, alias = root / "nested/fixtures", root / "link"
            target.mkdir(parents=True)
            alias.symlink_to(target, target_is_directory=True)
            dangling = root / "dangling"
            dangling.symlink_to(target / "absent", target_is_directory=True)
            errors = self._assert_fixture_dispatch(dangling, root / "state", accepted=False)
            self.assertIn("dangling symbolic link", errors)
            self.assertIn(str(dangling), errors)
            errors = self._assert_fixture_dispatch(dangling / ".." / "media", root / "state", accepted=False)
            self.assertIn("'..' follows an unavailable directory", errors)
            self.assertIn(str(dangling), errors)
            self._assert_fixture_dispatch(alias / "media", target / "state", accepted=True)
            self._assert_fixture_dispatch(
                alias / ".." / "media", root / "state", accepted=True, resolved_library=root / "nested/media"
            )

    @unittest.skipIf(os.name == "nt", "directory symlinks can require Windows privileges")
    def test_relative_symlink_target_and_chain_require_traversable_parent_components(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            media = root / "media"
            media.mkdir()
            alias, chain = root / "link", root / "chain"
            alias.symlink_to("absent/../media", target_is_directory=True)
            chain.symlink_to("link", target_is_directory=True)
            for invalid in (alias, chain, chain / "child"):
                with self.subTest(path=invalid):
                    errors = self._assert_fixture_dispatch(invalid, root / "state", accepted=False)
                    self.assertIn("dangling symbolic link", errors)
            self.assertEqual(list(media.iterdir()), [])
            self.assertFalse((root / "state").exists())
            (root / "absent").mkdir()
            self._assert_fixture_dispatch(chain, root / "state", accepted=True, resolved_library=media)

    @unittest.skipUnless(os.name == "posix", "requires POSIX directory search permissions")
    def test_cannot_traverse_parent_after_unsearchable_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            blocked = root / "blocked"
            blocked.mkdir()
            (root / "media").mkdir()
            alias = root / "link"
            alias.symlink_to("blocked/../media", target_is_directory=True)
            blocked.chmod(0)
            try:
                try:
                    (blocked / "..").stat()
                except PermissionError:
                    pass
                else:
                    self.skipTest("process can traverse mode-000 directories")
                errors = self._assert_fixture_dispatch(blocked / ".." / "media", root / "state", accepted=False)
                self.assertIn("'..' follows an unavailable directory", errors)
                self.assertIn(str(blocked), errors)
                errors = self._assert_fixture_dispatch(alias, root / "state", accepted=False)
                self.assertIn("cannot inspect --library fixture ancestor", errors)
                self.assertIn(str(alias), errors)
                self.assertFalse((root / "state").exists())
            finally:
                blocked.chmod(0o700)

    @unittest.skipUnless(sys.platform == "darwin", "missing fixture case aliases are handled on macOS")
    def test_symlinked_missing_fixture_case_aliases_are_rejected_before_dispatch(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            target, alias = root / "fixtures", root / "link"
            target.mkdir()
            if not (root / "FIXTURES").is_dir():
                self.skipTest("fixture volume is case-sensitive")
            alias.symlink_to(target, target_is_directory=True)
            first, second = alias / "missing", target / "MISSING/child"
            for library, state_root in ((first, second), (second, first)):
                with self.subTest(library=library, state_root=state_root):
                    self._assert_fixture_overlap_rejected(library, state_root)
            self.assertEqual(list(target.iterdir()), [])

    def test_desktop_requires_matching_markers_after_successful_process_exits(self):
        for wrong_marker in ("desktop-pass.txt", "successor-pass.txt", "missing", None):
            with self.subTest(wrong_marker=wrong_marker), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                state_root = root / "state"
                run_id = "0123456789abcdef0123456789abcdef"

                def completed_processes(
                    *args, state_root=state_root, run_id=run_id, wrong_marker=wrong_marker, **kwargs
                ):
                    self.assertEqual(kwargs["environment"][appkit_process.RUN_ID_ENV], run_id)
                    state_root.mkdir()
                    for marker in ("desktop-pass.txt", "successor-pass.txt"):
                        if wrong_marker == "missing" and marker == "successor-pass.txt":
                            continue
                        value = "another-invocation\n" if marker == wrong_marker else f"{run_id}\n"
                        (state_root / marker).write_text(value, encoding="utf-8")
                    return 0

                with (
                    mock.patch.object(appkit_process.uuid, "uuid4", return_value=mock.Mock(hex=run_id)),
                    mock.patch.object(appkit_process, "run_desktop", side_effect=completed_processes),
                    contextlib.redirect_stderr(io.StringIO()),
                ):
                    self.assertEqual(
                        appkit_process.main(
                            [
                                str(root / "smoke"),
                                "--library",
                                str(root / "music"),
                                "--state-root",
                                str(state_root),
                            ]
                        ),
                        0 if wrong_marker is None else 1,
                    )


if __name__ == "__main__":
    unittest.main()
