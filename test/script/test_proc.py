"""Subprocess regressions for output preservation and inherited-pipe shutdown."""

import os
import signal
import socket
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ENV = {**os.environ, "PYTHONPATH": str(ROOT / "script")}

_CONTROLLED_RUNNER = r"""
import errno
import os
import signal
import socket
import sys
from pathlib import Path
from types import SimpleNamespace
from ao.core import bazel_provision, buildlock, proc

root, port, scenario = Path(sys.argv[1]), int(sys.argv[2]), sys.argv[3]
control = socket.create_connection(("127.0.0.1", port), timeout=15)
control.sendall(b"P")
popen = proc.subprocess.Popen
owned = None
wait_states = 0
reader_closed = False
status_polls = 0
waitpid = os.waitpid if os.name == "posix" else None

def observe_wait():
    global wait_states
    if scenario == "interrupt":
        if wait_states < 3:
            wait_states += 1
            control.sendall(b"W")
            # Two interrupted wait states, then a final controller-released
            # state. No per-poll notifications or readiness-to-wait guess.
            assert control.recv(1) == b"G"
    elif scenario.startswith("reader-") or scenario == "early-setup":
        if wait_states == 0:
            wait_states = 1
            control.sendall(b"W")
            raise KeyboardInterrupt("interrupted deferred join")
        if wait_states == 1:
            wait_states = 2
            control.sendall(b"W")
    elif wait_states == 0:
        wait_states = 1
        control.sendall(b"W")

def observed_waitpid(pid, flags):
    global status_polls
    assert owned is not None and pid == owned.pid and flags == os.WNOHANG
    if scenario == "reader-poll" and not reader_closed:
        status_polls += 1
        if status_polls == 1:
            raise RuntimeError("initial reader failure")
        raise OSError("poll failed")
    observe_wait()
    result = waitpid(pid, flags)
    if result[0] and scenario in ("reap-interrupt", "lost-status"):
        assert os.waitstatus_to_exitcode(result[1]) == 0
        if scenario == "reap-interrupt":
            raise KeyboardInterrupt("interrupted after reap")
        raise ChildProcessError(errno.ECHILD, "injected lost status after reap")
    return result

class EarlySetupProcess(popen):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self._interrupt_stdout_setup = True
    def __getattribute__(self, name):
        if name == "stdout" and self.__dict__.pop("_interrupt_stdout_setup", False):
            control.sendall(b"S")
            raise KeyboardInterrupt("early stdout setup")
        return super().__getattribute__(name)

class FailedReader:
    def __init__(self, stream):
        self.stream = stream
        self.reads = 0
    def fileno(self):
        return self.stream.fileno()
    def read(self, size):
        if scenario == "reader-poll":
            return self.stream.read(size)
        self.reads += 1
        if self.reads == 1:
            raise RuntimeError("initial reader failure")
        if scenario == "reader-closed":
            raise ValueError("closed reader")
        raise OSError("unreadable pipe")
    def close(self):
        global reader_closed
        self.stream.close()
        reader_closed = True
        control.sendall(b"C")

def spawn(*args, **kwargs):
    global owned
    process_type = EarlySetupProcess if scenario == "early-setup" else popen
    owned = process_type(*args, **kwargs)
    if os.name == "posix":
        def unsafe_status(*args, **kwargs):
            raise AssertionError("POSIX must not enter Popen's interrupt-sensitive status lock")
        owned.wait = owned.poll = unsafe_status
        proc.os.waitpid = observed_waitpid
    else:
        wait = owned.wait
        def observed_wait(timeout=None):
            observe_wait()
            return wait(timeout=timeout)
        owned.wait = observed_wait
        if scenario == "reader-poll":
            polls = 0
            def failed_poll():
                nonlocal polls
                polls += 1
                if polls == 1:
                    raise RuntimeError("initial reader failure")
                raise OSError("poll failed")
            owned.poll = failed_poll
    if scenario.startswith("reader-"):
        owned.stdout = FailedReader(owned.stdout)
    return owned

proc.subprocess.Popen = spawn
interrupts = 0

def interrupt(signum, frame):
    global interrupts
    interrupts += 1
    control.sendall(b"I")
    raise KeyboardInterrupt("interrupt-" + str(interrupts))

if scenario == "interrupt":
    signal.signal(signal.SIGINT, interrupt)

class FailedSink:
    def write(self, data):
        raise OSError("failed " + scenario)
    def flush(self):
        pass
    def close(self):
        raise OSError("close failed")

stdout = sys.stdout
if scenario == "stdout":
    sys.stdout = SimpleNamespace(buffer=FailedSink(), flush=lambda: None)
elif scenario == "log":
    proc.open = lambda *args: FailedSink()

try:
    with buildlock.build_tree_lock(root / "tree"), buildlock.shared_file_lock(root / "restore.lock"):
        argv = [sys.executable, "-c", sys.argv[4], str(port), scenario]
        if scenario == "interrupt":
            bazel_provision._run_logged(argv, root / "install.log")
        else:
            proc.run(argv, log=root / "install.log")
except BaseException as error:
    assert owned is not None
    if scenario in ("reap-interrupt", "lost-status"):
        assert owned.returncode is not None and owned.returncode != 0, "unknown status became clean exit 0"
    else:
        assert owned.returncode == 0, "installer not joined before unlocking"
    (root / "exception").write_text(type(error).__name__ + ":" + str(error), encoding="utf-8")
else:
    raise AssertionError("pending exception was lost")
finally:
    sys.stdout = stdout
control.close()
"""

