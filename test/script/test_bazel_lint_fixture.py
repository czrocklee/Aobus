"""Direct fixture commands, real marker preflight, and cached-result transport."""

import argparse
import contextlib
import io
import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao.core import bazel_lint_action, bazel_lint_fixture, linttest, tidyconfig, tidyengine


class DirectFixtureToolsTest(unittest.TestCase):
    def test_fixed_database_has_posix_language_flags_before_its_implicit_input(self):
        tools = linttest.FixtureTools("verified-host", ("-isystem/native",), windows=False)
        for suffix, language in ((".h", "c++"), (".cpp", "c++"), (".mm", "objective-c++")):
            with self.subTest(suffix=suffix):
                fixture = linttest.Fixture(Path("Fixture" + suffix), "aobus-example")
                command = linttest._tidy_command(fixture, Path("unused-build"), tools=tools)
                separator = command.index("--")
                self.assertEqual(command[separator - 1], str(fixture.path))
                # The fixed database puts this compiler argv fragment BEFORE
                # the input; an --extra-arg=-x would be placed AFTER it.
                self.assertEqual(command[separator + 1 :], ["-isystem/native", "-x", language])
        windows = linttest.FixtureTools("verified-host", ("--driver-mode=cl", "/TP"), windows=True)
        command = linttest._tidy_command(
            linttest.Fixture(Path("Fixture.h"), "aobus-example"), Path("unused-build"), tools=windows
        )
        self.assertEqual(command[command.index("--") + 1 :], list(windows.flags))

    def test_posix_direct_diagnostic_uses_plugin_language_and_included_header_assertions(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            main = root / "Main.mm"
            header = root / "Context.h"
            main.write_text('#import "Context.h"\nint good; // NEGATIVE\n', encoding="utf-8")
            header.write_text("int bad; // POSITIVE | required explanation\n", encoding="utf-8")
            tools = linttest.FixtureTools("verified-tidy", ("-isystem/sdk",), windows=False, plugin=root / "plugin.so")
            fixture = linttest.Fixture(main, "aobus-example")
            before = {path: path.read_bytes() for path in (main, header)}

            def invoke(command, **kwargs):
                self.assertEqual(command[0], tools.tidy)
                self.assertEqual(kwargs["cwd"], linttest.PROJECT_ROOT)
                self.assertEqual(command[command.index("--") - 1], str(main))
                self.assertIn(f"-load={tools.plugin}", command)
                self.assertEqual(command[command.index("--") + 1 :], [*tools.flags, "-x", "objective-c++"])
                self.assertIn("--extra-arg=objective-c++", command)
                self.assertIn("--extra-arg=-fblocks", command)
                self.assertIn("--extra-arg=-Wno-unused-variable", command)
                self.assertNotIn("--extra-arg=/W0", command)
                self.assertNotIn("--no-build", command)
                self.assertIn(
                    f"-config={tidyconfig.config_for(tidyconfig.checks_for('STRICT', fixture.check))}", command
                )
                return subprocess.CompletedProcess(
                    command, 0, stdout=f"{header}:1:1: warning: required explanation [aobus-example]\n"
                )

            with mock.patch.object(linttest.subprocess, "run", side_effect=invoke):
                success, log = linttest._run_diagnostic(fixture, root, root, tools=tools)
            self.assertTrue(success, log.read_text(encoding="utf-8"))
            self.assertEqual({path: path.read_bytes() for path in before}, before)

            with mock.patch.object(linttest.subprocess, "run", return_value=subprocess.CompletedProcess([], 0, "")):
                success, log = linttest._run_diagnostic(fixture, root, root, tools=tools)
            self.assertFalse(success)
            self.assertIn(f"missing expected diagnostic on line {header.resolve()}:1", log.read_text(encoding="utf-8"))

    def test_direct_preflight_validates_deferred_mm_and_its_local_headers_before_any_tool(self):
        discover = linttest.discover_fixtures
        for windows in (False, True):
            with self.subTest(windows=windows), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                check = root / "fixture" / "aobus-example"
                check.mkdir(parents=True)
                (check / "Good.cpp").write_text("int good; // NEGATIVE\n", encoding="utf-8")
                main = check / "Main.mm"
                main.write_text('#include "nested/Context.h"\nint bad; // POSITIVE |\n', encoding="utf-8")
                header = check / "nested" / "Context.h"
                header.parent.mkdir()
                header.write_text("// POSITIVE\nint bad; // NEGATIVE\n", encoding="utf-8")
                tools = linttest.FixtureTools(
                    "verified-host", (), windows=windows, plugin=None if windows else root / "plugin"
                )
                with (
                    mock.patch.object(
                        linttest, "discover_fixtures", side_effect=lambda root=root: discover(root / "fixture")
                    ),
                    mock.patch.object(linttest.subprocess, "run") as invoke,
                    mock.patch.object(
                        linttest.builddir, "platform_profile", side_effect=AssertionError("ambient platform")
                    ),
                    contextlib.redirect_stdout(io.StringIO()) as output,
                ):
                    self.assertEqual(linttest.run(root / "no-cmake", tools=tools, work=root / "work", jobs=1), 1)
                invoke.assert_not_called()
                self.assertIn(f"{main.resolve()}:2: empty explicit diagnostic text", output.getvalue())
                self.assertIn(f"{header.resolve()}:2: conflicting POSITIVE and NEGATIVE", output.getvalue())
                self.assertNotIn("[DEFER]", output.getvalue())
                self.assertFalse((root / "work").exists())

    def test_only_windows_defers_mm_and_direct_work_is_contained_without_a_compile_database(self):
        discover = linttest.discover_fixtures
        for windows in (False, True):
            with self.subTest(windows=windows), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                check = root / "fixture" / "aobus-example"
                check.mkdir(parents=True)
                (check / "First.mm").write_text("int good; // NEGATIVE\n", encoding="utf-8")
                (check / "Second.cpp").write_text("int good; // NEGATIVE\n", encoding="utf-8")
                work = root / "work"
                work.mkdir()
                prior = work / "prior-run.txt"
                prior.write_text("keep", encoding="utf-8")
                tools = linttest.FixtureTools(
                    "verified-host", (), windows=windows, plugin=None if windows else root / "plugin"
                )

                def stop_at_native_boundary(command, tools=tools, windows=windows, **_kwargs):
                    self.assertEqual(command[0], tools.tidy)
                    main = Path(command[command.index("--") - 1])
                    self.assertIn(main.name, ("Second.cpp",) if windows else ("First.mm", "Second.cpp"))
                    raise OSError("intentional native boundary stop")

                with (
                    mock.patch.object(
                        linttest, "discover_fixtures", side_effect=lambda root=root: discover(root / "fixture")
                    ),
                    mock.patch.object(linttest.subprocess, "run", side_effect=stop_at_native_boundary),
                    contextlib.redirect_stdout(io.StringIO()) as output,
                    self.assertRaisesRegex(OSError, "intentional native boundary stop"),
                ):
                    linttest.run(root / "no-cmake", tools=tools, work=work, jobs=1)
                self.assertEqual("[DEFER] aobus-example/First.mm" in output.getvalue(), windows)
                self.assertEqual(prior.read_text(encoding="utf-8"), "keep")
                # Caller-owned action outputs contain stages directly; another
                # suite prefix needlessly consumes the Windows path budget.
                self.assertTrue(list(work.glob("*.diag.*")))
                self.assertFalse(list(work.glob("aobus-lint-integration-*")))

    def test_compiler_error_canaries_use_gnu_or_msvc_fixture_warning_flags(self):
        for windows in (False, True):
            with self.subTest(windows=windows), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                tools = linttest.FixtureTools(
                    "verified-host", (), windows=windows, plugin=None if windows else root / "plugin"
                )
                seen = set()

                def invoke(command, tools=tools, windows=windows, seen=seen, **_kwargs):
                    self.assertEqual(command[0], tools.tidy)
                    path = Path(command[command.index("--") - 1])
                    seen.add(path.stem)
                    self.assertEqual("--extra-arg=/W0" in command, windows)
                    self.assertEqual("--extra-arg=/WX-" in command, windows)
                    self.assertEqual("--extra-arg=-Wno-unused-function" in command, not windows)
                    self.assertEqual("--extra-arg=-Wno-unused-parameter" in command, not windows)
                    if path.stem == "unused-code":
                        self.assertIn("unusedVariable", path.read_text(encoding="utf-8"))
                        return subprocess.CompletedProcess(command, 0, "")
                    self.assertIn(path.stem, {"missing-include", "syntax-error", "unknown-type"})
                    return subprocess.CompletedProcess(
                        command, 1, f"{path}:1:1: error: rejected canary [clang-diagnostic-error]\n"
                    )

                with mock.patch.object(linttest.subprocess, "run", side_effect=invoke):
                    success, log = linttest._run_compiler_error_smoke(root, root, tools=tools)
                self.assertTrue(success, log.read_text(encoding="utf-8"))
                self.assertEqual(seen, {"missing-include", "syntax-error", "unknown-type", "unused-code"})

    def test_posix_source_policy_materializes_real_sources_with_direct_sdk_flags(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            tools = linttest.FixtureTools(
                "verified-host", ("-resource-dir=/sdk/resource", "-isystem/sdk/include"), False, root / "plugin"
            )

            def stop_after_generation(command, **_kwargs):
                self.assertEqual(command[0], tools.tidy)
                self.assertIn(f"-load={tools.plugin}", command)
                self.assertEqual(command[command.index("--") + 1 :], [*tools.flags, "-std=c++26"])
                self.assertNotIn("/clang:-std=c++26", command)
                sources = list(root.rglob("*.cpp"))
                self.assertTrue(any(path.parent.name == "test" for path in sources))
                self.assertTrue(any(path.parent.name == "lib" for path in sources))
                self.assertTrue(list(root.rglob("*-macros.h")))
                self.assertFalse(any("forward" in path.relative_to(root).parts for path in sources))
                main = Path(command[command.index("--") - 1])
                source = main.read_text(encoding="utf-8")
                self.assertIn("std::abort()", source)
                self.assertIn("throw 1", source)
                self.assertIn("std::filesystem::path", source)
                self.assertEqual(
                    len(linttest.parse_expectations(source, linttest._SOURCE_POLICY_CHECKS[0]).expected), 3
                )
                raise OSError("stop before real native acceptance")

            with (
                mock.patch.object(linttest.tidyengine, "clang_tool", side_effect=AssertionError("CMake discovery")),
                mock.patch.object(linttest.subprocess, "run", side_effect=stop_after_generation),
                self.assertRaisesRegex(OSError, "stop before real native acceptance"),
            ):
                linttest._run_source_policy_smoke(root, root, plugin=None, tools=tools)

    def test_success_logs_are_recorded_without_echoing_them(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            raw = root / "case.log"
            raw.write_text("native diagnostic trace\n", encoding="utf-8")
            log = root / "suite.log"
            with contextlib.redirect_stdout(io.StringIO()) as output:
                result = linttest._report_results(
                    "Diagnostics",
                    [linttest.Fixture(root / "Main.cpp", "aobus-example")],
                    [(True, raw)],
                    linttest.Reporter(log),
                    log_success=True,
                )
            self.assertTrue(result)
            self.assertIn("native diagnostic trace", log.read_text(encoding="utf-8"))
            self.assertNotIn("native diagnostic trace", output.getvalue())


class BazelFixtureTransportTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.fixture_dir = self.root / "test/lint/fixture"
        check = self.fixture_dir / "aobus-example"
        check.mkdir(parents=True)
        self.fixture = check / "Bad.cpp"
        self.fixture.write_text("int bad; // POSITIVE |\n", encoding="utf-8")
        self.policy = self.root / "policy.json"
        configuration = self.root / "configuration.json"
        configuration.write_text(
            json.dumps(
                {
                    "schema": 1,
                    "source_root": str(self.root),
                    "check": None,
                    "header_filter": None,
                    "clang_tidy_args": [],
                }
            ),
            encoding="utf-8",
        )
        bazel_lint_action.prepare(argparse.Namespace(configuration=configuration, output=self.policy))
        self.sdk = self.root / "sdk.json"
        self.sdk_data = {
            "schema": 1,
            "platform": "linux",
            "version": "22.1.0",
            "host_kind": "plugin",
            "sdk_root": str(self.root / "sdk"),
            "resource_dir": str(self.root / "sdk/resource"),
            "system_arguments": ["-isystem/native/include"],
            "analysis_arguments": ["-resource-dir=/sdk/resource"],
        }
        self.sdk.write_text(json.dumps(self.sdk_data), encoding="utf-8")
        self.tidy, self.plugin = self.root / "clang-tidy", self.root / "plugin.so"
        self.tidy.touch()
        self.plugin.touch()
        self.verification = self.root / "verified.json"
        self.args = argparse.Namespace(
            policy=self.policy,
            sdk=self.sdk,
            verification=self.verification,
            tidy=self.tidy,
            plugin=self.plugin,
            work=self.root / "work",
            report=self.root / "result.json",
            log=self.root / "suite.log",
            jobs=2,
        )

        def verified_host(command, **_kwargs):
            self.assertEqual(command[0], str(self.tidy))
            if command[1:] == ["--version"]:
                return subprocess.CompletedProcess(command, 0, "LLVM version 22.1.0\n")
            self.assertEqual(command[1:], [f"-load={self.plugin}", "-checks=-*,aobus-*", "-list-checks"])
            expected = json.loads(self.policy.read_text(encoding="utf-8"))["expected_checks"]
            return subprocess.CompletedProcess(command, 0, "Enabled checks:\n" + "\n".join(expected))

        with mock.patch.object(bazel_lint_action.subprocess, "run", side_effect=verified_host):
            bazel_lint_action.verify(
                argparse.Namespace(
                    **vars(self.args),
                    output=self.verification,
                )
            )

    def argv(self):
        result = []
        for name in ("policy", "sdk", "verification", "tidy", "plugin", "work", "report", "log", "jobs"):
            result.extend([f"--{name}", str(getattr(self.args, name))])
        return result

    def test_real_grammar_failure_is_a_completed_cacheable_result_without_reverification(self):
        discover = linttest.discover_fixtures
        self.args.work.mkdir()
        prior = self.args.work / "prior.log"
        prior.write_text("retain", encoding="utf-8")
        with (
            mock.patch.object(linttest, "discover_fixtures", side_effect=lambda: discover(self.fixture_dir)),
            mock.patch.object(linttest.subprocess, "run", side_effect=AssertionError("must not reverify or analyze")),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            self.assertEqual(bazel_lint_fixture.main(self.argv()), 0)
            self.assertEqual(bazel_lint_fixture.main(self.argv()), 0)
        report = json.loads(self.args.report.read_text(encoding="utf-8"))
        self.assertEqual(report["schema"], 1)
        self.assertEqual(report["status"], 1)
        self.assertEqual(report["platform"], "linux")
        log = self.args.log.read_text(encoding="utf-8")
        self.assertEqual(log.count("empty explicit diagnostic text"), 1)
        self.assertIn(str(self.fixture.resolve()), log)
        self.assertEqual(prior.read_text(encoding="utf-8"), "retain")

    def test_invalid_verification_or_missing_host_is_infrastructure_failure_without_report(self):
        original = self.verification.read_text(encoding="utf-8")
        for fault in ("verification", "host", "plugin", "sdk", "registry", "jobs"):
            with self.subTest(fault=fault):
                self.verification.write_text(original, encoding="utf-8")
                self.tidy.touch()
                self.plugin.touch()
                self.sdk.write_text(json.dumps(self.sdk_data), encoding="utf-8")
                self.args.jobs = 2
                if fault == "verification":
                    self.verification.write_text('{"schema": 1, "verified": true}', encoding="utf-8")
                elif fault in ("host", "plugin"):
                    (self.tidy if fault == "host" else self.plugin).unlink()
                elif fault == "sdk":
                    self.sdk.write_text(json.dumps({**self.sdk_data, "version": "23.0.0"}), encoding="utf-8")
                elif fault == "registry":
                    value = json.loads(original)
                    value["expected_checks"] = ["aobus-wrong-check"]
                    self.verification.write_text(json.dumps(value), encoding="utf-8")
                else:
                    self.args.jobs = 0
                self.args.report.write_text('{"schema":1,"status":0}', encoding="utf-8")
                with (
                    mock.patch.object(linttest.subprocess, "run", side_effect=AssertionError("must not start host")),
                    contextlib.redirect_stderr(io.StringIO()),
                ):
                    self.assertNotEqual(bazel_lint_fixture.main(self.argv()), 0)
                self.assertFalse(self.args.report.exists())

    def test_missing_declared_fixtures_are_an_infrastructure_failure_not_a_cached_suite_result(self):
        with (
            mock.patch.object(linttest, "discover_fixtures", return_value=[]),
            mock.patch.object(linttest.subprocess, "run", side_effect=AssertionError("must not start host")),
            contextlib.redirect_stderr(io.StringIO()),
        ):
            self.assertEqual(bazel_lint_fixture.main(self.argv()), 2)
        self.assertFalse(self.args.report.exists())

    def test_sdk_profiles_are_driver_arguments_not_product_compile_commands(self):
        tools = bazel_lint_fixture._fixture_tools(self.args, self.sdk_data, str(self.tidy))
        self.assertFalse(tools.windows)
        self.assertEqual(tools.flags, ("-isystem/native/include", "-resource-dir=/sdk/resource"))
        macos = {
            **self.sdk_data,
            "platform": "macos",
            "fixture_arguments": [
                "--no-default-config",
                "-stdlib=libc++",
                "-isysroot",
                "/native SDK",
                "-mmacosx-version-min=26.0",
                "-isystem/shim",
            ],
        }
        tools = bazel_lint_fixture._fixture_tools(self.args, macos, str(self.tidy))
        self.assertEqual(tools.flags, (*macos["fixture_arguments"], *macos["analysis_arguments"]))
        self.assertNotIn("-isystem/native/include", tools.flags)
        macos.pop("fixture_arguments")
        with self.assertRaisesRegex(ValueError, "SDK/deployment/shim"):
            bazel_lint_fixture._fixture_tools(self.args, macos, str(self.tidy))

    def test_windows_profile_reuses_verified_sdk_manifest_and_native_include_without_cmake(self):
        _version, _checksum, required = tidyengine.windows_llvm_sdk_manifest()
        sdk_root = self.root / "native-sdk"
        for relative in required:
            path = sdk_root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.touch()
        resource = sdk_root / "lib/clang/22"
        sdk = {**self.sdk_data, "platform": "windows", "sdk_root": str(sdk_root), "resource_dir": str(resource)}
        with mock.patch.dict(os.environ, {"INCLUDE": "VS headers;;Windows SDK;"}):
            tools = bazel_lint_fixture._fixture_tools(self.args, sdk, "verified-linked-host")
        self.assertTrue(tools.windows)
        self.assertIsNone(tools.plugin)
        self.assertEqual(tools.tidy, "verified-linked-host")
        self.assertIn("--driver-mode=cl", tools.flags)
        self.assertIn("/X", tools.flags)
        self.assertIn(f"/clang:-resource-dir={resource}", tools.flags)
        self.assertIn("/D_USE_STD_VECTOR_ALGORITHMS=0", tools.flags)
        self.assertEqual(
            [flag for flag in tools.flags if flag.startswith("/imsvc")],
            [f"/imsvc{resource / 'include'}", "/imsvcVS headers", "/imsvcWindows SDK"],
        )
        self.assertFalse((self.args.work / "CMakeCache.txt").exists())


if __name__ == "__main__":
    unittest.main()
