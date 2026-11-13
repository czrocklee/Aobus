"""CLI smoke tests: every command registers and parses its documented invocations."""

import argparse
import contextlib
import io
import json
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import threading
import unittest
import xml.etree.ElementTree as ElementTree
from concurrent.futures import Future
from dataclasses import replace
from pathlib import Path
from unittest import mock
from urllib.parse import quote

from phase_support import PhaseRecorder as _PhaseRecorder

from ao.__main__ import main, make_parser, parse_arguments
from ao.command import analyze as analyze_command
from ao.command import coverage as coverage_command
from ao.command import perf as perf_command
from ao.command import run as run_command_mod
from ao.command import test as test_command
from ao.command import tidy as tidy_command
from ao.core import bazel_coverage, bazel_lint_action, bazel_workflows, builddir, buildenv, doccheck
from ao.core.bazel_lint_reports import Identity

# Raw clang-tidy forms rejected by the option type and the programmatic guard.
# A bare word such as "fix" is intentionally absent: it is not an option.
_MUTATING_RAW_TIDY_ARGUMENTS = (
    "-fix",
    "--fix",
    "-fix-errors",
    "--fix-errors",
    "-fix-notes",
    "--fix-notes",
    "-export-fixes",
    "--export-fixes",
    "-fix=true",
    "--fix=true",
    "-fix-errors=true",
    "--fix-errors=true",
    "-fix-notes=true",
    "--fix-notes=true",
    "-export-fixes=fixes.yaml",
    "--export-fixes=fixes.yaml",
    "---fix",
    "----export-fixes=fixes.yaml",
    '"--fix"',
    "'--export-fixes=fixes.yaml'",
    "@responses.rsp",
    "@nested/path/responses.rsp",
    '"@quoted responses.rsp"',
    "'@single-quoted responses.rsp'",
    "@",
    "-export-fixes=/tmp/secret-fixes.yaml",
    "--export-fixes=/tmp/secret-fixes.yaml",
    "@/tmp/secret-responses.rsp",
)

# Generic validator text. Flag-shaped options and raw response files are different
# classes. Neither message echoes the raw token or a path.
_FIX_OPTION_REJECTION = "clang-tidy fix and export options are unsupported; portal analysis is diagnostic-only"
_RESPONSE_FILE_REJECTION = "raw clang-tidy response files are unsupported; pass diagnostic arguments explicitly"
_REMOVED_TIDY_FIX = "--fix was removed; ao tidy is diagnostic-only and does not apply replacements"


def _diagnostic_rejection(argument: str) -> str:
    if argument.strip("'\"").startswith("@"):
        return _RESPONSE_FILE_REJECTION
    return _FIX_OPTION_REJECTION


class NativePortalTest(unittest.TestCase):
    def test_macos_bootstrap_maps_native_architectures_to_project_triplets(self):
        helper = Path(__file__).resolve().parents[2] / "script" / "ao" / "macos-vcpkg-bootstrap.sh"
        bash = shutil.which("bash")
        if bash is None:
            self.skipTest("bash is unavailable on this host")

        result = subprocess.run(
            [
                bash,
                "-c",
                'source "$1"; aobus_macos_triplet x86_64; aobus_macos_triplet arm64',
                "aobus-bootstrap-test",
                str(helper),
            ],
            capture_output=True,
            text=True,
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.splitlines(), ["x64-aobus-osx", "arm64-aobus-osx"])

    def test_macos_expected_shim_disables_exactly_five_libcxx_gates(self):
        helper = Path(__file__).resolve().parents[2] / "script" / "ao" / "macos-vcpkg-bootstrap.sh"
        bash = shutil.which("bash")
        if bash is None:
            self.skipTest("bash is unavailable on this host")

        with tempfile.TemporaryDirectory() as temp_dir:
            temp = Path(temp_dir)
            header = temp / "llvm" / "include" / "c++" / "v1" / "__expected" / "expected.h"
            header.parent.mkdir(parents=True)
            header.write_text("\n".join(["#  if _LIBCPP_STD_VER >= 26"] * 5) + "\n", encoding="utf-8")
            result = subprocess.run(
                [
                    bash,
                    "-c",
                    'source "$1"; aobus_macos_prepare_expected_shim "$2" 22.1.8 "$3"; '
                    'grep -c "^#  if 0$" "$AOBUS_LIBCXX_EXPECTED_SHIM/__expected/expected.h"',
                    "aobus-bootstrap-test",
                    str(helper),
                    str(temp / "llvm"),
                    str(temp / "state"),
                ],
                capture_output=True,
                text=True,
            )

            destination = temp / "state" / "tools" / "libcxx-expected-shim" / "22.1.8" / "__expected" / "expected.h"
            os.utime(destination, ns=(1_000_000_000, 1_000_000_000))
            preserved_mtime = destination.stat().st_mtime_ns
            repeated = subprocess.run(
                [
                    bash,
                    "-c",
                    'source "$1"; aobus_macos_prepare_expected_shim "$2" 22.1.8 "$3"',
                    "aobus-bootstrap-test",
                    str(helper),
                    str(temp / "llvm"),
                    str(temp / "state"),
                ],
                capture_output=True,
                text=True,
            )
            repeated_mtime = destination.stat().st_mtime_ns

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), "5")
        self.assertEqual(repeated.returncode, 0, repeated.stderr)
        self.assertEqual(repeated_mtime, preserved_mtime)

    @unittest.skipUnless(os.name == "posix" and os.uname().sysname == "Linux", "Linux portal fixture")
    def test_portal_help_preserves_existing_hook_configuration(self):
        root = Path(__file__).resolve().parents[2]
        with tempfile.TemporaryDirectory() as temporary:
            fixture = Path(temporary)
            shutil.copy2(root / "ao", fixture / "ao")
            package = fixture / "script" / "ao"
            package.mkdir(parents=True)
            (package / "__init__.py").touch()
            (package / "__main__.py").write_text("print('help fixture')", encoding="utf-8")
            subprocess.run(["git", "init", "-q", str(fixture)], check=True)
            subprocess.run(["git", "-C", str(fixture), "config", "core.hooksPath", "custom-hooks"], check=True)
            (package / "core").mkdir()
            (package / "core/__init__.py").touch()
            (package / "core/buildenv.py").write_text("print('0')", encoding="utf-8")
            (package / "macos-vcpkg-bootstrap.sh").write_text(
                f"aobus_macos_prepare_python() {{ AOBUS_PYTHON={shlex.quote(sys.executable)}; }}\n",
                encoding="utf-8",
            )
            for platform in ("Linux", "Darwin"):
                with self.subTest(platform=platform):
                    temp_state = fixture / f"temporary-{platform}"
                    temp_state.mkdir()
                    result = subprocess.run(
                        [
                            "bash",
                            "-c",
                            f'uname() {{ echo {platform}; }}; export -f uname; bash "$1" help',
                            "fixture",
                            str(fixture / "ao"),
                        ],
                        env={**os.environ, "AO_IN_NIX_PORTAL": "1", "TMPDIR": str(temp_state)},
                        capture_output=True,
                        text=True,
                        timeout=15,
                    )
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(list(temp_state.iterdir()), [])
                    configured = subprocess.check_output(
                        ["git", "-C", str(fixture), "config", "--get", "core.hooksPath"], text=True
                    ).strip()
                    self.assertEqual(configured, "custom-hooks")

    def test_macos_managed_python_is_requested_for_checks_and_documentation(self):
        for command in ("format", "tidy", "hygiene"):
            self.assertTrue(buildenv.requires_python_tools(command))
        self.assertTrue(buildenv.requires_python_tools("docs", ["check"]))
        self.assertTrue(buildenv.requires_python_tools("check"))
        for command, arguments in (
            ("build", []),
            ("deps", ["report"]),
            ("run", ["cli"]),
            ("test", []),
            ("verify", []),
        ):
            self.assertFalse(buildenv.requires_python_tools(command, arguments))

        bootstrap = (Path(__file__).resolve().parents[2] / "script" / "ao" / "macos-vcpkg-bootstrap.sh").read_text(
            encoding="utf-8"
        )
        self.assertIn('export AOBUS_PYTHON="$managed_python"', bootstrap)
        self.assertNotIn('export PATH="$(dirname "$managed_python"):$PATH"', bootstrap)


class WindowsBatchPortalTest(unittest.TestCase):
    def test_python_bootstrap_normalizes_state_arguments_and_ignores_ambient_packages(self):
        portal = Path(__file__).resolve().parents[2] / "ao.bat"
        content = portal.read_text(encoding="utf-8").lower()

        self.assertIn('set "aobus_state_argument=%aobus_state_root%"', content)
        self.assertIn('set "aobus_state_argument=%aobus_state_argument%."', content)
        self.assertIn('set "pythonpath=%root%script"', content)
        self.assertNotIn(';%pythonpath%"', content)

    def test_visual_studio_discovery_does_not_trust_an_ambient_installation_root(self):
        helper = Path(__file__).resolve().parents[2] / "script" / "ao" / "windows-vsenv.bat"
        content = helper.read_text(encoding="utf-8").lower()

        self.assertNotIn("if defined vsroot", content)
        self.assertIn("microsoft.visualstudio.component.vc.tools.x86.x64", content)
        self.assertIn('set "vsroot="', content)

    def test_python_bootstrap_does_not_trust_ambient_path_aliases(self):
        portal = Path(__file__).resolve().parents[2] / "ao.bat"
        content = portal.read_text(encoding="utf-8").lower()

        self.assertNotIn("where python.exe", content)
        self.assertIn("aobus_python", content)
        self.assertIn("bootstrap-python.ps1", content)
        self.assertIn("ao.core.pythonenv", content)


def _fixture_binary(tree, name, *, platform=None):
    # Foreign runner fixtures must match the selected target, not the real host.
    platform = platform or builddir.platform_profile().name
    suffix = ".exe" if platform == "windows" else ""
    return tree / "state/output/execroot/_main/bazel-out/fixture/bin" / (name + suffix)


