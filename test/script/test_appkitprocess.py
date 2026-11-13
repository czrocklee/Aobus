"""Exercise complete desktop child lifetimes with real, disposable processes."""

import contextlib
import io
import os
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path
from unittest import mock

from ao.core import appkitprocess


@unittest.skipIf(os.name == "nt", "AppKit launch supervision uses a POSIX bridge")
class AppKitProcessTests(unittest.TestCase):
    def test_success_requires_both_markers_and_successful_child_exit(self):
        for mode in ("success", "nonzero", "hang", "missing-marker", "parent-fails", "no-successor"):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory(prefix="appkit process ") as directory:
                root = Path(directory)
                binary = root / "smoke.app/Contents/MacOS/custom-smoke"
                binary.parent.mkdir(parents=True)
                binary.write_text(
                    f"#!{sys.executable}\n"
                    + textwrap.dedent(
                        """\
                        import os
                        import signal
                        import subprocess
                        import sys
                        import time
                        from pathlib import Path

                        mode = os.environ["APPKIT_PROCESS_TEST_MODE"]
                        if "--scenario" in sys.argv:
                            state = Path(sys.argv[sys.argv.index("--state-root") + 1])
                            state.mkdir()
                            if mode == "no-successor":
                                sys.exit(0)
                            launcher = os.environ["AOBUS_APPKIT_TEST_SUCCESSOR_LAUNCHER"]
                            bridge = subprocess.Popen([launcher, "--successor", str(state)])
                            if bridge.wait(timeout=5) != 0:
                                sys.exit(2)
                            (state / "desktop-pass.txt").write_text(os.environ["AOBUS_APPKIT_TEST_RUN_ID"] + "\\n")
                            sys.exit(23 if mode == "parent-fails" else 0)
                        state = Path(sys.argv[2])
                        if mode != "missing-marker":
                            (state / "successor-pass.txt").write_text(os.environ["AOBUS_APPKIT_TEST_RUN_ID"] + "\\n")
                        if mode in ("hang", "parent-fails"):
                            signal.signal(signal.SIGTERM, signal.SIG_IGN)
                            time.sleep(60)
                        sys.exit(42 if mode == "nonzero" else 0)
                        """
                    ),
                    encoding="utf-8",
                )
                binary.chmod(0o700)
                started = []
                real_popen = subprocess.Popen

                def launch(*args, real_popen=real_popen, started=started, **kwargs):
                    process = real_popen(*args, **kwargs)
                    started.append(process)
                    return process

                errors = io.StringIO()
                with mock.patch.dict(os.environ, {"APPKIT_PROCESS_TEST_MODE": mode}):
                    with mock.patch.object(appkitprocess.subprocess, "Popen", side_effect=launch):
                        with mock.patch.object(appkitprocess, "PARENT_TIMEOUT_SECONDS", 10.0):
                            with mock.patch.object(appkitprocess, "SUCCESSOR_TIMEOUT_SECONDS", 2.0):
                                with mock.patch.object(appkitprocess, "TERMINATE_TIMEOUT_SECONDS", 0.1):
                                    with (
                                        contextlib.redirect_stdout(io.StringIO()),
                                        contextlib.redirect_stderr(errors),
                                    ):
                                        if mode == "missing-marker":
                                            with self.assertRaises(FileNotFoundError):
                                                appkitprocess.run_smoke(
                                                    binary,
                                                    scenario="desktop",
                                                    library=root / "music",
                                                    state_root=root / "state",
                                                )
                                        else:
                                            status = appkitprocess.run_smoke(
                                                binary,
                                                scenario="desktop",
                                                library=root / "music",
                                                state_root=root / "state",
                                            )
                                            self.assertEqual(
                                                status,
                                                {
                                                    "success": 0,
                                                    "nonzero": 42,
                                                    "hang": 1,
                                                    "parent-fails": 23,
                                                    "no-successor": 1,
                                                }[mode],
                                            )
                if mode == "no-successor":
                    self.assertIn("successor launch request was not received", errors.getvalue())
                    self.assertNotIn("successor did not exit", errors.getvalue())
                elif mode == "hang":
                    self.assertIn("successor did not exit", errors.getvalue())
                    self.assertNotIn("launch request was not received", errors.getvalue())
                self.assertEqual(len(started), 1 if mode == "no-successor" else 2)
                for process in started:
                    self.assertIsNotNone(process.returncode)
                    self.assertEqual(process.poll(), process.returncode)
                if mode in ("hang", "parent-fails"):
                    self.assertNotEqual(started[1].returncode, 0)

    def test_standalone_supervisor_forwards_native_arguments_and_nonzero_exit(self):
        with tempfile.TemporaryDirectory(prefix="appkit native launcher ") as directory:
            root = Path(directory)
            binary = root / "native-smoke"
            binary.write_text(
                f"#!{sys.executable}\n"
                + textwrap.dedent(
                    """\
                    import os
                    import sys
                    from pathlib import Path

                    library, state = Path(sys.argv[4]), Path(sys.argv[6])
                    assert sys.argv[1:3] == ["--scenario", "authoring"]
                    assert library.is_absolute() and state.is_absolute()
                    assert os.environ["UBSAN_OPTIONS"] == "halt_on_error=1"
                    assert os.environ["AOBUS_APPKIT_TEST_RUN_ID"] != "old"
                    sys.exit(7)
                    """
                ),
                encoding="utf-8",
            )
            binary.chmod(0o700)
            result = subprocess.run(
                [
                    sys.executable,
                    appkitprocess.__file__,
                    str(binary),
                    "--scenario",
                    "authoring",
                    "--library",
                    str(root / "music"),
                    "--state-root",
                    str(root / "state"),
                ],
                env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1", "AOBUS_APPKIT_TEST_RUN_ID": "old"},
                capture_output=True,
                text=True,
                timeout=10,
            )
            self.assertEqual(result.returncode, 7, result.stdout + result.stderr)

    def test_accepts_detached_launch_request_after_successful_parent_exit(self):
        parent = mock.Mock()
        parent.poll.return_value = 0
        successor = mock.Mock()
        successor.poll.return_value = 0
        listener = mock.MagicMock()
        listener.__enter__.return_value = listener
        connection = mock.MagicMock()
        connection.__enter__.return_value = connection
        connection.recv.side_effect = [b'["--successor", "fixture"]', b""]
        listener.accept.return_value = (connection, None)
        with (
            tempfile.TemporaryDirectory(prefix="appkit delayed launch ") as directory,
            mock.patch.object(appkitprocess, "_local_socket", return_value=listener),
            mock.patch.object(appkitprocess.subprocess, "Popen", side_effect=[parent, successor]) as launch,
            mock.patch.object(appkitprocess.time, "monotonic", return_value=0.0),
        ):
            # The controller observes the exited parent before accepting the bridge connection.
            status = appkitprocess.run_desktop(
                [sys.executable, "--parent"],
                cwd=Path(directory),
                environment={},
                parent_timeout=10.0,
                successor_timeout=2.0,
                terminate_timeout=0.1,
            )
        self.assertEqual(status, 0)
        self.assertEqual(launch.call_count, 2)
        self.assertEqual(launch.call_args_list[1].args[0], [sys.executable, "--successor", "fixture"])
        connection.sendall.assert_called_once_with(b"OK")
        parent.terminate.assert_not_called()
        successor.terminate.assert_not_called()


if __name__ == "__main__":
    unittest.main()
