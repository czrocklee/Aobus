"""Tests for the Python lint integration runner."""

import contextlib
import io
import tempfile
import threading
import unittest
from pathlib import Path
from unittest import mock

from ao.core import linttest


def pinned_fixes_export(main: str = "/tmp/case/BasicFixture.cpp", replacements: str = "Replacements:    []") -> str:
    """One diagnostic in the known machine-generated clang-tidy export format."""
    return "\n".join(
        (
            "---",
            f"MainSourceFile:  '{main}'",
            "Diagnostics:",
            "  - DiagnosticName:  aobus-example",
            "    DiagnosticMessage:",
            "      Message:         'expected'",
            f"      FilePath:        '{main}'",
            "      FileOffset:      8",
            f"      {replacements}",
            "    Level:           Warning",
            "    BuildDirectory:  '/tmp/case'",
            "...",
            "",
        )
    )


class LintTestVerifierTest(unittest.TestCase):
    def test_fixture_discovery_uses_parent_directory_as_check_name(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            check_dir = root / "aobus-example"
            check_dir.mkdir()
            fixture = check_dir / "BasicFixture.cpp"
            fixture.touch()
            objc_fixture = check_dir / "ObjCIvarFixture.mm"
            objc_fixture.touch()
            (check_dir / "notes.txt").touch()

            self.assertEqual(
                linttest.discover_fixtures(root),
                [linttest.Fixture(fixture, "aobus-example"), linttest.Fixture(objc_fixture, "aobus-example")],
            )

    def test_expectations_apply_standalone_annotations_to_the_next_line(self):
        source = """\
// POSITIVE
int bad;
int good; // NEGATIVE
"""

        expectations = linttest.parse_expectations(source, "aobus-example")

        self.assertEqual(expectations.expected, {2: {"aobus-example"}})
        self.assertEqual(expectations.negated, {3: {"aobus-example"}})

    def test_expectations_support_named_checks_and_fix_markers(self):
        source = """\
int current; // POSITIVE
int named; // POSITIVE:aobus-other
int forbidden; // NEGATIVE:aobus-other
// POSITIVE: FIX-TO: int replacement;
int fixTarget;
"""

        expectations = linttest.parse_expectations(source, "aobus-current")

        self.assertEqual(
            expectations.expected,
            {
                1: {"aobus-current"},
                2: {"aobus-other"},
                5: {"aobus-current"},
            },
        )
        self.assertEqual(expectations.negated, {3: {"aobus-other"}})

    def test_diagnostic_verification_reports_missing_unexpected_and_negative_cases(self):
        source = """\
// POSITIVE
int missing;
int forbidden; // NEGATIVE
int unexpected;
"""
        output = """\
/tmp/case.cpp:3:1: warning: forbidden [aobus-example]
/tmp/case.cpp:4:1: warning: surprise [aobus-other]
"""

        self.assertEqual(
            linttest.verify_diagnostics({Path("/tmp/case.cpp"): source}, output, "aobus-example"),
            [
                f"missing expected diagnostic on line {Path('/tmp/case.cpp').resolve()}:2: [aobus-example]",
                f"diagnostic found on explicitly NEGATIVE line {Path('/tmp/case.cpp').resolve()}:3: [aobus-example]",
                f"unexpected diagnostic on line {Path('/tmp/case.cpp').resolve()}:4: [aobus-other]",
            ],
        )

    def test_diagnostic_verification_does_not_require_other_named_checks(self):
        source = "int named; // POSITIVE:aobus-other\n"

        self.assertEqual(linttest.verify_diagnostics({Path("case.cpp"): source}, "", "aobus-current"), [])

    def test_header_diagnostic_cannot_replace_a_source_diagnostic(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            source = Path(temp_dir) / "Source.mm"
            header = Path(temp_dir) / "Declarations.h"
            sources = {source: "// POSITIVE\nint missing;\n", header: "// NEGATIVE\nint forbidden;\n"}

            errors = linttest.verify_diagnostics(
                sources, f"{header}:2:1: warning: wrong file [aobus-example]\n", "aobus-example"
            )

            self.assertEqual(len(errors), 2)
            self.assertIn(f"missing expected diagnostic on line {source.resolve()}:2: [aobus-example]", errors)
            self.assertIn(f"diagnostic found on explicitly NEGATIVE line {header.resolve()}:2: [aobus-example]", errors)

    def test_source_diagnostic_cannot_hide_a_forbidden_header_diagnostic(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            source = Path(temp_dir) / "Source.mm"
            header = Path(temp_dir) / "Declarations.h"
            sources = {source: "// POSITIVE\nint expected;\n", header: "// NEGATIVE\nint forbidden;\n"}
            output = "".join(f"{path}:2:1: warning: same line [aobus-example]\n" for path in sources)

            self.assertEqual(
                linttest.verify_diagnostics(sources, output, "aobus-example"),
                [f"diagnostic found on explicitly NEGATIVE line {header.resolve()}:2: [aobus-example]"],
            )

    def test_each_file_requires_its_own_positive_diagnostic(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            source = Path(temp_dir) / "Source.mm"
            header = Path(temp_dir) / "Declarations.h"
            sources = dict.fromkeys((source, header), "// POSITIVE\nint expected;\n")
            output = f"{source}:2:1: warning: expected [aobus-example]\n"

            self.assertEqual(
                linttest.verify_diagnostics(sources, output, "aobus-example"),
                [f"missing expected diagnostic on line {header.resolve()}:2: [aobus-example]"],
            )
            output += f"{header}:2:1: warning: expected [aobus-example]\n"
            self.assertEqual(linttest.verify_diagnostics(sources, output, "aobus-example"), [])

    def test_diagnostic_paths_normalize_relative_and_native_absolute_spellings(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            source = root / "Source.mm"
            output = (
                "nested/../Source.mm:2:1: warning: relative [aobus-example]\n"
                f"{source.as_posix()}:2:1: warning: slash spelling [aobus-example]\n"
                f"{source}:2:1: warning: native spelling [aobus-example]\n"
                f"{root / 'other' / 'Source.mm'}:2:1: warning: different file [aobus-example]\n"
            )
            with mock.patch.object(linttest, "PROJECT_ROOT", root):
                actual = linttest.parse_diagnostics(output)

            self.assertEqual(
                actual,
                {
                    (source.resolve(), 2): {"aobus-example"},
                    ((root / "other/Source.mm").resolve(), 2): {"aobus-example"},
                },
            )

    def test_diagnostic_from_an_unknown_file_is_not_discarded(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            header = Path(temp_dir) / "External.h"
            errors = linttest.verify_diagnostics(
                {Path(temp_dir) / "Source.mm": ""},
                f"{header}:2:1: warning: external [aobus-example]\n",
                "aobus-example",
            )
            self.assertEqual(errors, [f"unexpected diagnostic on line {header.resolve()}:2: [aobus-example]"])

    def test_fix_expectations_are_derived_from_fixture_markers(self):
        source = """\
// POSITIVE: FIX-TO: int replacement;
int original;
"""

        self.assertEqual(
            linttest.expected_fixes(source),
            [("// POSITIVE: FIX-TO: int replacement;", "int replacement;")],
        )
        self.assertEqual(
            linttest.verify_fixes(source, "// POSITIVE: FIX-TO: int replacement;\nint replacement;\n"),
            [],
        )

    def test_fix_verification_supports_escaped_newlines(self):
        source = "  } // POSITIVE: FIX-TO: }\\n\n"
        fixed = "  } // POSITIVE: FIX-TO: }\\n\n\n"

        self.assertEqual(linttest.verify_fixes(source, fixed), [])

    def test_fix_marker_anchor_survives_an_inline_code_rewrite(self):
        source = "int old; // POSITIVE: FIX-TO: int replacement;\n"
        fixed = "int replacement; // POSITIVE: FIX-TO: int replacement;\n"

        self.assertEqual(linttest.verify_fixes(source, fixed), [])

    def test_fix_verification_checks_multiple_markers_in_order(self):
        source = """\
// POSITIVE: FIX-TO: int first;
int oldFirst;
// POSITIVE: FIX-TO: int second;
int oldSecond;
"""
        fixed = """\
// POSITIVE: FIX-TO: int first;
int first;
// POSITIVE: FIX-TO: int second;
int second;
"""

        self.assertEqual(linttest.verify_fixes(source, fixed), [])

    def test_fix_verification_reports_missing_marker_and_mismatch(self):
        source = """\
// POSITIVE: FIX-TO: int replacement;
int original;
"""

        self.assertIn(
            "expected comment line not found",
            linttest.verify_fixes(source, "int replacement;\n")[0],
        )
        self.assertIn(
            "expected 'int replacement;'",
            linttest.verify_fixes(
                source,
                "// POSITIVE: FIX-TO: int replacement;\nint wrong;\n",
            )[0],
        )

    def test_tidy_command_reuses_the_current_python_cli_without_building(self):
        fixture = linttest.Fixture(Path("/tmp/BasicFixture.cpp"), "aobus-example")

        command = linttest._tidy_command(fixture, Path("/tmp/build"), "--fix")

        self.assertEqual(command[:3], [linttest.sys.executable, "-m", "ao"])
        self.assertIn("--no-build", command)
        self.assertIn("--fix", command)
        self.assertEqual(command[-1], str(fixture.path))

    def test_fixture_tidy_args_define_a_parseable_warning_profile(self):
        include_dir = Path("/tmp/fixture")

        args = linttest._fixture_tidy_args(include_dir)

        self.assertIn("--tidy-arg=--extra-arg=-Wno-error", args)
        self.assertIn("--tidy-arg=--extra-arg=-Wno-unused-variable", args)
        self.assertIn(
            f"--tidy-arg=--extra-arg=-I{linttest.PROJECT_ROOT / 'include'}",
            args,
        )
        self.assertIn(f"--tidy-arg=--extra-arg=-I{include_dir}", args)
        self.assertIn("--tidy-arg=--extra-arg=c++", args)
        self.assertNotIn("--tidy-arg=--extra-arg=-fblocks", args)
        self.assertFalse(any(arg.startswith("--header-filter=") for arg in args))

    def test_objective_cpp_fixture_uses_objective_cpp_language(self):
        args = linttest._fixture_tidy_args(Path("/tmp/fixture"), "objective-c++")

        self.assertIn("--tidy-arg=--extra-arg=objective-c++", args)
        self.assertIn("--tidy-arg=--extra-arg=-fblocks", args)
        self.assertNotIn("--tidy-arg=--extra-arg=c++", args)

    def test_no_fix_marker_requires_a_standalone_comment_line(self):
        self.assertTrue(linttest.no_fix_asserted("// NO-FIX\nint original;\n"))
        self.assertTrue(linttest.no_fix_asserted("// NO-FIX: exported replacements must stay empty\nint original;\n"))
        self.assertFalse(linttest.no_fix_asserted("int original; // NO-FIX\n"))
        self.assertFalse(linttest.no_fix_asserted("// NO-FIXES is not the marker\n"))

    def test_no_fix_marker_is_not_a_diagnostic_expectation(self):
        expectations = linttest.parse_expectations("// NO-FIX\nint original;\n", "aobus-example")

        self.assertEqual(expectations.expected, {})
        self.assertEqual(expectations.negated, {})

    def test_expects_diagnostics_requires_the_owning_check(self):
        self.assertTrue(linttest.expects_diagnostics("// POSITIVE\nint bad;\n", "aobus-example"))
        self.assertFalse(linttest.expects_diagnostics("int named; // POSITIVE:aobus-other\n", "aobus-example"))

    def test_no_fix_export_accepts_the_pinned_empty_report(self):
        self.assertEqual(linttest.verify_no_fix_export(pinned_fixes_export()), [])

    def test_no_fix_export_rejects_a_report_without_diagnostic_messages(self):
        export = "---\nMainSourceFile:  'x.cpp'\nDiagnostics:      []\n...\n"

        self.assertEqual(
            linttest.verify_no_fix_export(export),
            ["fixes export contains no diagnostic message; a NO-FIX fixture with expected diagnostics must export one"],
        )

    def test_no_fix_export_accepts_note_entries_with_empty_replacements(self):
        export = "\n".join(
            (
                "---",
                "MainSourceFile:  'x.cpp'",
                "Diagnostics:",
                "  - DiagnosticName:  aobus-example",
                "    DiagnosticMessage:",
                "      Message:         'expected'",
                "      FilePath:        'x.cpp'",
                "      FileOffset:      8",
                "      Replacements:    []",
                "    Level:           Warning",
                "    BuildDirectory:  '/tmp'",
                "    Notes:",
                "      - Message:         'related finding'",
                "        FilePath:        'x.cpp'",
                "        FileOffset:      24",
                "        Replacements:    []",
                "...",
                "",
            )
        )

        self.assertEqual(linttest.verify_no_fix_export(export), [])

    def test_no_fix_export_rejects_a_report_without_the_pinned_envelope(self):
        export = "Diagnostics:\n  - DiagnosticName:  aobus-example\n    DiagnosticMessage:\n      Message: 'x'\n"

        errors = linttest.verify_no_fix_export(export)

        self.assertIn("fixes export does not start with the pinned '---' document header", errors)
        self.assertIn("fixes export does not end with the pinned '...' document terminator", errors)
        self.assertIn("fixes export does not name its MainSourceFile", errors)
        self.assertIn("diagnostic message on line 3 has no Replacements field", errors)

    def test_no_fix_export_rejects_any_replacement_record(self):
        record = """Replacements:
        - FilePath:        '{path}'
          Offset:          8
          Length:          5
          ReplacementText: '{text}'"""
        cases = {
            "no-op replacement": ("/tmp/case/BasicFixture.cpp", "int original"),
            "same-file empty text": ("/tmp/case/BasicFixture.cpp", ""),
            "other-header replacement": ("/tmp/case/Declarations.h", "int replacement"),
        }
        for label, (path, text) in cases.items():
            with self.subTest(label):
                export = pinned_fixes_export(replacements=record.format(path=path, text=text))

                errors = linttest.verify_no_fix_export(export)

            self.assertTrue(any(error.startswith("replacement record exported on line 10") for error in errors))
            self.assertTrue(any(error.startswith("replacement record exported on line 13") for error in errors))
            self.assertIn("diagnostic message on line 5 exports replacements on line 9", errors)

    def test_no_fix_export_rejects_a_missing_replacements_field(self):
        export = "\n".join(
            (
                "---",
                "MainSourceFile:  'x.cpp'",
                "Diagnostics:",
                "  - DiagnosticName:  aobus-example",
                "    DiagnosticMessage:",
                "      Message:         'expected'",
                "      FilePath:        'x.cpp'",
                "      FileOffset:      8",
                "    Level:           Warning",
                "    BuildDirectory:  '/tmp'",
                "...",
                "",
            )
        )

        self.assertIn(
            "diagnostic message on line 5 has no Replacements field",
            linttest.verify_no_fix_export(export),
        )

    def test_no_fix_export_rejects_unexpected_section_and_field_shapes(self):
        errors = linttest.verify_no_fix_export("---\nMainSourceFile:  'x.cpp'\nDiagnostics:      [surprise]\n...\n")

        self.assertIn("fixes export declares an unexpected Diagnostics section on line 3", errors)
        export = pinned_fixes_export(replacements="Replacements:       ~")

        self.assertIn(
            "diagnostic message on line 5 exports replacements on line 9",
            linttest.verify_no_fix_export(export),
        )

    def test_objective_cpp_header_diagnostics_stay_in_the_fixture_directory(self):
        include_dir = Path("/tmp/fixture [owned]")
        args = linttest._fixture_tidy_args(include_dir, "objective-c++")
        header_filter = next(arg.removeprefix("--header-filter=") for arg in args if arg.startswith("--header-filter="))

        self.assertRegex(f"{include_dir.as_posix()}/Declarations.h", header_filter)
        self.assertNotRegex(f"{include_dir.as_posix()}-other/Declarations.h", header_filter)


class LintTestRunnerTest(unittest.TestCase):
    def test_objective_cpp_context_follows_local_headers_without_loading_unincluded_siblings(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            source = root / "Source.mm"
            first = root / "First.h"
            second = root / "Second.h"
            source.write_text('#import "First.h"\n', encoding="utf-8")
            first.write_text('#include "Second.h"\n', encoding="utf-8")
            second.write_text('#include "First.h"\nint value; // NEGATIVE\n', encoding="utf-8")
            (root / "Unincluded.h").write_text("int other; // POSITIVE\n", encoding="utf-8")

            sources = linttest._fixture_sources(linttest.Fixture(source, "aobus-example"))

            self.assertEqual(set(sources), {path.resolve() for path in (source, first, second)})
            self.assertEqual(linttest.verify_diagnostics(sources, "", "aobus-example"), [])

    def test_diagnostic_stage_verifies_the_included_header_expectations(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            source = root / "Source.mm"
            header = root / "Declarations.h"
            source.write_text('#include "Declarations.h"\nint expected; // POSITIVE\n', encoding="utf-8")
            header.write_text("// NEGATIVE\nint forbidden;\n", encoding="utf-8")
            completed = linttest.subprocess.CompletedProcess(
                [], 1, stdout=f"{header}:2:1: warning: wrong file [aobus-example]\n"
            )

            with mock.patch.object(linttest.subprocess, "run", return_value=completed):
                success, log = linttest._run_diagnostic(linttest.Fixture(source, "aobus-example"), root, root)

            self.assertFalse(success)
            output = log.read_text(encoding="utf-8")
            self.assertIn(f"missing expected diagnostic on line {source.resolve()}:2", output)
            self.assertIn(f"explicitly NEGATIVE line {header.resolve()}:2", output)

    def test_context_header_fix_markers_are_not_silently_accepted(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            source = root / "Source.mm"
            header = root / "Declarations.h"
            source.write_text('#include "Declarations.h"\n', encoding="utf-8")
            header.write_text("// POSITIVE: FIX-TO: int replacement;\nint original;\n", encoding="utf-8")
            completed = linttest.subprocess.CompletedProcess(
                [], 1, stdout=f"{header}:2:1: warning: expected [aobus-example]\n"
            )

            with mock.patch.object(linttest.subprocess, "run", return_value=completed):
                success, log = linttest._run_diagnostic(linttest.Fixture(source, "aobus-example"), root, root)

            self.assertFalse(success)
            self.assertIn("context header FIX-TO markers require a standalone fixture", log.read_text(encoding="utf-8"))

    def test_diagnostic_stage_rejects_nonzero_tidy_exit_without_expected_output(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            fixture_path = root / "BasicFixture.cpp"
            fixture_path.write_text("int valid;\n", encoding="utf-8")
            fixture = linttest.Fixture(fixture_path, "aobus-example")
            completed = linttest.subprocess.CompletedProcess([], 2, stdout="clang-tidy failed\n")

            with mock.patch.object(linttest.subprocess, "run", return_value=completed):
                success, log = linttest._run_diagnostic(fixture, root, root)

            self.assertFalse(success)
            self.assertIn("clang-tidy exited with status 2", log.read_text(encoding="utf-8"))

    def test_diagnostic_stage_accepts_nonzero_tidy_exit_for_expected_warnings(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            fixture_path = root / "BasicFixture.cpp"
            fixture_path.write_text("int bad; // POSITIVE\n", encoding="utf-8")
            fixture = linttest.Fixture(fixture_path, "aobus-example")
            completed = linttest.subprocess.CompletedProcess(
                [],
                1,
                stdout=f"{fixture_path}:1:1: warning: expected [aobus-example]\n",
            )

            with mock.patch.object(linttest.subprocess, "run", return_value=completed):
                success, log = linttest._run_diagnostic(fixture, root, root)

            self.assertTrue(success)
            self.assertNotIn("clang-tidy exited", log.read_text(encoding="utf-8"))

    def test_diagnostic_stage_rejects_compiler_errors_even_with_expected_warnings(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            fixture_path = root / "BasicFixture.cpp"
            fixture_path.write_text("int bad; // POSITIVE\n", encoding="utf-8")
            fixture = linttest.Fixture(fixture_path, "aobus-example")
            completed = linttest.subprocess.CompletedProcess(
                [],
                1,
                stdout=(
                    f"{fixture_path}:1:1: warning: expected [aobus-example]\n"
                    f"{fixture_path}:2:1: error: missing type [clang-diagnostic-error]\n"
                ),
            )

            with mock.patch.object(linttest.subprocess, "run", return_value=completed):
                success, log = linttest._run_diagnostic(fixture, root, root)

            self.assertFalse(success)
            text = log.read_text(encoding="utf-8")
            self.assertIn("clang-tidy reported a fatal compiler diagnostic", text)
            self.assertIn("clang-tidy exited with status 1", text)

    def test_no_fix_fixture_exports_and_verifies_an_empty_replacement_report(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            fixture_path = root / "BasicFixture.cpp"
            fixture_path.write_text("// NO-FIX\nint bad; // POSITIVE\n", encoding="utf-8")
            fixture = linttest.Fixture(fixture_path, "aobus-example")

            def run_tidy(command, **_kwargs):
                export = next(
                    Path(argument.removeprefix("--tidy-arg=-export-fixes="))
                    for argument in command
                    if argument.startswith("--tidy-arg=-export-fixes=")
                )
                export.write_text(pinned_fixes_export(main=str(fixture_path)), encoding="utf-8")
                return linttest.subprocess.CompletedProcess(
                    command, 1, stdout=f"{fixture_path}:2:1: warning: expected [aobus-example]\n"
                )

            with mock.patch.object(linttest.subprocess, "run", side_effect=run_tidy):
                success, log = linttest._run_diagnostic(fixture, root, root)

            self.assertTrue(success)
            self.assertNotIn("ERROR:", log.read_text(encoding="utf-8"))

    def test_no_fix_fixture_rejects_exported_replacement_records(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            fixture_path = root / "BasicFixture.cpp"
            fixture_path.write_text("// NO-FIX\nint bad; // POSITIVE\n", encoding="utf-8")
            fixture = linttest.Fixture(fixture_path, "aobus-example")
            record = """Replacements:
        - FilePath:        '/tmp/case/Declarations.h'
          Offset:          8
          Length:          5
          ReplacementText: 'int replacement'"""

            def run_tidy(command, **_kwargs):
                export = next(
                    Path(argument.removeprefix("--tidy-arg=-export-fixes="))
                    for argument in command
                    if argument.startswith("--tidy-arg=-export-fixes=")
                )
                export.write_text(pinned_fixes_export(main=str(fixture_path), replacements=record), encoding="utf-8")
                return linttest.subprocess.CompletedProcess(
                    command, 1, stdout=f"{fixture_path}:2:1: warning: expected [aobus-example]\n"
                )

            with mock.patch.object(linttest.subprocess, "run", side_effect=run_tidy):
                success, log = linttest._run_diagnostic(fixture, root, root)

            self.assertFalse(success)
            text = log.read_text(encoding="utf-8")
            self.assertIn("replacement record exported on line 10", text)
            self.assertIn("=== Exported fixes report", text)

    def test_no_fix_fixture_fails_when_the_report_is_missing(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            fixture_path = root / "BasicFixture.cpp"
            fixture_path.write_text("// NO-FIX\nint bad; // POSITIVE\n", encoding="utf-8")
            fixture = linttest.Fixture(fixture_path, "aobus-example")
            completed = linttest.subprocess.CompletedProcess(
                [], 1, stdout=f"{fixture_path}:2:1: warning: expected [aobus-example]\n"
            )

            with mock.patch.object(linttest.subprocess, "run", return_value=completed):
                success, log = linttest._run_diagnostic(fixture, root, root)

            self.assertFalse(success)
            self.assertIn(
                "clang-tidy created no fixes export; a NO-FIX fixture with expected diagnostics must produce one",
                log.read_text(encoding="utf-8"),
            )

    def test_no_fix_fixture_rejects_an_empty_diagnostic_report(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            fixture_path = root / "BasicFixture.cpp"
            fixture_path.write_text("// NO-FIX\nint bad; // POSITIVE\n", encoding="utf-8")
            fixture = linttest.Fixture(fixture_path, "aobus-example")

            def run_tidy(command, **_kwargs):
                export = next(
                    Path(argument.removeprefix("--tidy-arg=-export-fixes="))
                    for argument in command
                    if argument.startswith("--tidy-arg=-export-fixes=")
                )
                export.write_text(
                    f"---\nMainSourceFile:  '{fixture_path}'\nDiagnostics:      []\n...\n", encoding="utf-8"
                )
                return linttest.subprocess.CompletedProcess(
                    command, 1, stdout=f"{fixture_path}:2:1: warning: expected [aobus-example]\n"
                )

            with mock.patch.object(linttest.subprocess, "run", side_effect=run_tidy):
                success, log = linttest._run_diagnostic(fixture, root, root)

            self.assertFalse(success)
            self.assertIn("fixes export contains no diagnostic message", log.read_text(encoding="utf-8"))

    def test_unmarked_fixture_does_not_request_a_fixes_export(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            fixture_path = root / "BasicFixture.cpp"
            fixture_path.write_text("int bad; // POSITIVE\n", encoding="utf-8")
            fixture = linttest.Fixture(fixture_path, "aobus-example")
            completed = linttest.subprocess.CompletedProcess(
                [], 1, stdout=f"{fixture_path}:1:1: warning: expected [aobus-example]\n"
            )

            with mock.patch.object(linttest.subprocess, "run", return_value=completed) as run:
                success, _log = linttest._run_diagnostic(fixture, root, root)

            self.assertTrue(success)
            for call in run.call_args_list:
                self.assertFalse(any(argument.startswith("--tidy-arg=-export-fixes=") for argument in call.args[0]))

    def test_context_header_no_fix_markers_require_a_standalone_fixture(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            source = root / "Source.mm"
            header = root / "Declarations.h"
            source.write_text('#include "Declarations.h"\n', encoding="utf-8")
            header.write_text("// NO-FIX\nint adviceOnly;\n", encoding="utf-8")
            completed = linttest.subprocess.CompletedProcess(
                [], 1, stdout=f"{header}:2:1: warning: expected [aobus-example]\n"
            )

            with mock.patch.object(linttest.subprocess, "run", return_value=completed):
                success, log = linttest._run_diagnostic(linttest.Fixture(source, "aobus-example"), root, root)

            self.assertFalse(success)
            self.assertIn(
                "context header NO-FIX markers require a standalone fixture",
                log.read_text(encoding="utf-8"),
            )

    def test_runner_requires_existing_compile_database_and_plugin(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            build_dir = Path(temp_dir)
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(linttest.run(build_dir), 1)

            (build_dir / "compile_commands.json").touch()
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(linttest.run(build_dir), 1)

    def test_compiler_error_smoke_requires_fatal_errors_but_allows_unused_code(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)

            def run_canary(command, **_kwargs):
                name = Path(command[-1]).stem
                if name == "unused-code":
                    return linttest.subprocess.CompletedProcess(command, 0, stdout="")
                return linttest.subprocess.CompletedProcess(
                    command,
                    1,
                    stdout=f"{name}.cpp:1:1: error: injected [clang-diagnostic-error]\n",
                )

            with mock.patch.object(linttest.subprocess, "run", side_effect=run_canary):
                success, _log = linttest._run_compiler_error_smoke(root, root)

            self.assertTrue(success)

    def test_compiler_error_smoke_rejects_a_suppressed_parse_error(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            completed = linttest.subprocess.CompletedProcess([], 0, stdout="")

            with mock.patch.object(linttest.subprocess, "run", return_value=completed):
                success, log = linttest._run_compiler_error_smoke(root, root)

            self.assertFalse(success)
            self.assertIn(
                "missing-include canary did not produce a fatal compiler diagnostic",
                log.read_text(encoding="utf-8"),
            )

    def test_fixture_copy_transfers_contents_without_cross_filesystem_metadata(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            fixture_dir = root / "aobus-example"
            fixture_dir.mkdir()
            case_dir = root / "case"
            case_dir.mkdir()
            fixture_path = fixture_dir / "BasicFixture.cpp"
            fixture_path.write_text("int fixture;\n", encoding="utf-8")
            sibling_path = fixture_dir / "FixtureSupport.h"
            sibling_path.write_text("struct FixtureSupport {};\n", encoding="utf-8")
            (root / "TestHelpers.h").write_text("struct TestHelper {};\n", encoding="utf-8")
            fixture = linttest.Fixture(fixture_path, "aobus-example")

            with mock.patch.object(linttest, "FIXTURE_DIR", root):
                with mock.patch.object(
                    linttest.shutil,
                    "copy2",
                    side_effect=AssertionError("metadata-preserving copy is not portable"),
                ):
                    fixed = linttest._copy_fixture_context(fixture, case_dir)

            self.assertEqual(fixed.read_text(encoding="utf-8"), "int fixture;\n")
            self.assertEqual(
                (case_dir / sibling_path.name).read_text(encoding="utf-8"),
                "struct FixtureSupport {};\n",
            )
            self.assertEqual((case_dir / "TestHelpers.h").read_text(encoding="utf-8"), "struct TestHelper {};\n")

    def test_fix_stage_rejects_unchanged_files_with_fix_expectations(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            fixture_dir = root / "fixture"
            fixture_dir.mkdir()
            fixture_path = fixture_dir / "BasicFixture.cpp"
            fixture_path.write_text(
                "// POSITIVE: FIX-TO: int replacement;\nint original;\n",
                encoding="utf-8",
            )
            fixture = linttest.Fixture(fixture_path, "aobus-example")
            completed = linttest.subprocess.CompletedProcess([], 0, stdout="")

            with mock.patch.object(linttest.subprocess, "run", return_value=completed):
                success, log = linttest._run_fix(fixture, root, root)

            self.assertFalse(success)
            self.assertIn(
                "fixture declares FIX-TO expectations but clang-tidy made no changes",
                log.read_text(encoding="utf-8"),
            )

    def test_fix_stage_accepts_nonzero_tidy_exit_when_expected_fix_is_applied(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            fixture_dir = root / "fixture"
            fixture_dir.mkdir()
            fixture_path = fixture_dir / "BasicFixture.cpp"
            fixture_path.write_text(
                "// POSITIVE: FIX-TO: int replacement;\nint original;\n",
                encoding="utf-8",
            )
            fixture = linttest.Fixture(fixture_path, "aobus-example")

            def run_fix_or_syntax(command, **_kwargs):
                if "-fsyntax-only" in command:
                    return linttest.subprocess.CompletedProcess(command, 0, stdout="")

                fixed = Path(command[-1])
                fixed.write_text(
                    "// POSITIVE: FIX-TO: int replacement;\nint replacement;\n",
                    encoding="utf-8",
                )
                return linttest.subprocess.CompletedProcess(
                    command,
                    1,
                    stdout=f"{fixed}:2:1: warning: expected [aobus-example]\n",
                )

            with mock.patch.object(linttest.subprocess, "run", side_effect=run_fix_or_syntax):
                success, log = linttest._run_fix(fixture, root, root)

            self.assertTrue(success)
            self.assertNotIn("ERROR:", log.read_text(encoding="utf-8"))

    def test_fix_stage_preserves_header_identity_and_pre_fix_line_numbers(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            fixture_dir = root / "fixture"
            fixture_dir.mkdir()
            source = fixture_dir / "Source.mm"
            header = fixture_dir / "nested" / "Declarations.h"
            header.parent.mkdir()
            source.write_text(
                '#include "nested/Declarations.h"\n// POSITIVE: FIX-TO: int replacement;\nint original;\n',
                encoding="utf-8",
            )
            header_text = "\n// NEGATIVE\nint forbidden;\n"
            header.write_text(header_text, encoding="utf-8")
            copied_headers = []

            def run_fix_or_syntax(command, **_kwargs):
                if "-fsyntax-only" in command:
                    return linttest.subprocess.CompletedProcess(command, 0, stdout="")
                fixed = Path(command[-1])
                copied_header = fixed.parent / header.relative_to(fixture_dir)
                copied_headers.append(copied_header.resolve())
                fixed.write_text(fixed.read_text(encoding="utf-8").replace("int original;", "int replacement;"))
                copied_header.write_text("// inserted\n" + header_text, encoding="utf-8")
                return linttest.subprocess.CompletedProcess(
                    command,
                    1,
                    stdout=(
                        f"{fixed}:3:1: warning: expected [aobus-example]\n"
                        f"{copied_header}:3:1: warning: forbidden [aobus-example]\n"
                    ),
                )

            with mock.patch.object(linttest.subprocess, "run", side_effect=run_fix_or_syntax):
                success, log = linttest._run_fix(linttest.Fixture(source, "aobus-example"), root, root)

            self.assertFalse(success)
            self.assertIn(f"explicitly NEGATIVE line {copied_headers[0]}:3", log.read_text(encoding="utf-8"))
            self.assertEqual(header.read_text(encoding="utf-8"), header_text)

    def test_syntax_command_uses_configured_compiler_and_libcxx_shim(self):
        fixed = Path("/tmp/fixed fixture.cpp")
        case_dir = Path("/tmp/fixture context")

        with mock.patch.dict(
            linttest.os.environ,
            {
                "CXX": "/nix/store/compiler/bin/clang++ --target=arm64-apple-darwin",
                "AOBUS_LIBCXX_EXPECTED_SHIM": "/nix/store/expected shim/include",
            },
            clear=True,
        ):
            command = linttest._syntax_command(fixed, case_dir)

        self.assertEqual(
            command[:2],
            ["/nix/store/compiler/bin/clang++", "--target=arm64-apple-darwin"],
        )
        self.assertEqual(command[-3:], ["-isystem", "/nix/store/expected shim/include", str(fixed)])
        self.assertNotIn("-fblocks", command)

    def test_syntax_command_defaults_to_clang_on_darwin(self):
        with mock.patch.dict(linttest.os.environ, {}, clear=True):
            with mock.patch.object(linttest.sys, "platform", "darwin"):
                command = linttest._syntax_command(Path("fixture.cpp"), Path("case"))

        self.assertEqual(command[0], "clang++")

    def test_objective_cpp_fix_uses_clang_even_when_cxx_is_gcc(self):
        with mock.patch.dict(linttest.os.environ, {"CXX": "g++"}, clear=True):
            command = linttest._syntax_command(Path("fixed.mm"), Path("case"))

        self.assertEqual(command[0], "clang++")
        self.assertEqual(command[-1], "fixed.mm")
        self.assertIn("-fblocks", command)

    def test_objective_cpp_fix_honors_its_compiler_override(self):
        with mock.patch.dict(
            linttest.os.environ, {"CXX": "g++", "OBJCXX": "clang++ --target=arm64-apple-darwin"}, clear=True
        ):
            command = linttest._syntax_command(Path("fixed.mm"), Path("case"))

        self.assertEqual(command[:2], ["clang++", "--target=arm64-apple-darwin"])
        self.assertIn("-fblocks", command)

    def test_blank_compiler_overrides_use_the_native_language_default(self):
        for suffix, variable, platform, expected in (
            (".mm", "OBJCXX", "linux", "clang++"),
            (".cpp", "CXX", "linux", "g++"),
            (".cpp", "CXX", "darwin", "clang++"),
        ):
            for value in ("", " \t "):
                with self.subTest(variable=variable, platform=platform, value=value):
                    with mock.patch.dict(linttest.os.environ, {variable: value}, clear=True):
                        with mock.patch.object(linttest.sys, "platform", platform):
                            command = linttest._syntax_command(Path("fixed" + suffix), Path("case"))
                    self.assertEqual(command[0], expected)

    def test_invalid_compiler_overrides_report_the_environment_variable(self):
        for suffix, variable in ((".mm", "OBJCXX"), (".cpp", "CXX")):
            for value in ('""', '"unterminated'):
                with self.subTest(variable=variable, value=value):
                    stderr = io.StringIO()
                    with mock.patch.dict(linttest.os.environ, {variable: value}, clear=True):
                        with contextlib.redirect_stderr(stderr), self.assertRaises(SystemExit):
                            linttest._syntax_command(Path("fixed" + suffix), Path("case"))
                    self.assertIn(variable, stderr.getvalue())

    def test_a_stage_reports_its_first_fixture_before_the_next_one_finishes(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            build_dir = Path(temp_dir) / "build"
            build_dir.mkdir()
            (build_dir / "compile_commands.json").touch()
            plugin = build_dir / "tool" / "lint" / "libAobusLintPlugin.so"
            plugin.parent.mkdir(parents=True)
            plugin.touch()
            result_log = Path(temp_dir) / "case.log"
            result_log.touch()
            # Neither fixture carries a FIX-TO annotation, so both belong to the
            # diagnostic stage and share one pool.
            fixtures: list[linttest.Fixture] = []
            for name in ("First", "Second"):
                path = Path(temp_dir) / f"{name}.cpp"
                path.write_text("int original;\n", encoding="utf-8")
                fixtures.append(linttest.Fixture(path, "aobus-example"))
            first, second = fixtures

            reported: list[str] = []
            first_reported = threading.Event()
            released: list[bool] = []

            class RecordingReporter(linttest.Reporter):
                def write(self, message: str = "") -> None:
                    reported.append(message)
                    if first.path.name in message:
                        first_reported.set()

            def run_diagnostic(fixture: linttest.Fixture, *_: Path) -> tuple[bool, Path]:
                if fixture is second:
                    # Far longer than appending one line takes, and short
                    # enough to keep a regression cheap: a stage that collects
                    # its results before reporting any of them can never set
                    # this event, so it times out here instead of deadlocking.
                    released.append(first_reported.wait(timeout=5))
                return True, result_log

            with (
                mock.patch.object(linttest, "discover_fixtures", return_value=fixtures),
                mock.patch.object(linttest, "Reporter", RecordingReporter),
                mock.patch.object(linttest, "_run_diagnostic", side_effect=run_diagnostic),
                mock.patch.object(linttest, "_run_replacement_smoke", return_value=(True, result_log)),
                mock.patch.object(linttest, "_run_compiler_error_smoke", return_value=(True, result_log)),
            ):
                self.assertEqual(linttest.run(build_dir, jobs=1), 0)

            self.assertEqual(released, [True])
            self.assertEqual(
                reported[:4],
                [
                    "=== Diagnostic Verification (2 fixtures) ===",
                    "  [PASS] aobus-example/First.cpp",
                    "  [PASS] aobus-example/Second.cpp",
                    "Diagnostics: passed",
                ],
            )

    def test_runner_aggregates_all_stages_and_removes_success_workspace(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            build_dir = Path(temp_dir) / "build"
            build_dir.mkdir()
            (build_dir / "compile_commands.json").touch()
            plugin = build_dir / "tool" / "lint" / "libAobusLintPlugin.so"
            plugin.parent.mkdir(parents=True)
            plugin.touch()
            fixture_path = Path(temp_dir) / "BasicFixture.cpp"
            fixture_path.write_text(
                "// POSITIVE: FIX-TO: int replacement;\nint original;\n",
                encoding="utf-8",
            )
            fixture = linttest.Fixture(fixture_path, "aobus-example")
            result_log = Path(temp_dir) / "case.log"
            result_log.touch()

            with mock.patch.object(linttest, "discover_fixtures", return_value=[fixture]):
                with mock.patch.object(
                    linttest,
                    "_run_diagnostic",
                    return_value=(True, result_log),
                ) as run_diagnostic:
                    with mock.patch.object(
                        linttest,
                        "_run_fix",
                        return_value=(True, result_log),
                    ) as run_fix:
                        with mock.patch.object(linttest, "_run_replacement_smoke", return_value=(True, result_log)):
                            with mock.patch.object(
                                linttest,
                                "_run_compiler_error_smoke",
                                return_value=(True, result_log),
                            ):
                                with mock.patch.object(linttest.shutil, "rmtree") as remove_tree:
                                    with contextlib.redirect_stdout(io.StringIO()):
                                        self.assertEqual(linttest.run(build_dir, jobs=1), 0)

            run_diagnostic.assert_not_called()
            run_fix.assert_called_once_with(fixture, build_dir, mock.ANY)
            remove_tree.assert_called_once()

    def test_runner_rejects_a_no_fix_and_fix_to_combination_before_applying_fixes(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            build_dir = root / "build"
            build_dir.mkdir()
            (build_dir / "compile_commands.json").touch()
            plugin = build_dir / "tool" / "lint" / "libAobusLintPlugin.so"
            plugin.parent.mkdir(parents=True)
            plugin.touch()
            fixture_path = root / "BasicFixture.cpp"
            fixture_path.write_text(
                "// NO-FIX\n// POSITIVE: FIX-TO: int replacement;\nint original;\n",
                encoding="utf-8",
            )
            fixture = linttest.Fixture(fixture_path, "aobus-example")

            with (
                mock.patch.object(linttest, "discover_fixtures", return_value=[fixture]),
                mock.patch.object(linttest, "_run_diagnostic") as run_diagnostic,
                mock.patch.object(linttest, "_run_fix") as run_fix,
            ):
                with contextlib.redirect_stdout(io.StringIO()) as stdout:
                    self.assertEqual(linttest.run(build_dir, jobs=1), 1)

            self.assertIn("fixture cannot combine NO-FIX and FIX-TO markers", stdout.getvalue())
            run_diagnostic.assert_not_called()
            run_fix.assert_not_called()

    def test_runner_rejects_a_no_fix_fixture_without_positive_expectations(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            build_dir = root / "build"
            build_dir.mkdir()
            (build_dir / "compile_commands.json").touch()
            plugin = build_dir / "tool" / "lint" / "libAobusLintPlugin.so"
            plugin.parent.mkdir(parents=True)
            plugin.touch()
            fixture_path = root / "BasicFixture.cpp"
            fixture_path.write_text("// NO-FIX\nint clean;\n", encoding="utf-8")
            fixture = linttest.Fixture(fixture_path, "aobus-example")

            with (
                mock.patch.object(linttest, "discover_fixtures", return_value=[fixture]),
                mock.patch.object(linttest, "_run_diagnostic") as run_diagnostic,
                mock.patch.object(linttest, "_run_fix") as run_fix,
            ):
                with contextlib.redirect_stdout(io.StringIO()) as stdout:
                    self.assertEqual(linttest.run(build_dir, jobs=1), 1)

            self.assertIn(
                "NO-FIX fixture must declare a POSITIVE expectation of aobus-example",
                stdout.getvalue(),
            )
            run_diagnostic.assert_not_called()
            run_fix.assert_not_called()


if __name__ == "__main__":
    unittest.main()
