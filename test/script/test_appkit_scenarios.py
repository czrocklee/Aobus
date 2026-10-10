"""Portal contracts for independently selectable native AppKit credits scenarios."""

import argparse
import contextlib
import io
import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao.__main__ import make_parser, parse_arguments
from ao.command import build as build_command
from ao.command import test as test_command
from ao.core import builddir

_CREDITS_SCENARIOS = ("credits-controls", "credits-previews")


def _parse(arguments: list[str]) -> argparse.Namespace:
    return parse_arguments(make_parser(), arguments)


class AppKitCreditsScenarioTest(unittest.TestCase):
    def test_macos_parser_accepts_each_exact_credits_scenario_name(self):
        with mock.patch.object(builddir, "platform_profile", return_value=builddir.MACOS_PROFILE):
            for scenario in _CREDITS_SCENARIOS:
                with self.subTest(scenario=scenario):
                    args = _parse(["test", "--appkit", "--scenario", scenario])
                    self.assertEqual(args.suite, "appkit")
                    self.assertEqual(args.scenario, scenario)

    def test_macos_parser_rejects_unknown_or_inexact_credits_scenario_names(self):
        with mock.patch.object(builddir, "platform_profile", return_value=builddir.MACOS_PROFILE):
            for scenario in ("unknown", "credits", "credits-control", "credits-preview", "CREDITS-CONTROLS"):
                with self.subTest(scenario=scenario):
                    with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error:
                        _parse(["test", "--appkit", "--scenario", scenario])
                    self.assertEqual(error.exception.code, 2)

    def test_non_macos_profiles_do_not_offer_appkit_or_credits_scenarios(self):
        for profile in (builddir.LINUX_PROFILE, builddir.WINDOWS_PROFILE):
            with self.subTest(profile=profile.name):
                with mock.patch.object(builddir, "platform_profile", return_value=profile):
                    self.assertNotIn("appkit", test_command.selectable_suites())
                    for scenario in _CREDITS_SCENARIOS:
                        for selection in (["--appkit"], ["--suite", "appkit"]):
                            with self.subTest(scenario=scenario, selection=selection):
                                with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error:
                                    _parse(["test", *selection, "--scenario", scenario])
                                self.assertEqual(error.exception.code, 2)

    def test_credits_dispatch_preserves_selected_tree_fixtures_and_asan_option(self):
        with tempfile.TemporaryDirectory() as directory:
            build_dir = Path(directory)
            library = build_dir / "music"
            state_root = build_dir / "state"
            with (
                mock.patch.object(builddir, "platform_profile", return_value=builddir.MACOS_PROFILE),
                mock.patch.object(build_command, "validate_build_tree", return_value="clang"),
                mock.patch.object(
                    test_command.compiler_cache, "read_cmake_cache", return_value={"AOBUS_BUILD_TESTS": "ON"}
                ),
                mock.patch.object(test_command, "run") as build_process,
            ):
                for scenario in _CREDITS_SCENARIOS:
                    for asan in (False, True):
                        with self.subTest(scenario=scenario, asan=asan):
                            arguments = [
                                "test",
                                "--appkit",
                                "--scenario",
                                scenario,
                                "--library",
                                str(library),
                                "--state-root",
                                str(state_root),
                                "--no-build",
                                "--path",
                                str(build_dir),
                            ]
                            if asan:
                                arguments.append("--asan")
                            args = _parse(arguments)
                            with mock.patch.object(test_command, "run_appkit_smoke", return_value=7) as run_smoke:
                                self.assertEqual(test_command.run_command(args), 7)
                            run_smoke.assert_called_once_with(
                                build_dir,
                                scenario=scenario,
                                library=library.resolve(),
                                state_root=state_root.resolve(),
                                asan=asan,
                                tsan=False,
                            )
                build_process.assert_not_called()

    def test_credits_use_existing_parent_runner_with_120_second_timeout_and_no_desktop_markers(self):
        with tempfile.TemporaryDirectory() as directory:
            build_dir = Path(directory) / "build"
            binary = build_dir / "test" / "ao_appkit_smoke.app" / "Contents" / "MacOS" / "ao_appkit_smoke"
            binary.parent.mkdir(parents=True)
            binary.touch()
            library = Path(directory) / "music"
            state_root = Path(directory) / "state"
            run_id = "0123456789abcdef0123456789abcdef"
            inherited = {"INHERITED": "yes", test_command.APPKIT_RUN_ID_ENV: "old"}
            environment = {test_command.APPKIT_RUN_ID_ENV: run_id}
            with (
                mock.patch.object(builddir, "platform_profile", return_value=builddir.MACOS_PROFILE),
                mock.patch.dict(os.environ, inherited, clear=True),
                mock.patch.object(test_command.uuid, "uuid4", return_value=mock.Mock(hex=run_id)),
                mock.patch.object(test_command.appkitprocess, "run_desktop") as desktop,
                mock.patch.object(test_command, "_require_appkit_marker") as marker,
            ):
                for scenario in _CREDITS_SCENARIOS:
                    for status in (0, 7):
                        with self.subTest(scenario=scenario, status=status):
                            parent = mock.Mock()
                            parent.wait.return_value = status
                            command = [
                                str(binary),
                                "--scenario",
                                scenario,
                                "--library",
                                str(library.resolve()),
                                "--state-root",
                                str(state_root.resolve()),
                            ]
                            with (
                                mock.patch.object(test_command.subprocess, "Popen", return_value=parent) as popen,
                                mock.patch.object(
                                    test_command, "_run_appkit_parent", wraps=test_command._run_appkit_parent
                                ) as run_parent,
                                contextlib.redirect_stdout(io.StringIO()),
                            ):
                                self.assertEqual(
                                    test_command.run_appkit_smoke(
                                        build_dir, scenario=scenario, library=library, state_root=state_root, asan=False
                                    ),
                                    status,
                                )
                            run_parent.assert_called_once_with(command, environment)
                            popen.assert_called_once_with(
                                command, cwd=test_command.PROJECT_ROOT, env=inherited | environment
                            )
                            # Pin the controller deadline, not merely the current constant's value.
                            parent.wait.assert_called_once_with(timeout=120.0)
                            parent.terminate.assert_not_called()
                            parent.kill.assert_not_called()
                desktop.assert_not_called()
                marker.assert_not_called()
