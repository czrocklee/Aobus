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

_FIX_ARGUMENT_PREFIXES = (
    "--export-fixes",
    "-export-fixes",
    "--fix-errors",
    "--fix-notes",
    "--fix",
    "-fix",
)
_TIDY_ARGUMENT_WRAPPERS = ("--tidy-arg=", "--extra-arg=")


def _raw_tidy_argument(token: str) -> str:
    raw = token
    while True:
        for wrapper in _TIDY_ARGUMENT_WRAPPERS:
            if raw.startswith(wrapper):
                raw = raw.removeprefix(wrapper)
                break
        else:
            return raw


def _is_fix_argument(token: str) -> bool:
    raw = _raw_tidy_argument(token)
    return any(raw == prefix or raw.startswith(f"{prefix}=") for prefix in _FIX_ARGUMENT_PREFIXES)


def assert_no_fix_arguments(test: unittest.TestCase, command: list[str]) -> None:
    """Fail if any token is a fix or export flag, including wrapped and joined forms."""
    offenders = [token for token in command if _is_fix_argument(token)]
    test.assertEqual(offenders, [])


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

    def test_expectations_support_named_checks(self):
        source = """\
int current; // POSITIVE
int named; // POSITIVE:aobus-other
int forbidden; // NEGATIVE:aobus-other
"""

        expectations = linttest.parse_expectations(source, "aobus-current")

        self.assertEqual(
            expectations.expected,
            {
                1: {"aobus-current"},
                2: {"aobus-other"},
            },
        )
        self.assertEqual(expectations.negated, {3: {"aobus-other"}})
        self.assertEqual(expectations.messages, {})

    def test_positive_pipe_requires_diagnostic_text_on_the_target_line(self):
        source = """\
// POSITIVE | use starts_with() member instead of find == 0
auto positive = text.find(prefix) == 0;
int other; // POSITIVE:aobus-other | use !starts_with() member instead of find != 0
int plain; // POSITIVE
"""
        expectations = linttest.parse_expectations(source, "aobus-example")

        self.assertEqual(
            expectations.messages,
            {
                (2, "aobus-example"): "use starts_with() member instead of find == 0",
                (3, "aobus-other"): "use !starts_with() member instead of find != 0",
            },
        )
        self.assertEqual(expectations.expected[4], {"aobus-example"})
        self.assertNotIn(4, {line for line, _alias in expectations.messages})

    def test_empty_explicit_pipe_is_a_marker_contract_error(self):
        path = Path("fixture.cpp")
        for source in (
            "// POSITIVE |\nint bad;\n",
            "// POSITIVE |   \nint bad;\n",
            "int bad; // POSITIVE:aobus-other|\n",
        ):
            with self.subTest(source=source), self.assertRaises(ValueError) as raised:
                linttest.parse_expectations(source, "aobus-example", path=path)
            self.assertIn("empty explicit diagnostic text in POSITIVE marker", str(raised.exception))
            self.assertIn(f"{path}:", str(raised.exception))
            self.assertIn("omit '|' or provide required text", str(raised.exception))

    def test_message_assertion_fails_wrong_polarity_and_invalid_api(self):
        path = Path("/tmp/case.cpp")
        source = "// POSITIVE | use !starts_with() member instead of find != 0\nint bad;\n"
        cases = (
            f"{path}:2:1: warning: use starts_with() member instead of find == 0 [aobus-example]\n",
            f"{path}:2:1: warning: use std::starts_with instead of find != 0 [aobus-example]\n",
        )
        for output in cases:
            with self.subTest(output=output):
                errors = linttest.verify_diagnostics({path: source}, output, "aobus-example")
                self.assertTrue(
                    any("diagnostic text mismatch" in error and "[aobus-example]" in error for error in errors)
                )
                self.assertFalse(any("missing expected diagnostic on line" in error for error in errors))
        matched = f"{path}:2:1: warning: use !starts_with() member instead of find != 0 [aobus-example]\n"
        self.assertEqual(linttest.verify_diagnostics({path: source}, matched, "aobus-example"), [])

    def test_message_assertion_uses_the_context_header_path(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            source = root / "Source.mm"
            header = root / "Declarations.h"
            sources = {
                source: "int ok;\n",
                header: "// POSITIVE | use std::ranges::contains instead of std::ranges::find != end\nint bad;\n",
            }
            required = "use std::ranges::contains instead of std::ranges::find != end"
            wrong = f"{header}:2:1: warning: use std::ranges::find != end [aobus-example]\n"
            errors = linttest.verify_diagnostics(sources, wrong, "aobus-example")
            self.assertTrue(
                any(str(header.resolve()) in error and "diagnostic text mismatch" in error for error in errors)
            )
            misplaced = f"{source}:2:1: warning: {required} [aobus-example]\n"
            errors = linttest.verify_diagnostics(sources, misplaced, "aobus-example")
            self.assertTrue(
                any("missing expected diagnostic text" in error and str(header.resolve()) in error for error in errors)
            )
            matched = f"{header}:2:1: warning: {required} [aobus-example]\n"
            self.assertEqual(linttest.verify_diagnostics(sources, matched, "aobus-example"), [])

    def test_named_alias_without_message_stays_optional(self):
        source = "int named; // POSITIVE:aobus-other | use starts_with() member instead of find == 0\n"
        self.assertEqual(
            linttest.verify_diagnostics(
                {Path("case.cpp"): "int named; // POSITIVE:aobus-other\n"}, "", "aobus-current"
            ),
            [],
        )
        errors = linttest.verify_diagnostics({Path("case.cpp"): source}, "", "aobus-current")
        self.assertTrue(
            any("missing expected diagnostic text" in error and "[aobus-other]" in error for error in errors)
        )
        self.assertFalse(any("missing expected diagnostic on line" in error for error in errors))

    def test_any_same_alias_warning_can_satisfy_the_required_substring(self):
        path = Path("/tmp/case.cpp")
        source = "int bad; // POSITIVE | contains\n"
        output = (
            f"{path}:1:1: warning: unrelated wording [aobus-example]\n"
            f"{path}:1:1: warning: use std::ranges::contains instead [aobus-example]\n"
        )
        self.assertEqual(linttest.verify_diagnostics({path: source}, output, "aobus-example"), [])

    def test_negative_prose_aliases_use_the_first_word(self):
        prose = """\
// NEGATIVE: a defaulted destructor still owns cleanup through an RAII member.
// NEGATIVE: an inherited RAII base still owns cleanup for the derived value.
// NEGATIVE: an abstract lifecycle interface cannot itself be constructed as a discarded value.
// NEGATIVE: a raw pointer observes an RAII object but does not own its cleanup.
// NEGATIVE: a dependent generic parameter can be instantiated as either a value or a pointer.
// NEGATIVE: a dependent member can be instantiated as either a value or a pointer.
// NEGATIVE: a parameter carries an established lock lifetime across a call boundary.
"""
        expectations = linttest.parse_expectations(prose, "aobus-example")
        self.assertEqual({alias for checks in expectations.negated.values() for alias in checks}, {"a", "an"})

    def test_marker_conflicts_fail_instead_of_letting_a_later_marker_pass(self):
        path = Path("/tmp/conflict.cpp")
        overlapping = (
            (
                "// POSITIVE | alpha-token\nint bad; // POSITIVE | beta-token\n",
                f"{path.resolve()}:2:1: warning: beta-token only [aobus-example]\n",
                "conflicting POSITIVE diagnostic text",
            ),
            (
                "// POSITIVE\nint bad; // NEGATIVE\n",
                f"{path.resolve()}:2:1: warning: present [aobus-example]\n",
                "conflicting POSITIVE and NEGATIVE markers",
            ),
            (
                "// NEGATIVE\nint bad; // POSITIVE\n",
                f"{path.resolve()}:2:1: warning: present [aobus-example]\n",
                "conflicting POSITIVE and NEGATIVE markers",
            ),
        )
        for source, output, fragment in overlapping:
            with self.subTest(fragment=fragment, source=source), self.assertRaises(ValueError) as raised:
                linttest.verify_diagnostics({path: source}, output, "aobus-example")
            message = str(raised.exception)
            self.assertIn(fragment, message)
            self.assertIn(str(path.resolve()), message)
            self.assertIn("on line 2", message)

        physical = "int bad; // POSITIVE:alias-a // NEGATIVE:alias-b\n"
        with self.assertRaises(ValueError) as raised:
            linttest.parse_expectations(physical, "aobus-example", path=Path("fixture.cpp"))
        self.assertIn("unsupported multiple lint fixture markers on one line", str(raised.exception))
        self.assertIn("fixture.cpp:1", str(raised.exception))

        distinct = "// POSITIVE:alias-a | alpha-token\nint bad; // POSITIVE:alias-b | beta-token\n"
        both = (
            f"{path.resolve()}:2:1: warning: alpha-token [alias-a]\n"
            f"{path.resolve()}:2:1: warning: beta-token [alias-b]\n"
        )
        self.assertEqual(linttest.verify_diagnostics({path: distinct}, both, "aobus-example"), [])
        identical = "// POSITIVE | alpha-token\nint bad; // POSITIVE | alpha-token\n"
        matched = f"{path.resolve()}:2:1: warning: alpha-token [aobus-example]\n"
        self.assertEqual(linttest.verify_diagnostics({path: identical}, matched, "aobus-example"), [])
        preserved = "// POSITIVE | alpha-token\nint bad; // POSITIVE\n"
        self.assertEqual(
            linttest.parse_expectations(preserved, "aobus-example").messages,
            {(2, "aobus-example"): "alpha-token"},
        )
        literal = "int NEGATIVE = 1; // POSITIVE | mentions POSITIVE and // notes\n"
        expectations = linttest.parse_expectations(literal, "aobus-example")
        self.assertEqual(expectations.expected, {1: {"aobus-example"}})
        self.assertEqual(expectations.negated, {})
        self.assertEqual(expectations.messages[(1, "aobus-example")], "mentions POSITIVE and // notes")

    def test_live_fixtures_parse(self):
        saw_message = False
        for fixture in linttest.discover_fixtures():
            for path, source in linttest._fixture_sources(fixture).items():
                expectations = linttest.parse_expectations(source, fixture.check, path=path)
                self.assertTrue(all(required for required in expectations.messages.values()), path)
                saw_message = saw_message or bool(expectations.messages)
        self.assertTrue(saw_message)

    def test_fix_argument_helper_rejects_wrapped_and_joined_forms(self):
        assert_no_fix_arguments(self, ["tidy", "--tidy-arg=--extra-arg=-std=c++26", "--no-build", "fixture.cpp"])
        rejected = (
            ["tidy", "--tidy-arg=-export-fixes=/tmp/fixes.yaml"],
            ["tidy", "-export-fixes=/tmp/fixes.yaml"],
            ["tidy", "--fix=true"],
            ["tidy", "--fix-errors=true"],
            ["tidy", "--fix-notes=true"],
        )
        self.assertEqual(len(rejected), 5)
        for command in rejected:
            with self.subTest(command=command), self.assertRaises(AssertionError):
                assert_no_fix_arguments(self, command)

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

    def test_tidy_command_reuses_the_current_python_cli_without_building(self):
        fixture = linttest.Fixture(Path("/tmp/BasicFixture.cpp"), "aobus-example")

        command = linttest._tidy_command(fixture, Path("/tmp/build"), "--tidy-arg=--extra-arg=-std=c++26")

        self.assertEqual(command[:3], [linttest.sys.executable, "-m", "ao"])
        self.assertIn("--no-build", command)
        self.assertIn("--tidy-arg=--extra-arg=-std=c++26", command)
        self.assertEqual(command[-1], str(fixture.path))
        assert_no_fix_arguments(self, command)

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
        assert_no_fix_arguments(self, list(args))

    def test_objective_cpp_fixture_uses_objective_cpp_language(self):
        args = linttest._fixture_tidy_args(Path("/tmp/fixture"), "objective-c++")

        self.assertIn("--tidy-arg=--extra-arg=objective-c++", args)
        self.assertIn("--tidy-arg=--extra-arg=-fblocks", args)
        self.assertNotIn("--tidy-arg=--extra-arg=c++", args)

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

    def test_diagnostic_stage_keeps_original_bytes_and_passes_no_fix_arguments(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            fixture_dir = root / "aobus-example"
            fixture_dir.mkdir()
            fixture_path = fixture_dir / "BasicFixture.cpp"
            fixture_path.write_text('#include "Sibling.h"\nint original; // POSITIVE\n', encoding="utf-8")
            sibling = fixture_dir / "Sibling.h"
            sibling.write_text("struct Sibling {};\n", encoding="utf-8")
            fixture = linttest.Fixture(fixture_path, "aobus-example")
            original_bytes = fixture_path.read_bytes()
            sibling_bytes = sibling.read_bytes()
            commands = []

            def run_native(command, **_kwargs):
                commands.append(command)
                return linttest.subprocess.CompletedProcess(
                    command, 1, stdout=f"{fixture_path}:2:1: warning: expected [aobus-example]\n"
                )

            with mock.patch.object(linttest.subprocess, "run", side_effect=run_native):
                success, _log = linttest._run_diagnostic(fixture, root, root)

            self.assertTrue(success)
            self.assertEqual(len(commands), 1)
            assert_no_fix_arguments(self, commands[0])
            self.assertEqual(commands[0][-1], str(fixture_path))
            self.assertEqual(fixture_path.read_bytes(), original_bytes)
            self.assertEqual(sibling.read_bytes(), sibling_bytes)

    def test_invalid_expectations_fail_before_any_native_work(self):
        cases = (
            ("// POSITIVE |\nint bad;\n", "empty explicit diagnostic text"),
            ("// POSITIVE | alpha\nint bad; // POSITIVE | beta\n", "conflicting POSITIVE diagnostic text"),
            ("int bad; // POSITIVE // NEGATIVE\n", "unsupported multiple lint fixture markers"),
        )
        for source, fragment in cases:
            with self.subTest(fragment=fragment):
                with tempfile.TemporaryDirectory() as temp_dir:
                    root = Path(temp_dir)
                    build_dir = root / "build"
                    build_dir.mkdir()
                    (build_dir / "compile_commands.json").touch()
                    plugin = build_dir / "tool" / "lint" / "libAobusLintPlugin.so"
                    plugin.parent.mkdir(parents=True)
                    plugin.touch()
                    fixture_path = root / "BasicFixture.cpp"
                    fixture_path.write_text(source, encoding="utf-8")
                    fixture = linttest.Fixture(fixture_path, "aobus-example")
                    recorder = _PhaseRecorder()
                    with (
                        mock.patch.object(linttest, "phases", recorder),
                        mock.patch.object(linttest, "discover_fixtures", return_value=[fixture]),
                        mock.patch.object(linttest.subprocess, "run") as run_native,
                        contextlib.redirect_stdout(io.StringIO()) as output,
                    ):
                        self.assertEqual(linttest.run(build_dir, jobs=1), 1)
                    run_native.assert_not_called()
                    self.assertEqual(recorder.records, [])
                    text = output.getvalue()
                    self.assertIn(fragment, text)
                    self.assertIn(str(fixture_path.resolve()), text)
                    self.assertIn("ERROR:", text)

    def test_expectation_errors_in_mm_and_context_header_fail_before_native_work(self):
        original_discover = linttest.discover_fixtures
        for profile in (builddir.LINUX_PROFILE, builddir.WINDOWS_PROFILE):
            with self.subTest(profile=profile.name), tempfile.TemporaryDirectory() as temp_dir:
                root = Path(temp_dir)
                fixture_root = root / "fixture"
                check = fixture_root / "aobus-example"
                nested = check / "nested"
                nested.mkdir(parents=True)
                (check / "Clean.cpp").write_text("int clean;\n", encoding="utf-8")
                source = check / "Source.mm"
                source.write_text('#include "nested/OnlyContext.h"\n// POSITIVE |\nint value;\n', encoding="utf-8")
                header = nested / "OnlyContext.h"
                header.write_text("// POSITIVE\nint hidden; // NEGATIVE\n", encoding="utf-8")
                build_dir = root / "build"
                build_dir.mkdir()
                (build_dir / "compile_commands.json").touch()
                plugin = build_dir / "tool" / "lint" / "libAobusLintPlugin.so"
                plugin.parent.mkdir(parents=True)
                plugin.touch()
                recorder = _PhaseRecorder()
                with (
                    mock.patch.object(builddir, "platform_profile", return_value=profile),
                    mock.patch.object(linttest, "phases", recorder),
                    mock.patch.object(
                        linttest,
                        "discover_fixtures",
                        side_effect=lambda fixture_dir=fixture_root: original_discover(fixture_dir),
                    ),
                    mock.patch.object(linttest, "_windows_fixture_tools") as fixture_tools,
                    mock.patch.object(linttest.subprocess, "run") as run_native,
                    contextlib.redirect_stdout(io.StringIO()) as output,
                ):
                    self.assertEqual(linttest.run(build_dir, jobs=1, toolchain=object()), 1)
                run_native.assert_not_called()
                fixture_tools.assert_not_called()
                self.assertEqual(recorder.records, [])
                text = output.getvalue()
                self.assertIn(f"{source.resolve()}:2", text)
                self.assertIn("empty explicit diagnostic text", text)
                self.assertIn(f"{header.resolve()}:2", text)
                self.assertIn("conflicting POSITIVE and NEGATIVE markers", text)
                self.assertNotIn("no compatible lint fixtures selected", text)
                self.assertNotIn("=== Diagnostic Verification", text)

    def test_direct_diagnostic_run_raises_on_conflicting_expectations(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            fixture_path = root / "BasicFixture.cpp"
            fixture_path.write_text("// POSITIVE | alpha\nint bad; // POSITIVE | beta\n", encoding="utf-8")
            completed = linttest.subprocess.CompletedProcess([], 0, stdout="")
            with mock.patch.object(linttest.subprocess, "run", return_value=completed):
                with self.assertRaises(ValueError) as raised:
                    linttest._run_diagnostic(linttest.Fixture(fixture_path, "aobus-example"), root, root)
            self.assertIn("conflicting POSITIVE diagnostic text", str(raised.exception))
            self.assertIn(str(fixture_path.resolve()), str(raised.exception))

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
            # Both fixtures belong to the diagnostic stage and share one pool.
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
                mock.patch.object(linttest, "_run_source_policy_smoke", return_value=(True, result_log)),
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
            fixture_path.write_text("int original;\n", encoding="utf-8")
            fixture = linttest.Fixture(fixture_path, "aobus-example")
            result_log = Path(temp_dir) / "case.log"
            result_log.touch()

            with (
                mock.patch.object(linttest, "discover_fixtures", return_value=[fixture]),
                mock.patch.object(linttest, "_run_diagnostic", return_value=(True, result_log)) as run_diagnostic,
                mock.patch.object(linttest, "_run_source_policy_smoke", return_value=(True, result_log)),
                mock.patch.object(linttest, "_run_compiler_error_smoke", return_value=(True, result_log)),
                mock.patch.object(linttest.shutil, "rmtree") as remove_tree,
                contextlib.redirect_stdout(io.StringIO()),
            ):
                self.assertEqual(linttest.run(build_dir, jobs=1), 0)

            run_diagnostic.assert_called_once_with(fixture, build_dir, mock.ANY, tools=None)
            remove_tree.assert_called_once()


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
                mock.patch.object(linttest, "_run_diagnostic", return_value=(False, case_log)),
                mock.patch.object(linttest, "_run_source_policy_smoke", return_value=(True, case_log)),
                mock.patch.object(linttest, "_run_compiler_error_smoke", return_value=(True, case_log)),
                contextlib.redirect_stdout(io.StringIO()),
            ):
                self.assertEqual(linttest.run(build_dir, jobs=1), 1)

            self.assertEqual(
                recorder.records,
                [
                    {"name": "lint-diagnostics", "tree": "tidy", "jobs": 1, "count": 1, "status": 1},
                    {
                        "name": "lint-source-policy",
                        "tree": "tidy",
                        "jobs": 1,
                        "count": linttest._source_policy_case_count(windows=False),
                        "status": 0,
                    },
                    {"name": "lint-compiler-errors", "tree": "tidy", "count": 4, "status": 0},
                ],
            )


class WindowsLintFixtureTest(unittest.TestCase):
    def setUp(self):
        self.version, _, self.required_files = tidyengine.windows_llvm_sdk_manifest()
        temporary = tempfile.TemporaryDirectory(prefix="aobus windows fixtures ")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.build = self.root / "build"
        self.sdk = self.root / "LLVM SDK"
        self.build.mkdir()
        (self.build / "compile_commands.json").write_text("[]", encoding="utf-8")
        (self.build / "CMakeCache.txt").write_text(
            f"AOBUS_LLVM_SDK_RESOLVED_ROOT:INTERNAL={self.sdk.as_posix()}\n"
            f"AOBUS_LLVM_SDK_RESOLVED_VERSION:INTERNAL={self.version}\n",
            encoding="utf-8",
        )
        self.host = self.build / "tool/lint/AobusClangTidy.exe"
        self.host.parent.mkdir(parents=True)
        self.host.touch()
        for relative in self.required_files:
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
            expected_llvm_version=self.version,
            sdk_root=self.sdk,
        )

    def test_tools_use_only_configured_static_host_sdk_and_exact_vs_includes(self):
        tools = linttest._windows_fixture_tools(self.build, self.verified_context())
        self.assertEqual(tools.tidy, str(self.host))
        self.assertFalse(hasattr(tools, "compiler"))
        resource = self.sdk / "lib/clang/22"
        self.assertIn(f"/clang:-resource-dir={resource}", tools.flags)
        self.assertIn("/X", tools.flags)
        self.assertEqual(
            [flag for flag in tools.flags if flag.startswith("/imsvc")],
            [f"/imsvc{resource / 'include'}", *(f"/imsvc{path}" for path in self.includes)],
        )
        self.assertNotIn("ambient-g++", " ".join(tools.flags))
        self.assertNotIn("g++", tools.flags)

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
        for relative in self.required_files:
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
        self.assertFalse(hasattr(tools, "compiler"))
        # Fixture-only capabilities stay enforced against the context's SDK:
        # the complete required-file set is validated from the verified root.
        for relative in self.required_files:
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
            mock.patch.object(linttest, "_run_source_policy_smoke", return_value=(True, log)),
            mock.patch.object(linttest, "_run_compiler_error_smoke", return_value=(True, log)),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            self.assertEqual(linttest.run(self.build, jobs=1, toolchain=context), 0)
        fixture_tools.assert_called_once_with(self.build, context)
        self.assertEqual(run_diagnostic.call_args.kwargs["tools"], fixture_tools.return_value)

    def test_native_fixture_command_has_fixed_cl_flags_shared_config_and_no_load(self):
        tools = linttest._windows_fixture_tools(self.build, self.verified_context())
        fixture = linttest.Fixture(self.root / "fixture.h", "aobus-example")
        command = linttest._tidy_command(
            fixture,
            self.build,
            "--tidy-arg=--extra-arg=-std=c++26",
            *linttest._fixture_tidy_args(self.root, windows=True),
            tools=tools,
        )
        self.assertEqual(command[0], str(self.host))
        self.assertEqual(command[command.index("--") - 1], str(fixture.path))
        self.assertEqual(command[command.index("--") + 1 :], list(tools.flags))
        # Raw extras pass through without a fixing or exporting argument.
        self.assertIn("--extra-arg=-std=c++26", command)
        assert_no_fix_arguments(self, command)
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
                assert_no_fix_arguments(self, command)

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

    def test_native_runner_runs_the_diagnostic_stage_and_accounts_for_mm(self):
        fixtures = []
        for name, source in (
            ("Diagnostic.h", "int bad; // POSITIVE\n"),
            ("Plain.cpp", "int original;\n"),
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
            mock.patch.object(linttest, "_run_source_policy_smoke", return_value=(True, log)),
            mock.patch.object(linttest, "_run_compiler_error_smoke", return_value=(True, log)) as compiler,
            contextlib.redirect_stdout(io.StringIO()) as output,
        ):
            self.assertEqual(linttest.run(self.build, jobs=1, toolchain=self.verified_context()), 0)
        self.assertEqual([call.args[0] for call in diagnostic.call_args_list], fixtures[:2])
        self.assertIn("tools", diagnostic.call_args.kwargs)
        compiler.assert_called_once()
        self.assertIn("[DEFER] aobus-example/Deferred.mm", output.getvalue())
        self.assertIn("2 selected, 1 Objective-C++ deferred (not tested)", output.getvalue())

    def test_valid_objective_cpp_only_suite_is_incompatible_without_native_work(self):
        fixture_root = self.root / "fixtures"
        check = fixture_root / "aobus-example"
        check.mkdir(parents=True)
        (check / "Only.mm").write_text("int value; // NEGATIVE\n", encoding="utf-8")
        original_discover = linttest.discover_fixtures
        with (
            mock.patch.object(
                linttest,
                "discover_fixtures",
                side_effect=lambda fixture_dir=fixture_root: original_discover(fixture_dir),
            ),
            mock.patch.object(linttest, "_windows_fixture_tools") as fixture_tools,
            mock.patch.object(linttest, "_run_diagnostic") as diagnostic,
            mock.patch.object(linttest.subprocess, "run") as run_native,
            contextlib.redirect_stdout(io.StringIO()) as output,
        ):
            self.assertEqual(linttest.run(self.build, jobs=1, toolchain=self.verified_context()), 1)
        run_native.assert_not_called()
        fixture_tools.assert_not_called()
        diagnostic.assert_not_called()
        text = output.getvalue()
        self.assertIn("[DEFER] aobus-example/Only.mm", text)
        self.assertIn("0 selected, 1 Objective-C++ deferred (not tested)", text)
        self.assertIn("ERROR: no compatible lint fixtures selected", text)


if __name__ == "__main__":
    unittest.main()
