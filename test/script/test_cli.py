"""CLI smoke tests: every command registers and parses its documented invocations."""

import contextlib
import io
import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao.__main__ import main, make_parser, parse_arguments
from ao.command import build as build_command
from ao.command import check as check_command
from ao.command import council as council_command
from ao.command import coverage as coverage_command
from ao.command import run as run_command_mod
from ao.command import test as test_command
from ao.command import tidy as tidy_command
from ao.command.build import BuildResult
from ao.core import builddir


class LinuxShellPortalTest(unittest.TestCase):
    def test_nix_reentry_discards_python_paths_from_a_stale_shell(self):
        portal = Path(__file__).resolve().parents[2] / "ao"
        content = portal.read_text(encoding="utf-8")

        unset_offset = content.index("unset PYTHONHOME PYTHONPATH")
        reentry_offset = content.index('exec nix-shell "$ROOT/shell.nix"')

        self.assertLess(unset_offset, reentry_offset)


class WindowsBatchPortalTest(unittest.TestCase):
    def test_python_bootstrap_normalizes_state_arguments_and_ignores_ambient_packages(self):
        portal = Path(__file__).resolve().parents[2] / "ao.bat"
        content = portal.read_text(encoding="utf-8").lower()

        self.assertIn('set "aobus_state_argument=%aobus_state_root%"', content)
        self.assertIn('set "aobus_state_argument=%aobus_state_argument%."', content)
        self.assertIn('set "pythonpath=%root%script"', content)
        self.assertNotIn(';%pythonpath%"', content)

    def test_toolchain_commands_initialize_the_visual_studio_environment(self):
        portal = Path(__file__).resolve().parents[2] / "ao.bat"
        content = portal.read_text(encoding="utf-8").lower()

        # ao.bat asks the portal package which commands need MSVC/vcpkg instead
        # of keeping its own command list.
        self.assertIn("-m ao.core.buildenv", content)
        self.assertIn('if not "%needs_build_env%"=="1" goto environment_ready', content)
        self.assertNotIn('if /i "%~1"=="build" set "needs_build_env=1"', content)

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

    def test_windows_presets_separate_platform_from_build_type(self):
        presets_file = Path(__file__).resolve().parents[2] / "CMakePresets.json"
        presets = json.loads(presets_file.read_text(encoding="utf-8"))["configurePresets"]
        windows_presets = {preset["name"]: preset for preset in presets if preset["name"].startswith("windows-")}
        tidy_preset = next(preset for preset in presets if preset["name"] == "windows-tidy")

        self.assertEqual(set(windows_presets), {"windows-base", "windows-debug", "windows-release", "windows-tidy"})
        self.assertEqual(windows_presets["windows-debug"]["inherits"], "windows-base")
        self.assertEqual(windows_presets["windows-release"]["inherits"], "windows-base")
        base_cache = windows_presets["windows-base"]["cacheVariables"]
        self.assertFalse(any(name.startswith("AOBUS_BUILD_") for name in base_cache))
        self.assertEqual(tidy_preset["inherits"], "windows-release")
        self.assertEqual(tidy_preset["cacheVariables"], {"AOBUS_BUILD_LINT_PLUGIN": "ON"})

        manifest = json.loads((presets_file.parent / "vcpkg.json").read_text(encoding="utf-8"))
        self.assertIn("tests", manifest["default-features"])

    def test_windows_presets_keep_build_trees_out_of_the_source_checkout(self):
        presets_file = Path(__file__).resolve().parents[2] / "CMakePresets.json"
        presets = json.loads(presets_file.read_text(encoding="utf-8"))["configurePresets"]
        windows_presets = [preset for preset in presets if preset["name"].startswith("windows-")]

        for preset in windows_presets:
            if binary_dir := preset.get("binaryDir"):
                self.assertTrue(binary_dir.startswith("$env{LOCALAPPDATA}/Aobus/build/"))
                self.assertNotIn("out/build", binary_dir)

    def test_linux_presets_use_ninja(self):
        presets_file = Path(__file__).resolve().parents[2] / "CMakePresets.json"
        presets = json.loads(presets_file.read_text(encoding="utf-8"))["configurePresets"]
        linux_presets = [preset for preset in presets if not preset["name"].startswith("windows-")]

        self.assertTrue(linux_presets)
        self.assertTrue(all(preset["generator"] == "Ninja" for preset in linux_presets))


