"""Tests for Git-backed source discovery."""

import contextlib
import io
import unittest
from unittest import mock

from ao.core import gitfiles


class GitFilesTest(unittest.TestCase):
    def test_windows_git_config_is_process_local_and_checkout_scoped(self):
        self.assertEqual(
            gitfiles._git_command("status", os_name="nt"),
            [
                "git",
                "-c",
                f"safe.directory={gitfiles.PROJECT_ROOT.as_posix()}",
                "-c",
                "core.filemode=false",
                "status",
            ],
        )
        self.assertEqual(gitfiles._git_command("status", os_name="posix"), ["git", "status"])

    def test_git_discovery_failure_is_not_reported_as_an_empty_scope(self):
        completed = mock.Mock(returncode=128, stdout="", stderr="fatal: dubious ownership")
        errors = io.StringIO()

        with mock.patch.object(gitfiles.subprocess, "run", return_value=completed):
            with contextlib.redirect_stderr(errors):
                with self.assertRaises(SystemExit):
                    gitfiles._git_lines("diff", "--name-only")

        self.assertIn("dubious ownership", errors.getvalue())


if __name__ == "__main__":
    unittest.main()
