"""Tests for ao.command.coverage gcov parsing and union-merge."""

import contextlib
import io
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao.command import coverage

GCOV_A = """\
        -:    0:Source:lib/audio/Foo.cpp
        -:    0:Graph:/tmp/build/coverage/foo.gcno
        -:    1:#include "Foo.h"
        3:    2:int foo() {
    #####:    3:  return bar();
        -:    4:}
"""

GCOV_B = """\
        -:    0:Source:lib/audio/Foo.cpp
        -:    1:#include "Foo.h"
        1:    2:int foo() {
        2:    3:  return bar();
        -:    4:}
"""


class ParseGcovTest(unittest.TestCase):
    def test_parse_extracts_source_and_line_counts(self):
        source, lines = coverage.parse_gcov_text(GCOV_A)
        self.assertEqual(source, "lib/audio/Foo.cpp")
        self.assertEqual(lines[1][0], None)
        self.assertEqual(lines[2][0], 3)
        self.assertEqual(lines[3][0], 0)
        self.assertEqual(lines[4][0], None)

    def test_unexecuted_exceptional_marker_counts_as_zero(self):
        _, lines = coverage.parse_gcov_text("    =====:    7:  throw;\n")
        self.assertEqual(lines[7][0], 0)

    def test_starred_counts_are_parsed(self):
        _, lines = coverage.parse_gcov_text("       4*:    9:  inline;\n")
        self.assertEqual(lines[9][0], 4)


GCOV_DUP = """\
        -:    0:Source:lib/audio/Ctor.cpp
        2:    5:  Thing::Thing() {
        3:    5:
        -:    7:  member_init;
        4:    7:  member_init;
"""


class ParseGcovDuplicateLineTest(unittest.TestCase):
    """gcov emits the same line twice for C1/C2 constructor thunks."""

    def test_duplicate_line_hits_accumulate_and_first_content_kept(self):
        _, lines = coverage.parse_gcov_text(GCOV_DUP)
        self.assertEqual(lines[5][0], 5)  # 2 + 3
        self.assertEqual(lines[5][1], "  Thing::Thing() {")  # first non-empty content kept

    def test_non_executable_then_executable_keeps_count(self):
        _, lines = coverage.parse_gcov_text(GCOV_DUP)
        self.assertEqual(lines[7][0], 4)  # None combined with 4


class MergeReportTest(unittest.TestCase):
    def test_union_merge_covers_lines_hit_in_any_translation_unit(self):
        _, first = coverage.parse_gcov_text(GCOV_A)
        _, second = coverage.parse_gcov_text(GCOV_B)
        merged: dict = {}
        coverage.merge_report(merged, first)
        coverage.merge_report(merged, second)
        self.assertEqual(merged[2][0], 4)  # 3 + 1
        self.assertEqual(merged[3][0], 2)  # missing in A, covered in B
        self.assertIsNone(merged[1][0])


class ContextBlocksTest(unittest.TestCase):
    def test_context_window_marks_missing_lines(self):
        _, lines = coverage.parse_gcov_text(GCOV_A)
        rows = coverage.context_blocks(lines, [3], before=1, after=1)
        self.assertEqual(len(rows), 3)
        self.assertIn("#####", rows[1])
        self.assertIn("return bar();", rows[1])

    def test_distant_blocks_are_separated(self):
        lines = {n: (1 if n != 1 and n != 50 else 0, f"line {n}") for n in [1, 2, 49, 50]}
        rows = coverage.context_blocks(lines, [1, 50], before=1, after=1)
        self.assertIn("--", rows)


class ScopedStatsTest(unittest.TestCase):
    def test_scoped_stats_filters_and_aggregates_file_rows(self):
        merged = {
            "app/linux-gtk/Foo.cpp": {
                1: (1, "hit"),
                2: (0, "miss"),
                3: (None, "comment"),
            },
            "app/linux-gtk/sub/Bar.cpp": {
                1: (2, "hit"),
                2: (3, "hit"),
            },
            "lib/core/Baz.cpp": {
                1: (0, "miss"),
            },
        }

        rows = coverage.scoped_stats(merged, ["app/linux-gtk"])

        self.assertEqual(
            rows,
            [
                ("app/linux-gtk/Foo.cpp", 1, 2, 1, 50.0),
                ("app/linux-gtk/sub/Bar.cpp", 2, 2, 0, 100.0),
            ],
        )

    def test_scope_accepts_trailing_slash(self):
        merged = {
            "app/linux-gtk/Foo.cpp": {
                1: (1, "hit"),
            },
        }

        self.assertEqual(len(coverage.scoped_stats(merged, ["app/linux-gtk/"])), 1)


class CoverageSuiteStatusTest(unittest.TestCase):
    def test_failed_suite_status_survives_partial_coverage_collection(self):
        with mock.patch.object(coverage, "run_suite", side_effect=[0, 7, 5]) as run_suite:
            status = coverage.run_coverage_tests(
                ("core", "tui", "gtk"),
                Path("/tmp/coverage-test"),
                "[concurrency]",
            )

        self.assertEqual(status, 7)
        self.assertEqual(run_suite.call_count, 3)
        run_suite.assert_any_call("gtk", Path("/tmp/coverage-test"), test_filter="[concurrency]")


class CoverageMeasurementTest(unittest.TestCase):
    def test_empty_and_unmeasured_scopes_cannot_report_success(self):
        measured = {"lib/audio/Foo.cpp": {2: (1, "covered")}}
        for merged, scopes in (
            ({}, None),
            ({"lib/audio/Foo.cpp": {1: (None, "comment")}}, None),
            (measured, ["missing"]),
            (measured, ["lib/audio", "missing"]),
        ):
            with self.subTest(scopes=scopes), contextlib.redirect_stdout(io.StringIO()) as output:
                with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                    coverage.report(merged, 40, scopes)
                self.assertNotIn("100.00%", output.getvalue())

    def test_gcov_failure_preserves_diagnostics_and_rejects_stale_reports(self):
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary)
            (build / "lib").mkdir()
            (build / "lib" / "foo.gcda").touch()
            stale = build / "stale.gcov"
            stale.write_text(GCOV_B, encoding="utf-8")
            for status in (0, 1):
                errors = io.StringIO()
                result = subprocess.CompletedProcess([], status, "", "invalid gcno version")
                with mock.patch.object(coverage.subprocess, "run", return_value=result):
                    with contextlib.redirect_stderr(errors), self.assertRaises(SystemExit):
                        coverage.collect_coverage(build)
                self.assertIn("incomplete", errors.getvalue())
                if status:
                    self.assertIn("invalid gcno version", errors.getvalue())
                self.assertTrue(stale.exists())

    def test_successful_extraction_merges_only_fresh_project_reports(self):
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary)
            (build / "lib").mkdir()
            (build / "lib" / "first.gcda").touch()
            (build / "lib" / "second.gcda").touch()
            source = coverage.PROJECT_ROOT / "lib" / "audio" / "Foo.cpp"

            def extract(argv, *, cwd, **kwargs):
                text = GCOV_A if "first.gcda" in argv[1] else GCOV_B
                (cwd / "Foo.cpp.gcov").write_text(text.replace("lib/audio/Foo.cpp", str(source)), encoding="utf-8")
                return subprocess.CompletedProcess(argv, 0, "", "")

            with mock.patch.object(coverage.subprocess, "run", side_effect=extract):
                merged = coverage.collect_coverage(build)
            self.assertEqual(coverage.file_stats(merged["lib/audio/Foo.cpp"]), (2, 2, 0, 100.0))


if __name__ == "__main__":
    unittest.main()