class CliParseTest(unittest.TestCase):
    def parse(self, argv):
        return parse_arguments(make_parser(), argv)

    def test_all_commands_are_registered(self):
        buffer = io.StringIO()
        with contextlib.redirect_stdout(buffer):
            self.assertEqual(main(["help"]), 0)
        for command in (
            "build",
            "check",
            "test",
            "test-audit",
            "name-audit",
            "coverage",
            "deps",
            "docs",
            "tidy",
            "analyze",
            "format",
            "hygiene",
            "run",
            "council",
        ):
            self.assertIn(command, buffer.getvalue())
        self.assertNotIn("selftest", buffer.getvalue())
        self.assertNotIn("pycheck", buffer.getvalue())

    def test_build_arguments(self):
        args = self.parse(["build", "release", "--clang", "--clean", "--target", "aobus-gtk"])
        self.assertEqual(args.flavor, "release")
        self.assertTrue(args.clang)
        self.assertEqual(args.target, ["aobus-gtk"])

    def test_build_rejects_unknown_flavor(self):
        with self.assertRaises(SystemExit):
            self.parse(["build", "fastest"])

    def test_sanitizers_are_mutually_exclusive(self):
        with self.assertRaises(SystemExit):
            self.parse(["build", "--asan", "--tsan"])

    def test_check_defaults_to_debug(self):
        args = self.parse(["check"])
        self.assertEqual(args.flavor, "debug")
        self.assertFalse(args.asan)

    def test_parallel_build_arguments_forward_the_environment_limit(self):
        with mock.patch.dict(build_command.os.environ, {"CMAKE_BUILD_PARALLEL_LEVEL": "8"}):
            self.assertEqual(build_command.parallel_build_arguments(), ["--parallel", "8"])

    def test_parallel_build_arguments_reject_an_invalid_environment_limit(self):
        for configured in ("", "all", "0", "-1"):
            with self.subTest(configured=configured):
                with mock.patch.dict(build_command.os.environ, {"CMAKE_BUILD_PARALLEL_LEVEL": configured}):
                    with self.assertRaises(SystemExit):
                        build_command.parallel_build_arguments()

    def test_parallel_build_arguments_leave_one_cpu_available_by_default(self):
        with mock.patch.dict(build_command.os.environ, {}, clear=True):
            with mock.patch.object(build_command.os, "cpu_count", return_value=32):
                self.assertEqual(build_command.parallel_build_arguments(), ["--parallel", "31"])

    def test_dependency_report_arguments(self):
        args = self.parse(["deps", "report", "-p", "/tmp/aobus-deps", "--json", "/tmp/aobus-deps.json"])

        self.assertEqual(args.deps_action, "report")
        self.assertEqual(args.path, "/tmp/aobus-deps")
        self.assertEqual(args.json, Path("/tmp/aobus-deps.json"))

    def test_dependency_verify_arguments(self):
        args = self.parse(["deps", "verify", "-p", "/tmp/aobus-deps"])

        self.assertEqual(args.deps_action, "verify")
        self.assertEqual(args.path, "/tmp/aobus-deps")

    def test_docs_check_arguments(self):
        args = self.parse(["docs", "check"])

        self.assertEqual(args.docs_action, "check")

    def test_windows_build_selects_the_shared_flavor_preset(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            args = self.parse(["build", "-p", temp_dir])
            with mock.patch.object(builddir, "platform_profile", return_value=builddir.WINDOWS_PROFILE):
                with mock.patch.object(build_command, "run", return_value=0) as run:
                    result = build_command.do_build(args, ["ao_core_test"])

        configure = run.call_args_list[0].args[0]
        self.assertIn("windows-debug", configure)
        self.assertEqual(result.preset, "windows-debug")
        self.assertEqual(result.compiler, "msvc")

    def test_windows_clean_uses_extended_length_path(self):
        build_path = mock.MagicMock(spec=Path)
        build_path.resolve.return_value = Path(r"C:\local\aobus-build")

        with mock.patch.object(build_command.shutil, "rmtree") as remove_tree:
            build_command._remove_build_directory(build_path, builddir.WINDOWS_PROFILE)

        remove_tree.assert_called_once_with(r"\\?\C:\local\aobus-build")

    def test_windows_extended_length_path_preserves_unc_semantics(self):
        self.assertEqual(
            build_command._windows_extended_path(r"\\server\share\aobus-build"),
            r"\\?\UNC\server\share\aobus-build",
        )

    def test_windows_asan_build_enables_instrumentation(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            args = self.parse(["build", "--asan", "-p", temp_dir])
            with mock.patch.object(builddir, "platform_profile", return_value=builddir.WINDOWS_PROFILE):
                with mock.patch.object(build_command, "run", return_value=0) as run:
                    build_command.do_build(args, [])

        self.assertIn("-DAOBUS_ENABLE_ASAN=ON", run.call_args_list[0].args[0])

    def test_windows_tsan_build_is_rejected_before_configure(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            args = self.parse(["build", "--tsan", "-p", temp_dir])
            with mock.patch.object(builddir, "platform_profile", return_value=builddir.WINDOWS_PROFILE):
                with mock.patch.object(build_command, "run") as run:
                    with self.assertRaisesRegex(SystemExit, "1"):
                        build_command.do_build(args, [])

        run.assert_not_called()

    def test_windows_clang_build_is_rejected_before_configure(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            args = self.parse(["build", "--clang", "-p", temp_dir])
            with mock.patch.object(builddir, "platform_profile", return_value=builddir.WINDOWS_PROFILE):
                with mock.patch.object(build_command, "run") as run:
                    with self.assertRaisesRegex(SystemExit, "1"):
                        build_command.do_build(args, [])

        run.assert_not_called()

    def test_council_forwards_subcommand_arguments(self):
        args = self.parse(
            ["council", "-p", "/tmp/aobus-test-build", "-n", "validate-config", "--registry", "config.yaml"]
        )
        self.assertEqual(args.path, "/tmp/aobus-test-build")
        self.assertTrue(args.no_build)
        self.assertEqual(args.council_args, ["validate-config", "--registry", "config.yaml"])

    def test_council_builds_and_runs_selected_executable(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            build_dir = Path(temp_dir)
            binary = build_dir / "tool" / "council" / "aobus-council"
            binary.parent.mkdir(parents=True)
            binary.touch()
            args = self.parse(["council", "-p", str(build_dir), "validate-config", "--registry", "config.yaml"])

            with mock.patch.object(builddir, "platform_profile", return_value=builddir.LINUX_PROFILE):
                with mock.patch.object(
                    council_command.build,
                    "parallel_build_arguments",
                    return_value=["--parallel", "8"],
                ):
                    with mock.patch.object(council_command, "run", return_value=0) as run:
                        self.assertEqual(council_command.run_command(args), 0)

        self.assertEqual(
            run.call_args_list[0].args[0],
            ["cmake", "--build", str(build_dir), "--parallel", "8", "--target", "aobus-council"],
        )
        self.assertEqual(run.call_args_list[1].args[0], [str(binary), "validate-config", "--registry", "config.yaml"])

    def test_council_rejects_windows_before_building_an_unsupported_target(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            build_dir = Path(temp_dir)
            args = self.parse(["council", "-p", str(build_dir), "validate-config"])

            with mock.patch.object(builddir, "platform_profile", return_value=builddir.WINDOWS_PROFILE):
                with mock.patch.object(council_command, "run") as run:
                    with self.assertRaisesRegex(SystemExit, "1"):
                        council_command.run_command(args)

        run.assert_not_called()

    def test_test_suite_shortcuts(self):
        with mock.patch.object(builddir, "platform_profile", return_value=builddir.LINUX_PROFILE):
            args = self.parse(["test", "--gtk", "[layout],[model]", "-n", "--clang", "--asan"])
            self.assertEqual(args.suite, "gtk")
            self.assertEqual(args.filter, "[layout],[model]")
            self.assertTrue(args.no_build)
            self.assertTrue(args.clang)
            self.assertTrue(args.asan)

            args = self.parse(["test", "--cli"])
            self.assertEqual(args.suite, "cli")

    def test_test_defaults_to_default_suite_group(self):
        args = self.parse(["test"])
        self.assertEqual(args.suite, "default")

    def test_test_audit_arguments(self):
        args = self.parse(["test-audit", "--fail-on-issue", "test/unit/query"])
        self.assertTrue(args.fail_on_issue)
        self.assertEqual(args.paths, ["test/unit/query"])

    def test_name_audit_arguments(self):
        args = self.parse(["name-audit", "--fail-on-issue", "app/include/ao/uimodel"])
        self.assertTrue(args.fail_on_issue)
        self.assertEqual(args.paths, ["app/include/ao/uimodel"])

    def test_test_suite_targets_use_suite_name_suffixes(self):
        self.assertEqual(
            test_command.SUITE_TARGETS,
            {
                "core": ["ao_core_test"],
                "tui": ["ao_tui_test"],
                "cli": ["ao_cli_test"],
                "gtk": ["ao_gtk_test"],
                "integration": ["ao_integration_test"],
                "council": ["ao_council_test"],
            },
        )

    def test_test_all_runs_every_suite(self):
        args = self.parse(["test", "--all", "-n", "-p", "/tmp/aobus-test-build"])

        with mock.patch.object(builddir, "platform_profile", return_value=builddir.LINUX_PROFILE):
            with mock.patch.object(test_command, "run_suites", return_value=0) as run_suites:
                self.assertEqual(test_command.run_command(args), 0)

        run_suites.assert_called_once_with(
            ("core", "tui", "cli", "gtk", "integration", "council", "tooling", "lint"),
            Path("/tmp/aobus-test-build"),
            test_filter="",
            list_only=False,
            repeat=1,
            tsan=False,
        )

    def test_suite_group_dispatches_registered_runner_kinds_in_order(self):
        build_dir = Path("/tmp/aobus-test-build")

        with mock.patch.object(test_command, "run_suite", return_value=0) as run_suite:
            with mock.patch.object(test_command, "run_non_catch2_suite", return_value=0) as run_non_catch2:
                self.assertEqual(test_command.run_suites(test_command.SUITE_GROUPS["all"], build_dir), 0)

        self.assertEqual(
            [call.args[0] for call in run_suite.call_args_list],
            ["core", "tui", "cli", "gtk", "integration", "council"],
        )
        self.assertEqual([call.args[0] for call in run_non_catch2.call_args_list], ["tooling", "lint"])

    def test_gtk_suite_runs_inside_virtual_x11_display(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            build_dir = Path(temp_dir)
            binary = build_dir / "test" / "ao_gtk_test"
            binary.parent.mkdir()
            binary.touch()

            server = mock.Mock()
            server.stdout = io.StringIO("42\n")
            server.wait.return_value = 0

            with mock.patch.object(builddir, "platform_profile", return_value=builddir.LINUX_PROFILE):
                with mock.patch.object(test_command.subprocess, "Popen", return_value=server) as popen:
                    with mock.patch.object(test_command, "run", return_value=0) as run:
                        self.assertEqual(test_command.run_suite("gtk", build_dir, test_filter="[layout]"), 0)

        popen.assert_called_once_with(
            ["Xvfb", "-displayfd", "1", "-screen", "0", "1280x1024x24", "-nolisten", "tcp"],
            stdout=test_command.subprocess.PIPE,
            stderr=test_command.subprocess.DEVNULL,
            text=True,
        )
        run.assert_called_once_with(
            [str(binary), "[layout]"],
            env={"DISPLAY": ":42", "GDK_BACKEND": "x11", "GDK_DISABLE": "gl,vulkan", "GSK_RENDERER": "cairo"},
            log=None,
            append=False,
        )
        server.terminate.assert_called_once()
        server.wait.assert_called_once_with(timeout=5)

    def test_test_no_build_lint_uses_selected_tree_without_building(self):
        build_dir = Path("/tmp/aobus-test-build")

        with mock.patch.object(test_command.linttest, "run", return_value=0) as run:
            self.assertEqual(test_command.run_non_catch2_suite("lint", build_dir), 0)

        run.assert_called_once_with(build_dir, log=None)

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
            tsan=False,
        )

    def test_test_list_describes_non_catch2_suite_without_running_it(self):
        with mock.patch.object(test_command.linttest, "run") as run:
            self.assertEqual(
                test_command.run_non_catch2_suite("lint", Path("/tmp/aobus-test-build"), list_only=True),
                0,
            )

        run.assert_not_called()

    def test_check_runs_the_same_all_suite_group(self):
        args = self.parse(["check"])
        result = BuildResult(
            build_dir=Path("/tmp/aobus-test-build"),
            log=Path("/tmp/aobus-test-build/build.log"),
            compiler="gcc",
        )

        with mock.patch.object(builddir, "platform_profile", return_value=builddir.LINUX_PROFILE):
            with mock.patch.object(check_command.build, "do_build", return_value=result) as do_build:
                with mock.patch.object(check_command.dependency_policy, "verified_report") as verify:
                    with mock.patch.object(check_command.test, "run_suites", return_value=0) as run_suites:
                        with mock.patch.object(check_command.build, "print_summary"):
                            self.assertEqual(check_command.run_command(args), 0)

        do_build.assert_called_once_with(args, targets=[])
        verify.assert_called_once_with(result.build_dir)
        run_suites.assert_called_once_with(
            test_command.SUITE_GROUPS["all"],
            result.build_dir,
            tsan=False,
            log=result.log,
        )

    def test_tsan_check_builds_and_runs_only_baselined_suites(self):
        args = self.parse(["check", "--tsan"])
        result = BuildResult(
            build_dir=Path("/tmp/aobus-test-tsan-build"),
            log=Path("/tmp/aobus-test-tsan-build/build.log"),
            compiler="gcc",
        )

        with mock.patch.object(builddir, "platform_profile", return_value=builddir.LINUX_PROFILE):
            with mock.patch.object(check_command.build, "do_build", return_value=result) as do_build:
                with mock.patch.object(check_command.dependency_policy, "verified_report"):
                    with mock.patch.object(check_command.test, "run_suites", return_value=0) as run_suites:
                        with mock.patch.object(check_command.build, "print_summary"):
                            self.assertEqual(check_command.run_command(args), 0)

        do_build.assert_called_once_with(args, targets=["ao_core_test", "ao_gtk_test"])
        run_suites.assert_called_once_with(
            ("core", "gtk"),
            result.build_dir,
            tsan=True,
            log=result.log,
        )

    def test_windows_test_reuses_the_shared_debug_tree(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            args = self.parse(["test", "-p", temp_dir])
            build_dir = Path(temp_dir)

            with mock.patch.object(builddir, "platform_profile", return_value=builddir.WINDOWS_PROFILE):
                with mock.patch.object(
                    test_command.build,
                    "parallel_build_arguments",
                    return_value=["--parallel", "8"],
                ):
                    with mock.patch.object(test_command, "run", return_value=0) as run:
                        with mock.patch.object(test_command, "run_suites", return_value=0) as run_suites:
                            self.assertEqual(test_command.run_command(args), 0)

        run.assert_called_once_with(
            ["cmake", "--build", str(build_dir), "--parallel", "8", "--target", "ao_core_test", "ao_tui_test"]
        )
        run_suites.assert_called_once_with(
            ("core", "tui"),
            build_dir,
            test_filter="",
            list_only=False,
            repeat=1,
            tsan=False,
        )

    def test_windows_check_runs_only_native_suites(self):
        args = self.parse(["check"])
        result = BuildResult(
            build_dir=builddir.WINDOWS_BUILD_ROOT / "windows-debug",
            log=builddir.WINDOWS_BUILD_ROOT / "windows-debug" / "build.log",
            compiler="msvc",
            preset="windows-debug",
        )

        with mock.patch.object(builddir, "platform_profile", return_value=builddir.WINDOWS_PROFILE):
            with mock.patch.object(check_command.build, "do_build", return_value=result) as do_build:
                with mock.patch.object(check_command.dependency_policy, "verified_report") as verify:
                    with mock.patch.object(check_command.test, "run_suites", return_value=0) as run_suites:
                        with mock.patch.object(check_command.build, "print_summary"):
                            self.assertEqual(check_command.run_command(args), 0)

        do_build.assert_called_once_with(args, targets=[])
        verify.assert_called_once_with(result.build_dir)
        run_suites.assert_called_once_with(
            ("core", "tui", "cli", "integration", "tooling"),
            result.build_dir,
            tsan=False,
            log=result.log,
        )

    def test_tsan_defaults_to_the_baselined_suite_group(self):
        args = self.parse(["test", "--tsan", "-n", "-p", "/tmp/aobus-test-build"])

        with mock.patch.object(builddir, "platform_profile", return_value=builddir.LINUX_PROFILE):
            with mock.patch.object(test_command, "run_suites", return_value=0) as run_suites:
                self.assertEqual(test_command.run_command(args), 0)

        run_suites.assert_called_once_with(
            ("core", "gtk"),
            Path("/tmp/aobus-test-build"),
            test_filter="",
            list_only=False,
            repeat=1,
            tsan=True,
        )

    def test_windows_explicit_tsan_suite_is_rejected(self):
        with mock.patch.object(builddir, "platform_profile", return_value=builddir.WINDOWS_PROFILE):
            with self.assertRaisesRegex(SystemExit, "1"):
                test_command.suites_for("core", tsan=True)

    def test_windows_asan_does_not_configure_linux_leak_suppressions(self):
        with mock.patch.object(builddir, "platform_profile", return_value=builddir.WINDOWS_PROFILE):
            self.assertEqual(test_command._lsan_env(Path("windows-debug-asan")), {})

    def test_windows_clang_test_is_rejected_before_build_or_run(self):
        args = self.parse(["test", "--core", "--clang", "-n", "-p", "/tmp/aobus-test-build"])
        with mock.patch.object(builddir, "platform_profile", return_value=builddir.WINDOWS_PROFILE):
            with mock.patch.object(test_command, "run_suites") as run_suites:
                with self.assertRaisesRegex(SystemExit, "1"):
                    test_command.run_command(args)

        run_suites.assert_not_called()

    def test_concurrency_group_runs_tagged_tests_across_native_catch2_suites(self):
        args = self.parse(["test", "--concurrency", "--repeat", "3", "-n", "-p", "/tmp/aobus-test-build"])

        with mock.patch.object(builddir, "platform_profile", return_value=builddir.LINUX_PROFILE):
            with mock.patch.object(test_command, "run_suites", return_value=0) as run_suites:
                self.assertEqual(test_command.run_command(args), 0)

        run_suites.assert_called_once_with(
            ("core", "tui", "cli", "gtk", "integration", "council"),
            Path("/tmp/aobus-test-build"),
            test_filter="[concurrency]",
            list_only=False,
            allow_no_tests=True,
            repeat=3,
            tsan=False,
        )

    def test_tsan_concurrency_group_intersects_with_baselined_suites(self):
        args = self.parse(["test", "--concurrency", "--tsan", "-n", "-p", "/tmp/aobus-test-build"])

        with mock.patch.object(builddir, "platform_profile", return_value=builddir.LINUX_PROFILE):
            with mock.patch.object(test_command, "run_suites", return_value=0) as run_suites:
                self.assertEqual(test_command.run_command(args), 0)

        run_suites.assert_called_once_with(
            ("core", "gtk"),
            Path("/tmp/aobus-test-build"),
            test_filter="[concurrency]",
            list_only=False,
            allow_no_tests=True,
            repeat=1,
            tsan=True,
        )

    def test_cross_suite_filter_allows_suites_without_matching_cases(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            build_dir = Path(temp_dir)
            binary = builddir.executable(build_dir / "test" / "ao_cli_test")
            binary.parent.mkdir()
            binary.touch()

            with mock.patch.object(test_command, "run", return_value=0) as run:
                self.assertEqual(
                    test_command.run_suite(
                        "cli",
                        build_dir,
                        test_filter="[concurrency]",
                        allow_no_tests=True,
                    ),
                    0,
                )

        run.assert_called_once_with(
            [str(binary), "--allow-running-no-tests", "[concurrency]"],
            env=None,
            log=None,
            append=False,
        )

    def test_tsan_suite_enforces_fail_fast_runtime_options(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            build_dir = Path(temp_dir)
            binary = builddir.executable(build_dir / "test" / "ao_core_test")
            binary.parent.mkdir()
            binary.touch()

            with mock.patch.dict("os.environ", {"TSAN_OPTIONS": "history_size=7"}, clear=False):
                with mock.patch.object(test_command, "run", return_value=0) as run:
                    self.assertEqual(test_command.run_suite("core", build_dir, tsan=True), 0)

        run.assert_called_once_with(
            [str(binary)],
            env={
                "TSAN_OPTIONS": (
                    f"history_size=7:suppressions={test_command._TSAN_SUPP_PATH}:"
                    "halt_on_error=1:second_deadlock_stack=1"
                )
            },
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
            binary = builddir.executable(build_dir / "test" / "ao_core_test")
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
                        self.assertEqual(test_command.run_suite("core", build_dir, tsan=True), 0)

            expected_suppressions = (
                "race:external_runtime_symbol\n\n"
                + test_command._TSAN_SUPP_PATH.read_text(encoding="utf-8").rstrip()
                + "\n"
            )
            self.assertEqual(merged_suppressions.read_text(encoding="utf-8"), expected_suppressions)

        run.assert_called_once_with(
            [str(binary)],
            env={
                "TSAN_OPTIONS": (
                    f"history_size=7:suppressions={merged_suppressions}:halt_on_error=1:second_deadlock_stack=1"
                )
            },
            log=None,
            append=False,
        )

    def test_tsan_suppressions_cover_only_uninstrumented_ui_dependencies(self):
        rules = [
            line
            for line in test_command._TSAN_SUPP_PATH.read_text(encoding="utf-8").splitlines()
            if line and not line.startswith("#")
        ]

        self.assertEqual(
            rules,
            [
                "called_from_lib:libglib-2.0.so",
                "called_from_lib:libgio-2.0.so",
                "called_from_lib:libgobject-2.0.so",
                "called_from_lib:libglibmm-2.68.so",
                "called_from_lib:libgtk-4.so",
                "called_from_lib:libgdk_pixbuf-2.0.so",
                "called_from_lib:libpango-1.0.so",
                "called_from_lib:libpangoft2-1.0.so",
                "called_from_lib:libpangocairo-1.0.so",
                "called_from_lib:libfontconfig.so",
            ],
        )

    def test_windows_suite_binary_uses_exe_suffix(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            build_dir = Path(temp_dir)
            binary = build_dir / "test" / "ao_core_test.exe"
            binary.parent.mkdir()
            binary.touch()

            with mock.patch.object(builddir, "platform_profile", return_value=builddir.WINDOWS_PROFILE):
                with mock.patch.object(test_command, "run", return_value=0) as run:
                    self.assertEqual(test_command.run_suite("core", build_dir), 0)

        run.assert_called_once_with([str(binary)], env=None, log=None, append=False)

    def test_test_sanitizers_are_mutually_exclusive(self):
        with self.assertRaises(SystemExit):
            self.parse(["test", "--asan", "--tsan"])

    def test_test_suite_shortcuts_are_mutually_exclusive(self):
        with mock.patch.object(builddir, "platform_profile", return_value=builddir.LINUX_PROFILE):
            with self.assertRaises(SystemExit):
                self.parse(["test", "--core", "--gtk"])

    def test_coverage_defaults_to_core_suite(self):
        args = self.parse(["coverage", "rt::SmartListEvaluator"])
        self.assertEqual(args.suite, "core")
        self.assertEqual(args.filter, "rt::SmartListEvaluator")

    def test_coverage_accepts_scopes_and_summary_limit(self):
        args = self.parse(
            [
                "coverage",
                "--gtk",
                "--scope",
                "app/linux-gtk",
                "--scope",
                "include/aobus",
                "--summary-limit",
                "7",
                "[layout]",
            ]
        )
        self.assertEqual(args.suite, "gtk")
        self.assertEqual(args.scope, ["app/linux-gtk", "include/aobus"])
        self.assertEqual(args.summary_limit, 7)
        self.assertEqual(args.filter, "[layout]")

    def test_coverage_accepts_tui_suite_shortcut(self):
        args = self.parse(["coverage", "--tui", "--scope", "app/tui", "[tui]"])
        self.assertEqual(args.suite, "tui")
        self.assertEqual(args.scope, ["app/tui"])
        self.assertEqual(args.filter, "[tui]")

    def test_coverage_rejects_windows_before_configuring_a_linux_build(self):
        args = self.parse(["coverage"])

        with mock.patch.object(builddir, "platform_profile", return_value=builddir.WINDOWS_PROFILE):
            with self.assertRaisesRegex(SystemExit, "1"):
                coverage_command.run_command(args)

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
        self.assertEqual(args.tidy_arg, ["--extra-arg=-std=c++26"])
        self.assertEqual(args.output, "/tmp/report.txt")
        self.assertEqual(args.jobs, 4)

    def test_tidy_no_build_uses_existing_artifact_and_compile_database(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            build_dir = Path(temp_dir)
            (build_dir / "compile_commands.json").touch()
            sdk_root = build_dir / "llvm-sdk"
            (sdk_root / "lib" / "clang" / "22").mkdir(parents=True)
            (build_dir / "CMakeCache.txt").write_text(
                f"AOBUS_LLVM_SDK_RESOLVED_ROOT:INTERNAL={sdk_root}\nAOBUS_LLVM_SDK_RESOLVED_VERSION:INTERNAL=22.1.8\n",
                encoding="utf-8",
            )
            artifact = tidy_command.expected_lint_artifact_path(build_dir)
            artifact.parent.mkdir(parents=True)
            artifact.touch()

            with mock.patch.object(tidy_command.tidyengine, "ensure_compile_db") as ensure_compile_db:
                with mock.patch.object(tidy_command, "verify_tidy_toolchain") as verify:
                    toolchain = tidy_command.prepare_toolchain(build_dir, no_build=True)

        ensure_compile_db.assert_not_called()
        verify.assert_called_once_with(toolchain)
        profile = builddir.platform_profile()
        if profile.name == "windows":
            self.assertEqual(toolchain.clang_tidy, str(artifact))
            self.assertIsNone(toolchain.plugin)
            self.assertEqual(toolchain.resource_dir, sdk_root / "lib" / "clang" / "22")
        else:
            self.assertEqual(toolchain.clang_tidy, "clang-tidy")
            self.assertEqual(toolchain.plugin, artifact)
            self.assertIsNone(toolchain.resource_dir)

    def test_tidy_build_uses_the_native_lint_preset(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            build_dir = Path(temp_dir)
            sdk_root = build_dir / "llvm-sdk"
            (sdk_root / "lib" / "clang" / "22").mkdir(parents=True)
            (build_dir / "CMakeCache.txt").write_text(
                f"AOBUS_LLVM_SDK_RESOLVED_ROOT:INTERNAL={sdk_root}\nAOBUS_LLVM_SDK_RESOLVED_VERSION:INTERNAL=22.1.8\n",
                encoding="utf-8",
            )
            artifact = tidy_command.expected_lint_artifact_path(build_dir)
            artifact.parent.mkdir(parents=True)
            artifact.touch()

            with mock.patch.object(tidy_command.tidyengine, "ensure_compile_db") as ensure_compile_db:
                with mock.patch.object(tidy_command, "verify_tidy_toolchain"):
                    with mock.patch.object(
                        tidy_command.subprocess,
                        "run",
                        return_value=mock.Mock(returncode=0),
                    ):
                        toolchain = tidy_command.prepare_toolchain(build_dir, no_build=False)

        ensure_compile_db.assert_called_once_with(
            build_dir,
            ["-DAOBUS_BUILD_LINT_PLUGIN=ON"],
            preset=builddir.tidy_preset(),
            reconfigure_preset=False,
        )
        profile = builddir.platform_profile()
        if profile.name == "windows":
            self.assertEqual(toolchain.clang_tidy, str(artifact))
            self.assertIsNone(toolchain.plugin)
            self.assertEqual(toolchain.resource_dir, sdk_root / "lib" / "clang" / "22")
        else:
            self.assertEqual(toolchain.clang_tidy, "clang-tidy")
            self.assertEqual(toolchain.plugin, artifact)
            self.assertIsNone(toolchain.resource_dir)

    def test_tidy_artifact_path_is_native(self):
        build_dir = Path("build")
        self.assertEqual(
            tidy_command.expected_lint_artifact_path(build_dir, os_name="nt"),
            build_dir / "tool" / "lint" / "AobusClangTidy.exe",
        )
        self.assertEqual(
            tidy_command.expected_lint_artifact_path(build_dir, os_name="posix"),
            build_dir / "tool" / "lint" / "libAobusLintPlugin.so",
        )

    def test_tidy_toolchain_validation_requires_registered_aobus_checks(self):
        toolchain = tidy_command.TidyToolchain("clang-tidy", Path("plugin.so"), None)
        completed = mock.Mock(
            returncode=0,
            stdout="\n".join(f"  {name}" for name in sorted(tidy_command.EXPECTED_AOBUS_CHECKS)),
        )

        with mock.patch.object(tidy_command.subprocess, "run", return_value=completed) as run:
            tidy_command.verify_tidy_toolchain(toolchain)

        self.assertEqual(
            run.call_args.args[0],
            ["clang-tidy", "-load=plugin.so", "-checks=-*,aobus-*", "-list-checks"],
        )

        with mock.patch.object(
            tidy_command.subprocess,
            "run",
            return_value=mock.Mock(returncode=0, stdout="Enabled checks:\n"),
        ):
            with self.assertRaises(SystemExit):
                tidy_command.verify_tidy_toolchain(toolchain)

    def test_windows_tidy_toolchain_validation_requires_exact_llvm_version(self):
        toolchain = tidy_command.TidyToolchain(
            "AobusClangTidy.exe",
            None,
            Path("C:/llvm/lib/clang/22"),
            "22.1.8",
        )
        version = mock.Mock(returncode=0, stdout="LLVM version 22.1.8\n")
        checks = mock.Mock(
            returncode=0,
            stdout="\n".join(f"  {name}" for name in sorted(tidy_command.EXPECTED_AOBUS_CHECKS)),
        )

        with mock.patch.object(tidy_command.subprocess, "run", side_effect=[version, checks]) as run:
            tidy_command.verify_tidy_toolchain(toolchain)

        self.assertEqual(run.call_args_list[0].args[0], ["AobusClangTidy.exe", "--version"])

        wrong_version = mock.Mock(returncode=0, stdout="LLVM version 21.1.0\n")
        with mock.patch.object(tidy_command.subprocess, "run", return_value=wrong_version):
            with self.assertRaises(SystemExit):
                tidy_command.verify_tidy_toolchain(toolchain)

    def test_tidy_explicit_files(self):
        args = self.parse(["tidy", "lib/audio/Foo.cpp", "include/aobus/Foo.h"])
        self.assertEqual(args.files, ["lib/audio/Foo.cpp", "include/aobus/Foo.h"])

    def test_analyze_flags(self):
        args = self.parse(["analyze", "--all", "--alpha", "--fail-on-diagnostics"])
        self.assertTrue(args.all)
        self.assertTrue(args.alpha)
        self.assertTrue(args.fail_on_diagnostics)

    def test_format_check_mode(self):
        args = self.parse(["format", "--check", "--folder", "script"])
        self.assertTrue(args.check)
        self.assertEqual(args.files, [])
        self.assertEqual(args.folder, ["script"])

    def test_hygiene_scope_arguments(self):
        args = self.parse(["hygiene", "--folder", "script", "--commit", "HEAD~3", "-j", "4"])
        self.assertEqual(args.folder, ["script"])
        self.assertEqual(args.commit, "HEAD~3")
        self.assertEqual(args.jobs, 4)

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

    def test_run_accepts_tui_app(self):
        args = self.parse(["run", "tui", "-n"])
        self.assertEqual(args.app, "tui")
        self.assertTrue(args.no_build)

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

    def test_windows_run_command_builds_and_execs_exe(self):
        with mock.patch.object(builddir, "platform_profile", return_value=builddir.WINDOWS_PROFILE):
            args = self.parse(["run", "tui"])
            with mock.patch.object(run_command_mod.build, "do_build") as do_build:
                with mock.patch.object(run_command_mod.os, "execvp") as execvp:
                    with mock.patch.object(run_command_mod.Path, "exists", return_value=True):
                        run_command_mod.run_command(args)
        do_build.assert_called_once_with(args, ["aobus-tui"])
        self.assertTrue(execvp.call_args.args[0].endswith("aobus-tui.exe"))

    def test_run_command_builds_tui_target(self):
        args = self.parse(["run", "tui"])
        with mock.patch.object(run_command_mod.build, "do_build") as do_build:
            with mock.patch.object(run_command_mod.os, "execvp") as execvp:
                with mock.patch.object(run_command_mod.Path, "exists", return_value=True):
                    run_command_mod.run_command(args)
        do_build.assert_called_once_with(args, ["aobus-tui"])
        execvp.assert_called_once()

    def test_run_command_no_build_skips_build(self):
        args = self.parse(["run", "-n", "tui"])
        with mock.patch.object(run_command_mod.build, "do_build") as do_build:
            with mock.patch.object(run_command_mod.os, "execvp") as execvp:
                with mock.patch.object(run_command_mod.Path, "exists", return_value=True):
                    run_command_mod.run_command(args)
        do_build.assert_not_called()
        execvp.assert_called_once()

    def test_linux_run_parser_exposes_cli_tui_and_gtk(self):
        with mock.patch.object(builddir, "platform_profile", return_value=builddir.LINUX_PROFILE):
            for app in ("cli", "tui", "gtk"):
                with self.subTest(app=app):
                    self.assertEqual(self.parse(["run", app, "-n"]).app, app)

    def test_help_exits_zero(self):
        for argv in (["--help"], ["build", "--help"], ["tidy", "--help"], ["run", "--help"]):
            buffer = io.StringIO()
            with contextlib.redirect_stdout(buffer), self.assertRaises(SystemExit) as caught:
                make_parser().parse_args(argv)
            self.assertEqual(caught.exception.code, 0)
            self.assertTrue(buffer.getvalue())


if __name__ == "__main__":
    unittest.main()
