"""Retained suite behavior at the configured-artifact/native-process boundary."""

import argparse
import contextlib
import io
import tempfile
import unittest
from dataclasses import replace
from pathlib import Path
from unittest import mock

from workflow_support import executable_artifact

from ao.command import test
from ao.core import bazel_test_services, builddir


class NativeTestWorkflowTest(unittest.TestCase):
    def setUp(self):
        self.root = Path(self.enterContext(tempfile.TemporaryDirectory(prefix="aobus-test-workflow-")))
        self.enterContext(mock.patch.object(builddir, "platform_profile", return_value=builddir.LINUX_PROFILE))
        self.enterContext(contextlib.redirect_stdout(io.StringIO()))

    def arguments(self, *arguments):
        parser = argparse.ArgumentParser()
        test.register(parser.add_subparsers(dest="command", required=True))
        return parser.parse_args(["test", *arguments])

    def artifact(self, name="first", suite="core"):
        artifact = executable_artifact(self.root / name, target=f"//fixture:{name}")
        return replace(artifact, requested_by=(test.SUITES[suite].target,))

    def test_tooling_route_never_selects_a_native_sdk_or_bazel_tree(self):
        args = self.arguments("--tooling", "--no-build")
        with (
            mock.patch.object(test.bazel_workflows, "validate_request") as validate,
            mock.patch.object(test.bazel_workflows, "prepare_artifacts") as prepare,
            mock.patch.object(test.tooltest, "run", return_value=0) as tooling,
        ):
            self.assertEqual(test.run_command(args), 0)
        self.assertFalse(test.requires_build_environment(args))
        self.assertTrue(test.requires_python_tools(args))
        tooling.assert_called_once_with(log=None)
        validate.assert_not_called()
        prepare.assert_not_called()

    def test_group_membership_comes_from_configured_query_not_an_executable_inventory(self):
        first, second = self.artifact("first"), self.artifact("second")
        args = self.arguments("--core", "--no-build", "--path", str(self.root))
        with mock.patch.object(
            test.bazel_workflows,
            "prepare_artifacts",
            return_value=contextlib.nullcontext((first, second)),
        ) as prepare:
            with test._suite_artifacts(args, ("core",), no_build=True) as grouped:
                self.assertEqual(grouped["core"], (first, second))
        prepare.assert_called_once_with(args, (test.SUITES["core"].target,), no_build=True, expand_tests=True)

    def test_split_suite_sums_zero_matches_and_skipped_matches_across_executables(self):
        first, second = self.artifact("first"), self.artifact("second")
        invocations = []

        def child(command, *, env, **options):
            self.assertIn(command[0], (str(first.executable), str(second.executable)))
            self.assertIn("--allow-running-no-tests", command)
            self.assertIn("Contract - *", command)
            self.assertTrue(Path(env["TEST_TMPDIR"]).is_dir())
            self.assertEqual(env["TEST_WORKSPACE"], "_main")
            invocations.append(command[0])
            (report,) = [
                Path(argument.removeprefix("xml::out=")) for argument in command if argument.startswith("xml::out=")
            ]
            skips = "0" if command[0] == str(first.executable) else "2"
            report.write_text(
                '<Catch2TestRun><OverallResultsCases successes="0" failures="0" expectedFailures="0" skips="'
                + skips
                + '"/></Catch2TestRun>',
                encoding="utf-8",
            )
            return 0

        with mock.patch.object(test, "run", side_effect=child):
            outcome = test.run_catch2_suite(
                "core",
                self.root,
                test_filter="Contract - *",
                report_matches=True,
                artifacts=(first, second),
            )
        self.assertEqual(outcome, test.SuiteOutcome(0, 2))
        self.assertEqual(invocations, [str(first.executable), str(second.executable)])

    def test_split_suite_rejects_globally_empty_or_unreadable_matching_reports(self):
        artifacts = (self.artifact("first"), self.artifact("second"))
        for readable in (True, False):

            def child(command, *, env, readable=readable, **options):
                if readable:
                    (report,) = [
                        Path(argument.removeprefix("xml::out="))
                        for argument in command
                        if argument.startswith("xml::out=")
                    ]
                    report.write_text(
                        '<Catch2TestRun><OverallResultsCases successes="0" failures="0" '
                        'expectedFailures="0" skips="0"/></Catch2TestRun>',
                        encoding="utf-8",
                    )
                return 0

            with self.subTest(readable=readable), mock.patch.object(test, "run", side_effect=child):
                if readable:
                    with self.assertRaises(SystemExit):
                        test.run_catch2_suite("core", self.root, test_filter="absent", artifacts=artifacts)
                else:
                    self.assertNotEqual(
                        test.run_catch2_suite("core", self.root, test_filter="absent", artifacts=artifacts).status, 0
                    )

    def test_repeat_stops_at_first_failure_without_repreparing_or_rebuilding(self):
        artifact = self.artifact()
        with (
            mock.patch.object(
                test, "run_catch2_suite", side_effect=[test.SuiteOutcome(0), test.SuiteOutcome(9)]
            ) as run,
            mock.patch.object(test.bazel_workflows, "prepare_artifacts") as prepare,
        ):
            self.assertEqual(test.run_suites(("core",), self.root, repeat=4, artifacts={"core": (artifact,)}), 9)
        self.assertEqual(run.call_count, 2)
        self.assertTrue(all(call.kwargs["shards"] == 1 for call in run.call_args_list))
        prepare.assert_not_called()

    def test_concurrency_automatically_excludes_hidden_manual_cases(self):
        args = self.arguments("--concurrency", "--no-build", "--path", str(self.root))
        with (
            mock.patch.object(test, "_suite_artifacts", return_value=contextlib.nullcontext({})),
            mock.patch.object(test, "run_suites", return_value=0) as run,
        ):
            self.assertEqual(test.run_command(args), 0)
        self.assertEqual(run.call_args.kwargs["test_filter"], "[concurrency]~[.]")
        self.assertTrue(run.call_args.kwargs["concurrency"])
        self.assertNotIn("winui", run.call_args.args[0])
        self.assertNotIn("appkit", run.call_args.args[0])

    def test_listing_lint_names_needs_neither_sdk_nor_cached_fixture_action(self):
        args = self.arguments("--lint", "--list")
        with mock.patch.object(test.bazel_workflows, "prepare_artifacts") as prepare:
            self.assertEqual(test.run_command(args), 0)
        prepare.assert_not_called()
        self.assertFalse(test.requires_build_environment(args))

    def test_lint_filter_is_rejected_before_any_native_preparation_or_assertions(self):
        args = self.arguments("--lint", "a Catch2 filter")
        with (
            mock.patch.object(test.bazel_workflows, "prepare_artifacts") as artifacts,
            mock.patch.object(test.bazel_workflows, "prepare_lint_fixtures") as lint,
            self.assertRaises(SystemExit),
        ):
            test.run_command(args)
        artifacts.assert_not_called()
        lint.assert_not_called()

    def test_requested_feature_off_omits_group_suite_but_rejects_explicit_suite(self):
        args = self.arguments("--all", "--system-media", "off")
        self.assertNotIn("systemmedia", test.configured_suites_for("all", self.root, args=args))
        with self.assertRaises(SystemExit):
            test.configured_suites_for("systemmedia", self.root, args=args)

    def test_backend_owns_private_bus_and_audio_services_through_native_exit(self):
        artifact = self.artifact(suite="backend")
        display = {"DISPLAY": ":owned", "DBUS_SESSION_BUS_ADDRESS": "unix:owned", "AOBUS_OWNED_GTK_BUS": "unix:owned"}
        ownership = []

        @contextlib.contextmanager
        def pipewire(log_dir, environment):
            self.assertEqual(environment, display)
            ownership.append(True)
            try:
                yield environment | {"AOBUS_REQUIRE_PIPEWIRE": "1", "PIPEWIRE_REMOTE": "private"}
            finally:
                ownership.append(False)

        def child(command, *, env, **options):
            self.assertEqual(command, [str(artifact.executable)])
            self.assertEqual(ownership, [True])
            self.assertEqual(env["DBUS_SESSION_BUS_ADDRESS"], "unix:owned")
            self.assertEqual(env["PIPEWIRE_REMOTE"], "private")
            self.assertEqual(env["AOBUS_REQUIRE_PIPEWIRE"], "1")
            self.assertEqual(env["RUNFILES_DIR"], str(artifact.runfiles_directory))
            return 0

        with (
            mock.patch.object(test, "virtual_gtk_displays", return_value=contextlib.nullcontext([display])),
            mock.patch.object(bazel_test_services, "private_pipewire", side_effect=pipewire),
            mock.patch.object(test, "run", side_effect=child),
        ):
            self.assertEqual(test.run_catch2_suite("backend", self.root, artifacts=(artifact,)).status, 0)
        self.assertEqual(ownership, [True, False])

    def test_exact_xml_tallies_include_skips_and_expected_failures(self):
        report = self.root / "shard.xml"
        report.write_text(
            """<Catch2TestRun>
<OverallResults successes="11" failures="1" expectedFailures="2" skips="0"/>
<OverallResultsCases successes="3" failures="1" expectedFailures="1" skips="2"/>
</Catch2TestRun>""",
            encoding="utf-8",
        )
        assertions, cases = test._read_shard_totals(report)
        self.assertEqual(
            test._shard_summary("Core", assertions, cases, 3),
            "Core: 5 test cases, 14 assertions, 1 failed, 2 skipped across 3 shards",
        )
        self.assertEqual(test._read_matching_tests(report, list_only=False), 7)


if __name__ == "__main__":
    unittest.main()
