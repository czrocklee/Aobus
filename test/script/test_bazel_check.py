"""Checks for scope selection, evidence admission and failure propagation in `ao check`."""

import contextlib
import io
import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao.__main__ import make_parser
from ao.command import check as check_command
from ao.core import bazel_native
from ao.portal import bazel_attempt, bazel_workspace, build_dir, completion
from ao.portal.bazel_test_ownership import ConfiguredTarget

CHECKSUM = "c" * 64
UNIT_TEST = "//test:unit"


class CheckCommandTest(unittest.TestCase):
    def test_cli_preserves_sanitizer_and_fresh_requests_without_allowing_a_partial_check(self):
        parser = make_parser()
        args = parser.parse_args(["check", "--asan", "--fresh", "--jobs", "3", "-p", "selected"])
        with (
            mock.patch.object(build_dir, "platform_profile", return_value=build_dir.LINUX_PROFILE),
            mock.patch.object(completion, "run", return_value=7) as run,
        ):
            self.assertEqual(check_command.run_command(args), 7)
        request = run.call_args.args[0]
        self.assertEqual((request.asan, request.fresh, request.jobs, request.path), (True, True, 3, "selected"))
        with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
            parser.parse_args(["check", "//test:unit"])

    def test_sampling_and_non_fastbuild_ci_are_not_admitted_as_completion(self):
        with mock.patch.object(build_dir, "platform_profile", return_value=build_dir.LINUX_PROFILE):
            for request in (completion.Request(config="profile"), completion.Request(ci=True, config="debug")):
                with self.subTest(request=request), self.assertRaises(bazel_native.BazelWorkspaceError):
                    completion.validate(request)

    def test_full_check_selects_the_whole_graph_with_the_requested_cache_and_ci_policy(self):
        request = completion.Request(ci=True, clean=True, fresh=True, jobs=3, remote_cache="https://cache.invalid")
        with (
            tempfile.TemporaryDirectory() as temp,
            mock.patch.object(bazel_workspace, "bazel_executable", return_value=Path("bazel")),
            mock.patch.object(bazel_workspace, "prepare_workspace", return_value=Path(temp)),
            mock.patch.object(bazel_workspace, "execute", side_effect=[0, 3, 9]) as execute,
            contextlib.redirect_stdout(io.StringIO()),
        ):
            self.assertEqual(completion._test_graph(request, Path(temp), "fastbuild"), 3)
            clean, test = execute.call_args_list
            self.assertEqual((clean.args[3], test.args[3], test.args[5]), ("clean", "test", ["//..."]))
            self.assertEqual((test.kwargs["jobs"], test.kwargs["remote_cache"]), (3, "https://cache.invalid"))
            self.assertTrue(test.kwargs["isolate_rc"])
            self.assertIn("--nocache_test_results", test.kwargs["options"])
            self.assertIn("--config=ci", test.kwargs["options"])
            # A failed clean must stop, rather than report an older passing result.
            self.assertEqual(completion._test_graph(request, Path(temp), "fastbuild"), 9)

    def test_zero_bazel_exit_without_evidence_does_not_complete_the_check(self):
        with (
            tempfile.TemporaryDirectory() as temp,
            mock.patch.object(bazel_workspace, "bazel_executable", return_value=Path("bazel")),
            mock.patch.object(bazel_workspace, "prepare_workspace", return_value=Path(temp)),
            mock.patch.object(bazel_workspace, "execute", return_value=0),
            contextlib.redirect_stdout(io.StringIO()),
            self.assertRaises(bazel_native.BazelWorkspaceError),
        ):
            completion._test_graph(completion.Request(), Path(temp), "fastbuild")

    def test_sanitizer_runs_in_the_selected_tree_only_after_normal_completion_passes(self):
        with (
            tempfile.TemporaryDirectory() as temp,
            mock.patch.object(build_dir, "platform_profile", return_value=build_dir.LINUX_PROFILE),
            mock.patch.object(bazel_workspace, "workspace_path", return_value=Path(temp)),
            mock.patch.object(completion, "_test_graph", side_effect=[3, 0]) as normal,
            mock.patch.object(completion.bazel_sanitizers, "run", return_value=9) as sanitizer,
        ):
            self.assertEqual(completion.run(completion.Request(asan=True)), 3)
            self.assertEqual(normal.call_args.args[2], "fastbuild")
            sanitizer.assert_not_called()
            self.assertEqual(completion.run(completion.Request(config="debug", asan=True, fresh=True)), 9)
            self.assertEqual(normal.call_args.args[1:], (Path(temp), "debug"))
            instrumented = sanitizer.call_args.args[0]
            self.assertEqual((instrumented.path, instrumented.config, instrumented.fresh), (temp, "asan", True))

    def test_personal_rc_and_ambient_repo_selectors_cannot_filter_managed_checks(self):
        with (
            mock.patch.dict(os.environ, {"BAZELRC": "personal", "AOBUS_BAZEL_CONFIGURATION": "unrelated"}),
            mock.patch.object(bazel_workspace, "_linux_server_javabase", return_value=Path("jdk")),
        ):
            argv = bazel_workspace.invocation(Path("bazel"), Path("selected"), isolate_rc=True)
            env = bazel_workspace.launch_environment(isolate_rc=True)
        self.assertTrue({"--nosystem_rc", "--nohome_rc", "--noworkspace_rc"}.issubset(argv))
        self.assertNotIn("BAZELRC", env)
        self.assertNotIn("AOBUS_BAZEL_CONFIGURATION", env)


