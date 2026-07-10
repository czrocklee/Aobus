"""Tests for the combined project hygiene commands."""

import argparse
import contextlib
import io
import json
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

    def test_ruff_format_uses_the_current_python_environment(self):
        completed = mock.Mock(returncode=0)
        with mock.patch.object(format_command.subprocess, "run", return_value=completed) as run:
            self.assertEqual(format_command.run_ruff_format(["script/foo.py"], check=True), 0)

        run.assert_called_once_with(
            format_command.pythoncheck.module_command("ruff", "format", "--check", "script/foo.py"),
            cwd=format_command.PROJECT_ROOT,
        )

    def test_windows_clang_format_uses_the_pinned_llvm_sdk_tool(self):
        pinned = r"C:\llvm-22.1.8\bin\clang-format.exe"
        tidy_build_dir = Path(r"C:\local\aobus\windows-tidy")

        with mock.patch.object(
            format_command.builddir,
            "platform_profile",
            return_value=format_command.builddir.WINDOWS_PROFILE,
        ):
            with mock.patch.object(format_command.builddir, "tidy_dir", return_value=tidy_build_dir):
                with mock.patch.object(format_command.tidyengine, "ensure_windows_llvm_sdk") as ensure_sdk:
                    with mock.patch.object(
                        format_command.tidyengine,
                        "clang_tool",
                        return_value=pinned,
                    ):
                        with mock.patch.object(
                            format_command.subprocess,
                            "run",
                            return_value=mock.Mock(returncode=0),
                        ) as run:
                            self.assertEqual(format_command.run_clang_format(["lib/Foo.cpp"], check=True), 0)

        ensure_sdk.assert_called_once_with(tidy_build_dir)
        run.assert_called_once_with(
            [pinned, "--dry-run", "-Werror", "lib/Foo.cpp"],
            cwd=format_command.PROJECT_ROOT,
        )


