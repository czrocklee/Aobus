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
    """The shared resolver admits direct workflows, repository YAML, and named Bazel files."""

    def test_all_and_folder_scans_select_workflows_repository_yaml_and_the_root_build_file(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            _make_checkout(root)
            with (
                mock.patch.object(format_command, "PROJECT_ROOT", root),
                mock.patch.object(format_command.gitfiles, "PROJECT_ROOT", root),
            ):
                # Every .yaml/.yml file is a Prettier source, including
                # non-workflow and nested YAML; only the format scope is broad,
                # not the actionlint scope.
                expected = [
                    ".github/other.yml",
                    ".github/workflows/ci.yml",
                    ".github/workflows/nested/deep.yml",
                    ".github/workflows/release.yaml",
                    "BUILD",
                    "doc/example.yml",
                    "lib/Foo.cpp",
                ]
                self.assertEqual(
                    format_command.resolve_files(Namespace(files=[], all=True, folder=[], commit=None)), expected
                )
                arguments = Namespace(files=[], all=False, folder=[".github/workflows"], commit=None)
                self.assertEqual(
                    format_command.resolve_files(arguments),
                    [
                        ".github/workflows/ci.yml",
                        ".github/workflows/nested/deep.yml",
                        ".github/workflows/release.yaml",
                    ],
                )

    def test_changed_scope_keeps_workflows_and_repository_yaml_but_drops_removed_files(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            _make_checkout(root)
            # Sorted like the real changed_paths contract.
            changed_names = [
                ".github/other.yml",
                ".github/workflows/ci.yml",
                ".github/workflows/nested/deep.yml",
                ".github/workflows/removed.yml",  # deleted in the working tree
                "BUILD",
                "doc/example.yml",
                "lib/Foo.cpp",
            ]
            with (
                mock.patch.object(format_command, "PROJECT_ROOT", root),
                mock.patch.object(format_command.gitfiles, "PROJECT_ROOT", root),
                mock.patch.object(format_command.gitfiles, "changed_paths", return_value=changed_names) as changed,
            ):
                arguments = Namespace(files=[], all=False, folder=[], commit=None)
                self.assertEqual(
                    format_command.resolve_files(arguments),
                    [
                        ".github/other.yml",
                        ".github/workflows/ci.yml",
                        ".github/workflows/nested/deep.yml",
                        "BUILD",
                        "doc/example.yml",
                        "lib/Foo.cpp",
                    ],
                )
            changed.assert_called_once_with(None)


class WorkflowHygieneGateTest(unittest.TestCase):
    """Workflow findings propagate through the check-only gate."""

    def test_actionlint_findings_fail_the_gate_at_the_subprocess_boundary(self):
        # The scope mixes direct workflows with nested, repository, and
        # documentation YAML; actionlint still receives only the two direct
        # workflow paths.
        files = [
            ".github/workflows/ci.yml",
            ".github/workflows/release.yaml",
            ".github/workflows/nested/deep.yml",
            ".github/other.yml",
            "doc/example.yml",
            "lib/Foo.cpp",
        ]
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

    def test_yaml_only_scope_runs_prettier_without_native_tools_or_subprocesses(self):
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
            # Prettier is mocked at the tool boundary: the pinned tool must
            # never be provisioned or spawned inside this unit test.
            mock.patch("ao.core.prettier.run", return_value=0) as prettier_run,
            mock.patch.object(hygiene.subprocess, "run") as run,
            contextlib.redirect_stdout(io.StringIO()),
            contextlib.redirect_stderr(io.StringIO()),
        ):
            self.assertEqual(hygiene.run_command(arguments), 0)
        prettier_run.assert_called_once_with(["doc/example.yml"], check=True)
        run.assert_not_called()


if __name__ == "__main__":
    unittest.main()
