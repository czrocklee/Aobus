"""Bazel run preserves the frontend's native standard streams."""

import os
import select
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from ao.core import bazel_native as native

_CONTROLLER = """
import sys
from pathlib import Path
sys.path.insert(0, sys.argv[1])
from ao.core import bazel_native as native, bazel_workspace as workspace
root = Path(sys.argv[2])
native.PROJECT_ROOT = root
raise SystemExit(workspace.execute(
    Path(sys.argv[3]), root, root / 'configuration', 'run', 'debug',
    ['//fixture:frontend', '--', '--help'],
))
"""


class BazelRunStreamsTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-bazel-run streams-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.log = self.root / "build.log"
        self.log.write_bytes(b"previous build evidence\n")

    def controller(self, executable):
        return [
            sys.executable,
            "-I",
            "-B",
            "-c",
            _CONTROLLER,
            str(native.PROJECT_ROOT / "script"),
            str(self.root),
            str(executable),
        ]

    def test_run_forwards_input_output_error_and_exit_status_without_truncating_build_evidence(self):
        if os.name == "nt":
            executable = self.root / "fake-bazel.cmd"
            executable.write_text(
                "@echo off\necho program output\n1>&2 echo program error\n"
                'set /p "reply="\necho received:%reply%\nexit /b 9\n',
                encoding="utf-8",
            )
        else:
            executable = self.root / "fake-bazel"
            executable.write_text(
                f"#!{sys.executable}\nimport sys\n"
                "print('program output')\nprint('program error', file=sys.stderr)\n"
                "print('received:' + input())\nraise SystemExit(9)\n",
                encoding="utf-8",
            )
            executable.chmod(0o755)
        result = subprocess.run(
            self.controller(executable), input="hello\n", capture_output=True, text=True, timeout=30, check=False
        )
        self.assertEqual(result.returncode, 9, result.stderr)
        self.assertEqual(result.stdout, "program output\nreceived:hello\n")
        self.assertIn("program error\n", result.stderr)
        self.assertEqual(self.log.read_bytes(), b"previous build evidence\n")

    @unittest.skipUnless(hasattr(os, "openpty"), "native POSIX terminal descriptors")
    def test_run_preserves_terminal_detection_for_all_standard_descriptors(self):
        executable = self.root / "fake-bazel"
        executable.write_text(
            f"#!{sys.executable}\nimport os\nprint('terminal=' + str(all(os.isatty(fd) for fd in (0, 1, 2))))\n",
            encoding="utf-8",
        )
        executable.chmod(0o755)
        master, slave = os.openpty()
        try:
            result = subprocess.run(
                self.controller(executable), stdin=slave, stdout=slave, stderr=slave, timeout=30, check=False
            )
            # Darwin discards unread PTY output when the last slave closes.
            # The child has exited, so drain its bounded output before closing ours.
            output = bytearray()
            while select.select([master], [], [], 0)[0]:
                chunk = os.read(master, 4096)
                if not chunk:
                    break
                output.extend(chunk)
        finally:
            os.close(slave)
            os.close(master)
        self.assertEqual(result.returncode, 0, output.decode(errors="replace"))
        self.assertIn(b"terminal=True", output)
        self.assertEqual(self.log.read_bytes(), b"previous build evidence\n")


if __name__ == "__main__":
    unittest.main()
