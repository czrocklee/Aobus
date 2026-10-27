"""Regression coverage for source identity in reused performance executables."""

import contextlib
import io
import json
import os
import subprocess
import tempfile
import unittest
from functools import partial
from pathlib import Path
from unittest import mock

from ao.__main__ import make_parser
from ao.command import perf
from ao.core import builddir


class PerformanceIdentityTest(unittest.TestCase):
    def test_provenance_uses_checkout_scoped_git_commands_on_native_share_profiles(self):
        command = perf.gitfiles.git_command
        for profile, options in (
            ("windows", {"os_name": "nt", "platform_name": "win32", "checkout_uses_smb": True}),
            ("macos-smb", {"os_name": "posix", "platform_name": "darwin", "checkout_uses_smb": True}),
            ("linux", {"os_name": "posix", "platform_name": "linux", "checkout_uses_smb": False}),
        ):
            with self.subTest(profile=profile):
                with (
                    mock.patch.object(perf.gitfiles, "git_command", side_effect=partial(command, **options)) as git,
                    mock.patch.object(perf, "capture", side_effect=["revision-id\n", ""]) as capture,
                ):
                    self.assertEqual(perf._revision(), "revision-id")
                self.assertEqual(
                    git.call_args_list, [mock.call("rev-parse", "--verify", "HEAD"), mock.call("status", "--porcelain")]
                )
                self.assertEqual(
                    capture.call_args_list,
                    [
                        mock.call(command("rev-parse", "--verify", "HEAD", **options), cwd=perf.PROJECT_ROOT),
                        mock.call(command("status", "--porcelain", **options), cwd=perf.PROJECT_ROOT),
                    ],
                )

    def test_provenance_ignores_discovery_bindings_in_real_repositories(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)

            def git(root, *arguments):
                return subprocess.check_output(
                    [
                        "git",
                        "-C",
                        str(root),
                        "-c",
                        "user.name=Aobus fixture",
                        "-c",
                        "user.email=fixture@example.invalid",
                        "-c",
                        "commit.gpgsign=false",
                        "-c",
                        "core.hooksPath=unused-fixture-hooks",
                        *arguments,
                    ],
                    stderr=subprocess.STDOUT,
                    text=True,
                ).strip()

            revisions = {}
            for name in ("source", "alternate"):
                root = directory / name
                root.mkdir()
                git(root, "init", "-b", "main")
                (root / "value.txt").write_text(name + "\n", encoding="utf-8")
                git(root, "add", ".")
                git(root, "commit", "-m", "fixture")
                revisions[name] = git(root, "rev-parse", "HEAD")
            self.assertNotEqual(revisions["source"], revisions["alternate"])
            source = directory / "source"
            for dirty in (False, True):
                (source / "value.txt").write_text("changed\n" if dirty else "source\n", encoding="utf-8")
                for private in (str(directory / "alternate" / ".git"), ""):
                    with (
                        self.subTest(dirty=dirty, private=private),
                        mock.patch.dict(os.environ, {"AOBUS_GIT_DIR": private, "AOBUS_GIT_WORK_TREE": str(source)}),
                        mock.patch.object(perf, "PROJECT_ROOT", source),
                        mock.patch.object(perf.gitfiles, "PROJECT_ROOT", source),
                    ):
                        expected = revisions["source"] + ("+dirty" if dirty else "")
                        self.assertEqual(perf._revision(), expected)

    def test_meaningful_worktree_changes_still_mark_the_revision_dirty(self):
        with mock.patch.object(perf, "capture", side_effect=["revision-id\n", " M script/ao/command/perf.py\n"]):
            self.assertEqual(perf._revision(), "revision-id+dirty")

    def test_revision_read_failure_is_not_silently_reported_as_verified(self):
        with mock.patch.object(perf, "capture", side_effect=subprocess.CalledProcessError(128, "git")) as capture:
            with self.assertRaises(subprocess.CalledProcessError):
                perf._revision()
            capture.assert_called_once()

    def test_worktree_status_failure_cannot_report_a_clean_revision(self):
        def capture(argv, *, cwd, check=True):
            if "rev-parse" in argv:
                return "revision-id\n"
            if check:
                raise subprocess.CalledProcessError(128, argv)
            return ""

        with mock.patch.object(perf, "capture", side_effect=capture):
            with self.assertRaises(subprocess.CalledProcessError):
                perf._revision()

    @mock.patch.dict(os.environ, {"AOBUS_PERF_BASELINE_JSON": ""})
    def test_reused_executable_cannot_claim_the_current_checkout_revision(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            executable = builddir.executable(directory / "test" / perf.TARGET)
            executable.parent.mkdir()
            original = b"existing executable from a previous revision"
            executable.write_bytes(original)
            report_path = directory / "new-parent" / "report.json"
            args = make_parser().parse_args(["perf", "--no-build", "-p", str(directory), "--output", str(report_path)])

            def write_report(_command, *, env, **_kwargs):
                report = {
                    "schema": perf.REPORT_SCHEMA,
                    "metadata": {
                        "revision": env["AOBUS_PERF_REVISION"],
                        "compiler": env["AOBUS_PERF_COMPILER"],
                        "build_mode": env["AOBUS_PERF_BUILD_MODE"],
                        "platform": env["AOBUS_PERF_PLATFORM"],
                        "icu_version": "78.3",
                    },
                    "measurements": [
                        {
                            "capability": "ordering",
                            "scenario": "construction",
                            "dataset": "none",
                            "input_count": 0,
                            "median_ns": 1,
                            "p95_ns": 2,
                        }
                    ],
                }
                Path(env["AOBUS_PERF_REPORT_JSON"]).write_text(json.dumps(report), encoding="utf-8")
                return 0

            output = io.StringIO()
            with (
                mock.patch.object(perf, "_revision", return_value="new-checkout+dirty") as revision,
                mock.patch.object(perf.build, "do_build") as build,
                mock.patch.object(perf.build, "validate_build_tree", return_value="gcc") as validate,
                mock.patch.object(perf, "run", side_effect=write_report),
                contextlib.redirect_stdout(output),
            ):
                self.assertEqual(perf.run_command(args), 0)

            report = json.loads(report_path.read_text(encoding="utf-8"))
            self.assertEqual(report["metadata"]["revision"], "unverified")
            self.assertIn("Source revision: unverified", output.getvalue())
            self.assertNotIn("new-checkout", output.getvalue())
            self.assertEqual(executable.read_bytes(), original)
            validate.assert_called_once_with(args, directory, expected_build_type="Release")
            self.assertEqual(report["metadata"]["compiler"], "gcc")
            self.assertEqual(report["metadata"]["build_mode"], "release")
            build.assert_not_called()
            revision.assert_not_called()


if __name__ == "__main__":
    unittest.main()
