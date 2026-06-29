"""Tests for the combined project hygiene commands."""

import argparse
import contextlib
import io
import shutil
import tempfile
import unittest
from argparse import Namespace
from pathlib import Path
from unittest import mock

from ao.command import format as format_command
from ao.command import hygiene, tidy


class FormatCommandTest(unittest.TestCase):
    def test_dispatches_cpp_and_python_files_to_their_formatters(self):
        args = Namespace(files=["lib/Foo.cpp", "script/foo.py"], all=False, commit=None, check=True)

        with mock.patch.object(format_command, "_file_exists", return_value=True):
            with mock.patch.object(format_command, "run_clang_format", return_value=0) as clang:
                with mock.patch.object(format_command, "run_ruff_format", return_value=0) as ruff:
                    with contextlib.redirect_stdout(io.StringIO()):
                        self.assertEqual(format_command.run_command(args), 0)

        clang.assert_called_once_with(["lib/Foo.cpp"], check=True)
        ruff.assert_called_once_with(["script/foo.py"], check=True)


class TidyCommandTest(unittest.TestCase):
    def test_python_only_scope_runs_pythoncheck_without_preparing_clang_tidy(self):
        args = Namespace(
            files=["script/foo.py"],
            all=False,
            folder=[],
            commit=None,
            check=None,
            debug=False,
            output=None,
            jobs=1,
            path=None,
            fix=False,
            no_build=False,
            tidy_arg=[],
            header_filter=None,
        )

        with mock.patch.object(tidy.tidyengine, "resolve_scope", return_value=(["script/foo.py"], True)):
            with mock.patch.object(tidy, "split_existing", return_value=([], ["script/foo.py"])):
                with mock.patch.object(tidy.pythoncheck, "run_paths", return_value=0) as pythoncheck:
                    with mock.patch.object(tidy, "prepare_plugin") as prepare_plugin:
                        with contextlib.redirect_stdout(io.StringIO()):
                            self.assertEqual(tidy.run_command(args), 0)

        pythoncheck.assert_called_once_with(["script/foo.py"], sink=None)
        prepare_plugin.assert_not_called()

    def test_clang_tidy_warnings_make_tidy_fail_even_when_process_succeeds(self):
        args = Namespace(
            files=["lib/Foo.cpp"],
            all=False,
            folder=[],
            commit=None,
            check=None,
            debug=False,
            output=None,
            jobs=1,
            path=None,
            fix=False,
            no_build=False,
            tidy_arg=[],
            header_filter=None,
        )

        with tempfile.TemporaryDirectory(dir="/tmp") as temp_dir:
            root = Path(temp_dir)
            log = root / "foo.log"
            log.write_text("lib/Foo.cpp:12:3: warning: injected diagnostic [aobus-example]\n", encoding="utf-8")
            result = tidy.tidyengine.BatchResult(failed=False, logs=[log], failed_logs=[])

            with mock.patch.object(tidy.tidyengine, "resolve_scope", return_value=(["lib/Foo.cpp"], True)):
                with mock.patch.object(tidy, "split_existing", return_value=(["lib/Foo.cpp"], [])):
                    with mock.patch.object(tidy, "prepare_plugin", return_value=Path("/tmp/libAobusLintPlugin.so")):
                        with mock.patch.object(tidy.tidyengine, "system_include_args", return_value=[]):
                            buckets = {"STRICT": [Path("lib/Foo.cpp")], "RELAXED": []}
                            with mock.patch.object(tidy, "classify_existing", return_value=buckets):
                                with mock.patch.object(tidy.tidyengine, "make_tmpdir", return_value=root):
                                    with mock.patch.object(tidy.tidyengine, "run_parallel", return_value=result):
                                        with contextlib.redirect_stdout(io.StringIO()):
                                            self.assertEqual(tidy.run_command(args), 1)


def _hygiene_args(**overrides):
    defaults = {"files": [], "all": False, "folder": [], "commit": None, "path": None, "jobs": 1}
    defaults.update(overrides)
    return Namespace(**defaults)


class HygieneCommandTest(unittest.TestCase):
    def test_runs_format_check_then_tidy_for_the_same_scope(self):
        args = _hygiene_args(files=["script/foo.py"])

        with mock.patch.object(hygiene.format_command, "run_command", return_value=0) as format_run:
            with mock.patch.object(hygiene.tidy, "run_command", return_value=0) as tidy_run:
                with contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(hygiene.run_command(args), 0)

        self.assertTrue(format_run.call_args.args[0].check)
        self.assertEqual(format_run.call_args.args[0].files, ["script/foo.py"])
        self.assertEqual(format_run.call_args.args[0].folder, [])
        self.assertEqual(tidy_run.call_args.args[0].files, ["script/foo.py"])

    def test_subcommand_args_cannot_drift_from_the_real_parsers(self):
        args = _hygiene_args(jobs=4)
        for register_command, name, build in (
            (format_command.register, "format", hygiene._format_args),
            (tidy.register, "tidy", hygiene._tidy_args),
        ):
            parser = argparse.ArgumentParser()
            register_command(parser.add_subparsers())
            expected = vars(parser.parse_args([name])).keys()
            self.assertEqual(vars(build(args)).keys(), expected, name)

    def test_unknown_override_is_rejected(self):
        with self.assertRaises(AttributeError):
            hygiene._subcommand_defaults(tidy.register, "tidy", not_a_tidy_argument=1)

    def test_failure_is_check_only_and_prints_fix_reminders(self):
        args = _hygiene_args()
        stderr = io.StringIO()

        with mock.patch.object(hygiene.format_command, "run_command", return_value=1):
            with mock.patch.object(hygiene.tidy, "run_command", return_value=1):
                with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(stderr):
                    self.assertEqual(hygiene.run_command(args), 1)

        self.assertIn("check-only", stderr.getvalue())
        self.assertIn("./ao format", stderr.getvalue())
        self.assertIn("Fix the lint findings manually", stderr.getvalue())


