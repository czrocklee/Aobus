"""Tests for tidy/analyze file classification and tidy fix filtering."""

import tempfile
import unittest
from pathlib import Path

from ao.command import analyze, tidy


class TidyClassifyTest(unittest.TestCase):
    def test_production_code_is_strict(self):
        self.assertEqual(tidy.classify(Path("/repo/lib/audio/Foo.cpp"), explicit=False), "STRICT")
        self.assertEqual(tidy.classify(Path("/repo/app/linux-gtk/Window.cpp"), explicit=False), "STRICT")
        self.assertEqual(tidy.classify(Path("/repo/include/aobus/Foo.h"), explicit=False), "STRICT")

    def test_lint_checker_sources_are_strict_despite_tool_prefix(self):
        self.assertEqual(tidy.classify(Path("/repo/tool/lint/Check.cpp"), explicit=False), "STRICT")

    def test_test_code_is_relaxed(self):
        self.assertEqual(tidy.classify(Path("/repo/test/unit/audio/FooTest.cpp"), explicit=False), "RELAXED")

    def test_catch2_main_is_ignored(self):
        self.assertEqual(tidy.classify(Path("/repo/test/main.cpp"), explicit=False), "IGNORE")

    def test_lint_fixtures_only_checked_when_explicit(self):
        fixture = Path("/repo/test/integration/lint/fixture/aobus-check/case.cpp")
        self.assertEqual(tidy.classify(fixture, explicit=True), "STRICT")
        self.assertEqual(tidy.classify(fixture, explicit=False), "IGNORE")

    def test_other_lint_integration_files_are_ignored(self):
        helper = Path("/repo/test/integration/lint/fixture_helper.cpp")
        self.assertEqual(tidy.classify(helper, explicit=True), "IGNORE")


class TidySplitExistingTest(unittest.TestCase):
    def test_files_outside_the_repository_keep_their_absolute_path(self):
        with tempfile.NamedTemporaryFile(suffix=".py", dir="/tmp") as outside:
            cpp_files, python_files = tidy.split_existing([outside.name])
            self.assertEqual(cpp_files, [])
            self.assertEqual(python_files, [Path(outside.name).resolve().as_posix()])


class AnalyzeClassifyTest(unittest.TestCase):
    def test_project_sources_are_analyzed(self):
        self.assertTrue(analyze.classify(Path("/repo/lib/audio/Foo.cpp")))
        self.assertTrue(analyze.classify(Path("/repo/test/unit/audio/FooTest.cpp")))

    def test_lint_integration_and_main_are_skipped(self):
        self.assertFalse(analyze.classify(Path("/repo/test/integration/lint/fixture/x/case.cpp")))
        self.assertFalse(analyze.classify(Path("/repo/test/main.cpp")))


class AnalyzerChecksTest(unittest.TestCase):
    def test_single_check_overrides_groups(self):
        self.assertEqual(
            analyze.analyzer_checks(alpha=True, only="clang-analyzer-core.NullDereference"),
            "-*,clang-analyzer-core.NullDereference",
        )

    def test_alpha_appends_experimental_group(self):
        checks = analyze.analyzer_checks(alpha=True, only=None)
        self.assertIn("clang-analyzer-alpha.*", checks)
        self.assertNotIn("clang-analyzer-alpha.*", analyze.analyzer_checks(alpha=False, only=None))


FIXES_YAML = """\
---
MainSourceFile:  '/repo/lib/Foo.cpp'
Diagnostics:
  - DiagnosticName:  readability-identifier-naming
    DiagnosticMessage:
      Message:         'invalid case style'
      FilePath:        '/repo/lib/Foo.cpp'
      Replacements:
        - FilePath:        '/repo/lib/Foo.cpp'
          Offset:          10
  - DiagnosticName:  modernize-use-nullptr
    DiagnosticMessage:
      Message:         'use nullptr'
      FilePath:        '/repo/lib/Foo.cpp'
      Replacements:
        - FilePath:        '/repo/lib/Foo.cpp'
          Offset:          20
...
"""


class FilterFixesYamlTest(unittest.TestCase):
    def test_identifier_naming_fixes_are_dropped(self):
        filtered = tidy.filter_fixes_yaml(FIXES_YAML)
        self.assertNotIn("readability-identifier-naming", filtered)
        self.assertNotIn("Offset:          10", filtered)
        self.assertIn("modernize-use-nullptr", filtered)
        self.assertIn("Offset:          20", filtered)
        self.assertIn("MainSourceFile", filtered)


if __name__ == "__main__":
    unittest.main()
