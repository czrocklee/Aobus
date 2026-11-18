"""Path-sensitive analysis resolves a frozen scope and propagates native verdicts."""

import contextlib
import io
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao.portal import source_analyzer as analyze
from ao.portal import source_scope
from ao.portal.source_analyzer import AnalyzerRequest
from ao.portal.source_scope import SourceScope


class AnalyzerServiceTest(unittest.TestCase):
    def setUp(self):
        self.root = Path(self.enterContext(tempfile.TemporaryDirectory()))
        for module in (analyze, source_scope):
            self.enterContext(mock.patch.object(module, "PROJECT_ROOT", self.root))
        self.runner = self.enterContext(mock.patch.object(analyze.bazel_lint, "run_analyzer", return_value=0))
        self.enterContext(contextlib.redirect_stdout(io.StringIO()))
        self.enterContext(contextlib.redirect_stderr(io.StringIO()))

    def source(self, relative):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("// probe\n", encoding="utf-8")
        return str(path)

    def test_explicit_files_are_canonicalized_and_deduplicated_before_delegation(self):
        probe = self.source("lib/sample/Sample.cpp")
        other = self.source("lib/sample/Other.cpp")
        scope = SourceScope((probe, "lib/sample/Other.cpp", "lib/sample/Other.cpp"), (probe,))
        request = AnalyzerRequest()
        self.assertEqual(analyze.run(scope, request), 0)
        self.runner.assert_called_once_with(request, (probe, other), explicit_files=(probe,))

    def test_batch_scope_delegates_without_explicit_files(self):
        probe = self.source("lib/sample/Sample.cpp")
        request = AnalyzerRequest()
        self.assertEqual(analyze.run(SourceScope(("lib/sample/Sample.cpp",)), request), 0)
        self.runner.assert_called_once_with(request, (probe,), explicit_files=())

    def test_checkout_lint_fixtures_and_infrastructure_are_excluded_even_when_explicit(self):
        kept = self.source("lib/sample/Sample.cpp")
        fixture = self.source("test/lint/fixture/StyleCase.cpp")
        infrastructure = self.source("test/lint/Runner.cpp")
        request = AnalyzerRequest()
        self.assertEqual(analyze.run(SourceScope((kept, fixture, infrastructure), (fixture,)), request), 0)
        self.runner.assert_called_once_with(request, (kept,), explicit_files=())

    def test_empty_or_non_cpp_scope_does_not_prepare_a_native_context(self):
        document = self.source("README.md")
        for scope in (SourceScope(), SourceScope((document,), (document,))):
            with self.subTest(scope=scope):
                self.assertEqual(analyze.run(scope, AnalyzerRequest()), 0)
        self.runner.assert_not_called()

    def test_missing_selected_path_fails_before_native_preparation(self):
        with self.assertRaisesRegex(ValueError, "does not exist"):
            analyze.run(SourceScope(("lib/Missing.cpp",)), AnalyzerRequest())
        self.runner.assert_not_called()

    def test_invalid_diagnostic_arguments_fail_before_source_resolution(self):
        for argument in ("--fix", "--export-fixes=fixes.yaml", "@responses.txt"):
            with self.subTest(argument=argument), mock.patch.object(analyze, "analyzable_scope") as scope:
                with self.assertRaises(ValueError):
                    analyze.run(SourceScope(), AnalyzerRequest(tidy_args=(argument,)))
            scope.assert_not_called()
        self.runner.assert_not_called()

    def test_findings_are_gated_by_default_and_report_only_does_not_rewrite_engine_status(self):
        source = self.source("lib/Sample.cpp")
        for advisory, status in ((False, 0), (False, 1), (True, 1), (True, 2)):
            with self.subTest(advisory=advisory, status=status):
                self.runner.return_value = status
                request = AnalyzerRequest(report_only=advisory)
                self.assertEqual(request.fail_on_diagnostics, not advisory)
                self.assertEqual(analyze.run(SourceScope((source,)), request), status)


if __name__ == "__main__":
    unittest.main()
