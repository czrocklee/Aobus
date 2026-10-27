"""Tests for ao.command.coverage gcov parsing and union-merge."""

import contextlib
import io
import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao.command import coverage
from ao.command import test as test_command

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

# The mid-section noise `gcov -t` actually emits: a staleness lineno-0 record
# that is not a section header, function-summary blocks after a `----` ruler
# (a mangled name followed by repeated line records), and content that merely
# starts with "Source:". Only a lineno-0 `Source:` header starts a section.
GCOV_STDOUT_FIRST = """\
        -:    0:Source:lib/x.cpp
        -:    0:Graph:/tmp/build/lib/x.gcno
        -:    0:Data:/tmp/build/lib/x.gcda
        -:    0:Runs:2
        -:    0:Source is newer than graph
        -:    1:#include "x.h"
        3:    2:  int pick = flag ? 1 : 0;  // colon stays content
    #####:    3:  return pick;
        -:    4:Source: in a string, not a section header
------------------
_ZN1xC1Ev:
    #####:    2:  int pick = flag ? 1 : 0;  // colon stays content
        -:    5:}
"""

GCOV_STDOUT_SECOND = """\
        -:    0:Source:lib/y.cpp
        -:    0:Runs:1
        7:    1:int y() { return 1; }
"""

# A leading non-section line must be ignored, not prepended to a block.
GCOV_STDOUT = "Creating 'x.cpp.gcov'\n" + GCOV_STDOUT_FIRST + GCOV_STDOUT_SECOND