class RequiredScopeTest(unittest.TestCase):
    def test_losing_a_published_check_from_its_suite_does_not_remove_the_requirement(self):
        scope = completion._selection("linux", completion.Request())
        for label in (
            "//test:backend_test",
            "//test:media_test",
            "//test/script:ruff",
            "//:instrumentation_tests_coverage",
        ):
            self.assertIn(label, scope.required)

    def test_gtk_and_system_media_can_be_disabled_independently(self):
        for gtk, media, enabled, disabled in (
            ("off", "on", "//test:systemmedia_test", "//test:gtk_test"),
            ("on", "off", "//test:gtk_test", "//test:systemmedia_test"),
        ):
            scope = completion._selection("linux", completion.Request(gtk=gtk, system_media=media))
            self.assertIn(enabled, scope.required - scope.optional)
            self.assertIn(disabled, scope.optional)
        self.assertIn("//test:gtk_test", completion._selection("linux", completion.Request()).required)

    def test_header_sharing_is_a_linux_only_evidence_requirement(self):
        sparse_headers = "//bazel/header_share:sparse_headers_test"
        for platform in ("linux", "macos", "windows"):
            with self.subTest(platform=platform):
                selection = completion._selection(platform, completion.Request(ci=True))
                for label in ("//bazel/header_share:selector_test", sparse_headers):
                    self.assertEqual(label in selection.required - selection.optional, platform == "linux")
                # //:check_tests expands the consumer even on hosts that skip it.
                self.assertEqual(sparse_headers in selection.optional, platform != "linux")

    def test_windows_header_interfaces_require_evidence_only_on_windows(self):
        label = "//bazel/windows_header_share:mode_interfaces_test"
        for platform in ("linux", "macos", "windows"):
            with self.subTest(platform=platform):
                selection = completion._selection(platform, completion.Request())
                self.assertEqual(label in selection.required, platform == "windows")
                self.assertEqual(label in selection.optional, platform != "windows")

    def test_only_windows_ci_requires_the_interactive_probe(self):
        uia = "//test:winui_uia_test"
        self.assertIn(uia, completion._selection("windows", completion.Request(ci=True)).required)
        self.assertNotIn(uia, completion._selection("windows", completion.Request()).required)
        self.assertNotIn(uia, completion._selection("linux", completion.Request(ci=True)).required)

    def test_winui_projection_is_required_only_on_windows(self):
        label = "//test/winui/projection:winui_projection_test"
        for platform in ("linux", "macos", "windows"):
            for ci in (False, True):
                with self.subTest(platform=platform, ci=ci):
                    selection = completion._selection(platform, completion.Request(ci=ci))
                    self.assertEqual(label in selection.required - selection.optional, platform == "windows")


