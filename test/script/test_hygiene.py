"""Tests for the combined project hygiene commands."""

import argparse
import contextlib
import io
import os
import sys
import tempfile
import unittest
from argparse import Namespace
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

from ao.__main__ import make_parser
from ao.command import format as format_command
from ao.command import hygiene, tidy
from ao.core import bazel_native as native
from ao.core.paths import PROJECT_ROOT


class FormatCommandTest(unittest.TestCase):
    def test_dispatches_cpp_and_python_files_to_their_formatters(self):
        args = Namespace(files=["lib/Foo.cpp", "script/foo.py"], all=False, commit=None, check=True)

        with mock.patch.object(format_command, "_file_exists", return_value=True):
            with mock.patch.object(format_command, "run_clang_format", return_value=0) as clang:
                with mock.patch.object(format_command, "run_ruff_format", return_value=0) as ruff:
                    with contextlib.redirect_stdout(io.StringIO()):
                        self.assertEqual(format_command.run_command(args), 0)

        clang.assert_called_once_with(["lib/Foo.cpp"], check=True, build_dir=None, clang_format=None)
        ruff.assert_called_once_with(["script/foo.py"], check=True, python=None)

    def test_bazel_coverage_collector_stays_in_changed_and_all_format_scopes(self):
        collector = "bazel/coverage/collect.py"
        changed_args = Namespace(files=[], all=False, folder=[], commit=None)
        all_args = Namespace(files=[], all=True, folder=[], commit=None)

        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            collector_file = root / "bazel" / "coverage" / "collect.py"
            collector_file.parent.mkdir(parents=True)
            collector_file.write_text("# coverage collector\n", encoding="utf-8")
            generated = root / "bazel" / "coverage" / "__pycache__" / "collect.cpython-314.pyc"
            generated.parent.mkdir()
            generated.write_bytes(b"generated")
            foreign = root / "sdk-cache" / "Cache.cpp"
            foreign.parent.mkdir(parents=True)
            foreign.write_text("// foreign SDK cache tree\n", encoding="utf-8")

            with (
                mock.patch.object(format_command, "PROJECT_ROOT", root),
                mock.patch.object(format_command.gitfiles, "PROJECT_ROOT", root),
            ):
                with mock.patch.object(
                    format_command.gitfiles,
                    "changed_files",
                    return_value=[collector, "sdk-cache/Cache.cpp"],
                ) as changed:
                    self.assertEqual(format_command.resolve_files(changed_args), [collector])
                changed.assert_called_once_with(None, suffixes=format_command.gitfiles.SOURCE_SUFFIXES)

                # The --all scan walks the real folder selection: the tracked collector
                # is selected while the generated __pycache__ artifact and the foreign
                # SDK cache tree stay out of the scope.
                self.assertEqual(format_command.resolve_files(all_args), [collector])

    def test_ruff_format_uses_shared_cache_commands_and_preserves_tool_failures(self):
        # Mocked platform argv is not proof of a native filesystem or cache write.
        process_python_state = (sys.platform, sys.executable, sys.prefix)
        prefix = str(Path("managed tool env").absolute())
        files = ["script/a file.py", "script/other.py"]
        native_command = (
            "managed-python",
            "-I",
            "-m",
            "ruff",
            "format",
            "--cache-dir",
            prefix + os.sep + ".ruff_cache",
        )
        for platform, command in (
            ("linux", ("managed-python", "-m", "ruff", "format")),
            ("win32", native_command),
            ("darwin", native_command),
        ):
            for check in (False, True):
                for code in (0, 1, 2):
                    with (
                        self.subTest(platform=platform, check=check, code=code),
                        mock.patch.object(format_command.pythoncheck, "os", SimpleNamespace(name="posix")),
                        mock.patch.object(
                            format_command.pythoncheck,
                            "sys",
                            SimpleNamespace(platform=platform, prefix=prefix, executable="managed-python"),
                        ),
                        mock.patch.object(
                            format_command.subprocess, "run", return_value=mock.Mock(returncode=code)
                        ) as run,
                        contextlib.redirect_stdout(io.StringIO()),
                    ):
                        self.assertEqual(format_command.run_ruff_format(files, check=check), int(code != 0))
                        self.assertEqual((sys.platform, sys.executable, sys.prefix), process_python_state)
                        mode = ("--check",) if check else ()
                        run.assert_called_once_with((*command, *mode, *files), cwd=format_command.PROJECT_ROOT)

    def test_empty_python_format_scope_does_not_start_ruff(self):
        with mock.patch.object(format_command.subprocess, "run") as run:
            self.assertEqual(format_command.run_ruff_format([], check=True), 0)
        run.assert_not_called()

    def test_windows_clang_format_uses_the_ready_pinned_sdk_tool(self):
        sdk = Path(r"C:\state\llvm\toolchains\llvm-22.1.8-x86_64-windows-msvc")
        manifest = mock.Mock()

        with (
            mock.patch.object(
                format_command.builddir, "platform_profile", return_value=format_command.builddir.WINDOWS_PROFILE
            ),
            mock.patch.object(format_command.windows_llvm_sdk, "read_manifest", return_value=manifest) as read,
            mock.patch.object(format_command.windows_llvm_sdk, "ready_root", return_value=(sdk, True)),
            mock.patch.object(format_command.subprocess, "run", return_value=mock.Mock(returncode=0)) as run,
            contextlib.redirect_stdout(io.StringIO()),
        ):
            self.assertEqual(format_command.run_clang_format(["lib/Foo.cpp"], check=True), 0)

        read.assert_called_once_with(format_command.windows_llvm_sdk.MANIFEST)
        run.assert_called_once_with(
            [str(sdk / "bin" / "clang-format.exe"), "--dry-run", "-Werror", "lib/Foo.cpp"],
            cwd=format_command.PROJECT_ROOT,
        )

    def test_explicit_clang_format_replaces_the_sdk_selection(self):
        explicit = r"C:\sdk\bin\clang-format.exe"
        with (
            mock.patch.object(
                format_command.builddir, "platform_profile", return_value=format_command.builddir.WINDOWS_PROFILE
            ),
            mock.patch.object(
                format_command.windows_llvm_sdk, "read_manifest", side_effect=AssertionError("no SDK discovery")
            ),
            mock.patch.object(format_command.subprocess, "run", return_value=mock.Mock(returncode=0)) as run,
            contextlib.redirect_stdout(io.StringIO()),
        ):
            self.assertEqual(format_command.run_clang_format(["lib/Foo.cpp"], check=False, clang_format=explicit), 0)
        run.assert_called_once_with([explicit, "-i", "lib/Foo.cpp"], cwd=format_command.PROJECT_ROOT)

    def test_clang_format_batches_include_fragments_with_cpp_sources(self):
        with mock.patch.object(
            format_command.builddir,
            "platform_profile",
            return_value=format_command.builddir.LINUX_PROFILE,
        ):
            with mock.patch.object(
                format_command.subprocess,
                "run",
                return_value=mock.Mock(returncode=0),
            ) as run:
                self.assertEqual(
                    format_command.run_clang_format(
                        ["lib/Foo.cpp", "app/include/ao/i18n/MessageInventory.def"], check=True
                    ),
                    0,
                )

        run.assert_called_once_with(
            [
                "clang-format",
                "--dry-run",
                "-Werror",
                "lib/Foo.cpp",
                "app/include/ao/i18n/MessageInventory.def",
            ],
            cwd=format_command.PROJECT_ROOT,
        )


