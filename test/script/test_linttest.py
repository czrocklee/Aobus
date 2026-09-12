"""Tests for the Python lint integration runner."""

import contextlib
import io
import tempfile
import threading
import unittest
from pathlib import Path
from unittest import mock

from ao.core import linttest


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


if __name__ == "__main__":
    unittest.main()
