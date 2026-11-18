"""Disposable fixtures and native dispatch for opt-in AppKit smoke scenarios."""

import contextlib
import io
import os
import subprocess
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
                (root / "same", root / "same"),
                (root / "state", root / "state/music"),
                (root / "music/state", root / "music"),
            )
            with mock.patch.object(appkit_process.subprocess, "Popen") as popen:
                for state_root, library in cases:
                    with self.subTest(state_root=state_root, library=library):
                        with contextlib.redirect_stderr(io.StringIO()):
                            self.assertEqual(
                                appkit_process.main(
                                    [str(root / "smoke"), "--library", str(library), "--state-root", str(state_root)]
                                ),
                                1,
                            )
            popen.assert_not_called()

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
