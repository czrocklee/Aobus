"""Native launch argv, terminal inheritance, phases and artifact-lock lifetime."""

import argparse
import contextlib
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from phase_support import PhaseRecorder
from workflow_support import executable_artifact

from ao.__main__ import parse_arguments
from ao.command import run
from ao.core import builddir
from ao.core.bazel_native import BazelWorkspaceError


class RunPhaseTest(unittest.TestCase):
    def setUp(self):
        self.tree = Path(self.enterContext(tempfile.TemporaryDirectory(prefix="aobus-run-")))
        self.artifact = executable_artifact(self.tree)
        self.owned = False
        self.lifecycle = []
        self.enterContext(mock.patch.object(builddir, "platform_profile", return_value=builddir.LINUX_PROFILE))
        self.preparation = self.enterContext(
            mock.patch.object(
                run.bazel_workflows,
                "prepare_artifacts",
                side_effect=self.prepare,
            )
        )
        self.phases = PhaseRecorder()
        self.enterContext(mock.patch.object(run.phases, "measure", side_effect=self.phases.measure))

    @contextlib.contextmanager
    def prepare(self, args, labels, **options):
        self.owned = True
        self.lifecycle.append("lock-enter")
        try:
            yield (self.artifact,)
        finally:
            self.lifecycle.append("lock-exit")
            self.owned = False

    def arguments(self, *arguments):
        parser = argparse.ArgumentParser()
        run.register(parser.add_subparsers(dest="command", required=True))
        return parse_arguments(parser, ["run", *arguments])

    def test_supervised_child_keeps_the_terminal_argv_and_artifact_lock_until_exit(self):
        arguments = ["path with spaces", "--literal", "semi;colon", "雪"]
        args = self.arguments("cli", "--no-build", "--path", str(self.tree), "--", *arguments)
        child = mock.Mock()

        def wait():
            self.assertTrue(self.owned, "native outputs must remain locked through child exit")
            return 23

        child.wait.side_effect = wait
        with mock.patch.object(run.subprocess, "Popen", return_value=child) as popen:
            self.assertEqual(run.run_command(args), 23)
        self.assertFalse(self.owned)
        child.kill.assert_not_called()
        self.preparation.assert_called_once_with(args, [run.APPS["cli"].target], no_build=True)
        self.assertEqual(popen.call_args.args, ([str(self.artifact.executable), *arguments],))
        self.assertEqual(set(popen.call_args.kwargs), {"env"}, "stdio must inherit the caller's real terminal")
        self.assertEqual(popen.call_args.kwargs["env"]["RUNFILES_DIR"], str(self.artifact.runfiles_directory))
        self.assertEqual(
            [(record["name"], record["status"]) for record in self.phases.records],
            [
                ("launch-preparation", 0),
                ("application", 23),
            ],
        )

    def test_interrupt_reaps_owned_child_before_releasing_artifacts(self):
        args = self.arguments("cli", "--no-build", "--path", str(self.tree))
        child = mock.Mock()
        observations = []

        def wait():
            observations.append(self.owned)
            if len(observations) == 1:
                raise KeyboardInterrupt
            return -9

        child.wait.side_effect = wait
        with mock.patch.object(run.subprocess, "Popen", return_value=child):
            self.assertEqual(run.run_command(args), 130)
        child.kill.assert_called_once_with()
        self.assertEqual(observations, [True, True])
        self.assertFalse(self.owned)
        self.assertEqual(self.phases.records[-1]["status"], 130)

    def test_repeated_interrupts_and_already_exited_child_do_not_release_outputs_early(self):
        args = self.arguments("cli", "--no-build", "--path", str(self.tree))
        child = mock.Mock()
        child.kill.side_effect = ProcessLookupError
        observations = []

        def wait():
            observations.append(self.owned)
            if len(observations) < 3:
                raise KeyboardInterrupt
            return -2

        child.wait.side_effect = wait
        with mock.patch.object(run.subprocess, "Popen", return_value=child):
            self.assertEqual(run.run_command(args), 130)
        self.assertEqual(observations, [True, True, True])
        self.assertFalse(self.owned)

    def test_failed_artifact_admission_does_not_launch_or_silently_build(self):
        args = self.arguments("cli", "--no-build", "--path", str(self.tree))
        self.preparation.side_effect = BazelWorkspaceError("missing configured executable")
        with mock.patch.object(run.subprocess, "Popen") as child, self.assertRaises(SystemExit):
            run.run_command(args)
        child.assert_not_called()
        self.assertEqual([record["name"] for record in self.phases.records], ["launch-preparation"])

    def test_explicit_checker_environment_is_combined_with_declared_runfiles(self):
        with mock.patch.object(builddir, "platform_profile", return_value=builddir.MACOS_PROFILE):
            args = self.arguments("appkit", "--no-build", "--main-thread-checker")
            child = mock.Mock()
            child.wait.return_value = 0
            environment = {"DYLD_INSERT_LIBRARIES": "/admitted/Xcode/checker.dylib", "PRIVATE_CALLER": "preserved"}
            with (
                mock.patch.object(run, "_main_thread_checker_environment", return_value=environment) as checker,
                mock.patch.object(run.subprocess, "Popen", return_value=child) as popen,
            ):
                self.assertEqual(run.run_command(args), 0)
        checker.assert_called_once_with()
        self.assertEqual(popen.call_args.kwargs["env"], environment | self.artifact.environment())

    def test_checker_on_an_ineligible_app_fails_before_querying_native_artifacts(self):
        args = self.arguments("cli", "--no-build", "--main-thread-checker")
        with mock.patch.object(run.subprocess, "Popen") as child, self.assertRaises(SystemExit):
            run.run_command(args)
        self.preparation.assert_not_called()
        child.assert_not_called()

    def test_winui_product_admits_runtime_and_interactive_desktop_before_query(self):
        with mock.patch.object(builddir, "platform_profile", return_value=builddir.WINDOWS_PROFILE):
            args = self.arguments("winui", "--no-build")
            with (
                mock.patch.object(run.winui, "require_runtime") as runtime,
                mock.patch.object(run.winui, "require_interactive_session", side_effect=RuntimeError("session 0")),
                mock.patch.object(run.subprocess, "Popen") as child,
                self.assertRaises(SystemExit),
            ):
                run.run_command(args)
        runtime.assert_called_once_with()
        self.preparation.assert_not_called()
        child.assert_not_called()

    def test_no_build_still_requires_native_sdk_admission(self):
        self.assertTrue(run.requires_build_environment(self.arguments("cli", "--no-build")))

    def test_an_explicit_label_shares_the_artifact_lifecycle_of_the_flat_command(self):
        label = "//app/windows-winui:ao_winui_automation_probe"
        args = self.arguments(label, "--no-build", "--path", str(self.tree))
        child = mock.Mock()
        child.wait.return_value = 0
        with mock.patch.object(run.subprocess, "Popen", return_value=child) as popen:
            self.assertEqual(run.run_command(args), 0)
        self.preparation.assert_called_once_with(args, [label], no_build=True)
        popen.assert_called_once_with([str(self.artifact.executable)], env=mock.ANY)

    def test_a_canonical_application_label_keeps_its_application_only_checks(self):
        # The appkit label names the appkit application, so it inherits the
        # app admission instead of bypassing it as a bare label.
        args = self.arguments(run.APPS["appkit"].target, "--no-build", "--path", str(self.tree))
        with (
            mock.patch.object(run.subprocess, "Popen") as child,
            self.assertRaises(SystemExit),
        ):
            run.run_command(args)
        self.preparation.assert_not_called()
        child.assert_not_called()


if __name__ == "__main__":
    unittest.main()
