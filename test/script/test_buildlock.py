"""Tests for native build-tree process serialization."""

import errno
import io
import queue
import shutil
import subprocess
import sys
import tempfile
import threading
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest import mock

from ao.core import buildlock

_CONTENDER_SCRIPT = """
import sys
from pathlib import Path
from ao.core.buildlock import build_tree_lock

build_dir, started, acquired = map(Path, sys.argv[1:])
started.write_text("started", encoding="utf-8")
with build_tree_lock(build_dir):
    acquired.write_text("acquired", encoding="utf-8")
"""


class BuildTreeLockTest(unittest.TestCase):
    def test_lock_file_is_outside_build_tree(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            build_dir = Path(temp_dir) / "debug"

            self.assertEqual(buildlock.lock_path(build_dir), Path(temp_dir).resolve() / ".debug.ao-build.lock")

    def test_same_build_tree_reports_wait_before_active_process_releases(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            build_dir = root / "debug"
            started = root / "started"
            acquired = root / "acquired"
            build_dir.mkdir()
            child: subprocess.Popen[str] | None = None
            try:
                with buildlock.build_tree_lock(build_dir):
                    shutil.rmtree(build_dir)
                    child = subprocess.Popen(
                        [sys.executable, "-c", _CONTENDER_SCRIPT, str(build_dir), str(started), str(acquired)],
                        stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT,
                        text=True,
                    )
                    assert child.stdout is not None
                    lines: queue.Queue[str] = queue.Queue()
                    reader = threading.Thread(target=lambda: lines.put(child.stdout.readline()), daemon=True)
                    reader.start()
                    waiting_line = lines.get(timeout=5)

                    self.assertTrue(started.exists())
                    self.assertFalse(acquired.exists())
                    self.assertIn("Waiting for active build", waiting_line)
                    self.assertTrue(buildlock.lock_path(build_dir).is_file())

                output, _ = child.communicate(timeout=5)
                self.assertEqual(child.returncode, 0, output)
                self.assertTrue(acquired.exists())
            finally:
                if child is not None and child.poll() is None:
                    child.kill()
                    child.wait()

    def test_different_build_trees_do_not_wait(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            first = root / "debug"
            second = root / "debug-asan"

            with buildlock.build_tree_lock(first):
                with buildlock.build_tree_lock(second):
                    self.assertNotEqual(buildlock.lock_path(first), buildlock.lock_path(second))

    def test_same_thread_can_reenter_the_same_build_tree(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            build_dir = Path(temp_dir) / "debug"

            with buildlock.build_tree_lock(build_dir):
                with buildlock.build_tree_lock(build_dir):
                    pass

            with buildlock.build_tree_lock(build_dir):
                pass

    def test_other_thread_waits_for_the_same_build_tree(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            build_dir = Path(temp_dir) / "debug"
            started = threading.Event()
            acquired = threading.Event()

            def acquire() -> None:
                started.set()
                with buildlock.build_tree_lock(build_dir):
                    acquired.set()

            worker = threading.Thread(target=acquire)
            with buildlock.build_tree_lock(build_dir):
                worker.start()
                self.assertTrue(started.wait(timeout=5))
                self.assertFalse(acquired.wait(timeout=0.1))

            worker.join(timeout=5)
            self.assertFalse(worker.is_alive())
            self.assertTrue(acquired.is_set())

    def test_body_exception_releases_the_build_tree(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            build_dir = Path(temp_dir) / "debug"

            with self.assertRaisesRegex(RuntimeError, "body failed"):
                with buildlock.build_tree_lock(build_dir):
                    raise RuntimeError("body failed")

            with buildlock.build_tree_lock(build_dir):
                pass

    def test_uncontended_build_tree_does_not_report_waiting(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            output = io.StringIO()

            with redirect_stdout(output), buildlock.build_tree_lock(Path(temp_dir) / "debug"):
                pass

            self.assertEqual(output.getvalue(), "")

    def test_lock_file_failure_reports_portal_error(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            parent = Path(temp_dir) / "not-a-directory"
            parent.write_text("fixture", encoding="utf-8")
            error = io.StringIO()

            with redirect_stderr(error), self.assertRaises(SystemExit):
                with buildlock.build_tree_lock(parent / "debug"):
                    pass

            self.assertIn("cannot open build-tree lock", error.getvalue())

    def test_windows_nonblocking_lock_only_waits_for_contention(self):
        host_os = mock.Mock()
        host_os.name = "nt"

        with tempfile.TemporaryFile("w+b") as stream:
            locking = mock.Mock(LK_NBLCK=1)
            locking.locking.side_effect = PermissionError(errno.EACCES, "locked")
            with (
                mock.patch("ao.core.buildlock.os", host_os),
                mock.patch("ao.core.buildlock._windows_locking", return_value=locking),
            ):
                self.assertFalse(buildlock._try_lock(stream))

            locking.locking.side_effect = OSError(errno.EIO, "I/O failure")
            with (
                mock.patch("ao.core.buildlock.os", host_os),
                mock.patch("ao.core.buildlock._windows_locking", return_value=locking),
                self.assertRaises(OSError) as caught,
            ):
                buildlock._try_lock(stream)

            self.assertEqual(caught.exception.errno, errno.EIO)

    def test_windows_lock_retries_contention_and_unlocks_the_same_byte(self):
        host_os = mock.Mock()
        host_os.name = "nt"
        locking = mock.Mock(LK_NBLCK=1, LK_UNLCK=2)
        locking.locking.side_effect = [PermissionError(errno.EACCES, "locked"), None, None]

        with (
            tempfile.TemporaryFile("w+b") as stream,
            mock.patch("ao.core.buildlock.os", host_os),
            mock.patch("ao.core.buildlock._windows_locking", return_value=locking),
            mock.patch("time.sleep") as sleep,
        ):
            file_descriptor = stream.fileno()
            buildlock._lock(stream)
            buildlock._unlock(stream)

        self.assertEqual(
            locking.locking.call_args_list,
            [
                mock.call(file_descriptor, locking.LK_NBLCK, 1),
                mock.call(file_descriptor, locking.LK_NBLCK, 1),
                mock.call(file_descriptor, locking.LK_UNLCK, 1),
            ],
        )
        sleep.assert_called_once_with(0.1)


if __name__ == "__main__":
    unittest.main()