class CliParseTest(unittest.TestCase):
    def setUp(self):
        # Runner contracts use declared test-owned providers, never ambient
        # Bazel. Workflow tests independently own query/selection orchestration.
        self.enterContext(mock.patch.object(bazel_workflows, "prepare_artifacts", self.fixture_artifacts))
        self.lint_action = mock.Mock(spec=bazel_workflows.LintFixtureAction)
        self.lint_action.run.return_value = 0
        self.enterContext(mock.patch.object(bazel_workflows, "prepare_lint_fixtures", self.fixture_lint))

    @contextlib.contextmanager
    def fixture_lint(self, args, *, no_build=False):
        yield self.lint_action

    @contextlib.contextmanager
    def fixture_artifacts(self, args, labels, **_options):
        tree = Path(args.path)
        artifacts = []
        for label in labels:
            suite = next(name for name, spec in test_command.SUITES.items() if spec.target == label)
            binary = _fixture_binary(tree, "ao_" + suite + ("_smoke" if suite == "appkit" else "_test"))
            binary.chmod(0o700)
            root = tree / "state/output/execroot/_main"
            relative = binary.relative_to(root).as_posix()
            artifact = bazel_workflows.parse_artifact(
                {
                    "label": label,
                    "incompatible": False,
                    "executable": relative,
                    "files": [{"path": relative, "directory": False}],
                    "runfiles": [],
                    "manifest": None,
                    "environment": {},
                },
                target=label,
                checksum="a" * 64,
                config=getattr(args, "config", "debug"),
                tree=tree,
                execution_root=root,
            )
            artifacts.append(replace(artifact, requested_by=(label,)))
        yield tuple(artifacts)

    @staticmethod
    def artifact_environment(*, scratch=True):
        environment = {
            "RUNFILES_DIR": "",
            "JAVA_RUNFILES": "",
            "TEST_SRCDIR": "",
            "RUNFILES_MANIFEST_FILE": "",
            "RUNFILES_MANIFEST_ONLY": "0",
            "TEST_WORKSPACE": "_main",
        }
        if scratch:
            environment["TEST_TMPDIR"] = mock.ANY
        return environment

    def parse(self, argv):
        return parse_arguments(make_parser(), argv)

    def test_perf_arguments_default_to_release_review_sampling(self):
        args = self.parse(["perf"])

        self.assertEqual(args.flavor, "release")
        self.assertEqual(args.samples, 20)
        self.assertEqual(args.warmups, 1)
        self.assertEqual(args.filter, "PerformanceReview - ordering and completion-alias workloads")
        self.assertIsNone(args.library_root)
        self.assertEqual(args.library_locale, "en-US")

        buffer = io.StringIO()
        with self.assertRaises(SystemExit), contextlib.redirect_stdout(buffer):
            self.parse(["perf", "--help"])
        self.assertIn("build flavor (default: release)", buffer.getvalue())

    def test_perf_rejects_invalid_sampling_counts(self):
        for arguments in (["perf", "--samples", "0"], ["perf", "--warmups", "-1"]):
            with self.subTest(arguments=arguments):
                with self.assertRaises(SystemExit):
                    self.parse(arguments)

    def test_perf_rejects_missing_library_root_before_build(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            missing = Path(temp_dir) / "missing"
            args = self.parse(["perf", "--library-root", str(missing)])

            with self.assertRaises(SystemExit):
                perf_command.run_command(args)

    def test_perf_summary_supports_capability_specific_fields(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            report_path = Path(temp_dir) / "review.json"
            report_path.write_text(
                json.dumps(
                    {
                        "schema": "aobus-performance-review/v2",
                        "metadata": {
                            "platform": "linux",
                            "build_mode": "release",
                            "compiler": "gcc",
                            "icu_version": "78.3",
                        },
                        "measurements": [
                            {
                                "capability": "ordering",
                                "policy": "icu-secondary",
                                "scenario": "construction",
                                "locale": "de-DE",
                                "dataset": "none",
                                "input_count": 0,
                                "median_ns": 125_000,
                                "p95_ns": 250_000,
                            },
                            {
                                "capability": "completion-alias",
                                "scenario": "direct-hit",
                                "dataset": "ascii",
                                "input_count": 50_000,
                                "median_ns": 500_000,
                                "p95_ns": 750_000,
                            },
                            {
                                "capability": "completion-alias",
                                "policy": "icu-transliteration",
                                "scenario": "alias-hit",
                                "dataset": "cjk",
                                "input_count": 5_000,
                                "median_ns": 1_500_000,
                                "p95_ns": 2_000_000,
                                "byte_metric": {"kind": "snapshot-alias-bytes", "count": 2048},
                            },
                        ],
                    }
                ),
                encoding="utf-8",
            )
            output = io.StringIO()

            with contextlib.redirect_stdout(output):
                perf_command._print_report(report_path)

        self.assertIn("ordering/icu-secondary/construction/de-DE/none (input_count=0)", output.getvalue())
        self.assertIn("completion-alias/direct-hit/ascii (input_count=50000)", output.getvalue())
        self.assertIn("completion-alias/icu-transliteration/alias-hit/cjk (input_count=5000)", output.getvalue())
        self.assertIn("2048 snapshot-alias-bytes", output.getvalue())

    def test_docs_check_failure_reports_issues_and_returns_failure(self):
        args = self.parse(["docs", "check"])
        issue = doccheck.Issue(Path("doc/system/example.md"), 7, "broken-link", "missing target")
        output = io.StringIO()
        with (
            mock.patch.object(doccheck, "check_tree", return_value=[issue]) as check,
            contextlib.redirect_stderr(output),
        ):
            status = args.func(args)

        check.assert_called_once_with()
        self.assertEqual(status, 1)
        self.assertIn("broken-link: missing target", output.getvalue())
        self.assertIn("Documentation check failed: 1 issue(s).", output.getvalue())

    def test_docs_check_success_scans_once_and_reports_document_count(self):
        args = self.parse(["docs", "check"])
        output = io.StringIO()
        with (
            mock.patch.object(doccheck, "check_tree", return_value=[]) as check,
            mock.patch.object(doccheck, "discover_markdown", return_value=[Path("doc/README.md")]),
            contextlib.redirect_stdout(output),
        ):
            status = args.func(args)

        check.assert_called_once_with()
        self.assertEqual(status, 0)
        self.assertEqual(output.getvalue(), "Documentation check passed (1 Markdown files).\n")

    def test_appkit_suite_is_selectable_only_on_macos(self):
        arguments = [
            "test",
            "--appkit",
            "--library",
            "/tmp/aobus-library",
            "--state-root",
            "/tmp/aobus-state",
        ]
        with mock.patch.object(builddir, "platform_profile", return_value=builddir.LINUX_PROFILE):
            with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
                self.parse(arguments)

        with mock.patch.object(builddir, "platform_profile", return_value=builddir.MACOS_PROFILE):
            args = self.parse(arguments)

        self.assertEqual(args.suite, "appkit")

    def test_appkit_suite_stays_out_of_macos_groups(self):
        with mock.patch.object(builddir, "platform_profile", return_value=builddir.MACOS_PROFILE):
            self.assertEqual(test_command.suites_for("all"), builddir.MACOS_PROFILE.all_suites)
            self.assertNotIn("appkit", test_command.suites_for("default"))
            self.assertNotIn("appkit", test_command.suites_for("concurrency"))

    def test_appkit_fixture_options_are_rejected_for_other_suites(self):
        with mock.patch.object(builddir, "platform_profile", return_value=builddir.MACOS_PROFILE):
            args = self.parse(["test", "--core", "--library", "/tmp/aobus-library"])
            with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
                test_command.run_command(args)

    def test_appkit_runner_forwards_scenario_fixtures_run_id_and_sanitizer_environment(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            build_dir = Path(temp_dir) / "build"
            binary = _fixture_binary(build_dir, "ao_appkit_smoke", platform="macos")
            binary.parent.mkdir(parents=True)
            binary.touch()
            library = Path(temp_dir) / "music"
            state_root = Path(temp_dir) / "state"
            parent = mock.Mock()
            parent.wait.return_value = 0
            run_id = "0123456789abcdef0123456789abcdef"

            with mock.patch.object(builddir, "platform_profile", return_value=builddir.MACOS_PROFILE):
                environment = {"INHERITED": "yes", test_command.APPKIT_RUN_ID_ENV: "old"}
                with mock.patch.dict(os.environ, environment, clear=True):
                    with mock.patch.object(test_command.uuid, "uuid4", return_value=mock.Mock(hex=run_id)):
                        with mock.patch.object(test_command.subprocess, "Popen", return_value=parent) as popen:
                            self.assertEqual(
                                test_command.run_appkit_smoke(
                                    build_dir,
                                    scenario="authoring",
                                    library=library,
                                    state_root=state_root,
                                    asan=True,
                                ),
                                0,
                            )

        popen.assert_called_once_with(
            [
                str(binary),
                "--scenario",
                "authoring",
                "--library",
                str(library.resolve()),
                "--state-root",
                str(state_root.resolve()),
            ],
            cwd=test_command.PROJECT_ROOT,
            env={
                **self.artifact_environment(scratch=False),
                "INHERITED": "yes",
                test_command.APPKIT_RUN_ID_ENV: run_id,
                "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1",
            },
        )
        parent.wait.assert_called_once_with(timeout=test_command.APPKIT_PARENT_TIMEOUT_SECONDS)

    def test_appkit_rejects_nonempty_invalid_or_nested_fixtures_before_starting_parent(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            nonempty_state = root / "nonempty-state"
            nonempty_state.mkdir()
            (nonempty_state / "desktop.plist").touch()
            nonempty_library = root / "nonempty-library"
            nonempty_library.mkdir()
            (nonempty_library / "user-track.wav").touch()
            library_file = root / "library-file"
            library_file.touch()
            state_file = root / "state-file"
            state_file.touch()
            cases = (
                (nonempty_state, root / "music"),
                (state_file, root / "music-file-state"),
                (root / "state-a", nonempty_library),
                (root / "state-b", library_file),
                (root / "same", root / "same"),
                (root / "state", root / "state" / "music"),
                (root / "music" / "state", root / "music"),
            )
            with mock.patch.object(test_command.subprocess, "Popen") as popen:
                for state_root, library in cases:
                    with self.subTest(state_root=state_root, library=library):
                        with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
                            test_command.run_appkit_smoke(
                                root / "build", scenario="presentation", library=library, state_root=state_root
                            )

            popen.assert_not_called()

    def test_appkit_command_rejects_nonempty_state_before_build_subprocess(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            state_root = root / "state"
            state_root.mkdir()
            (state_root / "desktop.plist").touch()
            with mock.patch.object(builddir, "platform_profile", return_value=builddir.MACOS_PROFILE):
                args = self.parse(
                    [
                        "test",
                        "--appkit",
                        "--library",
                        str(root / "music"),
                        "--state-root",
                        str(state_root),
                        "--path",
                        str(root / "build"),
                    ]
                )
                with mock.patch.object(test_command, "run") as run:
                    with mock.patch.object(test_command.subprocess, "Popen") as popen:
                        with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
                            test_command.run_command(args)

        run.assert_not_called()
        popen.assert_not_called()

    def test_appkit_desktop_requires_matching_markers_after_successful_process_exits(self):
        for wrong_marker in ("desktop-pass.txt", "successor-pass.txt", "missing", None):
            with self.subTest(wrong_marker=wrong_marker), tempfile.TemporaryDirectory() as temp_dir:
                root = Path(temp_dir)
                build_dir = root / "build"
                binary = _fixture_binary(build_dir, "ao_appkit_smoke")
                binary.parent.mkdir(parents=True)
                binary.touch()
                state_root = root / "state"
                run_id = "0123456789abcdef0123456789abcdef"

                def completed_processes(
                    *args, state_root=state_root, run_id=run_id, wrong_marker=wrong_marker, **kwargs
                ):
                    self.assertEqual(kwargs["environment"][test_command.APPKIT_RUN_ID_ENV], run_id)
                    state_root.mkdir()
                    for marker in ("desktop-pass.txt", "successor-pass.txt"):
                        if wrong_marker == "missing" and marker == "successor-pass.txt":
                            continue
                        value = "another-invocation\n" if marker == wrong_marker else f"{run_id}\n"
                        (state_root / marker).write_text(value, encoding="utf-8")
                    return 0

                with mock.patch.object(test_command.uuid, "uuid4", return_value=mock.Mock(hex=run_id)):
                    with mock.patch.object(test_command.appkitprocess, "run_desktop", side_effect=completed_processes):
                        if wrong_marker is None:
                            self.assertEqual(
                                test_command.run_appkit_smoke(
                                    build_dir, scenario="desktop", library=root / "music", state_root=state_root
                                ),
                                0,
                            )
                        else:
                            with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
                                test_command.run_appkit_smoke(
                                    build_dir, scenario="desktop", library=root / "music", state_root=state_root
                                )

    def test_application_suite_is_independently_selectable_on_every_native_profile(self):
        for profile in (builddir.LINUX_PROFILE, builddir.MACOS_PROFILE, builddir.WINDOWS_PROFILE):
            with (
                self.subTest(platform=profile.name),
                mock.patch.object(builddir, "platform_profile", return_value=profile),
            ):
                self.assertEqual(self.parse(["test", "--application"]).suite, "application")
                self.assertEqual(test_command.suites_for("application"), ("application",))

    def test_suite_group_dispatches_registered_runner_kinds_in_order(self):
        build_dir = Path("/tmp/aobus-test-build")

        with mock.patch.object(
            test_command, "run_catch2_suite", return_value=test_command.SuiteOutcome(0)
        ) as run_suite:
            with mock.patch.object(test_command, "run_non_catch2_suite", return_value=0) as run_non_catch2:
                self.assertEqual(test_command.run_suites(builddir.LINUX_PROFILE.all_suites, build_dir), 0)

        self.assertEqual(
            [call.args[0] for call in run_suite.call_args_list],
            ["core", "application", "backend", "tui", "cli", "gtk", "systemmedia"],
        )
        self.assertEqual([call.args[0] for call in run_non_catch2.call_args_list], ["tooling", "lint"])

    def test_suite_phase_records_the_first_failure_status_and_stops(self):
        build_dir = Path("/tmp/aobus-test-build")
        recorder = _PhaseRecorder()

        with (
            mock.patch.dict(os.environ, {test_command.SHARD_ENV: "1"}),
            mock.patch.object(test_command, "run_catch2_suite", return_value=test_command.SuiteOutcome(3)),
            mock.patch.object(test_command, "run_non_catch2_suite") as run_non_catch2,
            mock.patch.object(test_command, "phases", recorder),
        ):
            self.assertEqual(test_command.run_suites(("core", "tooling"), build_dir), 3)

        self.assertEqual(
            recorder.records,
            [{"name": "suite", "tree": "native", "jobs": 1, "count": 1, "suite": "core", "status": 3}],
        )
        run_non_catch2.assert_not_called()

    def test_gtk_suite_runs_inside_virtual_x11_display(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            build_dir = Path(temp_dir)
            binary = _fixture_binary(build_dir, "ao_gtk_test", platform="linux")
            binary.parent.mkdir(parents=True)
            binary.touch()

            server = mock.Mock()
            server.stdout = io.StringIO("42\n")
            server.wait.return_value = 0
            bus = mock.Mock(stdout=io.StringIO("unix:path=/private-bus\n"), wait=mock.Mock(return_value=0))

            with (
                mock.patch.object(builddir, "platform_profile", return_value=builddir.LINUX_PROFILE),
                mock.patch.object(test_command, "_start_gtk_bus", return_value=bus),
                mock.patch.dict(os.environ, {"G_DEBUG": ""}),
            ):
                with mock.patch.object(test_command.subprocess, "Popen", return_value=server) as popen:
                    with mock.patch.object(test_command, "run", return_value=0) as run:
                        self.assertEqual(
                            test_command.run_catch2_suite("gtk", build_dir, test_filter="[layout]").status, 0
                        )

        popen.assert_called_once_with(
            ["Xvfb", "-displayfd", "1", "-screen", "0", "1280x1024x24", "-nolisten", "tcp"],
            stdout=test_command.subprocess.PIPE,
            stderr=test_command.subprocess.DEVNULL,
            text=True,
        )
        run.assert_called_once_with(
            [str(binary), "[layout]"],
            env={
                **self.artifact_environment(),
                "DISPLAY": ":42",
                "AOBUS_OWNED_GTK_DISPLAY": "1",
                "GTK_A11Y": "test",
                "GTK_IM_MODULE": "simple",
                "GDK_BACKEND": "x11",
                "GDK_DISABLE": "gl,vulkan",
                "GSK_RENDERER": "cairo",
                "G_DEBUG": "fatal-warnings",
                "DBUS_SESSION_BUS_ADDRESS": "unix:path=/private-bus",
                "AOBUS_OWNED_GTK_BUS": "unix:path=/private-bus",
            },
            log=None,
            append=False,
        )
        server.terminate.assert_called_once()
        server.wait.assert_called_once_with(timeout=5)
        bus.terminate.assert_called_once()
        bus.wait.assert_called_once_with(timeout=5)
        self.assertTrue(bus.stdout.closed)

    def test_macos_catch2_execution_runs_directly(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            build_dir = Path(temp_dir)
            binary = _fixture_binary(build_dir, "ao_core_test", platform="macos")
            binary.parent.mkdir(parents=True)
            binary.touch()

            with mock.patch.object(builddir, "platform_profile", return_value=builddir.MACOS_PROFILE):
                with mock.patch.object(test_command, "run", return_value=0) as run:
                    self.assertEqual(
                        test_command.run_catch2_suite("core", build_dir, test_filter="[coreaudio]").status, 0
                    )
                    self.assertEqual(test_command.run_catch2_suite("core", build_dir, list_only=True).status, 0)

        self.assertEqual(
            run.call_args_list,
            [
                mock.call(
                    [str(binary), "[coreaudio]"],
                    env=self.artifact_environment(),
                    log=None,
                    append=False,
                ),
                mock.call(
                    [str(binary), "--list-tests", "--verbosity", "high"],
                    env=self.artifact_environment(),
                    log=None,
                    append=False,
                ),
            ],
        )

    def test_posix_lint_runner_uses_selected_tree_without_preparing_another_host(self):
        build_dir = Path("/tmp/aobus-test-build")

        with mock.patch.object(bazel_workflows, "prepare_lint_fixtures", wraps=self.fixture_lint) as prepare:
            self.assertEqual(test_command.run_non_catch2_suite("lint", build_dir, no_build=True), 0)
        self.assertEqual(prepare.call_args.args[0].path, str(build_dir))
        self.assertTrue(prepare.call_args.kwargs["no_build"])
        self.lint_action.run.assert_called_once_with(log=None)

    def test_tooling_suite_does_not_require_a_cmake_build_tree(self):
        with mock.patch.object(builddir, "platform_profile", return_value=builddir.LINUX_PROFILE):
            args = self.parse(["test", "--tooling", "-p", "/tmp/nonexistent-aobus-build"])

            with mock.patch.object(test_command, "run_suites", return_value=0) as run_suites:
                with mock.patch.object(test_command, "run") as run:
                    self.assertEqual(test_command.run_command(args), 0)

        run.assert_not_called()
        run_suites.assert_called_once_with(
            ("tooling",),
            Path("/tmp/nonexistent-aobus-build"),
            test_filter="",
            list_only=False,
            repeat=1,
            asan=False,
            tsan=False,
            no_build=False,
            concurrency=False,
            allow_no_tests=False,
        )

    def test_test_list_describes_non_catch2_suite_without_running_it(self):
        with mock.patch.object(bazel_workflows, "prepare_lint_fixtures") as run:
            self.assertEqual(
                test_command.run_non_catch2_suite("lint", Path("/tmp/aobus-test-build"), list_only=True),
                0,
            )

        run.assert_not_called()

    def test_windows_lint_is_selectable_and_all_includes_it_without_changing_the_fast_loop(self):
        with mock.patch.object(builddir, "platform_profile", return_value=builddir.WINDOWS_PROFILE):
            self.assertEqual(self.parse(["test", "--lint"]).suite, "lint")
            self.assertEqual(test_command.suites_for("all")[-1], "lint")
            self.assertEqual(test_command.suites_for("default"), ("core", "application", "tui"))
            self.assertNotIn("lint", test_command.suites_for("concurrency"))

    def test_windows_backend_is_explicit_only_and_never_joins_an_unattended_group(self):
        with mock.patch.object(builddir, "platform_profile", return_value=builddir.WINDOWS_PROFILE):
            self.assertEqual(self.parse(["test", "--backend"]).suite, "backend")
            self.assertIn("backend", test_command.selectable_suites())
            groups = test_command.suite_groups()
            for group in ("default", "all", "concurrency"):
                self.assertNotIn("backend", groups[group])

    def test_systemmedia_is_selectable_only_on_linux(self):
        for profile in (builddir.MACOS_PROFILE, builddir.WINDOWS_PROFILE):
            with (
                self.subTest(platform=profile.name),
                mock.patch.object(builddir, "platform_profile", return_value=profile),
            ):
                self.assertNotIn("systemmedia", test_command.selectable_suites())
                with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
                    test_command.suites_for("systemmedia")

    def test_macos_groups_admit_backend_without_systemmedia(self):
        with mock.patch.object(builddir, "platform_profile", return_value=builddir.MACOS_PROFILE):
            groups = test_command.suite_groups()
            self.assertEqual(groups["default"], ("core", "application", "backend", "tui"))
            self.assertEqual(groups["tsan"], ("core", "application", "backend"))
            self.assertIn("backend", groups["concurrency"])
            self.assertNotIn("systemmedia", groups["all"])
            self.assertNotIn("systemmedia", groups["concurrency"])

    def test_windows_no_build_lint_still_initializes_visual_studio_but_listing_does_not(self):
        with mock.patch.object(builddir, "platform_profile", return_value=builddir.WINDOWS_PROFILE):
            self.assertTrue(buildenv.requires_build_env("test", ["--lint", "--no-build"]))
            self.assertTrue(buildenv.requires_build_env("test", ["--all", "--no-build"]))
            self.assertFalse(buildenv.requires_build_env("test", ["--lint", "--no-build", "--list"]))
            # Native no-build still analyzes configured DefaultInfo/runfiles.
            self.assertTrue(buildenv.requires_build_env("test", ["--all", "--no-build", "[focused]"]))
            self.assertFalse(buildenv.requires_build_env("test", ["--tooling"]))

    def test_windows_explicit_tsan_suite_is_rejected(self):
        with mock.patch.object(builddir, "platform_profile", return_value=builddir.WINDOWS_PROFILE):
            with self.assertRaisesRegex(SystemExit, "1"):
                test_command.suites_for("core", tsan=True)

    def test_windows_asan_does_not_configure_unavailable_sanitizers(self):
        with mock.patch.object(builddir, "platform_profile", return_value=builddir.WINDOWS_PROFILE):
            self.assertEqual(test_command._lsan_env(Path("windows-debug-asan")), {})
            self.assertEqual(test_command._ubsan_env(Path("windows-debug-asan")), {})

    def test_linux_asan_uses_the_repository_lsan_suppressions_without_writing_tmp(self):
        with mock.patch.object(builddir, "platform_profile", return_value=builddir.LINUX_PROFILE):
            environment = test_command._lsan_env(Path("custom-build-tree"), enabled=True)

        self.assertEqual(environment, {"LSAN_OPTIONS": f"suppressions={test_command._LSAN_SUPP_PATH}"})
        self.assertTrue(test_command._LSAN_SUPP_PATH.is_file())
        self.assertNotEqual(test_command._LSAN_SUPP_PATH.parent, Path("/tmp"))

    def test_windows_clang_test_is_rejected_before_build_or_run(self):
        args = self.parse(["test", "--core", "--clang", "-n", "-p", "/tmp/aobus-test-build"])
        with mock.patch.object(builddir, "platform_profile", return_value=builddir.WINDOWS_PROFILE):
            with mock.patch.object(test_command, "run_suites") as run_suites:
                with self.assertRaisesRegex(SystemExit, "1"):
                    test_command.run_command(args)

        run_suites.assert_not_called()

    def test_performance_targets_are_never_in_native_concurrency_groups(self):
        for profile in (builddir.LINUX_PROFILE, builddir.MACOS_PROFILE, builddir.WINDOWS_PROFILE):
            with self.subTest(platform=profile.name):
                with mock.patch.object(builddir, "platform_profile", return_value=profile):
                    suites = test_command.suites_for("concurrency")
                targets = {test_command.SUITES[suite].target for suite in suites}
                self.assertNotIn(perf_command.TARGET, targets)
                self.assertNotIn("ao_tui_bench", targets)
                self.assertTrue(suites)

    def test_cross_suite_filter_allows_suites_without_matching_cases(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            build_dir = Path(temp_dir)
            binary = _fixture_binary(build_dir, "ao_cli_test")
            binary.parent.mkdir(parents=True)
            binary.touch()

            with mock.patch.object(test_command, "run", return_value=0) as run:
                self.assertEqual(
                    test_command.run_catch2_suite(
                        "cli",
                        build_dir,
                        test_filter="[concurrency]",
                        allow_no_tests=True,
                    ).status,
                    0,
                )

        run.assert_called_once_with(
            [str(binary), "--allow-running-no-tests", "[concurrency]"],
            env=self.artifact_environment(),
            log=None,
            append=False,
        )

    def test_filtered_suite_group_requires_a_match_globally_not_per_suite(self):
        outcomes = [test_command.SuiteOutcome(0, 0), test_command.SuiteOutcome(0, 2)]
        with mock.patch.object(test_command, "suite_shards", return_value=1):
            with mock.patch.object(test_command, "run_catch2_suite", side_effect=outcomes) as run_suite:
                self.assertEqual(
                    test_command.run_suites(("core", "tui"), Path("/tmp/aobus-test-build"), test_filter="[focused]"),
                    0,
                )

        self.assertEqual([call.args[0] for call in run_suite.call_args_list], ["core", "tui"])
        for call in run_suite.call_args_list:
            self.assertTrue(call.kwargs["allow_no_tests"])
            self.assertTrue(call.kwargs["report_matches"])

    def test_filtered_suite_group_fails_when_no_suite_matches(self):
        with mock.patch.object(test_command, "suite_shards", return_value=1):
            with mock.patch.object(
                test_command,
                "run_catch2_suite",
                side_effect=(test_command.SuiteOutcome(0, 0), test_command.SuiteOutcome(0, 0)),
            ):
                with self.assertRaisesRegex(SystemExit, "1"):
                    test_command.run_suites(("core", "tui"), Path("/tmp/aobus-test-build"), test_filter="[missing]")

    def test_filtered_single_suite_with_no_match_fails(self):
        with mock.patch.object(test_command, "suite_shards", return_value=1):
            with mock.patch.object(test_command, "run_catch2_suite", return_value=test_command.SuiteOutcome(0, 0)):
                with self.assertRaisesRegex(SystemExit, "1"):
                    test_command.run_suites(("core",), Path("/tmp/aobus-test-build"), test_filter="[missing]")

    def test_filtered_suite_group_never_turns_a_real_failure_into_an_empty_suite(self):
        with mock.patch.object(test_command, "suite_shards", return_value=1):
            with mock.patch.object(test_command, "run_catch2_suite", return_value=test_command.SuiteOutcome(7, 0)):
                self.assertEqual(
                    test_command.run_suites(("core", "tui"), Path("/tmp/aobus-test-build"), test_filter="[focused]"),
                    7,
                )

    def test_filtered_repeat_requires_matches_on_every_iteration(self):
        outcomes = (
            test_command.SuiteOutcome(0, 0),
            test_command.SuiteOutcome(0, 1),
            test_command.SuiteOutcome(0, 1),
            test_command.SuiteOutcome(0, 0),
        )
        with mock.patch.object(test_command, "suite_shards", return_value=1):
            with mock.patch.object(test_command, "run_catch2_suite", side_effect=outcomes) as run_suite:
                self.assertEqual(
                    test_command.run_suites(
                        ("core", "tui"),
                        Path("/tmp/aobus-test-build"),
                        test_filter="[focused]",
                        repeat=2,
                    ),
                    0,
                )

        self.assertEqual(run_suite.call_count, 4)

    def test_filtered_list_uses_reported_matches_for_the_global_contract(self):
        with mock.patch.object(test_command, "suite_shards", return_value=1):
            with mock.patch.object(
                test_command, "run_catch2_suite", return_value=test_command.SuiteOutcome(0, 1)
            ) as run_suite:
                self.assertEqual(
                    test_command.run_suites(
                        ("core",),
                        Path("/tmp/aobus-test-build"),
                        test_filter="[focused]",
                        list_only=True,
                    ),
                    0,
                )

        self.assertTrue(run_suite.call_args.kwargs["list_only"])
        self.assertTrue(run_suite.call_args.kwargs["report_matches"])

    def test_explicit_filter_excludes_non_catch2_suites(self):
        output = io.StringIO()
        with mock.patch.object(test_command, "suite_shards", return_value=1):
            with mock.patch.object(
                test_command, "run_catch2_suite", return_value=test_command.SuiteOutcome(0, 1)
            ) as run_suite:
                with mock.patch.object(test_command, "run_non_catch2_suite") as run_non_catch2:
                    with contextlib.redirect_stdout(output):
                        self.assertEqual(
                            test_command.run_suites(
                                ("core", "tooling", "lint"),
                                Path("/tmp/aobus-test-build"),
                                test_filter="[focused]",
                            ),
                            0,
                        )

        run_suite.assert_called_once()
        run_non_catch2.assert_not_called()
        self.assertIn("Skipping Tooling Tests, Lint Integration", output.getvalue())

    def test_explicit_filter_requires_at_least_one_catch2_suite(self):
        with self.assertRaisesRegex(SystemExit, "1"):
            test_command.run_suites(("tooling",), Path("/tmp/aobus-test-build"), test_filter="[focused]")

    def test_skipped_cases_count_as_matching_tests(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            report = Path(temp_dir) / "report.xml"
            report.write_text(
                '<Catch2TestRun><OverallResultsCases successes="0" failures="0" '
                'expectedFailures="0" skips="3"/></Catch2TestRun>',
                encoding="utf-8",
            )
            self.assertEqual(test_command._read_matching_tests(report, list_only=False), 3)

    def test_tsan_suite_enforces_fail_fast_runtime_options(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            build_dir = Path(temp_dir)
            binary = _fixture_binary(build_dir, "ao_core_test")
            binary.parent.mkdir(parents=True)
            binary.touch()

            with mock.patch.dict("os.environ", {"TSAN_OPTIONS": "history_size=7"}, clear=False):
                with mock.patch.object(test_command, "run", return_value=0) as run:
                    self.assertEqual(test_command.run_catch2_suite("core", build_dir, tsan=True).status, 0)

        run.assert_called_once_with(
            [str(binary)],
            env={
                **self.artifact_environment(),
                "TSAN_OPTIONS": (
                    f"history_size=7:suppressions={test_command._TSAN_SUPP_PATH}:"
                    "halt_on_error=1:second_deadlock_stack=1"
                ),
            },
            log=None,
            append=False,
        )

    def test_macos_asan_suite_enforces_fail_fast_ubsan_runtime_options(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            build_dir = Path(temp_dir) / "custom-build-tree"
            binary = _fixture_binary(build_dir, "ao_core_test", platform="macos")
            binary.parent.mkdir(parents=True)
            binary.touch()

            with mock.patch.object(builddir, "platform_profile", return_value=builddir.MACOS_PROFILE):
                with mock.patch.dict(
                    "os.environ",
                    {"UBSAN_OPTIONS": "silence_unsigned_overflow=1:halt_on_error=0:print_stacktrace=0"},
                    clear=True,
                ):
                    with mock.patch.object(test_command, "run", return_value=0) as run:
                        self.assertEqual(test_command.run_catch2_suite("core", build_dir, asan=True).status, 0)

        run.assert_called_once_with(
            [str(binary)],
            env={
                **self.artifact_environment(),
                "UBSAN_OPTIONS": "silence_unsigned_overflow=1:halt_on_error=1:print_stacktrace=1",
            },
            log=None,
            append=False,
        )

    def test_macos_tui_asan_keeps_caller_container_annotations(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            build_dir = Path(temp_dir) / "custom-build-tree"
            binary = _fixture_binary(build_dir, "ao_tui_test", platform="macos")
            binary.parent.mkdir(parents=True)
            binary.touch()

            with mock.patch.object(builddir, "platform_profile", return_value=builddir.MACOS_PROFILE):
                with mock.patch.dict("os.environ", {"ASAN_OPTIONS": "detect_container_overflow=1"}, clear=True):
                    with mock.patch.object(test_command, "run", return_value=0) as run:
                        self.assertEqual(test_command.run_catch2_suite("tui", build_dir, asan=True).status, 0)

        run.assert_called_once_with(
            [str(binary)],
            env={**self.artifact_environment(), "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1"},
            log=None,
            append=False,
        )

    def test_tsan_options_preserve_windows_drive_colons(self):
        value = r"history_size=7:suppressions=C:\temp\external.supp:halt_on_error=0"

        self.assertEqual(
            test_command._split_sanitizer_options(value),
            ["history_size=7", r"suppressions=C:\temp\external.supp", "halt_on_error=0"],
        )

    def test_tsan_suite_merges_existing_suppressions_with_project_rules(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            build_dir = root / "build"
            binary = _fixture_binary(build_dir, "ao_core_test")
            binary.parent.mkdir(parents=True)
            binary.touch()
            external_suppressions = root / "external.supp"
            external_suppressions.write_text("race:external_runtime_symbol\n", encoding="utf-8")
            merged_suppressions = root / "merged.supp"

            with mock.patch.dict(
                "os.environ",
                {"TSAN_OPTIONS": f"history_size=7:suppressions={external_suppressions}"},
                clear=False,
            ):
                with mock.patch.object(test_command, "_TSAN_MERGED_SUPP_PATH", merged_suppressions):
                    with mock.patch.object(test_command, "run", return_value=0) as run:
                        self.assertEqual(test_command.run_catch2_suite("core", build_dir, tsan=True).status, 0)

            expected_suppressions = (
                "race:external_runtime_symbol\n\n"
                + test_command._TSAN_SUPP_PATH.read_text(encoding="utf-8").rstrip()
                + "\n"
            )
            self.assertEqual(merged_suppressions.read_text(encoding="utf-8"), expected_suppressions)

        run.assert_called_once_with(
            [str(binary)],
            env={
                **self.artifact_environment(),
                "TSAN_OPTIONS": (
                    f"history_size=7:suppressions={merged_suppressions}:halt_on_error=1:second_deadlock_stack=1"
                ),
            },
            log=None,
            append=False,
        )

    def test_windows_suite_binary_uses_exe_suffix(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            build_dir = Path(temp_dir)
            binary = _fixture_binary(build_dir, "ao_core_test").with_suffix(".exe")
            binary.parent.mkdir(parents=True)
            binary.touch()

            with mock.patch.object(builddir, "platform_profile", return_value=builddir.WINDOWS_PROFILE):
                with mock.patch.object(test_command, "run", return_value=0) as run:
                    self.assertEqual(test_command.run_catch2_suite("core", build_dir).status, 0)

        run.assert_called_once_with([str(binary)], env=self.artifact_environment(), log=None, append=False)

    def test_coverage_defaults_to_the_strict_bazel_default_suite(self):
        args = self.parse(["coverage", "SmartListEvaluator*"])
        self.assertEqual(args.coverage_suite, "default")
        self.assertEqual(args.filter, "SmartListEvaluator*")

    def test_coverage_accepts_backend_core_and_application_shortcuts(self):
        self.assertEqual(self.parse(["coverage", "--core"]).coverage_suite, "core")
        self.assertEqual(self.parse(["coverage", "--application"]).coverage_suite, "application")
        self.assertEqual(self.parse(["coverage", "--backend"]).coverage_suite, "backend")
        self.assertEqual(self.parse(["coverage", "--all"]).coverage_suite, "all")
        # The suite roots are Bazel-declared; admission is owned by the strict
        # coverage pipeline's supported-suite set.
        self.assertIn("default", bazel_coverage._SUITES)
        self.assertIn("gtk", bazel_coverage._SUITES)

    def test_coverage_accepts_scopes(self):
        args = self.parse(
            [
                "coverage",
                "--gtk",
                "--scope",
                "app/linux-gtk",
                "--scope",
                "include/aobus",
                "[layout]",
            ]
        )
        self.assertEqual(args.coverage_suite, "gtk")
        self.assertEqual(args.scope, ["app/linux-gtk", "include/aobus"])
        self.assertEqual(args.filter, "[layout]")

    def test_coverage_preflight_admits_only_linux_before_preparing_a_native_environment(self):
        args = self.parse(["coverage"])
        for host in ("linux", "macos", "windows"):
            with (
                self.subTest(host=host),
                mock.patch.object(coverage_command.bazel_native, "native_platform", return_value=host),
            ):
                if host == "linux":
                    self.assertTrue(coverage_command.requires_build_environment(args))
                else:
                    with self.assertRaisesRegex(coverage_command.bazel_native.BazelWorkspaceError, "only on Linux"):
                        coverage_command.requires_build_environment(args)

    def test_coverage_dispatch_hands_the_parsed_request_straight_to_the_core_producer(self):
        args = self.parse(["coverage", "--application", "--scope", "app/runtime"])
        with mock.patch.object(bazel_coverage, "run", return_value=7) as producer:
            self.assertEqual(coverage_command.run_command(args), 7)
        self.assertIs(producer.call_args.args[0], args)

    def test_tidy_scope_and_passthrough_arguments(self):
        args = self.parse(
            [
                "tidy",
                "--folder",
                "lib",
                "--folder",
                "app",
                "--check",
                "modernize-use-nullptr",
                "--tidy-arg=--extra-arg=-std=c++26",
                "-o",
                "/tmp/report.txt",
                "-j",
                "4",
            ]
        )
        self.assertEqual(args.folder, ["lib", "app"])
        self.assertEqual(args.check, "modernize-use-nullptr")
        self.assertEqual(args.clang_tidy_arg, ["--extra-arg=-std=c++26"])
        self.assertEqual(args.output, "/tmp/report.txt")
        self.assertEqual(args.jobs, 4)
        # The automatic-fix flow is gone; no invocation carries a fix flag.
        self.assertFalse(hasattr(args, "fix"))

    def _rejected_portal_and_preflight_stderr(self, argv: list[str]) -> str:
        """Status 2 from the portal and both preflight modes, with matching stderr."""
        calls: list[str] = []

        def reject(name: str):
            def wrapped(*_args, **_kwargs):
                calls.append(name)
                raise AssertionError(f"{name} entered before parser rejection")

            return wrapped

        with tempfile.TemporaryDirectory() as temporary:
            scope = Path(temporary) / "scope.json"
            portal_stderr = io.StringIO()
            with (
                mock.patch.dict(
                    os.environ,
                    {"AOBUS_PREFLIGHT_SCOPE": str(scope), "AOBUS_PHASE_LOG_DIR": ""},
                ),
                mock.patch("ao.__main__.buildenv.load_source_scope", side_effect=reject("load_source_scope")),
                mock.patch.object(tidy_command.tidyengine, "resolve_scope", side_effect=reject("resolve_scope")),
                mock.patch.object(tidy_command, "run_command", side_effect=reject("tidy_run")),
                mock.patch.object(analyze_command, "run_command", side_effect=reject("analyze_run")),
            ):
                with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(portal_stderr):
                    with self.assertRaises(SystemExit) as failure:
                        main(argv)
                self.assertEqual(failure.exception.code, 2)
                portal_text = portal_stderr.getvalue()
                for mode in ("--exit-code", "--python-tools"):
                    preflight_stderr = io.StringIO()
                    with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(preflight_stderr):
                        with self.assertRaises(SystemExit) as preflight:
                            buildenv.main([mode, *argv])
                    self.assertEqual(preflight.exception.code, 2)
                    self.assertEqual(preflight_stderr.getvalue(), portal_text)
            self.assertEqual(calls, [])
            self.assertFalse(scope.exists())
        return portal_text

    def test_tidy_rejects_the_removed_fix_flag(self):
        # Hidden tidy --fix is refused during parse on every host profile. A bare
        # flag has no following token, so it cannot consume a file or later option.
        # Preflight stderr matches. Help does not advertise the flag or its note.
        invocations = (
            ["tidy", "--fix", "lib/audio/Foo.cpp"],
            ["tidy", "--fix"],
            ["tidy", "--fix", "--no-build", "lib/audio/Foo.cpp"],
            ["tidy", "--no-build", "--fix", "lib/audio/Foo.cpp"],
        )
        for profile in (builddir.LINUX_PROFILE, builddir.WINDOWS_PROFILE):
            for argv in invocations:
                with (
                    self.subTest(profile=profile.name, argv=argv),
                    mock.patch.object(builddir, "platform_profile", return_value=profile),
                ):
                    stderr = self._rejected_portal_and_preflight_stderr(argv)
                    self.assertIn(_REMOVED_TIDY_FIX, stderr)
                    self.assertNotIn("unrecognized arguments", stderr)
                    self.assertNotIn("expected one argument", stderr)
                    self.assertNotIn(_FIX_OPTION_REJECTION, stderr)
                    self.assertNotIn("invalid diagnostic_argument value", stderr)
        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit) as failure:
                main(["tidy", "--help"])
        self.assertEqual(failure.exception.code, 0)
        help_text = stdout.getvalue()
        self.assertNotIn("--fix", help_text)
        self.assertNotIn(_REMOVED_TIDY_FIX, help_text)
        self.assertIn("--no-build", help_text)
        self.assertIn("--tidy-arg", help_text)

    def test_parser_rejects_mutating_tidy_args_before_scope_cache_workspace_or_native_env(self):
        # Portal parse and both buildenv preflight modes exit 2 before scope, cache,
        # workspace, or native environment selection. Nix or Python bootstrap may
        # already have run; this does not claim a bootstrap-free shell. Raw token
        # echo is not required. Flag options and response files keep separate generic
        # explanations, and those explanations do not include a path.
        for command in ("tidy", "analyze"):
            for argument in _MUTATING_RAW_TIDY_ARGUMENTS:
                with self.subTest(command=command, argument=argument):
                    stderr = self._rejected_portal_and_preflight_stderr(
                        [command, f"--tidy-arg={argument}", "lib/Selected.cpp"]
                    )
                    message = _diagnostic_rejection(argument)
                    other = _RESPONSE_FILE_REJECTION if message == _FIX_OPTION_REJECTION else _FIX_OPTION_REJECTION
                    self.assertIn(message, stderr)
                    self.assertNotIn(other, stderr)
                    self.assertNotIn("invalid diagnostic_argument value", stderr)
                    self.assertNotIn(_REMOVED_TIDY_FIX, stderr)
                    self.assertNotIn("ERROR:", stderr)
                    self.assertNotIn("No files specified", stderr)
                    if "secret" in argument:
                        self.assertNotIn("secret", stderr)
                        self.assertNotIn(argument, stderr)

    def test_analysis_identity_retains_owner_native_output_and_checkout_relative_main(self):
        # Per-process source-id timing is retired. Cached Bazel reports carry
        # the full native owner/configuration/source/main identity instead.
        value = {
            "owner": "@@//lib:audio",
            "configuration": "bazel-out/native/bin",
            "source": "lib/audio/Foo.cpp",
            "main": "include/ao/Foo.h",
            "outputs": ["bazel-out/native/bin/lib/audio/Foo.o"],
            "mode": "STRICT",
            "platform": "linux",
        }
        self.assertEqual(Identity.from_json(value).as_json(), value)

    def verification_tools(self, root, *, windows=False):
        host, plugin = root / "declared-checker", root / "declared-plugin"
        host.touch()
        plugin.touch()
        policy, sdk = root / "policy.json", root / "sdk.json"
        policy.write_text(
            json.dumps(
                {
                    "schema": 1,
                    "alpha": False,
                    "check": None,
                    "header_filter": None,
                    "clang_tidy_args": [],
                    "expected_checks": ["aobus-example"],
                }
            ),
            encoding="utf-8",
        )
        resource = root / "resource"
        (resource / "include").mkdir(parents=True)
        sdk.write_text(
            json.dumps(
                {
                    "schema": 1,
                    "version": "22.1.8",
                    "platform": "windows" if windows else "linux",
                    "host_kind": "standalone" if windows else "plugin",
                    "resource_dir": str(resource),
                    "system_arguments": [],
                    "analysis_arguments": list(tidy_command.tidyengine.windows_analysis_arguments(resource))
                    if windows
                    else [],
                }
            ),
            encoding="utf-8",
        )
        return argparse.Namespace(
            policy=policy,
            sdk=sdk,
            tidy=host,
            plugin=None if windows else plugin,
            log=root / "verify.log",
            output=root / "verified.json",
        )

    def test_tidy_artifact_paths_come_from_declared_tools_not_filename_conventions(self):
        with tempfile.TemporaryDirectory() as temporary:
            tools = self.verification_tools(Path(temporary))
            host, plugin = bazel_lint_action._tools(tools, {"platform": "linux"})
            self.assertEqual(host, str(tools.tidy))
            self.assertEqual(plugin, [f"-load={tools.plugin}"])
            tools.tidy.unlink()
            with self.assertRaisesRegex(ValueError, "missing clang-tidy host"):
                bazel_lint_action._tools(tools, {"platform": "linux"})

    def test_tidy_toolchain_validation_requires_registered_aobus_checks(self):
        with tempfile.TemporaryDirectory() as temporary:
            tools = self.verification_tools(Path(temporary))
            for registry, accepted in (("aobus-example", True), ("", False)):
                with (
                    self.subTest(registry=registry),
                    mock.patch.object(
                        bazel_lint_action.subprocess,
                        "run",
                        side_effect=[
                            subprocess.CompletedProcess([], 0, "LLVM version 22.1.8\n"),
                            subprocess.CompletedProcess([], 0, "Enabled checks:\n" + registry),
                        ],
                    ) as run,
                ):
                    if accepted:
                        self.assertEqual(bazel_lint_action.verify(tools), 0)
                    else:
                        with self.assertRaisesRegex(ValueError, "registry differs"):
                            bazel_lint_action.verify(tools)
                        self.assertFalse(tools.output.exists())
                    self.assertEqual(
                        run.call_args.args[0],
                        [str(tools.tidy), f"-load={tools.plugin}", "-checks=-*,aobus-*", "-list-checks"],
                    )

    def test_run_parsing_and_no_build(self):
        args = self.parse(["run", "-n", "--clang", "tui", "release", "arg1", "arg2"])
        self.assertEqual(args.app, "tui")
        self.assertEqual(args.flavor, "release")
        self.assertTrue(args.no_build)
        self.assertTrue(args.clang)
        self.assertEqual(args.app_args, ["arg1", "arg2"])

    def test_run_no_build_after_app_name(self):
        args = self.parse(["run", "tui", "-n"])
        self.assertEqual(args.app, "tui")
        self.assertTrue(args.no_build)
        self.assertEqual(args.app_args, [])

    def test_run_forwards_option_flags_after_double_dash(self):
        args = self.parse(["run", "tui", "--", "--library", "/home/u/Music"])
        self.assertEqual(args.app, "tui")
        self.assertEqual(args.flavor, "debug")
        self.assertEqual(args.app_args, ["--library", "/home/u/Music"])

    def test_run_forwards_flags_alongside_explicit_flavor(self):
        args = self.parse(["run", "tui", "release", "--", "--library", "/m", "--verbose"])
        self.assertEqual(args.flavor, "release")
        self.assertEqual(args.app_args, ["--library", "/m", "--verbose"])

    def test_run_double_dash_keeps_ao_flags_before_it(self):
        args = self.parse(["run", "-n", "tui", "--", "--config", "/etc/aobus"])
        self.assertEqual(args.app, "tui")
        self.assertTrue(args.no_build)
        self.assertEqual(args.app_args, ["--config", "/etc/aobus"])

    def test_run_empty_after_double_dash_forwards_nothing(self):
        args = self.parse(["run", "tui", "--"])
        self.assertEqual(args.app_args, [])

    def test_double_dash_only_special_cased_for_run(self):
        # Other commands keep argparse's stock `--` handling; the forwarding split is run-only.
        with self.assertRaises(SystemExit):
            self.parse(["build", "--", "--library", "/m"])

    def test_main_thread_checker_uses_selected_xcode_without_duplicate_injection(self):
        # This macOS protocol fixture must not contain a Windows drive-letter colon.
        developer = Path("/fixture-xcode")
        library = developer / "usr/lib/libMainThreadChecker.dylib"
        with (
            mock.patch.dict(run_command_mod.os.environ, {"DYLD_INSERT_LIBRARIES": str(library)}, clear=True),
            mock.patch.object(run_command_mod.subprocess, "check_output", return_value=f"{developer}\n") as select,
            mock.patch.object(run_command_mod.Path, "is_file", return_value=True),
        ):
            env = run_command_mod._main_thread_checker_environment()
        select.assert_called_once_with(["xcode-select", "--print-path"], text=True, stderr=subprocess.STDOUT)
        self.assertEqual(env["DYLD_INSERT_LIBRARIES"], str(library))

    def test_shard_count_leaves_one_core_free_and_stops_at_the_cap(self):
        with mock.patch.object(test_command.jobs, "available_cores", return_value=4):
            self.assertEqual(test_command.shard_count(environ={}), 3)
        with mock.patch.object(test_command.jobs, "available_cores", return_value=64):
            self.assertEqual(test_command.shard_count(environ={}), test_command.SHARD_CAP)
        with mock.patch.object(test_command.jobs, "available_cores", return_value=1):
            self.assertEqual(test_command.shard_count(environ={}), 1)

    def test_ci_shard_defaults_use_every_core_with_the_existing_cap_and_override(self):
        with mock.patch.object(test_command.jobs, "available_cores", return_value=4):
            self.assertEqual(test_command.shard_count(environ={"GITHUB_ACTIONS": "true"}), 4)
        with mock.patch.object(test_command.jobs, "available_cores", return_value=64):
            self.assertEqual(test_command.shard_count(environ={"GITHUB_ACTIONS": "true"}), test_command.SHARD_CAP)
            self.assertEqual(
                test_command.shard_count(environ={"GITHUB_ACTIONS": "true", test_command.SHARD_ENV: "64"}), 64
            )

    def test_shard_count_environment_override_is_validated(self):
        self.assertEqual(test_command.shard_count(environ={test_command.SHARD_ENV: "3"}), 3)
        # The override deliberately escapes the cap; a bisecting run may want one shard per core.
        self.assertEqual(test_command.shard_count(environ={test_command.SHARD_ENV: "64"}), 64)
        for invalid in ("0", "-2", "many", ""):
            with self.subTest(value=invalid), self.assertRaisesRegex(SystemExit, "1"):
                test_command.shard_count(environ={test_command.SHARD_ENV: invalid})

    def test_runs_that_measure_contention_are_never_sharded(self):
        with mock.patch.object(test_command, "shard_count", return_value=8):
            self.assertEqual(test_command.suite_shards(), 8)
            self.assertEqual(test_command.suite_shards(list_only=True), 1)
            self.assertEqual(test_command.suite_shards(repeat=2), 1)
            self.assertEqual(test_command.suite_shards(tsan=True), 1)
            self.assertEqual(test_command.suite_shards(concurrency=True), 1)

    def test_every_shard_shares_one_seed_and_owns_one_index(self):
        argv = [
            test_command._shard_argv(["ao_core_test"], index, 4, 1234, Path(f"/tmp/shard-{index}.xml"))
            for index in range(4)
        ]

        for index, command in enumerate(argv):
            self.assertEqual(command[command.index("--rng-seed") + 1], "1234")
            self.assertEqual(command[command.index("--shard-count") + 1], "4")
            self.assertEqual(command[command.index("--shard-index") + 1], str(index))
            # Empty trailing shards are expected whenever a filter matches fewer
            # tests than there are shards.
            self.assertIn("--allow-running-no-tests", command)

    @staticmethod
    def _write_catch2_process_stub(
        build_dir: Path,
        suite: str,
        *,
        successes: int = 0,
        skips: int = 0,
        status: int = 0,
        malformed: bool = False,
    ) -> None:
        """Write a process fixture that emits the Catch2 XML shape requested by the runner."""
        binary = _fixture_binary(build_dir, "ao_" + suite + "_test")
        binary.parent.mkdir(parents=True, exist_ok=True)
        binary.write_text(
            "#!/usr/bin/env python3\n"
            "import pathlib\n"
            "import sys\n"
            f"successes = {successes}\n"
            f"skips = {skips}\n"
            f"status = {status}\n"
            "report_arg = next(arg for arg in sys.argv if arg.startswith('xml::out='))\n"
            "report = pathlib.Path(report_arg.removeprefix('xml::out='))\n"
            + (
                "report.write_text('<not-xml', encoding='utf-8')\n"
                if malformed
                else "if '--list-tests' in sys.argv:\n"
                "    cases = ''.join('<TestCase><Name>stub</Name></TestCase>' for _ in range(successes + skips))\n"
                "    report.write_text(f'<MatchingTests>{cases}</MatchingTests>', encoding='utf-8')\n"
                "else:\n"
                "    report.write_text(\n"
                '        f\'<Catch2TestRun><OverallResultsCases successes="{successes}" failures="0" \''
                '        f\'expectedFailures="0" skips="{skips}"/></Catch2TestRun>\',\n'
                "        encoding='utf-8',\n"
                "    )\n"
            )
            + "print('catch2 process fixture')\n"
            "raise SystemExit(status)\n",
            encoding="utf-8",
        )
        binary.chmod(0o755)

    @unittest.skipIf(os.name == "nt", "POSIX executable process fixture")
    def test_filtered_process_group_accepts_an_empty_suite_when_another_suite_skips_a_match(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            build_dir = Path(temp_dir)
            self._write_catch2_process_stub(build_dir, "core")
            self._write_catch2_process_stub(build_dir, "tui", skips=1)

            with mock.patch.object(test_command, "shard_count", return_value=1):
                with self._captured_stdout():
                    status = test_command.run_suites(("core", "tui"), build_dir, test_filter="[focused]")

        self.assertEqual(status, 0)

    @unittest.skipIf(os.name == "nt", "POSIX executable process fixture")
    def test_filtered_process_single_suite_fails_when_xml_reports_no_match(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            build_dir = Path(temp_dir)
            self._write_catch2_process_stub(build_dir, "core")

            with mock.patch.object(test_command, "shard_count", return_value=1):
                with self._captured_stdout(), contextlib.redirect_stderr(io.StringIO()):
                    with self.assertRaisesRegex(SystemExit, "1"):
                        test_command.run_suites(("core",), build_dir, test_filter="[missing]")

    @unittest.skipIf(os.name == "nt", "POSIX executable process fixture")
    def test_filtered_process_list_uses_matching_tests_xml(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            build_dir = Path(temp_dir)
            self._write_catch2_process_stub(build_dir, "core")
            self._write_catch2_process_stub(build_dir, "tui", successes=1)

            with self._captured_stdout():
                status = test_command.run_suites(("core", "tui"), build_dir, test_filter="[focused]", list_only=True)

        self.assertEqual(status, 0)

    @unittest.skipIf(os.name == "nt", "POSIX executable process fixture")
    def test_filtered_process_propagates_failure_and_rejects_malformed_report(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            build_dir = Path(temp_dir)
            self._write_catch2_process_stub(build_dir, "core", status=7)
            with mock.patch.object(test_command, "shard_count", return_value=1):
                with self._captured_stdout():
                    self.assertEqual(
                        test_command.run_suites(("core",), build_dir, test_filter="[focused]"),
                        7,
                    )

            self._write_catch2_process_stub(build_dir, "core", successes=1, malformed=True)
            with mock.patch.object(test_command, "shard_count", return_value=1):
                with self._captured_stdout() as raw:
                    self.assertEqual(
                        test_command.run_suites(("core",), build_dir, test_filter="[focused]"),
                        1,
                    )
            output = raw.getvalue().decode("utf-8")

        self.assertIn("no readable Catch2 XML report", output)

    @staticmethod
    def _catch2_shard_stub(results):
        """Return a Popen stand-in that writes what a real Catch2 shard would."""

        def spawn(argv, *, cwd=None, env=None, stdout=None, stderr=None):
            index = int(argv[argv.index("--shard-index") + 1])
            result = results[index]
            assertions, cases, failures = result[:3]
            skips = result[3] if len(result) == 4 else 0
            report = next(Path(arg[len("xml::out=") :]) for arg in argv if arg.startswith("xml::out="))
            report.write_text(
                '<?xml version="1.0" encoding="UTF-8"?>\n'
                '<Catch2TestRun name="stub">\n'
                f'  <OverallResults successes="{assertions}" failures="{failures}"'
                f' expectedFailures="0" skips="{skips}"/>\n'
                f'  <OverallResultsCases successes="{cases}" failures="{failures}"'
                f' expectedFailures="0" skips="{skips}"/>\n'
                "</Catch2TestRun>\n",
                encoding="utf-8",
            )
            stdout.write(f"shard {index} console output\n".encode())
            return mock.Mock(returncode=3 if failures else 0, wait=mock.Mock(return_value=0))

        return spawn

    @staticmethod
    @contextlib.contextmanager
    def _captured_stdout():
        """Capture stdout through a stream that has the byte buffer a terminal has."""
        raw = io.BytesIO()
        stream = io.TextIOWrapper(raw, encoding="utf-8", write_through=True)
        with contextlib.redirect_stdout(stream):
            yield raw
        stream.flush()
        # Detach first: collecting the wrapper would otherwise close the buffer
        # the caller is about to read.
        stream.detach()

    def _run_sharded_core(self, spawn, shards):
        with tempfile.TemporaryDirectory() as temp_dir:
            build_dir = Path(temp_dir)
            binary = _fixture_binary(build_dir, "ao_core_test")
            binary.parent.mkdir(parents=True)
            binary.touch()

            with mock.patch.object(test_command.subprocess, "Popen", side_effect=spawn):
                with self._captured_stdout() as raw:
                    status = test_command.run_catch2_suite("core", build_dir, shards=shards).status
        return status, raw.getvalue().decode("utf-8")

    def test_sharded_run_reports_one_tally_summed_over_every_shard(self):
        status, output = self._run_sharded_core(self._catch2_shard_stub([(10, 2, 0), (7, 1, 0)]), shards=2)

        self.assertEqual(status, 0)
        self.assertIn("Core: 3 test cases, 17 assertions, 0 failed across 2 shards", output)
        # A passing shard's console output belongs in the log, not on the terminal.
        self.assertNotIn("shard 0 console output", output)

    def test_sharded_filtered_run_counts_skipped_cases_as_global_matches(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            build_dir = Path(temp_dir)
            binary = _fixture_binary(build_dir, "ao_core_test")
            binary.parent.mkdir(parents=True)
            binary.touch()

            spawn = self._catch2_shard_stub([(0, 0, 0), (0, 0, 0, 1)])
            with mock.patch.object(test_command.subprocess, "Popen", side_effect=spawn):
                with self._captured_stdout():
                    outcome = test_command.run_catch2_suite(
                        "core",
                        build_dir,
                        test_filter="[focused]",
                        allow_no_tests=True,
                        shards=2,
                        report_matches=True,
                    )

        self.assertEqual(outcome, test_command.SuiteOutcome(0, 1))

    def test_sharded_filtered_run_fails_when_the_aggregate_has_no_matches(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            build_dir = Path(temp_dir)
            binary = _fixture_binary(build_dir, "ao_core_test")
            binary.parent.mkdir(parents=True)
            binary.touch()

            spawn = self._catch2_shard_stub([(0, 0, 0), (0, 0, 0)])
            with mock.patch.object(test_command, "shard_count", return_value=2):
                with mock.patch.object(test_command.subprocess, "Popen", side_effect=spawn):
                    with self._captured_stdout(), contextlib.redirect_stderr(io.StringIO()):
                        with self.assertRaisesRegex(SystemExit, "1"):
                            test_command.run_suites(("core",), build_dir, test_filter="[missing]")

    def test_empty_sharded_backend_reports_whether_a_filter_was_supplied(self):
        expectations = (
            ("", "no Backend tests were selected by default. Hidden tests require an explicit filter."),
            ("[missing]", "no Backend tests matched the supplied filter."),
        )
        for test_filter, expected in expectations:
            # Empty-selection behavior is independent of Linux's service setup.
            with (
                self.subTest(test_filter=test_filter),
                tempfile.TemporaryDirectory() as temp_dir,
                mock.patch.object(builddir, "platform_profile", return_value=builddir.MACOS_PROFILE),
            ):
                build_dir = Path(temp_dir)
                binary = _fixture_binary(build_dir, "ao_backend_test")
                binary.parent.mkdir(parents=True)
                binary.touch()
                spawn = self._catch2_shard_stub([(0, 0, 0), (0, 0, 0)])
                with (
                    mock.patch.object(test_command.subprocess, "Popen", side_effect=spawn),
                    self._captured_stdout(),
                    contextlib.redirect_stderr(io.StringIO()) as errors,
                    self.assertRaises(SystemExit) as failure,
                ):
                    test_command.run_catch2_suite("backend", build_dir, test_filter=test_filter, shards=2)

                self.assertEqual(failure.exception.code, 1)
                self.assertIn(expected, errors.getvalue())
                if not test_filter:
                    self.assertNotIn("supplied filter", errors.getvalue())

    def test_empty_sharded_backend_preserves_explicit_zero_match_permission(self):
        with (
            tempfile.TemporaryDirectory() as temp_dir,
            mock.patch.object(builddir, "platform_profile", return_value=builddir.MACOS_PROFILE),
        ):
            build_dir = Path(temp_dir)
            binary = _fixture_binary(build_dir, "ao_backend_test")
            binary.parent.mkdir(parents=True)
            binary.touch()
            spawn = self._catch2_shard_stub([(0, 0, 0), (0, 0, 0)])
            with (
                mock.patch.object(test_command.subprocess, "Popen", side_effect=spawn),
                self._captured_stdout(),
            ):
                outcome = test_command.run_catch2_suite("backend", build_dir, allow_no_tests=True, shards=2)

        self.assertEqual(outcome, test_command.SuiteOutcome(0, 0))

    def test_sharded_run_surfaces_the_failing_shard_and_its_exit_code(self):
        status, output = self._run_sharded_core(self._catch2_shard_stub([(10, 2, 0), (7, 1, 1)]), shards=2)

        self.assertEqual(status, 3)
        self.assertIn("Core: 4 test cases, 18 assertions, 1 failed across 2 shards", output)
        self.assertIn("shard 1 console output", output)
        self.assertNotIn("shard 0 console output", output)

    def test_listing_is_never_sharded(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            build_dir = Path(temp_dir)
            binary = _fixture_binary(build_dir, "ao_core_test")
            binary.parent.mkdir(parents=True)
            binary.touch()

            with mock.patch.object(test_command, "run", return_value=0) as run:
                self.assertEqual(test_command.run_catch2_suite("core", build_dir, list_only=True, shards=8).status, 0)

        run.assert_called_once_with(
            [str(binary), "--list-tests", "--verbosity", "high"],
            env=self.artifact_environment(),
            log=None,
            append=False,
        )

    def test_a_failed_xvfb_start_shuts_down_the_servers_already_running(self):
        started = []

        def spawn(argv, **kwargs):
            if len(started) == 2:
                raise FileNotFoundError("Xvfb")
            server = mock.Mock(stdout=io.StringIO(f"{40 + len(started)}\n"), wait=mock.Mock(return_value=0))
            started.append(server)
            return server

        with mock.patch.object(test_command.subprocess, "Popen", side_effect=spawn):
            with contextlib.redirect_stdout(io.StringIO()), self.assertRaises(SystemExit):
                with test_command.virtual_gtk_displays(4):
                    self.fail("the context body must not run")

        self.assertEqual(len(started), 2)
        for server in started:
            server.terminate.assert_called_once()

    @unittest.skipIf(os.name == "nt", "stable service roots are POSIX-only")
    def test_stable_ipc_root_gives_a_fixed_socket_and_guid_free_address(self):
        def spawn(argv, **kwargs):
            if argv[0] == "Xvfb":
                output = "40\n"
            else:
                config = Path(
                    next(arg.removeprefix("--config-file=") for arg in argv if arg.startswith("--config-file="))
                )
                listen = ElementTree.fromstring(config.read_text(encoding="utf-8")).findtext("listen")
                output = f"{listen},guid=0123456789abcdef\n"
            return mock.Mock(stdout=io.StringIO(output), wait=mock.Mock(return_value=0))

        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir) / "gtk-bus"
            address = f"unix:path={quote((root / 'bus-0').as_posix(), safe='/')}"
            with mock.patch.object(test_command.subprocess, "Popen", side_effect=spawn):
                with contextlib.redirect_stdout(io.StringIO()):
                    with test_command.virtual_gtk_displays(1, ipc_root=root) as environments:
                        self.assertEqual(environments[0]["DBUS_SESSION_BUS_ADDRESS"], address)
                        self.assertEqual(environments[0]["AOBUS_OWNED_GTK_BUS"], address)
            self.assertFalse(root.exists())

    def test_gtk_processes_get_distinct_private_buses_without_host_activation(self):
        started = []
        configs = []

        def spawn(argv, **kwargs):
            if argv[0] == "Xvfb":
                output = f"{40 + len(started)}\n"
            else:
                self.assertEqual(argv[0], "dbus-daemon")
                self.assertIsNone(kwargs["stderr"])
                config = Path(
                    next(arg.removeprefix("--config-file=") for arg in argv if arg.startswith("--config-file="))
                )
                configs.append(config)
                root = ElementTree.fromstring(config.read_text(encoding="utf-8"))
                self.assertEqual([child.tag for child in root], ["type", "listen", "policy"])
                self.assertEqual(root.findtext("type"), "session")
                self.assertRegex(root.findtext("listen") or "", r"^unix:tmpdir=")
                output = f"unix:path=/private/bus-{len(started)}\n"
            server = mock.Mock(stdout=io.StringIO(output), wait=mock.Mock(return_value=0))
            started.append(server)
            return server

        with mock.patch.dict(
            os.environ,
            {
                "DBUS_SESSION_BUS_ADDRESS": "unix:path=/host/bus",
                "AOBUS_OWNED_GTK_BUS": "unix:path=/inherited-marker",
                "G_DEBUG": "gc-friendly",
            },
        ):
            with mock.patch.object(test_command.subprocess, "Popen", side_effect=spawn):
                with contextlib.redirect_stdout(io.StringIO()):
                    with test_command.virtual_gtk_displays(2) as environments:
                        self.assertEqual([env["DISPLAY"] for env in environments], [":40", ":41"])
                        self.assertEqual(
                            [env["DBUS_SESSION_BUS_ADDRESS"] for env in environments],
                            ["unix:path=/private/bus-2", "unix:path=/private/bus-3"],
                        )
                        self.assertEqual(os.environ["DBUS_SESSION_BUS_ADDRESS"], "unix:path=/host/bus")
                        self.assertTrue(
                            all(env["AOBUS_OWNED_GTK_BUS"] == env["DBUS_SESSION_BUS_ADDRESS"] for env in environments)
                        )
                        self.assertEqual(os.environ["AOBUS_OWNED_GTK_BUS"], "unix:path=/inherited-marker")
                        self.assertTrue(all(env["G_DEBUG"] == "gc-friendly,fatal-warnings" for env in environments))
                        self.assertEqual(os.environ["G_DEBUG"], "gc-friendly")
        self.assertEqual(len(started), 4)
        for server in started:
            server.terminate.assert_called_once()
            server.wait.assert_called_once_with(timeout=5)
            self.assertTrue(server.stdout.closed)
        self.assertTrue(configs)
        self.assertTrue(all(not config.exists() for config in configs))

    @unittest.skipUnless(sys.platform == "linux", "GTK Unix socket path limit")
    def test_gtk_bus_socket_directory_does_not_inherit_a_long_tmpdir(self):
        with tempfile.TemporaryDirectory() as parent:
            inherited = Path(parent) / ("nested-nix-shell-" + "x" * 100)
            inherited.mkdir()
            display = mock.Mock(stdout=io.StringIO("40\n"), wait=mock.Mock(return_value=0))
            bus = mock.Mock(stdout=io.StringIO("unix:path=/private/bus\n"), wait=mock.Mock(return_value=0))

            def start_bus(config):
                self.assertEqual(config.parent.parent, Path("/tmp"))
                root = ElementTree.fromstring(config.read_text(encoding="utf-8"))
                self.assertEqual(root.findtext("listen"), f"unix:tmpdir={config.parent.as_posix()}")
                return bus

            with (
                mock.patch.object(tempfile, "tempdir", str(inherited)),
                mock.patch.dict(os.environ, {"TMPDIR": str(inherited)}),
                mock.patch.object(test_command, "_start_xvfb", return_value=display),
                mock.patch.object(test_command, "_start_gtk_bus", side_effect=start_bus),
                contextlib.redirect_stdout(io.StringIO()),
            ):
                with test_command.virtual_gtk_displays():
                    self.assertEqual(os.environ["TMPDIR"], str(inherited))
                    self.assertEqual(tempfile.gettempdir(), str(inherited))
            bus.terminate.assert_called_once()
            bus.wait.assert_called_once_with(timeout=5)

    def test_failed_private_bus_start_retires_displays_and_prior_buses(self):
        for failure in ("missing-executable", "invalid-address"):
            with self.subTest(failure=failure):
                started = []

                def spawn(argv, *, started=started, failure=failure, **kwargs):
                    if len(started) == 3 and failure == "missing-executable":
                        raise FileNotFoundError("dbus-daemon")
                    output = f"{40 + len(started)}\n" if argv[0] == "Xvfb" else "unix:path=/private/bus\n"
                    if len(started) == 3:
                        output = "failed to bind the session socket\n"
                    server = mock.Mock(stdout=io.StringIO(output), wait=mock.Mock(return_value=0))
                    started.append(server)
                    return server

                with mock.patch.object(test_command.subprocess, "Popen", side_effect=spawn):
                    with contextlib.redirect_stdout(io.StringIO()), self.assertRaises(SystemExit):
                        with test_command.virtual_gtk_displays(2):
                            self.fail("an incomplete session must not run tests")
                self.assertEqual(len(started), 3 if failure == "missing-executable" else 4)
                for server in started:
                    server.terminate.assert_called_once()
                    server.wait.assert_called_once_with(timeout=5)
                    self.assertTrue(server.stdout.closed)

    def test_interrupted_gtk_readiness_retires_the_writer_before_closing_and_joining(self):
        released = threading.Event()
        finished = threading.Event()

        def read_until_killed():
            released.wait()
            finished.set()
            return ""

        def close():
            killed = released.is_set()
            # Keep the test's own reader join safe even if the implementation
            # regresses to closing the pipe before retiring its writer.
            released.set()
            self.assertTrue(killed, "close must not wait on a still-blocked reader")

        daemon = mock.Mock(
            stdout=mock.Mock(readline=mock.Mock(side_effect=read_until_killed), close=mock.Mock(side_effect=close)),
            kill=mock.Mock(side_effect=released.set),
            wait=mock.Mock(return_value=0),
        )
        with mock.patch.object(Future, "result", side_effect=KeyboardInterrupt), self.assertRaises(KeyboardInterrupt):
            test_command._gtk_daemon_ready(daemon, "interrupted daemon")
        daemon.kill.assert_called_once()
        daemon.wait.assert_called_once_with()
        daemon.stdout.close.assert_called_once()
        self.assertTrue(finished.is_set(), "the reader must be joined on interruption")

    def test_gtk_session_body_failure_still_forces_a_stalled_bus_to_exit(self):
        display = mock.Mock(stdout=io.StringIO("40\n"), wait=mock.Mock(return_value=0))
        bus = mock.Mock(
            stdout=io.StringIO("unix:path=/private/bus\n"),
            wait=mock.Mock(side_effect=[subprocess.TimeoutExpired("dbus-daemon", 5), 0]),
        )
        with mock.patch.object(test_command.subprocess, "Popen", side_effect=[display, bus]):
            with contextlib.redirect_stdout(io.StringIO()), self.assertRaisesRegex(RuntimeError, "body failed"):
                with test_command.virtual_gtk_displays():
                    raise RuntimeError("body failed")
        display.terminate.assert_called_once()
        bus.terminate.assert_called_once()
        bus.kill.assert_called_once()
        self.assertEqual(bus.wait.call_args_list, [mock.call(timeout=5), mock.call()])
        self.assertTrue(display.stdout.closed)
        self.assertTrue(bus.stdout.closed)

    def test_a_failed_shard_launch_closes_the_outputs_already_opened(self):
        opened = []
        real_open = Path.open

        def tracking_open(self, *args, **kwargs):
            stream = real_open(self, *args, **kwargs)
            opened.append(stream)
            return stream

        def spawn(argv, *, cwd=None, env=None, stdout=None, stderr=None):
            if "--shard-index 1" in " ".join(argv) or argv[argv.index("--shard-index") + 1] == "1":
                raise OSError("cannot start the second shard")
            return mock.Mock(returncode=0, wait=mock.Mock(return_value=0))

        with mock.patch.object(Path, "open", tracking_open):
            with self.assertRaises(OSError) as caught:
                self._run_sharded_core(spawn, shards=2)

        # The launch failure must survive: an output left open would fail this
        # TemporaryDirectory's cleanup on Windows and replace it with WinError 32.
        self.assertIn("cannot start the second shard", str(caught.exception))
        self.assertTrue(opened)
        self.assertTrue(all(stream.closed for stream in opened), opened)

    def test_every_gtk_shard_gets_a_display_of_its_own(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            build_dir = Path(temp_dir)
            binary = _fixture_binary(build_dir, "ao_gtk_test", platform="linux")
            binary.parent.mkdir(parents=True)
            binary.touch()

            displays = iter(("40\n", "41\n", "42\n", "43\n"))
            servers = []
            buses = []

            def spawn(argv, **kwargs):
                if argv[0] == "Xvfb":
                    server = mock.Mock(stdout=io.StringIO(next(displays)), wait=mock.Mock(return_value=0))
                    servers.append(server)
                    return server
                if argv[0] == "dbus-daemon":
                    bus = mock.Mock(
                        stdout=io.StringIO(f"unix:path=/private-bus-{len(buses)}\n"),
                        wait=mock.Mock(return_value=0),
                    )
                    buses.append(bus)
                    return bus
                return self._catch2_shard_stub([(1, 1, 0)] * 4)(argv, **kwargs)

            with mock.patch.object(builddir, "platform_profile", return_value=builddir.LINUX_PROFILE):
                with mock.patch.object(test_command.subprocess, "Popen", side_effect=spawn) as popen:
                    with self._captured_stdout():
                        self.assertEqual(test_command.run_catch2_suite("gtk", build_dir, shards=4).status, 0)

        shard_environments = [call.kwargs["env"] for call in popen.call_args_list if call.args[0][0] == str(binary)]
        self.assertEqual([env["DISPLAY"] for env in shard_environments], [":40", ":41", ":42", ":43"])
        self.assertEqual(
            [env["DBUS_SESSION_BUS_ADDRESS"] for env in shard_environments],
            [f"unix:path=/private-bus-{index}" for index in range(4)],
        )
        self.assertTrue(
            all(env["AOBUS_OWNED_GTK_BUS"] == env["DBUS_SESSION_BUS_ADDRESS"] for env in shard_environments)
        )
        for server in (*servers, *buses):
            server.terminate.assert_called_once()
            server.wait.assert_called_once_with(timeout=5)
            self.assertTrue(server.stdout.closed)

    def test_test_parser_preserves_escaped_literal_commas(self):
        test_filter = r"FieldCatalog - finds descriptors by canonical name\, alias\, and field"
        args = self.parse(["test", "--core", "--list", test_filter])
        self.assertEqual(args.filter, test_filter)


if __name__ == "__main__":
    unittest.main()