# The shape gcov prints when it cannot open the recorded source: lineno-0
# headers only, no line records (observed with exit 0 on gcov 16.2).
GCOV_HEADER_ONLY = """\
        -:    0:Source:lib/audio/Foo.cpp
        -:    0:Graph:/tmp/build/lib/x.gcno
        -:    0:Data:/tmp/build/lib/x.gcda
        -:    0:Runs:2
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


class SplitGcovStdoutTest(unittest.TestCase):
    """`gcov -t --preserve-paths` prints every .gcov text on stdout; only the
    lineno-0 `Source:` record starts a new per-source section."""

    def test_stdout_splits_into_sections_at_source_headers_only(self):
        # A block is exactly the text gcov would write into one .gcov file.
        blocks = coverage._split_gcov_stdout(GCOV_STDOUT)
        self.assertEqual(len(blocks), 2)
        self.assertEqual(blocks[0].splitlines(), GCOV_STDOUT_FIRST.splitlines())
        self.assertEqual(blocks[1].splitlines(), GCOV_STDOUT_SECOND.splitlines())

    def test_sections_round_trip_through_the_unchanged_parser(self):
        first_source, first_lines = coverage.parse_gcov_text(coverage._split_gcov_stdout(GCOV_STDOUT)[0])
        self.assertEqual(first_source, "lib/x.cpp")
        self.assertEqual(
            first_lines,
            {
                1: (None, '#include "x.h"'),
                2: (3, "  int pick = flag ? 1 : 0;  // colon stays content"),
                3: (0, "  return pick;"),
                4: (None, "Source: in a string, not a section header"),
                5: (None, "}"),
            },
        )
        second_source, second_lines = coverage.parse_gcov_text(coverage._split_gcov_stdout(GCOV_STDOUT)[1])
        self.assertEqual(second_source, "lib/y.cpp")
        self.assertEqual(second_lines, {1: (7, "int y() { return 1; }")})

    def test_stdout_without_source_headers_yields_no_sections(self):
        self.assertEqual(coverage._split_gcov_stdout("Lines executed:0.00% of 0\n"), [])


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
    """The shared-suite runner contract: run all, keep the first failure, and
    require a global match only for an explicitly filtered run."""

    @staticmethod
    def _suite_kwargs(test_filter):
        # The coverage invocation is deliberately unsharded and reports match
        # counts only when the caller supplied an explicit filter.
        return {
            "test_filter": test_filter,
            "shards": 1,
            "report_matches": bool(test_filter),
            "allow_no_tests": bool(test_filter),
        }

    def test_failed_suite_status_survives_partial_coverage_collection(self):
        outcomes = [
            test_command.SuiteOutcome(0, 1),
            test_command.SuiteOutcome(7, 0),
            test_command.SuiteOutcome(5, 1),
        ]
        with mock.patch.object(coverage, "run_catch2_suite", side_effect=outcomes) as run_suite:
            status = coverage.run_coverage_tests(
                ("core", "backend", "gtk"),
                Path("/tmp/coverage-test"),
                "[concurrency]",
            )

        self.assertEqual(status, 7)
        self.assertEqual(run_suite.call_count, 3)
        for suite in ("core", "backend", "gtk"):
            run_suite.assert_any_call(suite, Path("/tmp/coverage-test"), **self._suite_kwargs("[concurrency]"))

    def test_filtered_run_accepts_an_empty_member_suite_when_another_matches(self):
        outcomes = [test_command.SuiteOutcome(0, 0), test_command.SuiteOutcome(0, 1)]
        with mock.patch.object(coverage, "run_catch2_suite", side_effect=outcomes) as run_suite:
            status = coverage.run_coverage_tests(
                ("backend", "core"),
                Path("/tmp/coverage-test"),
                "[pipewire]",
            )

        self.assertEqual(status, 0)
        self.assertEqual(run_suite.call_count, 2)

    def test_filtered_run_fails_when_no_selected_suite_matches_globally(self):
        outcomes = [test_command.SuiteOutcome(0, 0), test_command.SuiteOutcome(0, 0)]
        stderr = io.StringIO()
        with (
            mock.patch.object(coverage, "run_catch2_suite", side_effect=outcomes) as run_suite,
            contextlib.redirect_stderr(stderr),
            self.assertRaises(SystemExit),
        ):
            coverage.run_coverage_tests(("core", "backend"), Path("/tmp/coverage-test"), "[missing]")

        self.assertEqual(run_suite.call_count, 2)
        self.assertIn("no tests in the selected coverage suites matched filter '[missing]'", stderr.getvalue())

    def test_earlier_native_failure_is_preserved_while_every_suite_still_runs(self):
        outcomes = [test_command.SuiteOutcome(7, 0), test_command.SuiteOutcome(0, 2), test_command.SuiteOutcome(0, 1)]
        with mock.patch.object(coverage, "run_catch2_suite", side_effect=outcomes) as run_suite:
            status = coverage.run_coverage_tests(
                ("core", "backend", "tui"),
                Path("/tmp/coverage-test"),
                "[focused]",
            )

        self.assertEqual(status, 7)
        self.assertEqual(run_suite.call_count, 3)

    def test_unreadable_match_metadata_keeps_its_promoted_failure_status(self):
        # The shared runner already promotes an unreadable filtered report to
        # a failed suite status; coverage keeps that failure and still runs
        # the remaining suites.
        outcomes = [test_command.SuiteOutcome(1, None), test_command.SuiteOutcome(0, 2)]
        with mock.patch.object(coverage, "run_catch2_suite", side_effect=outcomes) as run_suite:
            status = coverage.run_coverage_tests(
                ("core", "backend"),
                Path("/tmp/coverage-test"),
                "[focused]",
            )

        self.assertEqual(status, 1)
        self.assertEqual(run_suite.call_count, 2)

    def test_unfiltered_run_never_relaxes_the_native_empty_failure(self):
        outcome = test_command.SuiteOutcome(0, None)
        with mock.patch.object(coverage, "run_catch2_suite", return_value=outcome) as run_suite:
            status = coverage.run_coverage_tests(("core", "backend"), Path("/tmp/coverage-test"), "")

        self.assertEqual(status, 0)
        for call in run_suite.call_args_list:
            self.assertEqual(call.kwargs["shards"], 1)
            self.assertFalse(call.kwargs["report_matches"])
            self.assertFalse(call.kwargs["allow_no_tests"])

    def test_hostile_shard_environment_cannot_shard_a_coverage_run(self):
        outcome = test_command.SuiteOutcome(0, 1)
        with mock.patch.dict(os.environ, {test_command.SHARD_ENV: "16"}):
            with mock.patch.object(coverage, "run_catch2_suite", return_value=outcome) as run_suite:
                status = coverage.run_coverage_tests(
                    ("core", "backend"),
                    Path("/tmp/coverage-test"),
                    "[focused]",
                )

        self.assertEqual(status, 0)
        for call in run_suite.call_args_list:
            self.assertEqual(call.kwargs["shards"], 1)
            self.assertNotIn("--shard-count", str(call))


class CollectCoverageStdoutTest(unittest.TestCase):
    """The collector reads `gcov --preserve-paths -t` stdout with the gcov cwd
    on the build directory, so ccache's build-root-relative source spellings
    resolve and no report files are ever written."""

    @staticmethod
    def _gcov_run(stdout: str, stderr: str = "", returncode: int = 0) -> subprocess.CompletedProcess:
        return subprocess.CompletedProcess([], returncode, stdout, stderr)

    def test_gcov_runs_stdout_mode_from_build_dir_without_report_files(self):
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary)
            (build / "lib").mkdir()
            gcda = build / "lib" / "x.cpp.gcda"
            gcda.touch()
            source = coverage.PROJECT_ROOT / "lib" / "audio" / "Foo.cpp"
            text = GCOV_B.replace("lib/audio/Foo.cpp", str(source))
            with mock.patch.object(coverage.subprocess, "run", return_value=self._gcov_run(text)) as run_gcov:
                coverage.collect_coverage(build)
            run_gcov.assert_called_once_with(
                ["gcov", "--preserve-paths", "-t", str(coverage.absolute_path(gcda))],
                cwd=build,
                capture_output=True,
                text=True,
            )
            self.assertEqual(list(build.rglob("*.gcov")), [])

    def test_build_root_relative_spelling_collects_under_project_root(self):
        # The c27 incident: CCACHE_BASEDIR makes GCC record project sources
        # relative to the build directory, so gcov must run there for the
        # spelling to resolve back under PROJECT_ROOT.
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            build = root / "a" / "b" / "c" / "d" / "e" / "coverage"
            (build / "lib").mkdir(parents=True)
            (build / "lib" / "x.cpp.gcda").touch()
            text = GCOV_A.replace("lib/audio/Foo.cpp", "../../../../../dev/RS2/lib/x.cpp")
            with (
                mock.patch.object(coverage, "PROJECT_ROOT", coverage.absolute_path(root / "a" / "dev" / "RS2")),
                mock.patch.object(coverage.subprocess, "run", return_value=self._gcov_run(text)),
            ):
                merged = coverage.collect_coverage(build)
            self.assertEqual(
                merged,
                {
                    "lib/x.cpp": {
                        1: (None, '#include "Foo.h"'),
                        2: (3, "int foo() {"),
                        3: (0, "  return bar();"),
                        4: (None, "}"),
                    }
                },
            )

    def test_header_only_project_section_fails_closed_despite_zero_exit(self):
        # gcov still exits 0 when it cannot open the recorded source; the
        # header-only section must not silently measure as zero coverage.
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary)
            (build / "lib").mkdir()
            (build / "lib" / "x.cpp.gcda").touch()
            source = coverage.PROJECT_ROOT / "lib" / "audio" / "Foo.cpp"
            text = GCOV_HEADER_ONLY.replace("lib/audio/Foo.cpp", str(source))
            result = self._gcov_run(text, stderr=f"Cannot open source file {source}")
            errors = io.StringIO()
            with (
                mock.patch.object(coverage.subprocess, "run", return_value=result),
                contextlib.redirect_stderr(errors),
                self.assertRaises(SystemExit),
            ):
                coverage.collect_coverage(build)
            message = errors.getvalue()
            self.assertIn("could not read project source lib/audio/Foo.cpp", message)
            self.assertIn("Cannot open source file", message)

    def test_benign_staleness_warning_with_full_data_is_allowed(self):
        # The fail-closed trigger is the header-only project section, not any
        # stderr text: a mere staleness warning with complete line data passes.
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary)
            (build / "lib").mkdir()
            (build / "lib" / "x.cpp.gcda").touch()
            source = coverage.PROJECT_ROOT / "lib" / "audio" / "Foo.cpp"
            text = GCOV_B.replace("lib/audio/Foo.cpp", str(source))
            warning = f"{source}:source file is newer than notes file (requires remaking?)"
            with mock.patch.object(coverage.subprocess, "run", return_value=self._gcov_run(text, stderr=warning)):
                merged = coverage.collect_coverage(build)
            self.assertEqual(list(merged), ["lib/audio/Foo.cpp"])
            self.assertEqual(coverage.file_stats(merged["lib/audio/Foo.cpp"]), (2, 2, 0, 100.0))


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

    def test_gcov_failure_preserves_diagnostics(self):
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary)
            (build / "lib").mkdir()
            (build / "lib" / "foo.gcda").touch()
            result = subprocess.CompletedProcess([], 1, "", "invalid gcno version")
            errors = io.StringIO()
            with (
                mock.patch.object(coverage.subprocess, "run", return_value=result),
                contextlib.redirect_stderr(errors),
                self.assertRaises(SystemExit),
            ):
                coverage.collect_coverage(build)
            self.assertIn("gcov failed for", errors.getvalue())
            self.assertIn("invalid gcno version", errors.getvalue())

    def test_stdout_without_sections_fails_closed(self):
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary)
            (build / "lib").mkdir()
            (build / "lib" / "foo.gcda").touch()
            # A stale file-mode report in the build tree must neither satisfy
            # the empty-stdout failure nor be consumed or removed by it.
            stale = build / "stale.gcov"
            stale.write_text(GCOV_B, encoding="utf-8")
            result = subprocess.CompletedProcess([], 0, "", "")
            errors = io.StringIO()
            with (
                mock.patch.object(coverage.subprocess, "run", return_value=result),
                contextlib.redirect_stderr(errors),
                self.assertRaises(SystemExit),
            ):
                coverage.collect_coverage(build)
            self.assertIn("gcov produced no reports", errors.getvalue())
            self.assertTrue(stale.exists())

    def test_extraction_merges_counts_across_gcda_for_one_source(self):
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary)
            (build / "lib").mkdir()
            (build / "lib" / "first.gcda").touch()
            (build / "lib" / "second.gcda").touch()
            source = coverage.PROJECT_ROOT / "lib" / "audio" / "Foo.cpp"

            def extract(argv, **kwargs):
                text = GCOV_A if "first.gcda" in argv[-1] else GCOV_B
                return subprocess.CompletedProcess(argv, 0, text.replace("lib/audio/Foo.cpp", str(source)), "")

            with mock.patch.object(coverage.subprocess, "run", side_effect=extract):
                merged = coverage.collect_coverage(build)
            self.assertEqual(coverage.file_stats(merged["lib/audio/Foo.cpp"]), (2, 2, 0, 100.0))

    def test_same_basename_headers_from_one_object_both_survive_extraction(self):
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary)
            (build / "test").mkdir()
            (build / "test" / "headers.gcda").touch()
            sources = ("app/tui/Executor.h", "include/ao/async/Executor.h")
            # gcov -t prints one stdout section per source, so two headers with
            # the same basename need no filename mangling to both survive.
            stdout = "".join(
                GCOV_B.replace("lib/audio/Foo.cpp", str(coverage.PROJECT_ROOT / source)) for source in sources
            )
            result = subprocess.CompletedProcess([], 0, stdout, "")
            with mock.patch.object(coverage.subprocess, "run", return_value=result):
                merged = coverage.collect_coverage(build)
            self.assertEqual(sorted(merged), sorted(sources))
            for source in sources:
                self.assertEqual(coverage.file_stats(merged[source]), (2, 2, 0, 100.0))

    def test_test_objects_contribute_production_headers_without_test_sources(self):
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary)
            (build / "test").mkdir()
            (build / "test" / "header.gcda").touch()
            # A valid production row plus a test source and an external system
            # header: both non-reportable sections are filtered without failing.
            stdout = "".join(
                GCOV_B.replace("lib/audio/Foo.cpp", spelling)
                for spelling in (
                    str(coverage.PROJECT_ROOT / "include" / "ao" / "Example.h"),
                    str(coverage.PROJECT_ROOT / "test" / "query" / "ExampleTest.cpp"),
                    str(build.parent / "system" / "string"),
                )
            )
            result = subprocess.CompletedProcess([], 0, stdout, "")
            with mock.patch.object(coverage.subprocess, "run", return_value=result):
                merged = coverage.collect_coverage(build)
            self.assertEqual(list(merged), ["include/ao/Example.h"])
            self.assertEqual(coverage.file_stats(merged["include/ao/Example.h"]), (2, 2, 0, 100.0))


if __name__ == "__main__":
    unittest.main()
