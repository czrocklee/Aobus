"""Source paths are admitted before tool preparation and frozen scopes stay frozen."""

import contextlib
import io
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao.__main__ import make_parser
from ao.core.paths import PROJECT_ROOT
from ao.portal import (
    bazel_lint,
    build_env,
    prettier,
    source_analyzer,
    source_format,
    source_lint,
    source_scope,
    windows_portal,
)
from ao.portal.source_scope import Selection, SourceScope


class ValidationScopeTest(unittest.TestCase):
    def setUp(self):
        self.root = Path(self.enterContext(tempfile.TemporaryDirectory()))

    def run_portal(self, *arguments, module="ao", prepared_scope=None):
        env = {**os.environ, "PYTHONPATH": str(PROJECT_ROOT / "script"), "PYTHONDONTWRITEBYTECODE": "1"}
        env.pop("AOBUS_PREFLIGHT_SCOPE", None)
        env.pop("_AOBUS_WINDOWS_SCOPE_FILE", None)
        if prepared_scope is not None:
            marker = "_AOBUS_WINDOWS_SCOPE_FILE" if module == "ao.portal.windows_portal" else "AOBUS_PREFLIGHT_SCOPE"
            env[marker] = str(prepared_scope)
        return subprocess.run(
            [sys.executable, "-B", "-m", module, *arguments],
            cwd=PROJECT_ROOT,
            env=env,
            capture_output=True,
            text=True,
            timeout=30,
        )

    def record(self, arguments, scope):
        transfer = self.root / "scope.json"
        args = make_parser().parse_args(arguments)
        with mock.patch.dict(os.environ, {"AOBUS_PREFLIGHT_SCOPE": str(transfer)}):
            build_env.record_source_scope(args, scope)
        return transfer

    def test_missing_explicit_directory_is_not_an_empty_check(self):
        missing = str(self.root / "missing-folder")
        for module, route in (("ao", ["check", "format"]), ("ao.portal.build_env", ["check", "analyzer"])):
            with self.subTest(module=module):
                result = self.run_portal(*route, missing, module=module)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn(missing, result.stderr)

    def test_missing_irrelevant_path_prevents_any_format_rewrite(self):
        source = self.root / "Example.cpp"
        original = "int   answer=42;\n"
        source.write_text(original, encoding="utf-8")
        missing = str(self.root / "missing.txt")
        result = self.run_portal("format", str(source), missing)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn(missing, result.stderr)
        self.assertEqual(source.read_text(encoding="utf-8"), original)

    def test_existing_empty_directory_is_a_valid_no_work_scope(self):
        result = self.run_portal("check", "lint", str(self.root))
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_prepared_empty_scope_does_not_hide_removed_explicit_path(self):
        source = self.root / "removed.py"
        source.touch()
        argv = ["check", "lint", str(source)]
        transfer = self.record(argv, SourceScope())
        source.unlink()
        result = self.run_portal(*argv, prepared_scope=transfer)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn(str(source), result.stderr)

    def test_removed_frozen_scanned_file_fails_before_windows_native_preparation(self):
        source = self.root / "Scanned.cpp"
        source.touch()
        arguments = ["check", "lint", str(self.root)]
        transfer = self.record(arguments, SourceScope((source.as_posix(),)))
        source.unlink()
        with (
            mock.patch.dict(os.environ, {"_AOBUS_WINDOWS_SCOPE_FILE": str(transfer)}),
            mock.patch.object(source_scope, "resolve", side_effect=AssertionError("frozen scope must not rescan")),
            mock.patch.object(windows_portal, "prepare_build_environment") as native,
            contextlib.redirect_stderr(io.StringIO()) as diagnostics,
            self.assertRaises(SystemExit) as failure,
        ):
            windows_portal.main(arguments)
        self.assertEqual(failure.exception.code, 1)
        self.assertIn(source.as_posix(), diagnostics.getvalue())
        native.assert_not_called()

    def test_empty_frozen_scope_roundtrips_to_the_portal_without_rediscovery_or_rewrite(self):
        source = self.root / "Example.cpp"
        original = "int   answer=42;\n"
        source.write_text(original, encoding="utf-8")
        argv = ["format", str(source)]
        transfer = self.record(argv, SourceScope())
        result = self.run_portal(*argv, prepared_scope=transfer)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(source.read_text(encoding="utf-8"), original)


class ExternalDirectoryScopeTest(unittest.TestCase):
    def test_external_directory_scan_is_bounded_and_retains_explicit_file_provenance(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            scanned = root / "sources/nested/Generated.cpp"
            scanned.parent.mkdir(parents=True)
            scanned.touch()
            (root / "sources/notes.txt").touch()
            (root / "Other.cpp").touch()
            explicit = root / "Explicit.cpp"
            explicit.touch()
            with mock.patch.object(source_scope.git_files, "prettier_sources", return_value=[]):
                scope = source_scope.resolve(Selection(paths=(str(root / "sources"), str(explicit))))
            self.assertEqual(scope.files, (explicit.as_posix(), scanned.as_posix()))
            self.assertEqual(scope.explicit_files, (explicit.as_posix(),))

    def test_external_cpp_directory_still_requires_checkout_compile_ownership(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "Example.cpp").touch()
            with mock.patch.object(source_scope.git_files, "prettier_sources", return_value=[]):
                scope = source_scope.resolve(Selection(paths=(str(root),)))
            for service, request in (
                (source_lint.run, source_lint.LintRequest(tidy_check="aobus-example", report_only=True)),
                (source_analyzer.run, source_analyzer.AnalyzerRequest(report_only=True)),
            ):
                with (
                    self.subTest(service=service.__module__),
                    mock.patch.object(
                        bazel_lint.bazel_workspace, "workspace_path", side_effect=AssertionError("no native tree")
                    ),
                    contextlib.redirect_stdout(io.StringIO()),
                    contextlib.redirect_stderr(io.StringIO()) as diagnostics,
                ):
                    self.assertEqual(service(scope, request), 2)
                self.assertIn("outside the checkout", diagnostics.getvalue())

    def test_external_markdown_directory_keeps_prettier_containment_admission(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "guide.md").touch()
            with mock.patch.object(source_scope.git_files, "prettier_sources", return_value=[]):
                scope = source_scope.resolve(Selection(paths=(str(root),)))
            with (
                mock.patch.object(
                    prettier, "command", side_effect=AssertionError("containment must precede tool selection")
                ),
                contextlib.redirect_stdout(io.StringIO()),
                contextlib.redirect_stderr(io.StringIO()) as diagnostics,
            ):
                self.assertEqual(source_format.check(scope), 2)
            self.assertIn("inside the checkout", diagnostics.getvalue())


if __name__ == "__main__":
    unittest.main()