class TidyCommandTest(unittest.TestCase):
    def test_header_filters_accept_native_and_posix_separators(self):
        native = str(tidy.PROJECT_ROOT / "lib" / "audio" / "Player.h")
        posix = (tidy.PROJECT_ROOT / "lib" / "audio" / "Player.h").as_posix()

        self.assertRegex(native, tidy.STRICT_HEADER_FILTER)
        self.assertRegex(posix, tidy.STRICT_HEADER_FILTER)

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
            with mock.patch.object(tidy, "missing_explicit_files", return_value=[]):
                with mock.patch.object(tidy, "split_existing", return_value=([], ["script/foo.py"])):
                    with mock.patch.object(tidy.pythoncheck, "run_paths", return_value=0) as pythoncheck:
                        with mock.patch.object(tidy, "prepare_toolchain") as prepare_toolchain:
                            with contextlib.redirect_stdout(io.StringIO()):
                                self.assertEqual(tidy.run_command(args), 0)

        pythoncheck.assert_called_once_with(["script/foo.py"], sink=None)
        prepare_toolchain.assert_not_called()

    def test_explicit_nonexistent_file_fails_before_toolchain_setup(self):
        args = Namespace(
            files=["lib/DoesNotExist.cpp"],
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
        stderr = io.StringIO()

        with mock.patch.object(tidy.tidyengine, "resolve_scope", return_value=(args.files, True)):
            with mock.patch.object(tidy, "prepare_toolchain") as prepare_toolchain:
                with contextlib.redirect_stderr(stderr):
                    self.assertEqual(tidy.run_command(args), 1)

        prepare_toolchain.assert_not_called()
        self.assertIn("do not exist or are not files", stderr.getvalue())
        self.assertIn(args.files[0], stderr.getvalue())

    def test_explicit_file_without_native_compile_command_fails(self):
        args = Namespace(
            files=["lib/Foreign.cpp"],
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
        foreign = Path("lib/Foreign.cpp")
        stderr = io.StringIO()

        with contextlib.ExitStack() as stack:
            stack.enter_context(mock.patch.object(tidy.tidyengine, "resolve_scope", return_value=(args.files, True)))
            stack.enter_context(mock.patch.object(tidy, "missing_explicit_files", return_value=[]))
            stack.enter_context(mock.patch.object(tidy, "split_existing", return_value=(args.files, [])))
            stack.enter_context(
                mock.patch.object(
                    tidy,
                    "prepare_toolchain",
                    return_value=tidy.TidyToolchain("clang-tidy", Path("plugin"), None),
                )
            )
            stack.enter_context(
                mock.patch.object(
                    tidy,
                    "classify_existing",
                    return_value={"STRICT": [foreign], "RELAXED": []},
                )
            )
            stack.enter_context(
                mock.patch.object(
                    tidy.tidyengine,
                    "compile_command_plan",
                    return_value=tidy.tidyengine.CompileCommandPlan((), (foreign,)),
                )
            )
            run_parallel = stack.enter_context(mock.patch.object(tidy.tidyengine, "run_parallel"))
            stack.enter_context(contextlib.redirect_stderr(stderr))
            self.assertEqual(tidy.run_command(args), 1)

        run_parallel.assert_not_called()
        self.assertIn("explicitly selected files", stderr.getvalue())

    def test_non_explicit_scope_visibly_defers_foreign_platform_files(self):
        args = Namespace(
            files=[],
            all=True,
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
        native = Path("lib/Native.cpp")
        foreign = Path("lib/Foreign.cpp")
        result = tidy.tidyengine.BatchResult(failed=False, logs=[], failed_logs=[])
        stderr = io.StringIO()

        with tempfile.TemporaryDirectory() as temp_dir, contextlib.ExitStack() as stack:
            stack.enter_context(
                mock.patch.object(
                    tidy.tidyengine,
                    "resolve_scope",
                    return_value=([str(native), str(foreign)], False),
                )
            )
            stack.enter_context(
                mock.patch.object(tidy, "split_existing", return_value=([str(native), str(foreign)], []))
            )
            stack.enter_context(
                mock.patch.object(
                    tidy,
                    "prepare_toolchain",
                    return_value=tidy.TidyToolchain("clang-tidy", Path("plugin"), None),
                )
            )
            stack.enter_context(
                mock.patch.object(
                    tidy,
                    "classify_existing",
                    return_value={"STRICT": [native, foreign], "RELAXED": []},
                )
            )
            stack.enter_context(
                mock.patch.object(
                    tidy.tidyengine,
                    "compile_command_plan",
                    return_value=tidy.tidyengine.CompileCommandPlan(
                        (tidy.tidyengine.CompileCommandTarget(native, native),),
                        (foreign,),
                    ),
                )
            )
            stack.enter_context(mock.patch.object(tidy.tidyengine, "system_include_args", return_value=[]))
            stack.enter_context(mock.patch.object(tidy.tidyengine, "make_tmpdir", return_value=Path(temp_dir) / "tidy"))
            run_parallel = stack.enter_context(mock.patch.object(tidy.tidyengine, "run_parallel", return_value=result))
            stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
            stack.enter_context(contextlib.redirect_stderr(stderr))
            self.assertEqual(tidy.run_command(args), 0)

        invocations = run_parallel.call_args.args[0]
        self.assertEqual(len(invocations), 1)
        self.assertEqual(invocations[0].selected, native.resolve())
        self.assertEqual(invocations[0].compile_command_source, native.resolve())
        self.assertFalse(invocations[0].is_header)
        self.assertIn("Deferred files", stderr.getvalue())
        self.assertIn("not checked here", stderr.getvalue())

    def test_non_explicit_all_deferred_scope_fails_as_incomplete(self):
        args = Namespace(
            files=[],
            all=True,
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
        foreign = Path("lib/Foreign.cpp")
        stderr = io.StringIO()

        with contextlib.ExitStack() as stack:
            stack.enter_context(
                mock.patch.object(tidy.tidyengine, "resolve_scope", return_value=([str(foreign)], False))
            )
            stack.enter_context(mock.patch.object(tidy, "split_existing", return_value=([str(foreign)], [])))
            stack.enter_context(
                mock.patch.object(
                    tidy,
                    "prepare_toolchain",
                    return_value=tidy.TidyToolchain("clang-tidy", Path("plugin"), None),
                )
            )
            stack.enter_context(
                mock.patch.object(
                    tidy,
                    "classify_existing",
                    return_value={"STRICT": [foreign], "RELAXED": []},
                )
            )
            stack.enter_context(
                mock.patch.object(
                    tidy.tidyengine,
                    "compile_command_plan",
                    return_value=tidy.tidyengine.CompileCommandPlan((), (foreign,)),
                )
            )
            run_parallel = stack.enter_context(mock.patch.object(tidy.tidyengine, "run_parallel"))
            stack.enter_context(contextlib.redirect_stderr(stderr))
            self.assertEqual(tidy.run_command(args), 1)

        run_parallel.assert_not_called()
        self.assertIn("coverage is incomplete", stderr.getvalue())

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

        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            log = root / "foo.log"
            log.write_text("lib/Foo.cpp:12:3: warning: injected diagnostic [aobus-example]\n", encoding="utf-8")
            result = tidy.tidyengine.BatchResult(failed=False, logs=[log], failed_logs=[])

            with mock.patch.object(tidy.tidyengine, "resolve_scope", return_value=(["lib/Foo.cpp"], True)):
                with mock.patch.object(tidy, "missing_explicit_files", return_value=[]):
                    with mock.patch.object(tidy, "split_existing", return_value=(["lib/Foo.cpp"], [])):
                        with mock.patch.object(
                            tidy,
                            "prepare_toolchain",
                            return_value=tidy.TidyToolchain(
                                "clang-tidy",
                                Path("/tmp/libAobusLintPlugin.so"),
                                None,
                            ),
                        ):
                            with mock.patch.object(
                                tidy.tidyengine,
                                "compile_command_plan",
                                return_value=tidy.tidyengine.CompileCommandPlan(
                                    (
                                        tidy.tidyengine.CompileCommandTarget(
                                            Path("lib/Foo.cpp"),
                                            Path("lib/Foo.cpp"),
                                        ),
                                    ),
                                    (),
                                ),
                            ):
                                with mock.patch.object(tidy.tidyengine, "system_include_args", return_value=[]):
                                    buckets = {"STRICT": [Path("lib/Foo.cpp")], "RELAXED": []}
                                    with mock.patch.object(tidy, "classify_existing", return_value=buckets):
                                        with mock.patch.object(tidy.tidyengine, "make_tmpdir", return_value=root):
                                            with mock.patch.object(
                                                tidy.tidyengine, "run_parallel", return_value=result
                                            ):
                                                with contextlib.redirect_stdout(io.StringIO()):
                                                    self.assertEqual(tidy.run_command(args), 1)

    def test_mapped_header_runs_as_main_file_with_synthetic_native_flags(self):
        args = Namespace(
            files=["include/ao/Foo.h"],
            all=False,
            folder=[],
            commit=None,
            check="aobus-include-convention",
            debug=False,
            output=None,
            jobs=1,
            path=None,
            fix=False,
            no_build=False,
            tidy_arg=[],
            header_filter=None,
        )
        header = Path("include/ao/Foo.h")
        translation_unit = Path("lib/Foo.cpp")
        toolchain = tidy.TidyToolchain(
            "AobusClangTidy.exe",
            None,
            Path("C:/llvm/lib/clang/22"),
        )

        def run_parallel(invocations, _jobs, tmpdir, runner):
            self.assertEqual(len(invocations), 1)
            log = tmpdir / "000000.log"
            status = runner(invocations[0], log)
            return tidy.tidyengine.BatchResult(
                failed=status != 0,
                logs=[log],
                failed_logs=[log] if status else [],
            )

        with tempfile.TemporaryDirectory() as temp_dir, contextlib.ExitStack() as stack:
            build_dir = Path(temp_dir) / "build"
            tmpdir = Path(temp_dir) / "tidy"
            build_dir.mkdir()
            tmpdir.mkdir()
            (build_dir / "compile_commands.json").write_text(
                json.dumps(
                    [
                        {
                            "directory": str(build_dir),
                            "file": str(translation_unit.resolve()),
                            "command": (f'clang++ -DAOBUS_NATIVE_FLAGS=1 -c "{translation_unit.resolve()}"'),
                        }
                    ]
                ),
                encoding="utf-8",
            )
            args.path = str(build_dir)
            stack.enter_context(mock.patch.object(tidy.tidyengine, "resolve_scope", return_value=(args.files, True)))
            stack.enter_context(mock.patch.object(tidy, "missing_explicit_files", return_value=[]))
            stack.enter_context(mock.patch.object(tidy, "split_existing", return_value=(args.files, [])))
            stack.enter_context(mock.patch.object(tidy, "prepare_toolchain", return_value=toolchain))
            stack.enter_context(
                mock.patch.object(
                    tidy,
                    "classify_existing",
                    return_value={"STRICT": [header], "RELAXED": []},
                )
            )
            stack.enter_context(
                mock.patch.object(
                    tidy.tidyengine,
                    "compile_command_plan",
                    return_value=tidy.tidyengine.CompileCommandPlan(
                        (tidy.tidyengine.CompileCommandTarget(header, translation_unit),),
                        (),
                    ),
                )
            )
            stack.enter_context(mock.patch.object(tidy.tidyengine, "system_include_args", return_value=[]))
            stack.enter_context(mock.patch.object(tidy.tidyengine, "make_tmpdir", return_value=tmpdir))
            stack.enter_context(mock.patch.object(tidy.tidyengine, "run_parallel", side_effect=run_parallel))
            stack.enter_context(mock.patch.object(tidy.shutil, "rmtree"))
            clang_tidy = stack.enter_context(mock.patch.object(tidy.subprocess, "call", return_value=0))
            stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
            stack.enter_context(contextlib.redirect_stderr(io.StringIO()))

            self.assertEqual(tidy.run_command(args), 0)
            command = clang_tidy.call_args.args[0]
            database_dir = Path(command[command.index("-p") + 1])
            synthetic = json.loads((database_dir / "compile_commands.json").read_text(encoding="utf-8"))

        self.assertEqual(command[-1], str(header.resolve()))
        self.assertNotEqual(database_dir, build_dir)
        self.assertEqual(synthetic[0]["file"], header.resolve().as_posix())
        self.assertIn("-DAOBUS_NATIVE_FLAGS=1", synthetic[0]["command"])
        self.assertIn(str(header.resolve()), synthetic[0]["command"])
        self.assertNotIn(str(translation_unit.resolve()), synthetic[0]["command"])
        self.assertIn(f"--extra-arg-before=-resource-dir={toolchain.resource_dir}", command)
        self.assertIn("--extra-arg-before=-D_USE_STD_VECTOR_ALGORITHMS=0", command)
        self.assertTrue(any(argument.startswith("-header-filter=^(") for argument in command))
        line_filter = next(argument for argument in command if argument.startswith("-line-filter="))
        self.assertIn(header.resolve().as_posix(), line_filter)
        self.assertNotIn(translation_unit.resolve().as_posix(), line_filter)
        self.assertFalse(any(argument.startswith("-load=") for argument in command))


def _hygiene_args(**overrides):
    defaults = {"files": [], "all": False, "folder": [], "commit": None, "path": None, "jobs": 1}
    defaults.update(overrides)
    return Namespace(**defaults)


class HygieneCommandTest(unittest.TestCase):
    def test_runs_format_check_then_tidy_for_the_same_scope(self):
        args = _hygiene_args(files=["script/foo.py"])

        with mock.patch.object(hygiene.format_command, "run_command", return_value=0) as format_run:
            with mock.patch.object(hygiene.test_audit, "run_command", return_value=0) as audit_run:
                with mock.patch.object(hygiene.name_audit, "run_command", return_value=0) as name_audit_run:
                    with mock.patch.object(hygiene.tidy, "run_command", return_value=0) as tidy_run:
                        with contextlib.redirect_stdout(io.StringIO()):
                            self.assertEqual(hygiene.run_command(args), 0)

        self.assertTrue(format_run.call_args.args[0].check)
        self.assertEqual(format_run.call_args.args[0].files, ["script/foo.py"])
        self.assertEqual(format_run.call_args.args[0].folder, [])
        self.assertTrue(audit_run.call_args.args[0].fail_on_issue)
        self.assertEqual(audit_run.call_args.args[0].paths, [])
        self.assertTrue(name_audit_run.call_args.args[0].fail_on_issue)
        self.assertEqual(name_audit_run.call_args.args[0].paths, [])
        self.assertEqual(tidy_run.call_args.args[0].files, ["script/foo.py"])

    def test_subcommand_args_cannot_drift_from_the_real_parsers(self):
        args = _hygiene_args(jobs=4)
        for register_command, name, build in (
            (format_command.register, "format", hygiene._format_args),
            (hygiene.test_audit.register, "test-audit", lambda _: hygiene._test_audit_args()),
            (hygiene.name_audit.register, "name-audit", lambda _: hygiene._name_audit_args()),
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
            with mock.patch.object(hygiene.test_audit, "run_command", return_value=1):
                with mock.patch.object(hygiene.name_audit, "run_command", return_value=1):
                    with mock.patch.object(hygiene.tidy, "run_command", return_value=1):
                        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(stderr):
                            self.assertEqual(hygiene.run_command(args), 1)

        self.assertIn("check-only", stderr.getvalue())
        self.assertIn("./ao format", stderr.getvalue())
        self.assertIn("Fix the lint findings manually", stderr.getvalue())
        self.assertIn("./ao test-audit --fail-on-issue", stderr.getvalue())
        self.assertIn("./ao name-audit --fail-on-issue", stderr.getvalue())

    def test_test_audit_failure_does_not_claim_tidy_failed(self):
        args = _hygiene_args()
        stderr = io.StringIO()

        with mock.patch.object(hygiene.format_command, "run_command", return_value=0):
            with mock.patch.object(hygiene.test_audit, "run_command", return_value=1):
                with mock.patch.object(hygiene.name_audit, "run_command", return_value=0):
                    with mock.patch.object(hygiene.tidy, "run_command", return_value=0):
                        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(stderr):
                            self.assertEqual(hygiene.run_command(args), 1)

        self.assertIn("Test names/tags", stderr.getvalue())
        self.assertNotIn("Lint findings", stderr.getvalue())

    def test_name_audit_failure_does_not_claim_tidy_failed(self):
        args = _hygiene_args()
        stderr = io.StringIO()

        with mock.patch.object(hygiene.format_command, "run_command", return_value=0):
            with mock.patch.object(hygiene.test_audit, "run_command", return_value=0):
                with mock.patch.object(hygiene.name_audit, "run_command", return_value=1):
                    with mock.patch.object(hygiene.tidy, "run_command", return_value=0):
                        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(stderr):
                            self.assertEqual(hygiene.run_command(args), 1)

        self.assertIn("Class/file names", stderr.getvalue())
        self.assertNotIn("Lint findings", stderr.getvalue())


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

    def test_apply_fixes_uses_the_resolved_abi_matched_tool(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            tmpdir = Path(temp_dir)
            fixes = tmpdir / "fixes.yaml"
            fixes.write_text(
                """---
MainSourceFile:  'foo.cpp'
Diagnostics:
  - DiagnosticName:  modernize-use-nullptr
    DiagnosticMessage:
      Message:         'use nullptr'
      FilePath:        'foo.cpp'
      FileOffset:      10
      Replacements:
        - FilePath:        'foo.cpp'
          Offset:          10
          Length:          1
          ReplacementText: 'nullptr'
...
""",
                encoding="utf-8",
            )
            tool = r"C:\build\vcpkg_installed\x64-windows\tools\llvm\clang-apply-replacements.exe"

            with mock.patch.object(tidy.subprocess, "call", return_value=0) as call:
                with contextlib.redirect_stdout(io.StringIO()):
                    self.assertTrue(tidy.apply_fixes(tmpdir, tool))

            call.assert_called_once_with([tool, "--ignore-insert-conflict", str(tmpdir)])


if __name__ == "__main__":
    unittest.main()