class TidyDiagnosticRoutingTest(unittest.TestCase):
    def test_mixed_scope_preserves_native_python_and_buildifier_diagnostics_in_one_report(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            files = ["lib/Example.cpp", "script/example.py", "BUILD.bazel"]
            for name in files:
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.touch()
            report = root / "diagnostics.txt"
            args = Namespace(
                check=None,
                output=str(report),
                config="debug",
                path=None,
                vcpkg_prefix=None,
                jobs=1,
                remote_cache=None,
            )

            def native(arguments):
                self.assertEqual(arguments._resolved_sources, ["lib/Example.cpp"])
                Path(arguments.output).write_text("native diagnostic\n", encoding="utf-8")
                return 1

            def python(paths, *, python, sink):
                self.assertEqual(paths, ["script/example.py"])
                sink.write("Python diagnostic\n")
                return 1

            def bazel(paths, *, check, lint, sink):
                self.assertEqual(paths, ["BUILD.bazel"])
                self.assertTrue(check and lint)
                sink.write("Buildifier diagnostic\n")
                return 1

            with (
                mock.patch.object(tidy, "PROJECT_ROOT", root),
                mock.patch.object(tidy.bazel_lint, "run", side_effect=native),
                mock.patch.object(tidy.pythoncheck, "run_paths", side_effect=python),
                mock.patch.object(tidy.buildifier, "run", side_effect=bazel),
                contextlib.redirect_stdout(io.StringIO()),
            ):
                self.assertEqual(tidy.run_lint_stages(args, files, True), 1)
            self.assertEqual(
                report.read_text(encoding="utf-8"), "native diagnostic\nPython diagnostic\nBuildifier diagnostic\n"
            )

    def test_cpp_check_selector_skips_unrelated_language_checks(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            files = ["example.py", "BUILD.bazel"]
            for name in files:
                (root / name).touch()
            with (
                mock.patch.object(tidy, "PROJECT_ROOT", root),
                mock.patch.object(tidy.pythoncheck, "run_paths") as python,
                mock.patch.object(tidy.buildifier, "run") as bazel,
            ):
                self.assertEqual(tidy.run_lint_stages(Namespace(check="aobus-example", output=None), files, True), 0)
            python.assert_not_called()
            bazel.assert_not_called()


def _hygiene_args(**overrides):
    defaults = {"files": [], "all": False, "folder": [], "commit": None, "path": None, "jobs": 1}
    defaults.update(overrides)
    return Namespace(**defaults)


class HygieneCommandTest(unittest.TestCase):
    def setUp(self):
        self.stack = contextlib.ExitStack()
        self.addCleanup(self.stack.close)
        self.files = [
            "test/audio/FooTest.cpp",
            "test/audio/FooTestSupport.h",
            "test/tui/TrackTableBenchmark.cpp",
            "lib/audio/Foo.h",
            "script/foo.py",
        ]
        self.test_tree_files = self.files[:3]
        self.resolve = self.stack.enter_context(
            mock.patch.object(hygiene.format_command, "resolve_files", return_value=self.files)
        )
        self.stack.enter_context(mock.patch.object(hygiene.format_command, "_file_exists", return_value=True))
        self.format = self.stack.enter_context(mock.patch.object(hygiene.format_command, "run_command", return_value=0))
        self.test_audit = self.stack.enter_context(mock.patch.object(hygiene.test_audit, "run_command", return_value=0))
        self.name_audit = self.stack.enter_context(mock.patch.object(hygiene.name_audit, "run_command", return_value=0))
        self.lint = self.stack.enter_context(mock.patch.object(tidy, "run_lint_stages", return_value=0))
        self.native_gate = self.stack.enter_context(mock.patch.object(hygiene, "_run_native", return_value=0))
        self.stderr = self.stack.enter_context(contextlib.redirect_stderr(io.StringIO()))
        self.stack.enter_context(contextlib.redirect_stdout(io.StringIO()))

    def test_the_default_route_delegates_to_the_governed_native_hygiene(self):
        args = _hygiene_args()
        self.native_gate.return_value = 3
        self.assertEqual(hygiene.run_command(args), 3)
        self.native_gate.assert_called_once_with(args)
        self.format.assert_not_called()
        self.lint.assert_not_called()

    def test_explicit_tree_reaches_formatting_and_the_lint_stage(self):
        selected = str(Path("/custom/builds/tidy"))
        self.assertEqual(hygiene.run_command(_hygiene_args(path=selected), lint=("tidy", self.lint)), 0)
        self.assertEqual(self.format.call_args.kwargs["build_dir"], Path(selected))
        self.lint.assert_called_once_with(self.files, False)

    def test_resolves_once_and_preserves_implicit_scope_for_the_lint_stage(self):
        self.assertEqual(hygiene.run_command(_hygiene_args(), lint=("tidy", self.lint)), 0)
        self.resolve.assert_called_once()
        self.assertTrue(self.format.call_args.args[0].check)
        self.assertEqual(self.format.call_args.kwargs["files"], self.files)
        # Content discovery, not the registration filename convention: the
        # header and the benchmark source reach the test audit alongside the
        # registration file, while lib/ and Python files do not.
        self.assertEqual(self.test_audit.call_args.args[0].paths, self.test_tree_files)
        self.assertEqual(self.name_audit.call_args.args[0].paths, self.files[:4])
        self.assertTrue(self.test_audit.call_args.args[0].fail_on_issue)
        self.lint.assert_called_once_with(self.files, False)

    def test_python_scope_skips_cpp_audits_and_preserves_explicit_selection(self):
        self.resolve.return_value = ["script/foo.py"]
        self.assertEqual(hygiene.run_command(_hygiene_args(files=["script/foo.py"]), lint=("tidy", self.lint)), 0)
        self.test_audit.assert_not_called()
        self.name_audit.assert_not_called()
        self.lint.assert_called_once_with(["script/foo.py"], True)

    def test_objective_cpp_tests_reach_the_test_audit(self):
        self.resolve.return_value = ["test/media/FooTest.mm"]
        self.assertEqual(hygiene.run_command(_hygiene_args(), lint=("tidy", self.lint)), 0)
        self.assertEqual(self.test_audit.call_args.args[0].paths, ["test/media/FooTest.mm"])
        self.assertTrue(self.test_audit.call_args.args[0].fail_on_issue)

    def test_registration_in_a_support_file_fails_hygiene(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            path = root / "test" / "query" / "FooSupport.cpp"
            path.parent.mkdir(parents=True)
            path.write_text('TEST_CASE("Foo - preserves the contract") {}\n', encoding="utf-8")
            self.resolve.return_value = [str(path)]
            self.test_audit.side_effect = lambda args: int(
                bool(hygiene.test_audit.testaudit.audit_paths(args.paths, root))
            )
            is_candidate = hygiene.testaudit.is_audit_candidate
            with mock.patch.object(
                hygiene.testaudit, "is_audit_candidate", side_effect=lambda name: is_candidate(name, root)
            ):
                self.assertEqual(hygiene.run_command(_hygiene_args(), lint=("tidy", self.lint)), 1)
        self.assertTrue(self.test_audit.call_args.args[0].fail_on_issue)
        self.name_audit.assert_called_once()
        self.lint.assert_called_once()

    def test_lint_fixtures_never_reach_the_test_audit(self):
        self.resolve.return_value = ["test/lint/fixture/aobus-check/BasicTest.cpp"]
        self.assertEqual(hygiene.run_command(_hygiene_args(), lint=("tidy", self.lint)), 0)
        self.test_audit.assert_not_called()
        self.assertEqual(self.name_audit.call_args.args[0].paths, self.resolve.return_value)

    def test_empty_scope_does_not_expand_to_a_repository_scan(self):
        self.resolve.return_value = []
        self.assertEqual(hygiene.run_command(_hygiene_args(), lint=("tidy", self.lint)), 0)
        for stage in (self.format, self.test_audit, self.name_audit, self.lint):
            stage.assert_not_called()

    def test_format_failure_stops_before_audits_and_the_lint_stage(self):
        self.format.return_value = 1
        self.assertEqual(hygiene.run_command(_hygiene_args(), lint=("tidy", self.lint)), 1)
        self.test_audit.assert_not_called()
        self.name_audit.assert_not_called()
        self.lint.assert_not_called()
        self.assertIn("check-only", self.stderr.getvalue())
        self.assertIn("./ao format", self.stderr.getvalue())

    def test_audit_failures_propagate_without_claiming_lint_failed(self):
        for stage, label in ((self.test_audit, "Test names/tags"), (self.name_audit, "Class/file names")):
            with self.subTest(label=label):
                stage.return_value = 1
                self.assertEqual(hygiene.run_command(_hygiene_args(), lint=("tidy", self.lint)), 1)
                self.assertIn(label, self.stderr.getvalue())
                self.assertNotIn("Lint findings", self.stderr.getvalue())
                stage.return_value = 0

    def test_caller_lint_stage_replaces_tidy_with_the_same_scope_and_failure_report(self):
        stage = mock.Mock(return_value=1)
        args = _hygiene_args(files=list(self.files))
        managed = Path("managed tool env") / "bin" / "python"
        self.assertEqual(
            hygiene.run_command(args, clang_format="sdk-clang-format", python=managed, lint=("bazel lint", stage)), 1
        )
        self.assertEqual(self.format.call_args.kwargs["clang_format"], "sdk-clang-format")
        self.assertEqual(self.format.call_args.kwargs["python"], managed)
        stage.assert_called_once_with(self.files, True)
        self.native_gate.assert_not_called()
        self.assertIn("Lint findings", self.stderr.getvalue())

    def test_subcommand_args_use_the_real_parser_defaults(self):
        args = _hygiene_args(jobs=4)
        for register_command, name, build in (
            (format_command.register, "format", lambda _: hygiene._format_args()),
            (hygiene.test_audit.register, "test-audit", lambda _: hygiene._test_audit_args([])),
            (hygiene.name_audit.register, "name-audit", lambda _: hygiene._name_audit_args([])),
        ):
            parser = argparse.ArgumentParser()
            register_command(parser.add_subparsers())
            self.assertEqual(vars(build(args)).keys(), vars(parser.parse_args([name])).keys(), name)


class NativeHygieneSelectionTest(unittest.TestCase):
    """The flat gate's native-tools selection and staged lint callback."""

    STATE = str(Path(tempfile.gettempdir()) / "aobus-native-hygiene-state")
    TOOLS = Path(tempfile.gettempdir()) / "aobus-native-hygiene-tools" / "bin" / "python"

    def gate(self, arguments, host="linux", environment=None, files=None):
        """Run the flat gate with its shared stages faked; return the gate's selections."""
        if files is None:
            files = ["script/ao/command/hygiene.py", "lib/utility/Base64.cpp"]
        args = make_parser().parse_args(["hygiene", *arguments])
        run_command = hygiene.run_command
        with (
            mock.patch.object(native, "native_platform", return_value=host),
            mock.patch.dict(os.environ, {"AOBUS_STATE_ROOT": self.STATE, **(environment or {})}),
            mock.patch("ao.core.gitfiles.changed_files", return_value=files),
            mock.patch("ao.core.pythonenv.ensure_environment", return_value=self.TOOLS) as tools,
            mock.patch.object(hygiene, "run_command", return_value=0) as gate,
        ):
            self.assertEqual(run_command(args), 0)
        gate.assert_called_once()
        self.assertIs(gate.call_args.args[0], args)
        # The Nix shell's interpreter already carries Ruff and mypy.
        if host == "linux" or not any(name.endswith(".py") for name in files):
            tools.assert_not_called()
            self.assertIsNone(gate.call_args.kwargs["python"])
        else:
            tools.assert_called_once_with(PROJECT_ROOT, Path(self.STATE), platform_name=host)
            self.assertEqual(gate.call_args.kwargs["python"], self.TOOLS)
        self.assertEqual(args._resolved_sources, files)
        return gate.call_args.kwargs

    def test_lint_stage_routes_python_to_its_checks_and_cpp_to_bazel_lint_in_the_gate_tree(self):
        tree = str(Path(tempfile.gettempdir()) / "aobus-native-hygiene")
        stage = self.gate(
            [
                *("--config", "release", "--path", tree, "--vcpkg-prefix", "/vcpkg", "-j", "6"),
                *("--remote-cache", "https://cache.example"),
            ]
        )
        self.assertIsNone(stage["clang_format"])
        label, lint = stage["lint"]
        self.assertEqual(label, "bazel lint")
        files = ["script/ao/command/hygiene.py", "lib/utility/Base64.cpp", "lib/utility/Missing.cpp"]
        # The lint stage parses tidy's own registration for its defaults.
        parser = argparse.ArgumentParser()
        tidy.register(parser.add_subparsers())
        defaults = vars(parser.parse_args(["tidy"]))
        for explicit in (True, False):
            with (
                self.subTest(explicit=explicit),
                mock.patch("ao.core.pythoncheck.run_paths", return_value=0) as python,
                mock.patch("ao.core.bazel_lint.run", return_value=1) as native_lint,
                contextlib.redirect_stdout(io.StringIO()),
            ):
                self.assertEqual(lint(files, explicit), 1)
            python.assert_called_once_with(["script/ao/command/hygiene.py"], python=None, sink=None)
            selected = vars(native_lint.call_args.args[0])
            self.assertEqual(selected.pop("_resolved_sources"), ["lib/utility/Base64.cpp"])
            expected = {
                **defaults,
                "config": "release",
                "path": tree,
                "vcpkg_prefix": "/vcpkg",
                "jobs": 6,
                "remote_cache": "https://cache.example",
                "files": ["lib/utility/Base64.cpp"] if explicit else [],
            }
            self.assertEqual(selected, expected)

    def test_python_failure_fails_the_stage_without_starting_bazel_for_no_cpp_scope(self):
        _, lint = self.gate([])["lint"]
        with (
            mock.patch("ao.core.pythoncheck.run_paths", return_value=1),
            mock.patch("ao.core.bazel_lint.run", side_effect=AssertionError("no C++ scope")),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            self.assertEqual(lint(["script/ao/command/hygiene.py"], True), 1)

    def test_native_hosts_run_only_ruff_and_mypy_in_the_managed_tool_environment(self):
        # Both native hosts take the same non-Linux branch in the native gate; one
        # representative host proves the managed-tool environment is used.
        with mock.patch("ao.core.windows_llvm_sdk.ready_root", return_value=(Path("sdk"), True)):
            _, lint = self.gate([], host="macos")["lint"]
            with (
                mock.patch("ao.core.pythoncheck.run_paths", return_value=0) as python,
                contextlib.redirect_stdout(io.StringIO()),
            ):
                self.assertEqual(lint(["script/ao/command/hygiene.py"], True), 0)
            python.assert_called_once_with(["script/ao/command/hygiene.py"], python=self.TOOLS, sink=None)

    def test_empty_scope_skips_native_tool_requirements(self):
        for host in ("windows", "macos"):
            args = make_parser().parse_args(["hygiene"])
            with (
                self.subTest(host=host),
                mock.patch.object(native, "native_platform", return_value=host),
                mock.patch("ao.core.gitfiles.changed_files", return_value=[]) as changed,
                mock.patch("ao.core.windows_llvm_sdk.ready_root", side_effect=AssertionError("no SDK for empty scope")),
                mock.patch(
                    "ao.core.pythonenv.ensure_environment", side_effect=AssertionError("no empty tooling stage")
                ),
                mock.patch("ao.command.format.run_command", side_effect=AssertionError("no formatting stage")),
                contextlib.redirect_stdout(io.StringIO()) as stdout,
            ):
                self.assertEqual(hygiene.run_command(args), 0)
            changed.assert_called_once()
            self.assertEqual(args._resolved_sources, [])
            self.assertIn("No files to check.", stdout.getvalue())

    def test_python_only_windows_scope_needs_no_llvm_sdk(self):
        source = "script/ao/core/pythoncheck.py"
        args = make_parser().parse_args(["hygiene", source])
        with (
            mock.patch.object(native, "native_platform", return_value="windows"),
            mock.patch.dict(os.environ, {"AOBUS_STATE_ROOT": self.STATE}),
            mock.patch("ao.core.windows_llvm_sdk.ready_root", side_effect=AssertionError("SDK not provisioned")),
            mock.patch("ao.core.pythonenv.ensure_environment", return_value=self.TOOLS) as tools,
            mock.patch("ao.command.format.run_ruff_format", return_value=0) as formatting,
            mock.patch("ao.core.pythoncheck.run_paths", return_value=0) as python,
            mock.patch("ao.core.bazel_lint.run", side_effect=AssertionError("no C++ stage")),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            self.assertEqual(hygiene.run_command(args), 0)
        tools.assert_called_once_with(PROJECT_ROOT, Path(self.STATE), platform_name="windows")
        formatting.assert_called_once_with([source], check=True, python=self.TOOLS)
        python.assert_called_once_with([source], python=self.TOOLS, sink=None)

    def test_cpp_only_native_scope_does_not_prepare_python_tools(self):
        source = "lib/utility/Base64.cpp"
        for host in ("windows", "macos"):
            args = make_parser().parse_args(["hygiene", source])
            with (
                self.subTest(host=host),
                mock.patch.object(native, "native_platform", return_value=host),
                mock.patch("ao.core.windows_llvm_sdk.ready_root", return_value=(Path("sdk"), True)) as sdk,
                mock.patch("ao.core.pythonenv.ensure_environment", side_effect=AssertionError("no Python scope")),
                mock.patch("ao.command.format.run_clang_format", return_value=0) as formatting,
                mock.patch("ao.command.name_audit.run_command", return_value=0),
                mock.patch("ao.core.pythoncheck.run_paths", side_effect=AssertionError("no Python stage")),
                mock.patch("ao.core.bazel_lint.run", return_value=0) as native_lint,
                contextlib.redirect_stdout(io.StringIO()),
            ):
                self.assertEqual(hygiene.run_command(args), 0)
            formatting.assert_called_once_with(
                [source],
                check=True,
                build_dir=None,
                clang_format=str(Path("sdk/bin/clang-format.exe")) if host == "windows" else None,
            )
            self.assertEqual(native_lint.call_args.args[0]._resolved_sources, [source])
            if host == "windows":
                sdk.assert_called_once()
            else:
                sdk.assert_not_called()

    def test_scope_is_reused_and_managed_tools_leave_bazel_on_the_portal_interpreter(self):
        sources = ["script/ao/core/pythoncheck.py", "lib/utility/Base64.cpp"]
        args = make_parser().parse_args(["hygiene"])
        portal_python = sys.executable

        def lint(selected):
            self.assertEqual(selected._resolved_sources, [sources[1]])
            self.assertEqual(sys.executable, portal_python)
            return 0

        with (
            mock.patch.object(native, "native_platform", return_value="windows"),
            mock.patch.dict(os.environ, {"AOBUS_STATE_ROOT": self.STATE}),
            mock.patch("ao.core.gitfiles.changed_files", return_value=sources) as changed,
            mock.patch("ao.core.windows_llvm_sdk.ready_root", return_value=(Path("sdk"), True)),
            mock.patch("ao.core.pythonenv.ensure_environment", return_value=self.TOOLS) as tools,
            mock.patch("ao.command.format.run_clang_format", return_value=0),
            mock.patch("ao.command.format.run_ruff_format", return_value=0) as formatting,
            mock.patch("ao.command.name_audit.run_command", return_value=0),
            mock.patch("ao.core.pythoncheck.run_paths", return_value=0) as python,
            mock.patch("ao.core.bazel_lint.run", side_effect=lint) as native_lint,
            contextlib.redirect_stdout(io.StringIO()),
        ):
            self.assertEqual(hygiene.run_command(args), 0)
        changed.assert_called_once()
        tools.assert_called_once_with(PROJECT_ROOT, Path(self.STATE), platform_name="windows")
        formatting.assert_called_once_with([sources[0]], check=True, python=self.TOOLS)
        python.assert_called_once_with([sources[0]], python=self.TOOLS, sink=None)
        native_lint.assert_called_once()
        self.assertEqual(sys.executable, portal_python)

    def test_tool_environment_failure_stops_before_any_stage(self):
        args = make_parser().parse_args(["hygiene", "script/ao/command/hygiene.py"])
        run_command = hygiene.run_command
        with (
            mock.patch.object(native, "native_platform", return_value="macos"),
            mock.patch.dict(os.environ, {"AOBUS_STATE_ROOT": self.STATE}),
            mock.patch("ao.core.pythonenv.ensure_environment", side_effect=RuntimeError("no locked wheel")),
            mock.patch.object(hygiene, "run_command", side_effect=AssertionError("no gate without tools")),
            contextlib.redirect_stderr(io.StringIO()) as stderr,
            self.assertRaises(SystemExit),
        ):
            run_command(args)
        self.assertIn("Python tooling bootstrap failed: no locked wheel", stderr.getvalue())

    def test_windows_formats_with_the_provisioned_sdk_or_names_provisioning(self):
        from ao.core import windows_llvm_sdk

        with tempfile.TemporaryDirectory(prefix="aobus-native-hygiene-sdk-") as temporary:
            root = Path(temporary)
            for relative in windows_llvm_sdk.read_manifest(windows_llvm_sdk.MANIFEST).required_files:
                (root / relative).parent.mkdir(parents=True, exist_ok=True)
                (root / relative).write_bytes(b"")
            stage = self.gate(
                [], host="windows", environment={"AOBUS_LLVM_SDK_ROOT": temporary}, files=["lib/utility/Base64.cpp"]
            )
            self.assertEqual(stage["clang_format"], str(root / "bin/clang-format.exe"))
            (root / "bin/clang-format.exe").unlink()
            args = make_parser().parse_args(["hygiene", "lib/utility/Base64.cpp"])
            run_command = hygiene.run_command
            with (
                mock.patch.object(native, "native_platform", return_value="windows"),
                mock.patch.dict(os.environ, {"AOBUS_LLVM_SDK_ROOT": temporary}),
                mock.patch.object(hygiene, "run_command", side_effect=AssertionError("no gate without an SDK")),
                contextlib.redirect_stderr(io.StringIO()) as stderr,
                self.assertRaises(SystemExit),
            ):
                run_command(args)
        self.assertIn("ao deps provision", stderr.getvalue())


if __name__ == "__main__":
    unittest.main()
