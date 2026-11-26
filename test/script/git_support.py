"""A disposable Git checkout isolated from the developer's config and index."""

import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock


class GitCheckoutTestCase(unittest.TestCase):
    def setUp(self):
        temporary = Path(self.enterContext(tempfile.TemporaryDirectory()))
        config = temporary / "git-config"
        config.write_text("[user]\n\tname = Aobus fixture\n\temail = fixture@example.invalid\n", encoding="utf-8")
        self.enterContext(mock.patch.dict(os.environ, {"GIT_CONFIG_GLOBAL": str(config), "GIT_CONFIG_NOSYSTEM": "1"}))
        for key in (
            "AOBUS_GIT_DIR",
            "AOBUS_GIT_WORK_TREE",
            "GIT_DIR",
            "GIT_WORK_TREE",
            "GIT_INDEX_FILE",
            "GIT_COMMON_DIR",
            "GIT_OBJECT_DIRECTORY",
            "GIT_CONFIG_COUNT",
            "GIT_CONFIG_PARAMETERS",
        ):
            os.environ.pop(key, None)
        self.root = temporary / "checkout"
        self.root.mkdir()
        self.git("init", "-b", "main")

    def git(self, *args: str) -> str:
        return subprocess.check_output(["git", *args], cwd=self.root, stderr=subprocess.STDOUT, text=True).strip()

    def commit(self, message: str) -> str:
        self.git("add", "--all")
        self.git("commit", "-m", message)
        return self.git("rev-parse", "HEAD")
