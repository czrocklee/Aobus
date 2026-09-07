"""Subprocess regressions for output preservation and inherited-pipe shutdown."""

import os
import socket
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ENV = {**os.environ, "PYTHONPATH": str(ROOT / "script")}


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


if __name__ == "__main__":
    unittest.main()
