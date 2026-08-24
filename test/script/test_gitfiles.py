"""Tests for Git-backed source discovery."""

import contextlib
import io
import unittest
from pathlib import Path
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
        self.assertEqual(
            gitfiles._git_command("status", os_name="posix", platform_name="linux"),
            ["git", "status"],
        )

    def test_macos_git_ignores_smb_executable_bit_artifacts_process_locally(self):
        self.assertEqual(
            gitfiles._git_command(
                "status",
                os_name="posix",
                platform_name="darwin",
                checkout_uses_smb=True,
            ),
            ["git", "-c", "core.filemode=false", "status"],
        )

    def test_macos_git_preserves_file_modes_on_local_filesystems(self):
        self.assertEqual(
            gitfiles._git_command(
                "status",
                os_name="posix",
                platform_name="darwin",
                checkout_uses_smb=False,
            ),
            ["git", "status"],
        )

    def test_macos_smb_detection_uses_the_mount_containing_the_checkout(self):
        mount_output = """\
/dev/disk3s1 on /System/Volumes/Data (apfs, local, journaled)
//guest:@10.200.200.1/aobus on /Users/alice/mnt/aobus (smbfs, nodev, noowners)
"""

        self.assertTrue(
            gitfiles._path_is_on_smb_mount(
                Path("/Users/alice/mnt/aobus/script/ao"),
                mount_output,
            )
        )
        self.assertFalse(
            gitfiles._path_is_on_smb_mount(
                Path("/Users/alice/dev/Aobus"),
                mount_output,
            )
        )

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
