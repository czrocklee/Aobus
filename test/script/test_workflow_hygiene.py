"""Tests for GitHub workflow hygiene: scope discovery and the pinned actionlint stage."""

import contextlib
import io
import os
import tempfile
import unittest
from argparse import Namespace
from pathlib import Path
from unittest import mock

from ao.__main__ import make_parser
from ao.command import format as format_command
from ao.command import hygiene
from ao.core import bazel_native as native
from ao.core import gitfiles


def _make_checkout(root: Path) -> None:
    """One small checkout: direct workflows plus the near misses that must stay out of scope."""
    names = (
        ".github/workflows/ci.yml",
        ".github/workflows/release.yaml",
        ".github/workflows/nested/deep.yml",  # nested YAML: GitHub Actions does not load it
        ".github/other.yml",  # repository YAML outside the workflows directory
        "doc/example.yml",
        "BUILD",  # the named Bazel file stays a whole-source citizen
        "lib/Foo.cpp",
    )
    for name in names:
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("# fixture\n", encoding="utf-8")


class WorkflowFileDiscoveryTest(unittest.TestCase):
    """The shared resolver admits direct workflow files next to named Bazel files."""

    def test_all_and_folder_scans_select_direct_workflows_and_the_root_build_file(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            _make_checkout(root)
            with (
                mock.patch.object(format_command, "PROJECT_ROOT", root),
                mock.patch.object(format_command.gitfiles, "PROJECT_ROOT", root),
            ):
                expected = [".github/workflows/ci.yml", ".github/workflows/release.yaml", "BUILD", "lib/Foo.cpp"]
                self.assertEqual(
                    format_command.resolve_files(Namespace(files=[], all=True, folder=[], commit=None)), expected
                )
                arguments = Namespace(files=[], all=False, folder=[".github/workflows"], commit=None)
                self.assertEqual(
                    format_command.resolve_files(arguments),
                    [".github/workflows/ci.yml", ".github/workflows/release.yaml"],
                )

    def test_changed_scope_keeps_workflows_but_drops_removed_and_nonworkflow_files(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            _make_checkout(root)
            changed_names = [
                ".github/workflows/ci.yml",
                ".github/workflows/removed.yml",  # deleted in the working tree
                ".github/other.yml",
                "BUILD",
                "lib/Foo.cpp",
            ]
            with (
                mock.patch.object(format_command, "PROJECT_ROOT", root),
                mock.patch.object(format_command.gitfiles, "PROJECT_ROOT", root),
                mock.patch.object(format_command.gitfiles, "changed_files", return_value=changed_names) as changed,
            ):
                arguments = Namespace(files=[], all=False, folder=[], commit=None)
                self.assertEqual(
                    format_command.resolve_files(arguments), [".github/workflows/ci.yml", "BUILD", "lib/Foo.cpp"]
                )
            changed.assert_called_once_with(None, suffixes=gitfiles.SOURCE_SUFFIXES)


class WorkflowHygieneGateTest(unittest.TestCase):
    """Workflow findings propagate through the check-only gate."""

    def test_actionlint_findings_fail_the_gate_at_the_subprocess_boundary(self):
        files = [".github/workflows/ci.yml", ".github/workflows/release.yaml", "lib/Foo.cpp"]
        tool = "/nix/store/aobus-actionlint-bin/actionlint"
        with (
            mock.patch.object(hygiene.format_command, "resolve_files", return_value=files),
            mock.patch.object(hygiene.format_command, "_file_exists", return_value=True),
            mock.patch.object(hygiene.format_command, "run_command", return_value=0),
            mock.patch.object(hygiene.test_audit, "run_command", return_value=0),
            mock.patch.object(hygiene.name_audit, "run_command", return_value=0),
            mock.patch.object(hygiene.sys, "platform", "linux"),
            mock.patch.dict(os.environ, {"AO_NIX_ACTIONLINT": tool}),
            mock.patch.object(hygiene.subprocess, "run", return_value=mock.Mock(returncode=1)) as actionlint,
            contextlib.redirect_stdout(io.StringIO()),
            contextlib.redirect_stderr(io.StringIO()),
        ):
            lint = mock.Mock(return_value=0)
            arguments = Namespace(files=[], all=False, folder=[], commit=None, path=None, jobs=1)
            self.assertEqual(hygiene.run_command(arguments, lint=("tidy", lint)), 1)
        actionlint.assert_called_once_with(
            [tool, "-no-color", ".github/workflows/ci.yml", ".github/workflows/release.yaml"],
            cwd=hygiene.PROJECT_ROOT,
        )
        lint.assert_called_once_with(files, False)

    def test_yaml_only_scope_runs_the_gate_without_native_tools_or_the_workflow_lint(self):
        arguments = make_parser().parse_args(["hygiene", "doc/example.yml"])
        with (
            mock.patch.object(native, "native_platform", return_value="windows"),
            mock.patch.object(hygiene.format_command, "_file_exists", return_value=True),
            mock.patch(
                "ao.core.windows_llvm_sdk.ready_root", side_effect=AssertionError("no LLVM SDK for a YAML-only scope")
            ),
            mock.patch(
                "ao.core.pythonenv.ensure_environment",
                side_effect=AssertionError("no managed Python for a YAML-only scope"),
            ),
            mock.patch.object(hygiene.subprocess, "run") as run,
            contextlib.redirect_stdout(io.StringIO()),
            contextlib.redirect_stderr(io.StringIO()),
        ):
            self.assertEqual(hygiene.run_command(arguments), 0)
        run.assert_not_called()


if __name__ == "__main__":
    unittest.main()
