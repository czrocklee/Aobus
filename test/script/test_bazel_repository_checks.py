"""Prevent repository checks from succeeding without checking their inputs."""

import contextlib
import io
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao.core import paths
from ao.portal import bazel_repository_checks as checks
from ao.portal import buildifier, documentation_checks, git_files, source_format, source_lint, source_scope


class RepositoryChecksTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="ao-repository-checks-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()

    def write(self, name, text=""):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        return path

    def test_live_entry_overrides_the_snapshot_root_and_returns_the_child_failure(self):
        anchor = self.write("VERSION.txt", "1.2.3\n")
        entry = self.write(
            "script/ao/portal/bazel_repository_checks.py",
            "import os, sys\n"
            "from pathlib import Path\n"
            "assert Path.cwd() == Path(os.environ['AOBUS_BAZEL_TEST_PROJECT_ROOT'])\n"
            "assert sys.argv[1:] == ['docs']\n"
            "raise SystemExit(17)\n",
        )
        # The parent may already be executing a symlink to the live entry.
        # Comparing its resolved path to the child is not a reason to skip.
        with (
            mock.patch.dict(os.environ, {"AOBUS_BAZEL_TEST_PROJECT_ROOT": str(self.root / "snapshot")}),
            mock.patch.object(checks, "__file__", str(entry)),
        ):
            self.assertEqual(checks.main(["--live-checkout", str(anchor), "docs"]), 17)

    def test_tree_mismatch_cannot_pass_by_checking_the_current_checkout(self):
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(checks.run_workflow_lint(root=self.root), 1)

    def test_workflow_lint_checks_direct_yaml_and_propagates_findings(self):
        self.write(".github/workflows/ci.yaml", "name: ci\n")
        self.write(".github/workflows/nested/other.yml", "name: unrelated\n")
        self.write(".github/workflows/notes.md")
        actionlint = str(self.root / "actionlint")

        def run(argv, *, cwd, **kwargs):
            self.assertEqual(argv, [actionlint, "-no-color", ".github/workflows/ci.yaml"])
            self.assertEqual(cwd, self.root)
            return subprocess.CompletedProcess(argv, 1)

        with (
            mock.patch.object(paths, "PROJECT_ROOT", self.root),
            mock.patch.object(git_files, "PROJECT_ROOT", self.root),
            mock.patch.object(source_scope, "PROJECT_ROOT", self.root),
            mock.patch.object(source_lint, "PROJECT_ROOT", self.root),
            mock.patch.object(sys, "platform", "linux"),
            mock.patch.dict(os.environ, {"AO_NIX_ACTIONLINT": actionlint}),
            mock.patch.object(subprocess, "run", side_effect=run),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            self.assertEqual(checks.run_workflow_lint(root=self.root), 1)

    def test_format_partition_keeps_non_doc_markdown_in_the_source_check(self):
        docs = source_scope.SourceScope(("README.md",))
        sources = ("test/fixture.md", "lib/file.cpp")
        whole = source_scope.SourceScope(docs.files + sources)
        with (
            mock.patch.dict(os.environ, {"TEST_TMPDIR": ""}),
            mock.patch.object(documentation_checks, "markdown_scope", return_value=docs),
            mock.patch.object(source_scope, "resolve", return_value=whole),
            mock.patch.object(source_format, "check", return_value=7) as formatter,
        ):
            self.assertEqual(checks.main(["format-docs"]), 7)
            self.assertEqual(checks.main(["format-sources"]), 7)
        self.assertEqual([call.args[0].files for call in formatter.call_args_list], [docs.files, sources])

    def test_bazel_lint_checks_root_starlark_without_requiring_a_diff(self):
        self.write("BUILD.bazel")
        self.write("README.md")
        with (
            mock.patch.object(paths, "PROJECT_ROOT", self.root),
            mock.patch.object(git_files, "PROJECT_ROOT", self.root),
            mock.patch.object(source_scope, "PROJECT_ROOT", self.root),
            mock.patch.object(git_files, "_git_lines_if_repository", return_value=None),
            mock.patch.object(buildifier, "lint_paths", return_value=7) as lint,
        ):
            self.assertEqual(checks.run_bazel_lint(root=self.root), 7)
        lint.assert_called_once_with(["BUILD.bazel"], provision=False)

    def test_mypy_checks_production_sources_and_reports_a_failed_tool(self):
        python = self.root / "selected-python"
        temporary = self.root / "tmp"
        self.addCleanup(setattr, sys, "pycache_prefix", sys.pycache_prefix)

        def run(argv, *, cwd):
            self.assertEqual(argv[0], str(python))
            self.assertEqual(argv[argv.index("-m") + 1], "mypy")
            self.assertEqual(argv[-1], "script/ao")
            self.assertNotIn("test/script", argv)
            self.assertEqual(cwd, self.root)
            self.assertTrue(Path(os.environ["MYPY_CACHE_DIR"]).is_relative_to(temporary))
            return subprocess.CompletedProcess(argv, 7)

        with (
            mock.patch.object(sys, "platform", "linux"),
            mock.patch.dict(os.environ, {"TEST_TMPDIR": str(temporary), "MYPY_CACHE_DIR": str(self.root / "ambient")}),
            mock.patch.object(subprocess, "run", side_effect=run),
            contextlib.redirect_stdout(io.StringIO()),
            contextlib.redirect_stderr(io.StringIO()),
        ):
            self.assertEqual(checks.run_mypy(["test/script", "script/ao"], root=self.root, python=python), 1)


if __name__ == "__main__":
    unittest.main()
