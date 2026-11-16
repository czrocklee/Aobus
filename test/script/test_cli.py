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
import unittest
from pathlib import Path
from unittest import mock

from ao.__main__ import main, make_parser, parse_arguments
from ao.command import analyze as analyze_command
from ao.command import coverage as coverage_command
from ao.command import perf as perf_command
from ao.command import tidy as tidy_command
from ao.core import bazel_coverage, bazel_lint_action, builddir, buildenv, doccheck
from ao.core.bazel_lint_reports import Identity

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
        for command in ("format", "tidy"):
            self.assertTrue(buildenv.requires_python_tools(command))
        # Hygiene follows its source scope; an explicit file keeps this checkout's Git diff out of the result.
        self.assertTrue(buildenv.requires_python_tools("hygiene", ["script/ao/core/proc.py"]))
        self.assertTrue(buildenv.requires_python_tools("docs", ["check"]))
        self.assertTrue(buildenv.requires_python_tools("check"))
        for command, arguments in (
            ("deps", ["report"]),
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


class CliParseTest(unittest.TestCase):
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


if __name__ == "__main__":
    unittest.main()
