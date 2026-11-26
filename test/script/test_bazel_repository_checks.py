"""Prevent repository checks from succeeding without checking their inputs."""

import contextlib
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao.core import paths
from ao.portal import bazel_repository_checks as checks
from ao.portal import buildifier, documentation_checks, git_files, prettier, source_format, source_lint, source_scope


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

    def manifest_args(self, *names):
        args = []
        for name in names:
            command = [str(self.root / name)]
            manifest = self.write(f"{name}.json", json.dumps({"name": name, "command": command}))
            args.extend(["--check-tool-manifest", str(manifest)])
        return args

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
            self.assertEqual(checks.run_workflow_lint(root=self.root, tool=str(self.root / "actionlint")), 1)

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
            mock.patch.object(source_lint, "actionlint_executable", side_effect=AssertionError("ambient actionlint")),
            mock.patch.object(subprocess, "run", side_effect=run),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            self.assertEqual(checks.run_workflow_lint(root=self.root, tool=actionlint), 1)

    def test_format_partition_keeps_non_doc_markdown_in_the_source_check(self):
        docs = source_scope.SourceScope(("README.md",))
        sources = ("test/fixture.md", "lib/file.cpp")
        whole = source_scope.SourceScope(docs.files + sources)
        ruff = self.root / "ruff"
        with (
            mock.patch.dict(os.environ, {"TEST_TMPDIR": ""}),
            mock.patch.object(documentation_checks, "markdown_scope", return_value=docs),
            mock.patch.object(source_scope, "resolve", return_value=whole),
            mock.patch.object(source_format, "check", return_value=7) as formatter,
        ):
            self.assertEqual(checks.main([*self.manifest_args("prettier"), "format-docs"]), 7)
            manifests = self.manifest_args("clang-format", "buildifier", "prettier")
            self.assertEqual(checks.main([*manifests, "--ruff", str(ruff), "format-sources"]), 7)
        self.assertEqual([call.args[0].files for call in formatter.call_args_list], [docs.files, sources])
        self.assertEqual([call.kwargs["ruff"] for call in formatter.call_args_list], [None, ruff])

    def test_missing_manifest_rejects_the_check_before_any_portal_tool_lookup(self):
        ruff = ["--ruff", str(self.root / "ruff")]
        for entry, args in (
            ("run_format", [*ruff, "format-sources"]),
            ("run_format", [*ruff, *self.manifest_args("clang-format", "prettier"), "format-sources"]),
            ("run_format", ["format-docs"]),
            ("run_bazel_lint", ["bazel-lint"]),
            ("run_workflow_lint", ["workflow-lint"]),
        ):
            with (
                self.subTest(args=args),
                mock.patch.object(checks, entry) as run,
                contextlib.redirect_stderr(io.StringIO()),
            ):
                self.assertEqual(checks.main(args), 1)
                run.assert_not_called()

    def test_source_format_reports_discovery_errors_at_the_command_boundary(self):
        manifests = self.manifest_args("clang-format", "buildifier", "prettier")
        for failure in (git_files.GitBindingError("invalid Git binding"), OSError("cannot read checkout")):
            with (
                self.subTest(failure=failure),
                mock.patch.object(documentation_checks, "markdown_scope", return_value=source_scope.SourceScope()),
                mock.patch.object(source_scope, "resolve", side_effect=failure),
                contextlib.redirect_stderr(io.StringIO()) as errors,
            ):
                self.assertEqual(checks.main([*manifests, "--ruff", str(self.root / "ruff"), "format-sources"]), 1)
                self.assertIn(str(failure), errors.getvalue())

    def test_formatter_programming_error_propagates_from_the_command_boundary(self):
        manifests = self.manifest_args("prettier")
        with (
            mock.patch.dict(os.environ, {"TEST_TMPDIR": ""}),
            mock.patch.object(documentation_checks, "markdown_scope", return_value=source_scope.SourceScope()),
            mock.patch.object(source_format, "check", side_effect=TypeError("unexpected formatter state")),
            self.assertRaisesRegex(TypeError, "unexpected formatter state"),
        ):
            checks.main([*manifests, "format-docs"])

    def test_source_formatter_requires_an_absolute_ruff_executable(self):
        manifests = self.manifest_args("clang-format", "buildifier", "prettier")
        with (
            contextlib.redirect_stderr(io.StringIO()),
            mock.patch.object(checks, "run_format") as run,
        ):
            self.assertEqual(checks.main([*manifests, "format-sources"]), 1)
            self.assertEqual(checks.main([*manifests, "--ruff", "ruff", "format-sources"]), 1)
        run.assert_not_called()

    def test_bad_manifest_rejects_all_formatter_stages_even_after_unavailable(self):
        reason = "run ao setup buildifier explicitly"
        unavailable = self.write("unavailable.json", json.dumps({"name": "buildifier", "unavailable": reason}))
        for content in (
            '{"broken":',
            '{"name":"prettier","command":["relative"]}',
            '{"name":"prettier","unavailable":""}',
        ):
            with (
                self.subTest(content=content),
                mock.patch.object(checks, "run_format") as run,
                contextlib.redirect_stderr(io.StringIO()),
            ):
                bad = self.write("bad.json", content)
                args = [
                    *self.manifest_args("clang-format"),
                    "--check-tool-manifest",
                    str(unavailable),
                    "--check-tool-manifest",
                    str(bad),
                    "--ruff",
                    str(self.root / "ruff"),
                    "format-sources",
                ]
                self.assertEqual(checks.main(args), 1)
                run.assert_not_called()

    def test_unavailable_formatter_only_fails_when_its_stage_has_files(self):
        base = ("script/foo.py", "test/fixture.md")
        reason = "run ao setup llvm-sdk explicitly"
        unavailable = self.write("unavailable.json", json.dumps({"name": "clang-format", "unavailable": reason}))
        args = [
            "--check-tool-manifest",
            str(unavailable),
            *self.manifest_args("buildifier", "prettier"),
            "--ruff",
            str(self.root / "ruff"),
            "format-sources",
        ]
        for files, expected, expected_tools in (
            ((*base, "lib/file.cpp"), 2, ("ruff", "prettier")),
            (base, 0, ("ruff", "prettier")),
            ((), 0, ()),
        ):
            stderr = io.StringIO()
            with (
                self.subTest(files=files),
                mock.patch.object(documentation_checks, "markdown_scope", return_value=source_scope.SourceScope(())),
                mock.patch.object(source_scope, "resolve", return_value=source_scope.SourceScope(files)),
                mock.patch.object(source_scope, "require_files"),
                mock.patch.object(prettier, "PROJECT_ROOT", self.root),
                mock.patch.object(subprocess, "run", return_value=subprocess.CompletedProcess([], 0, "", "")) as run,
                mock.patch.object(
                    source_format, "clang_format_executable", side_effect=AssertionError("ambient Clang")
                ),
                mock.patch.object(prettier, "command", side_effect=AssertionError("ambient Prettier")),
                mock.patch.dict(os.environ, {"TEST_TMPDIR": ""}),
                contextlib.redirect_stdout(io.StringIO()),
                contextlib.redirect_stderr(stderr),
            ):
                for path in (".prettierrc.json", ".prettierignore", ".gitignore", *files):
                    self.write(path)
                self.assertEqual(checks.main(args), expected)
            calls = [call.args[0] for call in run.call_args_list]
            self.assertEqual([Path(argv[0]) for argv in calls], [self.root / name for name in expected_tools])
            self.assertTrue(all("--check" in argv for argv in calls))
            self.assertEqual(reason in stderr.getvalue(), expected != 0)

    def test_unavailable_workflow_tool_still_fails_before_the_checker_runs(self):
        reason = "The pinned actionlint repository is only available on Linux"
        manifest = self.write("command.json", json.dumps({"name": "actionlint", "unavailable": reason}))
        stderr = io.StringIO()
        with mock.patch.object(checks, "run_workflow_lint") as run, contextlib.redirect_stderr(stderr):
            self.assertEqual(checks.main(["--check-tool-manifest", str(manifest), "workflow-lint"]), 1)
        self.assertIn(reason, stderr.getvalue())
        run.assert_not_called()

    def test_live_formatter_passes_the_declared_node_and_package_command(self):
        anchor = self.write("VERSION.txt", "1.2.3\n")
        command = [str(self.root / "node.exe"), str(self.root / "package/bin/prettier.cjs")]
        manifest = self.write("prettier.json", json.dumps({"name": "prettier", "command": command}))
        location = "+check_tools+aobus_prettier/command.json"
        from ao.graph import bazel_live_checkout

        def reexec(root, _entry, arguments, *, environment):
            self.assertEqual(root, self.root)
            self.assertEqual(arguments, ["--check-tool-manifest", str(manifest), "format-docs"])
            self.assertEqual(environment[bazel_live_checkout.PROJECT_ROOT_ENV], str(root))
            return checks.main(arguments)

        with (
            mock.patch.object(bazel_live_checkout, "_official_path", return_value=manifest) as resolve,
            mock.patch.object(bazel_live_checkout, "reexec", side_effect=reexec),
            mock.patch.object(checks, "run_format", return_value=0) as run,
        ):
            self.assertEqual(
                checks.main(["--live-checkout", str(anchor), "--check-tool-manifest", location, "format-docs"]), 0
            )
        self.assertEqual(resolve.call_args.args[0], location)
        run.assert_called_once_with(docs=True, commands={"prettier": tuple(command)}, ruff=None)

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
            tool = str(self.root / "buildifier")
            self.assertEqual(checks.run_bazel_lint(root=self.root, tool=tool), 7)
        lint.assert_called_once_with(["BUILD.bazel"], provision=False, tool=tool)

    def test_missing_python_tool_cannot_fall_back_to_the_portal_interpreter(self):
        with (
            mock.patch.dict(os.environ, {"AOBUS_TOOLING_PYTHON": sys.executable}),
            mock.patch.object(subprocess, "run") as run,
            contextlib.redirect_stderr(io.StringIO()),
        ):
            for command in ("ruff", "mypy"):
                with self.subTest(command=command):
                    self.assertEqual(checks.main([command]), 1)
                    self.assertEqual(checks.main([f"--{command}", command, command]), 1)
            run.assert_not_called()

    def test_mypy_checks_production_sources_and_reports_a_failed_tool(self):
        python = self.root / "selected-python"
        temporary = self.root / "tmp"
        self.addCleanup(setattr, sys, "pycache_prefix", sys.pycache_prefix)

        def run(argv, *, cwd):
            self.assertEqual(argv[:5], (str(python), "-I", "-B", "-m", "mypy"))
            self.assertEqual(argv[-1], "script/ao")
            self.assertNotIn("test/script", argv)
            self.assertEqual(cwd, self.root)
            self.assertTrue(Path(os.environ["MYPY_CACHE_DIR"]).is_relative_to(temporary))
            return subprocess.CompletedProcess(argv, 7)

        with (
            mock.patch.object(sys, "platform", "darwin"),
            mock.patch.dict(os.environ, {"TEST_TMPDIR": str(temporary), "MYPY_CACHE_DIR": str(self.root / "ambient")}),
            mock.patch.object(subprocess, "run", side_effect=run),
            contextlib.redirect_stdout(io.StringIO()),
            contextlib.redirect_stderr(io.StringIO()),
        ):
            self.assertEqual(checks.run_mypy(["test/script", "script/ao"], root=self.root, tool=python), 1)


if __name__ == "__main__":
    unittest.main()
