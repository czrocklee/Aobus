"""Tests for the Python lint integration runner."""

import contextlib
import io
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao.core import linttest


class LintTestVerifierTest(unittest.TestCase):
    def test_fixture_discovery_uses_parent_directory_as_check_name(self):
        with tempfile.TemporaryDirectory(dir="/tmp") as temp_dir:
            root = Path(temp_dir)
            check_dir = root / "aobus-example"
            check_dir.mkdir()
            fixture = check_dir / "BasicFixture.cpp"
            fixture.touch()
            (check_dir / "notes.txt").touch()

            self.assertEqual(
                linttest.discover_fixtures(root),
                [linttest.Fixture(fixture, "aobus-example")],
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
            linttest.verify_diagnostics(source, output, "aobus-example"),
            [
                "missing expected diagnostic on line 2: [aobus-example]",
                "diagnostic found on explicitly NEGATIVE line 3: [aobus-example]",
                "unexpected diagnostic on line 4: [aobus-other]",
            ],
        )

    def test_diagnostic_verification_does_not_require_other_named_checks(self):
        source = "int named; // POSITIVE:aobus-other\n"

        self.assertEqual(linttest.verify_diagnostics(source, "", "aobus-current"), [])

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


class LintTestRunnerTest(unittest.TestCase):
    def test_diagnostic_stage_rejects_nonzero_tidy_exit_without_expected_output(self):
        with tempfile.TemporaryDirectory(dir="/tmp") as temp_dir:
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
        with tempfile.TemporaryDirectory(dir="/tmp") as temp_dir:
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
        with tempfile.TemporaryDirectory(dir="/tmp") as temp_dir:
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
        with tempfile.TemporaryDirectory(dir="/tmp") as temp_dir:
            build_dir = Path(temp_dir)
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(linttest.run(build_dir), 1)

            (build_dir / "compile_commands.json").touch()
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(linttest.run(build_dir), 1)

    def test_compiler_error_smoke_requires_fatal_errors_but_allows_unused_code(self):
        with tempfile.TemporaryDirectory(dir="/tmp") as temp_dir:
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
        with tempfile.TemporaryDirectory(dir="/tmp") as temp_dir:
            root = Path(temp_dir)
            completed = linttest.subprocess.CompletedProcess([], 0, stdout="")

            with mock.patch.object(linttest.subprocess, "run", return_value=completed):
                success, log = linttest._run_compiler_error_smoke(root, root)

            self.assertFalse(success)
            self.assertIn(
                "missing-include canary did not produce a fatal compiler diagnostic",
                log.read_text(encoding="utf-8"),
            )

    def test_fix_stage_rejects_unchanged_files_with_fix_expectations(self):
        with tempfile.TemporaryDirectory(dir="/tmp") as temp_dir:
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
        with tempfile.TemporaryDirectory(dir="/tmp") as temp_dir:
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
                if command[0] == "g++":
                    return linttest.subprocess.CompletedProcess(command, 0, stdout="")

                fixed = Path(command[-1])
                fixed.write_text(
                    "// POSITIVE: FIX-TO: int replacement;\nint replacement;\n",
                    encoding="utf-8",
                )
                return linttest.subprocess.CompletedProcess(command, 1, stdout="expected warning\n")

            with mock.patch.object(linttest.subprocess, "run", side_effect=run_fix_or_syntax):
                success, log = linttest._run_fix(fixture, root, root)

            self.assertTrue(success)
            self.assertNotIn("ERROR:", log.read_text(encoding="utf-8"))

    def test_runner_aggregates_all_stages_and_removes_success_workspace(self):
        with tempfile.TemporaryDirectory(dir="/tmp") as temp_dir:
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
                with mock.patch.object(linttest, "_run_diagnostic", return_value=(True, result_log)):
                    with mock.patch.object(linttest, "_run_fix", return_value=(True, result_log)):
                        with mock.patch.object(linttest, "_run_replacement_smoke", return_value=(True, result_log)):
                            with mock.patch.object(
                                linttest,
                                "_run_compiler_error_smoke",
                                return_value=(True, result_log),
                            ):
                                with mock.patch.object(linttest.shutil, "rmtree") as remove_tree:
                                    with contextlib.redirect_stdout(io.StringIO()):
                                        self.assertEqual(linttest.run(build_dir, jobs=1), 0)

            remove_tree.assert_called_once()


if __name__ == "__main__":
    unittest.main()
