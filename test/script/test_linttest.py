"""Tests for the Python lint integration runner."""

import contextlib
import io
import tempfile
import threading
import unittest
from dataclasses import replace
from pathlib import Path
from unittest import mock

from phase_support import PhaseRecorder as _PhaseRecorder

from ao.core import builddir, linttest, tidyconfig, tidyengine


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

    def test_expected_warnings_cannot_hide_untagged_compiler_errors(self):
        path = Path("fixture.cpp")
        fixture = linttest.Fixture(path, "aobus-example")
        warning = f"{path}:1:1: warning: expected [aobus-example]\n"
        for direct_host in (False, True):
            for status in (0, 1):
                for error in (
                    f"{path}:2:1: error: unknown type\n",
                    f"{path}:2:1: fatal error: missing header\n",
                    "fixture.cpp(2,1): error: unknown type\n",
                    "clang-cl: error: invalid argument\n",
                    "error: invalid argument\n",
                ):
                    with self.subTest(direct_host=direct_host, status=status, error=error):
                        result = linttest.subprocess.CompletedProcess([], status, stdout=warning + error)
                        errors = linttest._diagnostic_errors(
                            fixture, {path.resolve(): "int bad; // POSITIVE\n"}, result, direct_host=direct_host
                        )
                        self.assertIn("clang-tidy reported a fatal compiler diagnostic", errors)

    def test_expected_warnings_cannot_hide_a_normalized_portal_tool_failure(self):
        path = Path("fixture.cpp")
        fixture = linttest.Fixture(path, "aobus-example")
        warning = f"{path}:1:1: warning: expected [aobus-example]\n"
        source = {path.resolve(): "int value; // POSITIVE\n"}
        findings = linttest.subprocess.CompletedProcess([], 1, stdout=warning)
        self.assertEqual(linttest._diagnostic_errors(fixture, source, findings, direct_host=False), [])

        # run_parallel marks a native failure even though the portal's final
        # status is the same 1 used for findings. Matching all warning markers
        # must not turn that failure into a successful fixture.
        failed = linttest.subprocess.CompletedProcess([], 1, stdout=f"FAILED: {path}\n=== Failed logs ===\n{warning}")
        errors = linttest._diagnostic_errors(fixture, source, failed, direct_host=False)
        self.assertIn("portal reported a failed clang-tidy invocation", errors)

    def test_echoed_source_lines_are_not_compiler_errors(self):
        # Actual portal output captured from the governed PR126 claim-7 repro:
        # clang-tidy echoes the warned source line behind an "<indent>N | " gutter,
        # and comments containing fake error headers or the diagnostic tag inside
        # that echo must not read as fatal compiler errors. Before the fix the
        # greedy '^(?:.+:\\s+)?error:' and the whole-output tag substring matched
        # the echoed line and failed the fixture.
        path = Path("/tmp/case/Echoed.cpp")
        warned_line = (
            "    static int const staticValue = 1; // POSITIVE parse: error: bad "
            "fake.cpp:1:2: error: bad fake.cpp(1,2): error: bad [clang-diagnostic-error]\n"
        )
        source = (
            "// SPDX-License-Identifier: MIT\n"
            "// Copyright (c) 2024-2026 Aobus Contributors\n"
            "\n"
            "int main()\n"
            "{\n" + warned_line + "    (void)staticValue;\n"
            "    return 0;\n"
            "}\n"
        )
        fixture = linttest.Fixture(path, "readability-identifier-naming")
        warning = (
            f"{path}:6:22: warning: invalid case style for static constant 'staticValue' "
            "[readability-identifier-naming]\n"
            "    6 |     static int const staticValue = 1; // POSITIVE parse: error: bad "
            "fake.cpp:1:2: error: bad fake.cpp(1,2): error: bad [clang-diagnostic-error]\n"
            "      |                      ^~~~~~~~~~~\n"
            "      |                      kStaticValue\n"
            "    7 |     (void)staticValue;\n"
            "      |           ~~~~~~~~~~~\n"
            "      |           kStaticValue\n"
        )
        self.assertFalse(linttest.has_fatal_compiler_error(warning))
        self.assertFalse(linttest.has_fatal_compiler_error(warning, tag="[clang-diagnostic-error]"))

        result = linttest.subprocess.CompletedProcess([], 1, stdout=warning)
        errors = linttest._diagnostic_errors(fixture, {path: source}, result, direct_host=False)
        self.assertEqual(errors, [])

        # Genuine error headers of every producer shape still fail, including a
        # real error whose message quotes a warning header.
        fatals = (
            f"{path}:9:1: error: unknown type name 'ParseError' [clang-diagnostic-error]\n",
            f"{path}:9:1: fatal error: 'Missing.h' file not found [clang-diagnostic-error]\n",
            f"{path}(9,1): error: unresolved external symbol [clang-diagnostic-error]\n",
            "clang-cl: error: invalid argument\n",
            "error: invalid argument\n",
            f"{path}:9:1: error: cannot recover from 'fake.cpp:1:2: warning: bad'\n",
        )
        for fatal in fatals:
            with self.subTest(fatal=fatal.splitlines()[0]):
                self.assertTrue(linttest.has_fatal_compiler_error(fatal))
                result = linttest.subprocess.CompletedProcess([], 1, stdout=warning + fatal)
                errors = linttest._diagnostic_errors(fixture, {path: source}, result, direct_host=False)
                self.assertIn("clang-tidy reported a fatal compiler diagnostic", errors)
        self.assertTrue(
            linttest.has_fatal_compiler_error(
                f"{path}:9:1: error: injected [clang-diagnostic-error]\n", tag="[clang-diagnostic-error]"
            )
        )

    def test_diagnostic_header_parser_reads_first_headers_at_any_severity(self):
        for line, severity in (
            ("/tmp/x.cpp:6:22: warning: invalid case style [aobus]", "warning"),
            ("C:/src/x.cpp:3:5: error: unknown type\n", "error"),
            ("C:\\src\\x.cpp(9,1): error: unresolved\n", "error"),
            ("scratch.cpp(1): fatal error C1034: no include path set\n", "fatal error"),
            ("clang-tidy.exe: error: no checks enabled\n", "error"),
            ("clang-cl: error: invalid argument\n", "error"),
            ("fatal error: 'Missing.h' file not found\n", "fatal error"),
            ("error: invalid argument\n", "error"),
            ("remark: include preprocessing note\n", "remark"),
            ("x.cpp:3:1: note: in included file\n", "note"),
        ):
            with self.subTest(line=line):
                header = linttest.parse_diagnostic_header(line)
                self.assertIsNotNone(header, line)
                assert header is not None
                self.assertEqual(header.severity, severity)
        # Gutter echoes never classify, whatever text they contain.
        for line in (
            "    6 |     static int const x = 1; // parse: error: bad fake.cpp:1:2: error: bad\n",
            "      |                      ^~~~~~~~~~~\n",
            "In file included from /tmp/x.cpp:3:\n",
        ):
            with self.subTest(line=line):
                self.assertIsNone(linttest.parse_diagnostic_header(line), line)
        # The FIRST header classifies; a message quoting another header cannot.
        header = linttest.parse_diagnostic_header("/tmp/x.cpp:3:5: warning: quotes fake.cpp:1:2: error: bad\n")
        self.assertIsNotNone(header)
        assert header is not None
        self.assertEqual(header.severity, "warning")
        self.assertFalse(header.is_error)
        header = linttest.parse_diagnostic_header(
            "/tmp/x.cpp:9:1: error: cannot recover from 'y.cpp:1:2: warning: fake'\n"
        )
        self.assertIsNotNone(header)
        assert header is not None
        self.assertTrue(header.is_error)
        # A bare severity or driver banner stops the parse before any location
        # quoted in its message, in both directions. A bare MSVC code header
        # (severity, space, code) stops the parse the same way. A severity word
        # followed by filename continuation characters still starts a location
        # header's filename, including a line-leading bare-severity filename;
        # a banner followed by a non-severity falls back to a location read;
        # of the two location grammars the header whose severity starts
        # earliest is read, so a message quoting the other form cannot win.
        for line, severity in (
            ("warning: quoting fake.cpp:1:2: error: bad\n", "warning"),
            ("fatal error: quoting fake.cpp:1:2: warning: bad\n", "fatal error"),
            ("error: quoting fake.cpp:1:2: warning: bad\n", "error"),
            ("clang-cl: warning: quoting fake.cpp:1:2: error: bad\n", "warning"),
            ("clang-cl: error: quoting fake.cpp:1:2: warning: bad\n", "error"),
            ("warning C4996: quoting fake.cpp:1:2: error: bad\n", "warning"),
            ("/repo/warning.cpp:1:2: error: bad\n", "error"),
            ("warning.cpp:1:2: error: bad\n", "error"),
            ("error.cpp(1,2): warning: bad\n", "warning"),
            ("clang-cl: warningfile.cpp:1:2: error: bad\n", "error"),
            ("x.cpp(1,2): error: quotes y.cpp:3:4: warning: bad\n", "error"),
            ("x.cpp:3:4: warning: quotes y.cpp(1,2): error: bad\n", "warning"),
        ):
            with self.subTest(line=line):
                header = linttest.parse_diagnostic_header(line)
                self.assertIsNotNone(header, line)
                assert header is not None
                self.assertEqual(header.severity, severity)
                self.assertEqual(header.is_error, severity in ("error", "fatal error"))

    def test_bare_or_driver_headers_never_reinterpret_quoted_locations(self):
        # Both directions of the quoting bug: a bare warning whose message
        # quotes an error location is not a false failure, and a bare or
        # driver error whose message quotes a warning location fails even
        # though the fixture's expected warnings match the portal findings
        # status.
        path = Path("/tmp/case/Bare.cpp")
        fixture = linttest.Fixture(path, "aobus-example")
        source = {path: "int value; // POSITIVE\n"}
        warning = f"{path}:1:11: warning: expected [aobus-example]\nwarning: quoting fake.cpp:1:2: error: bad\n"
        findings = linttest.subprocess.CompletedProcess([], 1, stdout=warning)
        self.assertEqual(linttest._diagnostic_errors(fixture, source, findings, direct_host=False), [])
        passed = linttest.subprocess.CompletedProcess([], 0, stdout=warning)
        self.assertEqual(linttest._diagnostic_errors(fixture, source, passed, direct_host=True), [])

        fatals = (
            "error: quoting fake.cpp:1:2: warning: bad\n",
            "clang-cl: error: quoting fake.cpp:1:2: warning: bad\n",
        )
        for fatal in fatals:
            with self.subTest(fatal=fatal.strip()):
                self.assertTrue(linttest.has_fatal_compiler_error(fatal))
                result = linttest.subprocess.CompletedProcess([], 1, stdout=warning + fatal)
                errors = linttest._diagnostic_errors(fixture, source, result, direct_host=False)
                self.assertIn("clang-tidy reported a fatal compiler diagnostic", errors)

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
    def setUp(self):
        # These cases exercise the established POSIX runner independently of host.
        profile = mock.patch.object(builddir, "platform_profile", return_value=builddir.LINUX_PROFILE)
        profile.start()
        self.addCleanup(profile.stop)

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

    def test_portal_diagnostic_stage_rejects_arbitrary_failures_after_all_expected_warnings(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            path = root / "BasicFixture.cpp"
            path.write_text("int first; // POSITIVE\nint second; // POSITIVE\n", encoding="utf-8")
            output = "".join(f"{path}:{line}:1: warning: expected [aobus-example]\n" for line in (1, 2))
            for status in (2, 3, -6, 3221225477):
                with self.subTest(status=status):
                    result = linttest.subprocess.CompletedProcess([], status, stdout=output)
                    with mock.patch.object(linttest.subprocess, "run", return_value=result):
                        success, log = linttest._run_diagnostic(linttest.Fixture(path, "aobus-example"), root, root)
                    self.assertFalse(success)
                    self.assertIn(f"clang-tidy exited with status {status}", log.read_text(encoding="utf-8"))
                    self.assertNotIn("missing expected diagnostic", log.read_text(encoding="utf-8"))

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

    def test_compiler_error_smoke_rejects_crashes_and_arbitrary_failures_even_after_a_tagged_error(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            for status in (2, -6, 3221225477):
                with self.subTest(status=status):
                    result = linttest.subprocess.CompletedProcess(
                        [], status, stdout="fixture.cpp:1:1: error: injected [clang-diagnostic-error]\n"
                    )
                    with mock.patch.object(linttest.subprocess, "run", return_value=result):
                        success, log = linttest._run_compiler_error_smoke(root, root)
                    self.assertFalse(success)
                    self.assertIn(
                        "missing-include canary did not produce a fatal compiler diagnostic",
                        log.read_text(encoding="utf-8"),
                    )

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

    def test_portal_fix_stage_rejects_exit_two_after_expected_warning_and_fix(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            path = root / "fixture" / "BasicFixture.cpp"
            path.parent.mkdir()
            path.write_text("// POSITIVE: FIX-TO: int replacement;\nint original;\n", encoding="utf-8")

            def run_fix_or_syntax(command, **_kwargs):
                if "-fsyntax-only" in command:
                    return linttest.subprocess.CompletedProcess(command, 0, stdout="")
                fixed = Path(command[-1])
                fixed.write_text("// POSITIVE: FIX-TO: int replacement;\nint replacement;\n", encoding="utf-8")
                return linttest.subprocess.CompletedProcess(
                    command, 2, stdout=f"{fixed}:2:1: warning: expected [aobus-example]\n"
                )

            with mock.patch.object(linttest.subprocess, "run", side_effect=run_fix_or_syntax):
                success, log = linttest._run_fix(linttest.Fixture(path, "aobus-example"), root, root)
            self.assertFalse(success)
            self.assertIn("clang-tidy exited with status 2", log.read_text(encoding="utf-8"))
            self.assertNotIn("missing expected diagnostic", log.read_text(encoding="utf-8"))

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

            def run_diagnostic(fixture: linttest.Fixture, *_args: object, **_kwargs: object) -> tuple[bool, Path]:
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
            run_fix.assert_called_once_with(fixture, build_dir, mock.ANY, tools=None)
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


class StagePhaseInstrumentationTest(unittest.TestCase):
    def test_stages_record_counts_and_failure_statuses_in_order(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            build_dir = root / "build"
            build_dir.mkdir()
            (build_dir / "compile_commands.json").touch()
            plugin = build_dir / "tool" / "lint" / "libAobusLintPlugin.so"
            plugin.parent.mkdir(parents=True)
            plugin.touch()
            fixture_path = root / "BasicFixture.cpp"
            fixture_path.write_text("int original;\n", encoding="utf-8")
            fixture = linttest.Fixture(fixture_path, "aobus-example")
            case_log = root / "case.log"
            case_log.touch()
            recorder = _PhaseRecorder()

            with (
                mock.patch.object(builddir, "platform_profile", return_value=builddir.LINUX_PROFILE),
                mock.patch.object(linttest, "phases", recorder),
                mock.patch.object(linttest, "discover_fixtures", return_value=[fixture]),
                mock.patch.object(linttest, "_run_diagnostic", return_value=(True, case_log)),
                mock.patch.object(linttest, "_run_replacement_smoke", return_value=(False, case_log)),
                mock.patch.object(linttest, "_run_compiler_error_smoke", return_value=(True, case_log)),
                contextlib.redirect_stdout(io.StringIO()),
            ):
                self.assertEqual(linttest.run(build_dir, jobs=1), 1)

            self.assertEqual(
                recorder.records,
                [
                    {"name": "lint-diagnostics", "tree": "tidy", "jobs": 1, "count": 1, "status": 0},
                    {"name": "lint-fixes", "tree": "tidy", "jobs": 1, "count": 0, "status": 0},
                    {"name": "lint-replacements", "tree": "tidy", "count": 1, "status": 1},
                    {"name": "lint-compiler-errors", "tree": "tidy", "count": 4, "status": 0},
                ],
            )

    def test_fix_stage_records_the_native_syntax_status(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            fixture_path = root / "fixture" / "BasicFixture.cpp"
            fixture_path.parent.mkdir()
            fixture_path.write_text(
                "// POSITIVE: FIX-TO: int replacement;\nint original;\n",
                encoding="utf-8",
            )
            fixture = linttest.Fixture(fixture_path, "aobus-example")
            recorder = _PhaseRecorder()

            def run_fix_or_syntax(command, **_kwargs):
                if "-fsyntax-only" in command:
                    return linttest.subprocess.CompletedProcess(command, 2, stdout="")
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

            with (
                mock.patch.object(linttest, "phases", recorder),
                mock.patch.object(linttest, "_syntax_command", return_value=["test-compiler", "-fsyntax-only"]),
                mock.patch.object(linttest.subprocess, "run", side_effect=run_fix_or_syntax),
            ):
                success, _log = linttest._run_fix(fixture, root, root)

            self.assertFalse(success)
            self.assertEqual(
                recorder.records,
                [{"name": "lint-syntax", "tree": "tidy", "count": 1, "status": 2}],
            )


class WindowsLintFixtureTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus windows fixtures ")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.build = self.root / "build"
        self.sdk = self.root / "LLVM SDK"
        self.build.mkdir()
        (self.build / "compile_commands.json").write_text("[]", encoding="utf-8")
        (self.build / "CMakeCache.txt").write_text(
            f"AOBUS_LLVM_SDK_RESOLVED_ROOT:INTERNAL={self.sdk.as_posix()}\n"
            f"AOBUS_LLVM_SDK_RESOLVED_VERSION:INTERNAL={tidyengine.PINNED_LLVM_VERSION}\n",
            encoding="utf-8",
        )
        self.host = self.build / "tool/lint/AobusClangTidy.exe"
        self.host.parent.mkdir(parents=True)
        self.host.touch()
        for relative in tidyengine.LLVM_SDK_REQUIRED_FILES:
            path = self.sdk / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.touch()
        self.includes = [self.root / "MSVC include", self.root / "Windows SDK include"]
        for path in self.includes:
            path.mkdir()
        for patcher in (
            mock.patch.object(builddir, "platform_profile", return_value=builddir.WINDOWS_PROFILE),
            mock.patch.dict(
                linttest.os.environ,
                {"INCLUDE": ";".join(map(str, self.includes)), "CXX": "ambient-g++", "PATH": "ambient-tools"},
                clear=True,
            ),
        ):
            patcher.start()
            self.addCleanup(patcher.stop)

    def verified_context(self) -> tidyengine.TidyToolchain:
        """The toolchain context the portal preflight hands the fixture runner."""
        return tidyengine.TidyToolchain(
            clang_tidy=str(self.host),
            plugin=None,
            resource_dir=self.sdk / "lib" / "clang" / "22",
            expected_llvm_version=tidyengine.PINNED_LLVM_VERSION,
            sdk_root=self.sdk,
        )

    def test_tools_use_only_configured_static_host_sdk_and_exact_vs_includes(self):
        tools = linttest._windows_fixture_tools(self.build, self.verified_context())
        self.assertEqual(tools.tidy, str(self.host))
        self.assertEqual(tools.compiler, str(self.sdk / "bin/clang-cl.exe"))
        self.assertEqual(tools.replacements, str(self.sdk / "bin/clang-apply-replacements.exe"))
        resource = self.sdk / "lib/clang/22"
        self.assertIn(f"/clang:-resource-dir={resource}", tools.flags)
        self.assertIn("/X", tools.flags)
        self.assertEqual(
            [flag for flag in tools.flags if flag.startswith("/imsvc")],
            [f"/imsvc{resource / 'include'}", *(f"/imsvc{path}" for path in self.includes)],
        )
        command = linttest._syntax_command(self.root / "fixed.cpp", self.root, tools=tools)
        self.assertEqual(command[0], tools.compiler)
        self.assertIn("/clang:-std=c++26", command)
        self.assertIn("/clang:-fsyntax-only", command)
        self.assertNotIn("ambient-g++", command)
        self.assertNotIn("g++", command)

    def test_missing_or_ambiguous_static_artifact_never_uses_sdk_tidy_or_unix_plugin(self):
        # The tidy host is resolved once, at preflight time, by clang_tool; the
        # fixture tools consume that verified host from the toolchain context.
        self.host.unlink()
        (self.host.parent / "libAobusLintPlugin.so").touch()
        with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
            tidyengine.clang_tool(self.build, "clang-tidy")
        for config in ("Debug", "Release"):
            path = self.host.parent / config / self.host.name
            path.parent.mkdir()
            path.touch()
        with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
            tidyengine.clang_tool(self.build, "clang-tidy")
        (self.host.parent / "Debug" / self.host.name).unlink()
        self.assertEqual(
            tidyengine.clang_tool(self.build, "clang-tidy"), str(self.host.parent / "Release" / self.host.name)
        )

    def test_every_required_configured_sdk_file_fails_closed_when_missing(self):
        for relative in tidyengine.LLVM_SDK_REQUIRED_FILES:
            with self.subTest(relative=relative):
                path = self.sdk / relative
                path.unlink()
                with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
                    linttest._windows_fixture_tools(self.build, self.verified_context())
                path.touch()

    def test_the_windows_suite_requires_a_prepared_toolchain_context(self):
        context = self.verified_context()
        for field in ("expected_llvm_version", "resource_dir", "sdk_root"):
            with self.subTest(field=field), self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
                linttest._windows_fixture_tools(self.build, replace(context, **{field: None}))
        with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
            linttest.run(self.build)

    def test_a_wholly_empty_include_environment_fails_closed(self):
        for value in ("", ";", ";;", "  "):
            with self.subTest(value=value), mock.patch.dict(linttest.os.environ, {"INCLUDE": value}):
                with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
                    linttest._windows_fixture_tools(self.build, self.verified_context())

    def test_compiler_tolerant_include_shapes_follow_the_compiler_search(self):
        # Native cl.exe accepts trailing semicolons, empty segments, and absent
        # directories; the fixture tools mirror that search, preserving
        # nonempty order, instead of a directory policy.
        for label, value in (
            ("trailing semicolon", ";".join(map(str, self.includes)) + ";"),
            ("empty middle entry", f"{self.includes[0]};;{self.includes[1]}"),
            ("stale directory", f"{self.includes[0]};{self.root / 'missing'}"),
        ):
            with self.subTest(label=label), mock.patch.dict(linttest.os.environ, {"INCLUDE": value}):
                tools = linttest._windows_fixture_tools(self.build, self.verified_context())
            expected = [entry for entry in value.split(";") if entry.strip()]
            resource = self.sdk / "lib/clang/22"
            self.assertEqual(
                [flag for flag in tools.flags if flag.startswith("/imsvc")],
                [f"/imsvc{resource / 'include'}", *(f"/imsvc{entry}" for entry in expected)],
            )

    def test_verified_toolchain_context_supplies_the_fixture_tools(self):
        context = self.verified_context()
        # A stale build cache can name another SDK entirely; the verified
        # context's SDK root is the single source for the fixture-only tools
        # and the required-file check.
        (self.build / "CMakeCache.txt").write_text(
            f"AOBUS_LLVM_SDK_RESOLVED_ROOT:INTERNAL={(self.root / 'Another SDK').as_posix()}\n"
            "AOBUS_LLVM_SDK_RESOLVED_VERSION:INTERNAL=0.0.0\n",
            encoding="utf-8",
        )
        tools = linttest._windows_fixture_tools(self.build, context)
        self.assertEqual(tools.tidy, str(self.host))
        self.assertIn(f"/clang:-resource-dir={self.sdk / 'lib/clang/22'}", tools.flags)
        self.assertEqual(tools.compiler, str(self.sdk / "bin/clang-cl.exe"))
        self.assertEqual(tools.replacements, str(self.sdk / "bin/clang-apply-replacements.exe"))
        # Fixture-only capabilities stay enforced against the context's SDK:
        # the complete required-file set is validated from the verified root.
        for relative in tidyengine.LLVM_SDK_REQUIRED_FILES:
            (self.sdk / relative).unlink()
            with (
                self.subTest(relative=relative),
                self.assertRaises(SystemExit),
                contextlib.redirect_stderr(io.StringIO()),
            ):
                linttest._windows_fixture_tools(self.build, context)
            (self.sdk / relative).touch()

    def test_run_forwards_the_verified_toolchain_on_windows(self):
        context = self.verified_context()
        fixture_path = self.root / "Basic.cpp"
        fixture_path.write_text("int clean;\n", encoding="utf-8")
        fixture = linttest.Fixture(fixture_path, "aobus-example")
        log = self.root / "case.log"
        log.touch()
        with (
            mock.patch.object(linttest, "discover_fixtures", return_value=[fixture]),
            mock.patch.object(linttest, "_windows_fixture_tools") as fixture_tools,
            mock.patch.object(linttest, "_run_diagnostic", return_value=(True, log)) as run_diagnostic,
            mock.patch.object(linttest, "_run_replacement_smoke", return_value=(True, log)),
            mock.patch.object(linttest, "_run_compiler_error_smoke", return_value=(True, log)),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            self.assertEqual(linttest.run(self.build, jobs=1, toolchain=context), 0)
        fixture_tools.assert_called_once_with(self.build, context)
        self.assertEqual(run_diagnostic.call_args.kwargs["tools"], fixture_tools.return_value)

    def test_windows_syntax_without_governed_tools_cannot_fall_back_to_cxx(self):
        with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
            linttest._syntax_command(self.root / "fixed.cpp", self.root)

    def test_native_fixture_command_has_fixed_cl_flags_shared_config_and_no_load(self):
        tools = linttest._windows_fixture_tools(self.build, self.verified_context())
        fixture = linttest.Fixture(self.root / "fixture.h", "aobus-example")
        command = linttest._tidy_command(
            fixture, self.build, "--fix", *linttest._fixture_tidy_args(self.root, windows=True), tools=tools
        )
        self.assertEqual(command[0], str(self.host))
        self.assertEqual(command[command.index("--") - 1], str(fixture.path))
        self.assertEqual(command[command.index("--") + 1 :], list(tools.flags))
        self.assertIn("--fix", command)
        self.assertIn("--extra-arg=/clang:-std=c++26", command)
        self.assertIn("--extra-arg=/WX-", command)
        self.assertIn(f"-config={tidyconfig.config_for(tidyconfig.checks_for('STRICT', 'aobus-example'))}", command)
        self.assertFalse(any("load=" in argument or "tidy-arg=" in argument for argument in command))

    def test_windows_fixture_config_matches_the_portal_strict_policy(self):
        tools = linttest._windows_fixture_tools(self.build, self.verified_context())
        # The portal runs explicit fixtures as STRICT; a check disabled there
        # must stay disabled for the direct host, and every existing fixture
        # check must resolve through the shared policy.
        for check in ("aobus-modernize-braced-initialization", "modernize-use-auto"):
            fixture = linttest.Fixture(self.root / "fixture.cpp", check)
            command = linttest._tidy_command(fixture, self.build, tools=tools)
            expected = tidyconfig.config_for(tidyconfig.checks_for("STRICT", check))
            self.assertIn(f"-config={expected}", command)
        self.assertIn("-modernize-use-auto", tidyconfig.checks_for("STRICT", "modernize-use-auto").split(","))
        for fixture in linttest.discover_fixtures():
            with self.subTest(check=fixture.check):
                command = linttest._tidy_command(fixture, self.build, tools=tools)
                expected = tidyconfig.config_for(tidyconfig.checks_for("STRICT", fixture.check))
                self.assertIn(f"-config={expected}", command)

    def test_native_no_fix_report_still_rejects_replacements(self):
        fixture = self.root / "NoFix.cpp"
        fixture.write_text("// NO-FIX\nint bad; // POSITIVE\n", encoding="utf-8")
        tools = linttest._windows_fixture_tools(self.build, self.verified_context())
        for replacements, expected in (("Replacements: []", True), ("Replacements: ~", False)):
            with self.subTest(replacements=replacements):

                def run_tidy(command, replacements=replacements, **_kwargs):
                    export = next(
                        Path(arg.removeprefix("-export-fixes=")) for arg in command if arg.startswith("-export-fixes=")
                    )
                    export.write_text(pinned_fixes_export(str(fixture), replacements), encoding="utf-8")
                    return linttest.subprocess.CompletedProcess(
                        command, 0, stdout=f"{fixture}:2:1: warning: expected [aobus-example]\n"
                    )

                with mock.patch.object(linttest.subprocess, "run", side_effect=run_tidy):
                    success, _ = linttest._run_diagnostic(
                        linttest.Fixture(fixture, "aobus-example"), self.build, self.root, tools=tools
                    )
                self.assertEqual(success, expected)

    def test_native_fix_requires_expected_output_and_governed_syntax_success(self):
        fixture = self.root / "fixture" / "Fix.cpp"
        fixture.parent.mkdir()
        fixture.write_text("// POSITIVE: FIX-TO: int replacement;\nint original;\n", encoding="utf-8")
        tools = linttest._windows_fixture_tools(self.build, self.verified_context())
        calls = []

        def run_fix(command, **_kwargs):
            calls.append(command)
            if command[0] == tools.compiler:
                return linttest.subprocess.CompletedProcess(command, 1, stdout="syntax failed\n")
            fixed = Path(command[command.index("--") - 1])
            fixed.write_text(
                fixed.read_text(encoding="utf-8").replace("int original;", "int replacement;"), encoding="utf-8"
            )
            return linttest.subprocess.CompletedProcess(
                command, 0, stdout=f"{fixed}:2:1: warning: expected [aobus-example]\n"
            )

        with mock.patch.object(linttest.subprocess, "run", side_effect=run_fix):
            success, log = linttest._run_fix(
                linttest.Fixture(fixture, "aobus-example"), self.build, self.root, tools=tools
            )
        self.assertFalse(success)
        self.assertIn("auto-fixed fixture does not compile", log.read_text(encoding="utf-8"))
        self.assertEqual([command[0] for command in calls], [tools.tidy, tools.compiler])

    def test_direct_host_requires_zero_exit_even_after_all_expected_warnings(self):
        path = self.root / "Diagnostic.cpp"
        path.write_text("int first; // POSITIVE\nint second; // POSITIVE\n", encoding="utf-8")
        output = "".join(f"{path}:{line}:1: warning: expected [aobus-example]\n" for line in (1, 2))
        tools = linttest._windows_fixture_tools(self.build, self.verified_context())
        for status in (0, 1, 2, 3, -6, -1073741819, 3221225477, 1073741845):
            with self.subTest(status=status):
                result = linttest.subprocess.CompletedProcess([], status, stdout=output)
                with mock.patch.object(linttest.subprocess, "run", return_value=result):
                    success, log = linttest._run_diagnostic(
                        linttest.Fixture(path, "aobus-example"), self.build, self.root, tools=tools
                    )
                self.assertEqual(success, status == 0)
                text = log.read_text(encoding="utf-8")
                self.assertNotIn("missing expected diagnostic", text)
                if status:
                    self.assertIn(f"clang-tidy exited with status {status}", text)

    def test_direct_host_fix_requires_zero_exit_even_after_all_expected_warnings_and_fixes(self):
        path = self.root / "fixture" / "Fix.cpp"
        path.parent.mkdir()
        path.write_text(
            "// POSITIVE: FIX-TO: int first;\nint oldFirst;\n// POSITIVE: FIX-TO: int second;\nint oldSecond;\n",
            encoding="utf-8",
        )
        tools = linttest._windows_fixture_tools(self.build, self.verified_context())
        for status in (0, 1, 2, 3, -6, -1073741819, 3221225477, 1073741845):
            with self.subTest(status=status):

                def run_fix_or_syntax(command, status=status, **_kwargs):
                    if command[0] == tools.compiler:
                        return linttest.subprocess.CompletedProcess(command, 0, stdout="")
                    fixed = Path(command[command.index("--") - 1])
                    fixed.write_text(
                        fixed.read_text(encoding="utf-8")
                        .replace("int oldFirst;", "int first;")
                        .replace("int oldSecond;", "int second;"),
                        encoding="utf-8",
                    )
                    output = "".join(f"{fixed}:{line}:1: warning: expected [aobus-example]\n" for line in (2, 4))
                    return linttest.subprocess.CompletedProcess(command, status, stdout=output)

                with mock.patch.object(linttest.subprocess, "run", side_effect=run_fix_or_syntax):
                    success, log = linttest._run_fix(
                        linttest.Fixture(path, "aobus-example"), self.build, self.root, tools=tools
                    )
                self.assertEqual(success, status == 0)
                text = log.read_text(encoding="utf-8")
                self.assertNotIn("missing expected diagnostic", text)
                self.assertNotIn("auto-fixed fixture does not compile", text)
                self.assertNotIn("but clang-tidy made no changes", text)
                if status:
                    self.assertIn(f"clang-tidy exited with status {status}", text)

    def test_native_replacement_smoke_uses_governed_tool(self):
        tools = linttest._windows_fixture_tools(self.build, self.verified_context())

        def apply(command, **_kwargs):
            (Path(command[-1]) / "sample.cpp").write_text("/*a*//*b*/int main() {}\n", encoding="utf-8")
            return linttest.subprocess.CompletedProcess(command, 0, stdout="")

        with mock.patch.object(linttest.subprocess, "run", side_effect=apply) as run:
            self.assertTrue(linttest._run_replacement_smoke(self.root, tools=tools)[0])
        self.assertEqual(run.call_args.args[0][0], tools.replacements)
        self.assertIn("--ignore-insert-conflict", run.call_args.args[0])

    def test_native_runner_retains_cpp_header_fix_no_fix_and_smoke_stages_and_accounts_for_mm(self):
        fixtures = []
        for name, source in (
            ("Diagnostic.h", "// NO-FIX\nint bad; // POSITIVE\n"),
            ("Fix.cpp", "// POSITIVE: FIX-TO: int replacement;\nint original;\n"),
            ("Deferred.mm", "int value;\n"),
        ):
            path = self.root / name
            path.write_text(source, encoding="utf-8")
            fixtures.append(linttest.Fixture(path, "aobus-example"))
        log = self.root / "case.log"
        log.touch()
        with (
            mock.patch.object(linttest, "discover_fixtures", return_value=fixtures),
            mock.patch.object(linttest, "_run_diagnostic", return_value=(True, log)) as diagnostic,
            mock.patch.object(linttest, "_run_fix", return_value=(True, log)) as fix,
            mock.patch.object(linttest, "_run_replacement_smoke", return_value=(True, log)) as replacement,
            mock.patch.object(linttest, "_run_compiler_error_smoke", return_value=(True, log)) as compiler,
            contextlib.redirect_stdout(io.StringIO()) as output,
        ):
            self.assertEqual(linttest.run(self.build, jobs=1, toolchain=self.verified_context()), 0)
        self.assertEqual(diagnostic.call_args.args[0], fixtures[0])
        self.assertEqual(fix.call_args.args[0], fixtures[1])
        self.assertIn("tools", diagnostic.call_args.kwargs)
        replacement.assert_called_once()
        compiler.assert_called_once()
        self.assertIn("[DEFER] aobus-example/Deferred.mm", output.getvalue())
        self.assertIn("2 selected, 1 Objective-C++ deferred (not tested)", output.getvalue())


if __name__ == "__main__":
    unittest.main()