_CONTROLLED_INSTALLER = r"""
import os
import socket
import sys
connection = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=15)
connection.sendall(b"R")
if sys.argv[2] in ("stdout", "log"):
    try:
        sys.stdout.buffer.write(b"first line\n" + b"x" * 200000)
        sys.stdout.flush()
        connection.sendall(b"D")
    except BrokenPipeError:
        connection.sendall(b"B")
# Keep the installer active after pipe EOF or parent-side reader failure, so
# the controller can inspect locks without a timing-based negative assertion.
if not sys.argv[2].startswith("reader-"):
    os.close(1)
    os.close(2)
assert connection.recv(1) == b"x"
connection.close()
os._exit(0)
"""

_LOCK_PROBE = r"""
import fcntl
import sys
from pathlib import Path
root = Path(sys.argv[1])
for filename, acquire in (("restore.lock", fcntl.lockf), (".tree.ao-build.lock", fcntl.flock)):
    with (root / filename).open("a+b") as stream:
        try:
            acquire(stream.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            print("blocked")
        else:
            print("free")
"""


class ProcessRunnerTest(unittest.TestCase):
    def test_preserves_large_output_partial_lines_status_and_appended_log(self):
        expected = b"x" * 90000 + b"\nlast\xff"
        child = (
            "import sys; sys.stdout.buffer.write(b'x' * 90000 + b'\\n'); sys.stdout.flush(); "
            "sys.stderr.write('Fontconfig warning: ignored\\n'); sys.stderr.flush(); "
            "sys.stdout.buffer.write(b'last\\xff'); sys.exit(7)"
        )
        wrapper = (
            "from pathlib import Path; import sys; from ao.core.proc import run; "
            "sys.exit(run([sys.executable, '-c', sys.argv[1]], log=Path(sys.argv[2]), append=True))"
        )
        with tempfile.TemporaryDirectory() as temporary:
            log = Path(temporary) / "build.log"
            log.write_bytes(b"previous\n")
            result = subprocess.run(
                [sys.executable, "-c", wrapper, child, str(log)], env=ENV, capture_output=True, timeout=15
            )
            self.assertEqual(result.returncode, 7, result.stderr)
            self.assertEqual(result.stdout, expected)
            self.assertEqual(log.read_bytes(), b"previous\n" + expected)

    def test_returns_while_a_descendant_still_holds_the_pipe_open(self):
        # The descendant cannot exit until this test releases it after run()
        # returns. The timeout is only a hang guard, not a timing assertion.
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen()
            listener.settimeout(15)
            descendant = (
                "import socket; "
                f"s = socket.create_connection({listener.getsockname()!r}, timeout=15); "
                "s.sendall(b'R'); s.recv(1); s.close()"
            )
            child = (
                "import subprocess, sys; "
                f"subprocess.Popen([sys.executable, '-c', {descendant!r}]); "
                "print('direct child output', flush=True)"
            )
            wrapper = "import sys; from ao.core.proc import run; sys.exit(run([sys.executable, '-c', sys.argv[1]]))"
            with subprocess.Popen(
                [sys.executable, "-c", wrapper, child], env=ENV, stdout=subprocess.PIPE, stderr=subprocess.PIPE
            ) as process:
                connection = None
                try:
                    connection, _ = listener.accept()
                    connection.settimeout(15)
                    self.assertEqual(connection.recv(1), b"R")
                    stdout, stderr = process.communicate(timeout=12)
                    self.assertEqual(process.returncode, 0, stderr)
                    self.assertEqual(stdout.replace(b"\r\n", b"\n"), b"direct child output\n")
                finally:
                    if connection is not None:
                        with connection:
                            connection.sendall(b"x")
                            self.assertEqual(connection.recv(1), b"")
                    if process.poll() is None:
                        process.kill()
                        process.communicate(timeout=5)

    def assert_locks(self, root, state):
        probe = subprocess.run(
            [sys.executable, "-c", _LOCK_PROBE, str(root)], capture_output=True, text=True, timeout=5
        )
        self.assertEqual(probe.returncode, 0, probe.stderr)
        self.assertEqual(probe.stdout.splitlines(), [state, state])

    def controlled_failure(self, scenario):
        with tempfile.TemporaryDirectory(prefix="aobus-installer-lifetime-") as temporary, socket.socket() as listener:
            root = Path(temporary)
            listener.bind(("127.0.0.1", 0))
            listener.listen()
            listener.settimeout(15)
            with subprocess.Popen(
                [
                    sys.executable,
                    "-c",
                    _CONTROLLED_RUNNER,
                    str(root),
                    str(listener.getsockname()[1]),
                    scenario,
                    _CONTROLLED_INSTALLER,
                ],
                env=ENV,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            ) as process:
                control = installer = None
                try:
                    control, _ = listener.accept()
                    control.settimeout(15)
                    self.assertEqual(control.recv(1), b"P")
                    installer, _ = listener.accept()
                    installer.settimeout(15)
                    self.assertEqual(installer.recv(1), b"R")
                    if scenario in ("stdout", "log"):
                        # More than a pipe buffer reaches EOF despite the failed sink.
                        self.assertEqual(installer.recv(1), b"D")
                    if scenario.startswith("reader-"):
                        self.assertEqual(control.recv(1), b"C")
                    elif scenario == "early-setup":
                        self.assertEqual(control.recv(1), b"S")
                    if scenario.startswith("reader-") or scenario == "early-setup":
                        self.assertEqual(control.recv(1), b"W")  # This join is interrupted once.
                    self.assertEqual(control.recv(1), b"W")
                    if os.name == "posix":
                        self.assert_locks(root, "blocked")
                    if scenario == "interrupt":
                        for _ in range(2):
                            os.kill(process.pid, signal.SIGINT)
                            self.assertEqual(control.recv(1), b"I")
                            self.assertEqual(control.recv(1), b"W")
                            self.assert_locks(root, "blocked")
                finally:
                    unwinding = sys.exception() is not None
                    try:
                        if control is not None and scenario == "interrupt":
                            try:
                                control.sendall(b"GGG")
                            except OSError:
                                if not unwinding:
                                    raise
                        if installer is not None:
                            with installer:
                                try:
                                    installer.sendall(b"x")
                                    closed = installer.recv(1)
                                except OSError:
                                    if not unwinding:
                                        raise
                                else:
                                    if not unwinding:
                                        self.assertEqual(closed, b"")
                    finally:
                        try:
                            stdout, stderr = process.communicate(timeout=15)
                        finally:
                            if control is not None:
                                control.close()
                            if process.poll() is None:
                                process.kill()
                                process.communicate(timeout=5)
                self.assertEqual(process.returncode, 0, stderr)
                exception = (root / "exception").read_text(encoding="utf-8")
                if os.name == "posix":
                    self.assert_locks(root, "free")
                return exception, stdout

    @unittest.skipUnless(os.name == "posix", "portal-only SIGINT and native record-lock probe require POSIX")
    def test_repeated_interrupts_keep_installer_locks_until_natural_exit(self):
        exception, _ = self.controlled_failure("interrupt")
        self.assertEqual(exception, "KeyboardInterrupt:interrupt-1")

    @unittest.skipUnless(os.name == "posix", "waitpid status-loss contract is POSIX-only")
    def test_reap_then_interrupt_preserves_cancellation_without_synthesizing_clean_exit(self):
        exception, _ = self.controlled_failure("reap-interrupt")
        self.assertEqual(exception, "KeyboardInterrupt:interrupted after reap")

    @unittest.skipUnless(os.name == "posix", "waitpid status-loss contract is POSIX-only")
    def test_missing_reaped_exit_status_is_an_error_not_synthetic_clean_exit(self):
        exception, _ = self.controlled_failure("lost-status")
        self.assertIn("ChildProcessError:exit status unavailable for owned child ", exception)

    def test_early_stdout_setup_interrupt_keeps_locks_until_installer_is_joined(self):
        exception, stdout = self.controlled_failure("early-setup")
        self.assertEqual(exception, "KeyboardInterrupt:early stdout setup")
        self.assertEqual(stdout, b"")

    def test_failed_reader_closes_pipe_and_retries_join_before_raising_original_exception(self):
        for scenario in ("reader-oserror", "reader-closed", "reader-poll"):
            with self.subTest(scenario=scenario):
                exception, stdout = self.controlled_failure(scenario)
                self.assertEqual(exception, "RuntimeError:initial reader failure")
                self.assertEqual(stdout, b"")

    def test_output_or_log_failure_discards_output_and_joins_before_raising(self):
        for sink in ("stdout", "log"):
            with self.subTest(sink=sink):
                exception, stdout = self.controlled_failure(sink)
                self.assertEqual(exception, "OSError:failed " + sink)
                self.assertEqual(stdout.replace(b"\r\n", b"\n"), b"" if sink == "stdout" else b"first line\n")


if __name__ == "__main__":
    unittest.main()