class TidyFixesDeduplicationTest(unittest.TestCase):
    def test_parse_diagnostics_from_yaml(self):
        content = """---
MainSourceFile:  'foo.cpp'
Diagnostics:
  - DiagnosticName:  readability-magic-numbers
    DiagnosticMessage:
      Message:         '1.5 is a magic number'
      FilePath:        'foo.cpp'
      FileOffset:      964
      Replacements:
        - FilePath:        'foo.cpp'
          Offset:          964
          Length:          3
          ReplacementText: 'kInitVal'
...
"""
        blocks = tidy.parse_diagnostics_from_yaml(content)
        self.assertEqual(len(blocks), 1)
        self.assertIn("DiagnosticName:  readability-magic-numbers", blocks[0])
        self.assertIn("ReplacementText: 'kInitVal'", blocks[0])

    def test_deduplicate_fixes(self):
        tmpdir = Path(tempfile.mkdtemp(prefix="ao-tidy-dedup-test-"))
        try:
            file1 = tmpdir / "1.yaml"
            file2 = tmpdir / "2.yaml"

            yaml_content = """---
MainSourceFile:  'foo.cpp'
Diagnostics:
  - DiagnosticName:  google-readability-namespace-comments
    DiagnosticMessage:
      Message:         'namespace detail not terminated'
      FilePath:        'header.h'
      FileOffset:      120
      Replacements:
        - FilePath:        'header.h'
          Offset:          120
          Length:          0
          ReplacementText: '  // namespace detail'
...
"""
            file1.write_text(yaml_content, encoding="utf-8")
            file2.write_text(yaml_content, encoding="utf-8")

            tidy.deduplicate_fixes(tmpdir)

            remaining = list(tmpdir.glob("*.yaml"))
            self.assertEqual(len(remaining), 1)
            self.assertEqual(remaining[0].name, "consolidated_fixes.yaml")

            consolidated_content = remaining[0].read_text(encoding="utf-8")
            self.assertEqual(consolidated_content.count("DiagnosticName:"), 1)
            self.assertIn("MainSourceFile:  'foo.cpp'", consolidated_content)
        finally:
            shutil.rmtree(tmpdir)

    def test_deduplicate_fixes_normalizes_paths(self):
        tmpdir = Path(tempfile.mkdtemp(prefix="ao-tidy-dedup-test-"))
        try:
            file1 = tmpdir / "1.yaml"
            file2 = tmpdir / "2.yaml"

            dummy_header = tmpdir / "dummy_header.h"
            dummy_header.touch()

            path1 = f"{tmpdir}/dummy_header.h"
            path2 = f"{tmpdir}/./dummy_header.h"

            yaml_content1 = f"""---
MainSourceFile:  'foo.cpp'
Diagnostics:
  - DiagnosticName:  google-readability-namespace-comments
    DiagnosticMessage:
      Message:         'namespace detail not terminated'
      FilePath:        '{path1}'
      FileOffset:      120
      Replacements:
        - FilePath:        '{path1}'
          Offset:          120
          Length:          0
          ReplacementText: '  // namespace detail'
...
"""
            yaml_content2 = f"""---
MainSourceFile:  'foo.cpp'
Diagnostics:
  - DiagnosticName:  google-readability-namespace-comments
    DiagnosticMessage:
      Message:         'namespace detail not terminated'
      FilePath:        '{path2}'
      FileOffset:      120
      Replacements:
        - FilePath:        '{path2}'
          Offset:          120
          Length:          0
          ReplacementText: '  // namespace detail'
...
"""
            file1.write_text(yaml_content1, encoding="utf-8")
            file2.write_text(yaml_content2, encoding="utf-8")

            tidy.deduplicate_fixes(tmpdir)

            remaining = list(tmpdir.glob("*.yaml"))
            self.assertEqual(len(remaining), 1)
            self.assertEqual(remaining[0].name, "consolidated_fixes.yaml")

            consolidated_content = remaining[0].read_text(encoding="utf-8")
            self.assertEqual(consolidated_content.count("DiagnosticName:"), 1)
        finally:
            shutil.rmtree(tmpdir)


if __name__ == "__main__":
    unittest.main()
