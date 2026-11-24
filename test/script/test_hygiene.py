"""Source-operation contracts formerly exposed by the combined hygiene route."""

import contextlib
import io
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao.policy import compiler_policy, windows_llvm_sdk
from ao.portal import (
    bazel_check_tools,
    build_dir,
    buildifier,
    compiler_rc,
    prettier,
    source_format,
    source_lint,
    source_scope,
)
from ao.portal.source_lint import LintRequest
from ao.portal.source_scope import SourceScope


class FormatServiceTest(unittest.TestCase):
    def test_cpp_python_and_bazel_sources_reach_independent_ready_only_formatters(self):
        scope = SourceScope(("lib/Foo.cpp", "script/foo.py", "BUILD"))
        for check in (False, True):
            with (
                self.subTest(check=check),
                mock.patch.object(source_scope, "require_files"),
                mock.patch.object(source_format, "run_clang_format", return_value=0) as clang,
                mock.patch.object(source_format, "run_ruff_format", return_value=0) as ruff,
                mock.patch.object(buildifier, "run", return_value=0) as bazel,
                contextlib.redirect_stdout(io.StringIO()),
            ):
                run = source_format.check if check else source_format.rewrite
                self.assertEqual(run(scope), 0)
            clang.assert_called_once_with(["lib/Foo.cpp"], check=check, clang_format=None)
            ruff.assert_called_once_with(["script/foo.py"], check=check, python=None)
            bazel.assert_called_once_with(["BUILD"], check=check, provision=False)

    def test_msvc_projection_check_never_rewrites_and_rewrite_preserves_unrelated_options(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            policy = root / "bazel/msvc_policy.bzl"
            policy.parent.mkdir()
            policy.write_text(
                'MSVC_REPOSITORY_ENV = {"BAZEL_CXXOPTS": ["/std:c++latest", "/utf-8"]}\n', encoding="utf-8"
            )
            path = root / ".bazelrc"
            original = (
                "build --compilation_mode=dbg\n"
                "# BEGIN GENERATED MSVC FLAGS: bazel/msvc_policy.bzl\n"
                "build:windows --repo_env=BAZEL_CXXOPTS=/std%:c++17\n"
                "# END GENERATED MSVC FLAGS\n"
                "test --test_output=errors\n"
            )
            path.write_text(original, encoding="utf-8")
            with (
                mock.patch.object(source_scope, "PROJECT_ROOT", root),
                mock.patch.object(compiler_policy, "PROJECT_ROOT", root),
                mock.patch.object(compiler_rc, "PROJECT_ROOT", root),
                contextlib.redirect_stdout(io.StringIO()),
                contextlib.redirect_stderr(io.StringIO()),
            ):
                scope = SourceScope((".bazelrc",))
                self.assertEqual(source_format.check(scope), 1)
                self.assertEqual(path.read_text(encoding="utf-8"), original)
                self.assertEqual(source_format.rewrite(scope), 0)
                self.assertEqual(
                    path.read_text(encoding="utf-8"), original.replace("/std%:c++17", "/std%:c++latest:/utf-8")
                )
                self.assertEqual(source_format.check(scope), 0)
                with path.open("a", encoding="utf-8") as stream:
                    stream.write("build:windows --repo_env BAZEL_CXXOPTS=/std%:c++17\n")
                with self.assertRaises(ValueError):
                    compiler_rc.format_msvc_rc(check=True)

    def test_windows_clang_format_uses_the_ready_pinned_sdk_or_an_explicit_tool(self):
        with tempfile.TemporaryDirectory() as temporary:
            sdk = Path(temporary)
            tool = sdk / "bin/clang-format.exe"
            tool.parent.mkdir()
            tool.touch(mode=0o755)
            self.check_windows_clang_format(sdk)

    def check_windows_clang_format(self, sdk):
        for explicit in (None, "explicit-clang-format"):
            with (
                self.subTest(explicit=explicit),
                mock.patch.object(build_dir, "platform_profile", return_value=build_dir.WINDOWS_PROFILE),
                mock.patch.object(source_format.windows_llvm_sdk, "read_manifest"),
                mock.patch.object(source_format.windows_llvm_sdk, "ready_root", return_value=(sdk, True)) as ready,
                mock.patch.object(source_format.subprocess, "run", return_value=mock.Mock(returncode=0)) as process,
                contextlib.redirect_stdout(io.StringIO()),
            ):
                self.assertEqual(
                    source_format.run_clang_format(
                        ["lib/Foo.cpp", "include/Inventory.def"], check=True, clang_format=explicit
                    ),
                    0,
                )
            process.assert_called_once_with(
                [
                    explicit or str(sdk / "bin/clang-format.exe"),
                    "--dry-run",
                    "-Werror",
                    "lib/Foo.cpp",
                    "include/Inventory.def",
                ],
                cwd=source_format.PROJECT_ROOT,
            )
            self.assertEqual(ready.call_count, int(explicit is None))

    @unittest.skipUnless(sys.platform == "linux", "Nix path admission runs only on Linux")
    def test_linux_clang_format_rejects_unadmitted_tool_paths(self):
        for selected, resolved, executable in (
            ("clang-format", "/nix/store/fixture/bin/clang-format", True),
            ("/tmp/clang-format", "/nix/store/fixture/bin/clang-format", True),
            ("/nix/store/fixture/bin/clang-format", "/tmp/clang-format", True),
            ("/nix/store/fixture/bin/clang-format", "/nix/store/fixture/bin/clang-format", False),
        ):
            with (
                self.subTest(selected=selected, resolved=resolved, executable=executable),
                mock.patch.object(build_dir, "platform_profile", return_value=build_dir.LINUX_PROFILE),
                mock.patch.dict(os.environ, {"AO_NIX_CLANG_FORMAT": selected}),
                mock.patch.object(Path, "resolve", return_value=Path(resolved)),
                mock.patch.object(Path, "is_file", return_value=True),
                mock.patch.object(os, "access", return_value=executable),
                self.assertRaises(ValueError),
            ):
                source_format.clang_format_executable()

    def test_macos_clang_format_uses_selected_llvm_and_never_path_fallback(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            tool = root / "bin/clang-format"
            tool.parent.mkdir()
            tool.touch(mode=0o755)
            with (
                mock.patch.object(build_dir, "platform_profile", return_value=build_dir.MACOS_PROFILE),
                mock.patch.dict(source_format.os.environ, {"AOBUS_LLVM_ROOT": str(root), "PATH": str(root / "other")}),
            ):
                facts = bazel_check_tools.resolve("clang-format")
                self.assertEqual(facts["selection"], {"name": "clang-format", "command": (str(tool),)})
                self.assertIn(str(root), facts["watch_files"])
                self.assertIn(str(tool), facts["watch_files"])
                self.assertEqual(facts["directories"], [])
                self.assertEqual(facts["trees"], [])
                tool.unlink()
                with self.assertRaises(ValueError):
                    source_format.run_clang_format(["lib/Foo.cpp"], check=True)

    def test_windows_clang_format_tracks_required_sdk_files_without_projecting_them(self):
        manifest = windows_llvm_sdk.Manifest("22.1.8", "b" * 64, ("bin/clang-format.exe", "include/required.h"))
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary)
            for cached in (False, True):
                explicit = cache / "explicit"
                environment = {"AOBUS_LLVM_SDK_ROOT": "" if cached else str(explicit)}
                environment["AOBUS_LLVM_SDK_CACHE_ROOT"] = str(cache)
                sdk = windows_llvm_sdk.cache_root(environment, manifest) if cached else explicit
                formatter, header = (sdk / relative for relative in manifest.required_files)
                for path in (formatter, header):
                    path.parent.mkdir(parents=True)
                    path.touch(mode=0o755)
                marker = sdk / windows_llvm_sdk.MARKER
                if cached:
                    marker.write_text(windows_llvm_sdk.marker_text(manifest), encoding="utf-8")
                with (
                    self.subTest(cached=cached),
                    mock.patch.object(build_dir, "platform_profile", return_value=build_dir.WINDOWS_PROFILE),
                    mock.patch.object(windows_llvm_sdk, "read_manifest", return_value=manifest),
                    mock.patch.dict(os.environ, environment),
                ):
                    facts = bazel_check_tools.resolve("clang-format")
                    self.assertIn(str(header), facts["watch_files"])
                    self.assertEqual(str(marker) in facts["watch_files"], cached)
                    self.assertEqual(facts["directories"], [])
                    self.assertEqual(facts["trees"], [])
                    header.unlink()
                    if cached:
                        missing = bazel_check_tools.resolve("clang-format")
                        self.assertIn("unavailable", missing["selection"])
                        self.assertIn(str(header), missing["watch_files"])
                    else:
                        with self.assertRaises(ValueError):
                            bazel_check_tools.resolve("clang-format")

    def test_python_only_format_does_not_select_llvm(self):
        with (
            mock.patch.object(source_scope, "require_files"),
            mock.patch.object(source_format, "clang_format_executable") as clang,
            mock.patch.object(source_format, "run_ruff_format", return_value=0),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            self.assertEqual(source_format.check(SourceScope(("script/foo.py",))), 0)
        clang.assert_not_called()

    def test_managed_prettier_tracks_shallow_cache_and_recursive_package(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            node = root / "node.exe"
            entry = root / "package/bin/prettier.cjs"
            plugin = root / "package/plugins/example.mjs"
            for path in (node, entry, plugin):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.touch()
            with mock.patch.object(prettier, "command", return_value=(str(node), str(entry))) as command:
                facts = bazel_check_tools.resolve("prettier")
            command.assert_called_once_with(provision=False)
            self.assertEqual(facts["selection"], {"name": "prettier", "command": (str(node), str(entry))})
            self.assertIn(str(node), facts["watch_files"])
            self.assertIn(str(prettier.MANIFEST_PATH), facts["watch_files"])
            self.assertIn(str(root / ".aobus-prettier-receipt.json"), facts["watch_files"])
            self.assertEqual(facts["directories"], [str(root)])
            self.assertEqual(facts["trees"], [str(root / "package")])

    def test_tool_resolver_uses_checkout_relative_state_but_writes_to_the_external_repo(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            checkout = root / "checkout"
            external = root / "external"
            checkout.mkdir()
            external.mkdir()
            with (
                contextlib.chdir(external),
                mock.patch.object(bazel_check_tools, "_ROOT", checkout),
                mock.patch.object(sys, "argv", ["resolver", "buildifier", "tool.json"]),
                mock.patch.object(sys, "path", sys.path.copy()),
                mock.patch.dict(os.environ, {"AOBUS_STATE_ROOT": "relative-state"}),
                mock.patch.object(
                    bazel_check_tools,
                    "resolve",
                    side_effect=lambda _: {"state": str(build_dir.state_root().absolute())},
                ),
            ):
                self.assertEqual(bazel_check_tools.main(), 0)
            facts = json.loads((external / "tool.json").read_text(encoding="utf-8"))
            self.assertEqual(facts, {"state": str(checkout / "relative-state")})

    def test_ruff_formatter_uses_the_declared_binary_instead_of_the_portal_interpreter(self):
        tool = Path("declared-ruff")
        with (
            mock.patch.object(source_format.python_check, "ruff_command", side_effect=AssertionError("ambient Ruff")),
            mock.patch.object(source_format.python_check, "tool_versions", return_value={"python": "3.12.8"}),
            mock.patch.object(source_format.subprocess, "run", return_value=mock.Mock(returncode=0)) as process,
            contextlib.redirect_stdout(io.StringIO()),
        ):
            self.assertEqual(source_format.run_ruff_format(["script/file.py"], check=True, tool=tool), 0)
        process.assert_called_once_with(
            (str(tool), "format", "--target-version", "py312", "--check", "script/file.py"),
            cwd=source_format.PROJECT_ROOT,
        )

    def test_ruff_formatter_preserves_check_mode_and_tool_failure(self):
        for check, code, arguments, expected in (
            (False, 0, ["format", "script/file.py"], 0),
            (True, 2, ["format", "--check", "script/file.py"], 1),
        ):
            with (
                self.subTest(check=check),
                mock.patch.object(
                    source_format.python_check, "ruff_command", return_value=("python", "-m", "ruff")
                ) as command,
                mock.patch.object(source_format.subprocess, "run", return_value=mock.Mock(returncode=code)),
                contextlib.redirect_stdout(io.StringIO()),
            ):
                self.assertEqual(
                    source_format.run_ruff_format(["script/file.py"], check=check, python=Path("managed")), expected
                )
            command.assert_called_once_with(*arguments, python=Path("managed"))


class SourceLintTest(unittest.TestCase):
    def setUp(self):
        self.root = Path(self.enterContext(tempfile.TemporaryDirectory()))
        for module in (source_lint, source_scope):
            self.enterContext(mock.patch.object(module, "PROJECT_ROOT", self.root))
        self.enterContext(contextlib.redirect_stdout(io.StringIO()))
        self.enterContext(contextlib.redirect_stderr(io.StringIO()))
        self.native = self.enterContext(mock.patch.object(source_lint.bazel_lint, "run", return_value=0))
        self.tests = self.enterContext(mock.patch.object(source_lint, "audit_tests", return_value=0))
        self.names = self.enterContext(mock.patch.object(source_lint, "audit_names", return_value=0))
        self.python = self.enterContext(mock.patch.object(source_lint.python_check, "run_paths", return_value=0))
        self.bazel = self.enterContext(mock.patch.object(buildifier, "lint_paths", return_value=0))
        self.workflows = self.enterContext(mock.patch.object(source_lint, "check_workflows", return_value=0))

    def scope(self, *files, explicit=()):
        for name in files:
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.touch()
        return SourceScope(files, explicit)

    def test_mixed_scope_appends_all_diagnostics_after_native_report(self):
        scope = self.scope("lib/Example.cpp", "script/example.py", "BUILD.bazel")
        report = self.root / "diagnostics.txt"

        def native(request):
            self.assertEqual(request._resolved_sources, ("lib/Example.cpp",))
            self.assertIsInstance(request, LintRequest)
            Path(request.output).write_text("native diagnostic\n", encoding="utf-8")
            return 1

        def python(paths, *, python, sink):
            self.assertEqual(paths, ["script/example.py"])
            sink.write("Python diagnostic\n")
            return 1

        def bazel(paths, *, sink, provision):
            self.assertEqual(paths, ["BUILD.bazel"])
            self.assertFalse(provision)
            sink.write("Buildifier diagnostic\n")
            return 1

        self.native.side_effect = native
        self.python.side_effect = python
        self.bazel.side_effect = bazel
        self.assertEqual(source_lint.run(scope, LintRequest(output=str(report))), 1)
        self.assertEqual(
            report.read_text(encoding="utf-8"), "native diagnostic\nPython diagnostic\nBuildifier diagnostic\n"
        )

    def test_named_native_check_skips_all_unrelated_languages_audits_and_formatters(self):
        scope = self.scope("lib/Example.cpp", "script/example.py", "BUILD.bazel")
        with mock.patch.object(source_format, "check", side_effect=AssertionError("no formatter")):
            self.assertEqual(source_lint.run(scope, LintRequest(tidy_check="aobus-no-c-assert")), 0)
        self.native.assert_called_once()
        for stage in (self.tests, self.names, self.python, self.bazel, self.workflows):
            stage.assert_not_called()

    def test_cpp_test_and_objective_cpp_support_files_reach_audits_without_production_files(self):
        scope = self.scope("test/audio/FooTest.cpp", "test/audio/FooSupport.h", "test/media/FooTest.mm", "lib/Foo.h")
        self.assertEqual(source_lint.run(scope, LintRequest()), 0)
        self.tests.assert_called_once_with(list(scope.files[:3]), report_only=False, sink=None)
        self.names.assert_called_once_with(scope.files, report_only=False, sink=None)

    def test_mixed_file_and_directory_provenance_does_not_promote_scanned_fixtures(self):
        explicit = "test/lint/fixture/aobus-no-c-assert/Explicit.cpp"
        scanned = "test/lint/fixture/aobus-no-c-assert/Scanned.cpp"
        scope = self.scope("lib/Foo.cpp", explicit, scanned, explicit=(explicit,))
        self.assertEqual(source_lint.run(scope, LintRequest()), 0)
        request = self.native.call_args.args[0]
        self.assertEqual(request._resolved_sources, ("lib/Foo.cpp", explicit))
        self.assertEqual(request.files, (explicit,))
        self.tests.assert_not_called()

    def test_empty_scope_and_missing_prepared_path_do_not_expand_or_prepare_native_context(self):
        self.assertEqual(source_lint.run(SourceScope(), LintRequest()), 0)
        with self.assertRaisesRegex(ValueError, "does not exist"):
            source_lint.run(SourceScope(("lib/Missing.cpp",)), LintRequest())
        for stage in (self.native, self.tests, self.names, self.python, self.bazel, self.workflows):
            stage.assert_not_called()

    def test_report_only_suppresses_findings_but_preserves_python_and_buildifier_tool_errors(self):
        scope = self.scope("script/example.py", "BUILD.bazel")
        self.python.return_value = self.bazel.return_value = 1
        self.assertEqual(source_lint.run(scope, LintRequest(report_only=True)), 0)
        self.python.return_value = 2
        self.assertEqual(source_lint.run(scope, LintRequest(report_only=True)), 2)
        self.python.return_value = 1
        self.bazel.side_effect = buildifier.BuildifierError("broken tool protocol")
        self.assertEqual(source_lint.run(scope, LintRequest(report_only=True)), 2)

    def test_audit_failures_propagate_and_report_only_is_explicit(self):
        scope = self.scope("test/audio/FooTest.cpp")
        for stage in (self.tests, self.names):
            stage.return_value = 1
            self.assertEqual(source_lint.run(scope, LintRequest()), 1)
            stage.return_value = 0
        self.assertEqual(source_lint.run(scope, LintRequest(report_only=True)), 0)
        self.tests.assert_called_with(["test/audio/FooTest.cpp"], report_only=True, sink=None)
        self.names.assert_called_with(("test/audio/FooTest.cpp",), report_only=True, sink=None)

    def test_explicit_owner_request_uses_native_engine_without_source_discovery(self):
        request = LintRequest(owner_targets=("//lib:audio",), config="debug", jobs=3)
        with mock.patch.object(source_scope, "resolve", side_effect=AssertionError("no discovery")):
            self.assertEqual(source_lint.run(None, request), 0)
        selected = self.native.call_args.args[0]
        self.assertEqual((selected.owner_targets, selected.config, selected.jobs), (("//lib:audio",), "debug", 3))


class SourceAuditTest(unittest.TestCase):
    def test_audit_findings_are_gated_by_default_and_remain_visible_in_report_only(self):
        for engine, run in (
            (source_lint.test_audit, source_lint.audit_tests),
            (source_lint.name_audit, source_lint.audit_names),
        ):
            issue = engine.Issue(Path("test/audio/FooSupport.cpp"), 4, "fixture-finding", "invalid source convention")
            for report_only in (False, True):
                with (
                    self.subTest(engine=engine.__name__, report_only=report_only),
                    mock.patch.object(engine, "audit_paths", return_value=[issue]) as audit,
                    contextlib.redirect_stdout(io.StringIO()),
                    contextlib.redirect_stderr(io.StringIO()) as stderr,
                ):
                    sink = io.StringIO()
                    self.assertEqual(
                        run(("test/audio/FooSupport.cpp",), report_only=report_only, sink=sink), int(not report_only)
                    )
                audit.assert_called_once_with(["test/audio/FooSupport.cpp"])
                self.assertIn("fixture-finding: invalid source convention", sink.getvalue())
                self.assertEqual(stderr.getvalue(), "")


if __name__ == "__main__":
    unittest.main()