def _summary(label=UNIT_TEST, *, status="PASSED", cached=0, config=CHECKSUM):
    return {
        "id": {"testSummary": {"label": label, "configuration": {"id": config}}},
        "testSummary": {"overallStatus": status, "totalRunCount": 1, "totalNumCached": cached},
    }


def _skipped(label, config, *, aspect=None):
    completed = {"label": label, "configuration": {"id": config}}
    if aspect:
        completed["aspect"] = aspect
    return {
        "id": {"targetCompleted": completed},
        "aborted": {"reason": "SKIPPED", "description": f"Target {label} build was skipped."},
    }


class CompletionEvidenceTest(unittest.TestCase):
    """One expanded suite, one permitted skip and one independently required test."""

    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.path = Path(temp.name) / "tests.bep.json"
        self.scope = completion._CheckSelection(suites={"//:suite"}, required={"//:gate"}, optional={"//test:foreign"})
        self.expansion = {
            "id": {"pattern": {"pattern": ["//..."]}},
            "expanded": {
                "testSuiteExpansions": [{"suiteLabel": "//:suite", "testLabels": [UNIT_TEST, "//test:foreign"]}]
            },
        }
        self.finish = {"id": {"buildFinished": {}}, "finished": {"overallSuccess": True}}
        self.last = {"id": {"buildMetrics": {}}, "lastMessage": True}

    def verify(self, events, *, fresh=False):
        self.path.write_text("\n".join(map(json.dumps, events)), encoding="utf-8")
        completion._verify_required_checks(self.path, self.scope, fresh=fresh)

    def test_cached_pass_counts_unless_a_fresh_execution_was_requested(self):
        events = [self.expansion, _summary(cached=1), _summary("//:gate"), self.finish, self.last]
        self.verify(events)
        with self.assertRaises(bazel_native.BazelWorkspaceError):
            self.verify(events, fresh=True)

    def test_missing_evidence_after_success_is_not_a_passing_gate(self):
        events = [self.expansion, _summary(), _summary("//:gate"), self.finish, self.last]
        for index, missing in enumerate(
            ("suite expansion", "suite member", "independent gate", "finish", "stream tail")
        ):
            with self.subTest(missing=missing), self.assertRaises(bazel_native.BazelWorkspaceError):
                self.verify(events[:index] + events[index + 1 :])

    def test_missing_and_skipped_required_checks_are_distinguished(self):
        skipped, missing = "//:native_policy", "//test:absent"
        self.scope = completion._CheckSelection(
            suites={"//:suite"}, required={"//:gate", UNIT_TEST, skipped, missing}, optional={"//test:foreign"}
        )
        events = [
            self.expansion,
            _summary(),
            _summary("//:gate"),
            _skipped(skipped, "skip-cfg"),
            _skipped("//test:unrelated", "other"),
            _skipped(UNIT_TEST, "other-cfg"),
            _skipped(missing, "aspect-cfg", aspect="//aspect"),
            {"id": {"started": {}}, "started": {"description": f"Target {missing} build was skipped."}},
            self.finish,
            self.last,
        ]
        with self.assertRaises(bazel_native.BazelWorkspaceError) as caught:
            self.verify(events)
        message = str(caught.exception)
        self.assertIn("missing required passing checks: //test:absent", message)
        self.assertIn("skipped required checks: //:native_policy [skip-cfg]", message)
        self.assertNotIn("//test:unrelated", message)
        self.assertNotIn(UNIT_TEST, message)
        self.scope = completion._CheckSelection(suites={"//:suite"}, required={"//:gate"}, optional={"//test:foreign"})
        failed = [
            self.expansion,
            _summary(status="FAILED"),
            _skipped(UNIT_TEST, "other-cfg"),
            _summary("//:gate"),
            self.finish,
            self.last,
        ]
        with self.assertRaises(bazel_native.BazelWorkspaceError) as caught:
            self.verify(failed)
        message = str(caught.exception)
        self.assertIn("required check did not pass: //test:unit", message)
        self.assertNotIn("skipped required checks", message)

    def expanded_check_suites(self, *members):
        return {
            "id": {"pattern": {"pattern": ["//..."]}},
            "expanded": {
                "testSuiteExpansions": [
                    {
                        "suiteLabel": suite,
                        "testLabels": [UNIT_TEST, *members] if suite == "//:check_tests" else [UNIT_TEST],
                    }
                    for suite in self.scope.suites
                ]
            },
        }

    def test_dictionary_publication_requires_a_linux_pass_even_without_suite_discovery(self):
        label = "//test:dictionary_publication_test"
        self.scope = completion._selection("linux", completion.Request())
        for discovered in ((), (label,)):
            expansions = self.expanded_check_suites(*discovered)
            events = [expansions, _summary(), *(_summary(item) for item in self.scope.required - {label})]
            for evidence in ([], [_skipped(label, "skip-cfg")], [_summary(label, status="FAILED")]):
                with (
                    self.subTest(discovered=discovered, evidence=evidence),
                    self.assertRaises(bazel_native.BazelWorkspaceError),
                ):
                    self.verify([*events, *evidence, self.finish, self.last])
            self.verify([*events, _summary(label), self.finish, self.last])

    def test_foreign_dictionary_skip_does_not_hide_missing_or_skipped_required_checks(self):
        label = "//test:dictionary_publication_test"
        for platform in ("macos", "windows"):
            with self.subTest(platform=platform):
                self.scope = completion._selection(platform, completion.Request())
                base = [self.expanded_check_suites(label), _summary(), _skipped(label, "skip-cfg")]
                for gate in ("//:native_policy", "//test/lint:production_lint"):
                    others = [_summary(item) for item in self.scope.required - {gate}]
                    for evidence in ([], [_skipped(gate, "skip-cfg")]):
                        with (
                            self.subTest(gate=gate, evidence=evidence),
                            self.assertRaises(bazel_native.BazelWorkspaceError),
                        ):
                            self.verify([*base, *others, *evidence, self.finish, self.last])
                self.verify([*base, *(_summary(item) for item in self.scope.required), self.finish, self.last])

    def test_a_required_member_must_have_one_unambiguous_pass(self):
        for summaries in (
            [_summary(status="FAILED")],
            [_summary(status="FLAKY")],
            [_summary(), _summary(config="other")],
        ):
            with self.subTest(summaries=summaries), self.assertRaises(bazel_native.BazelWorkspaceError):
                self.verify([self.expansion, *summaries, _summary("//:gate"), self.finish, self.last])


class ConfiguredResultTest(unittest.TestCase):
    """Managed instrumentation binds results to a configuration, not just a label."""

    def test_fresh_selection_matches_the_configured_target_even_with_unicode_progress(self):
        expected = frozenset({ConfiguredTarget(UNIT_TEST, CHECKSUM)})
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "tests.bep.json"
            events = [{"progress": {"stdout": "before\u2028after"}}, _summary(), {"lastMessage": True}]
            path.write_text("\n".join(json.dumps(event, ensure_ascii=False) for event in events), encoding="utf-8")
            self.assertEqual(bazel_attempt._test_results(path, expected, fresh=True)[0]["runs"], 1)
            for summary in (_summary(config="different"), _summary(cached=1)):
                path.write_text(json.dumps(summary), encoding="utf-8")
                with self.assertRaises(ValueError):
                    bazel_attempt._test_results(path, expected, fresh=True)


if __name__ == "__main__":
    unittest.main()
