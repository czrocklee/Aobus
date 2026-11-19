"""Exercise the native replay launcher's argv transport and child exit status."""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from python.runfiles import runfiles


class ReplayLauncherTest(unittest.TestCase):
    def test_forwards_arguments_and_child_status(self) -> None:
        resolver = runfiles.Create()
        assert resolver is not None
        path = resolver.Rlocation(sys.argv[1])
        assert path is not None
        source = Path(path)
        arguments = ["", "two words", "日本語", r"backslash\\\" quote", "space and trailing slash\\"]
        with tempfile.TemporaryDirectory(dir=os.environ["TEST_TMPDIR"]) as scratch:
            # Use the same launcher as the first child to exercise a quoted
            # executable path on both sides. The installed Python stays put.
            launcher = Path(scratch) / f"replay é{source.suffix}"
            shutil.copy2(source, launcher)
            driver = Path(scratch) / "driver é.py"
            driver.write_text("import json, sys\nprint(json.dumps(sys.argv[1:]))\nsys.exit(37)\n", encoding="utf-8")
            result = subprocess.run(
                [launcher, launcher, sys.executable, driver, *arguments],
                capture_output=True,
                encoding="utf-8",
                timeout=15,
                check=False,
            )
        self.assertEqual(result.returncode, 37, result.stderr)
        self.assertEqual(json.loads(result.stdout), arguments)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
