"""Checks for scope selection, evidence admission and failure propagation in `ao check`."""

import contextlib
import io
import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao.__main__ import make_parser, parse_arguments
from ao.command import check as check_command
from ao.core import bazel_native
from ao.core.build_lock import build_tree_lock
from ao.portal import bazel_attempt, bazel_lint, bazel_workspace, build_dir, completion, lint_scope
from ao.portal.bazel_test_ownership import ConfiguredTarget
from ao.portal.source_scope import SourceScope

CHECKSUM = "c" * 64
UNIT_TEST = "//test:unit"
REPOSITORY_SUITE = "//test/script:standalone_repository_checks"
PRODUCTION_LINT = "//test/lint:production_lint"
LINT_BASE = "b" * 40
LINT_FILE = "lib/query/Parser.cpp"


class CheckCommandTest(unittest.TestCase):
    def test_completion_rejects_partial_sanitizer_and_retired_selectors(self):
        for arguments in (
            ["//test:unit"],
            ["--asan"],
            ["--tsan"],
            ["--config", "asan"],
            ["--config", "tsan"],
            ["--ci"],
            ["--full"],
            ["--extend", "--lint-base", LINT_BASE],
        ):
            with (
                self.subTest(arguments=arguments),
                self.assertRaises(SystemExit),
                contextlib.redirect_stderr(io.StringIO()),
            ):
                parse_arguments(make_parser(), ["check", *arguments])

    def test_repository_rejects_completion_flags_on_either_side_of_the_subject(self):
        for arguments in (["--fresh", "repository"], ["repository", "--native-only"]):
            with (
                self.subTest(arguments=arguments),
                self.assertRaises(SystemExit),
                contextlib.redirect_stderr(io.StringIO()),
            ):
                parse_arguments(make_parser(), ["check", *arguments])

    def test_repository_requires_linux_build_environment_but_not_portal_python_tools(self):
        args = parse_arguments(make_parser(), ["check", "repository"])
        with mock.patch.object(bazel_native, "native_platform", return_value="linux"):
            self.assertTrue(check_command.requires_build_environment(args))
            self.assertFalse(check_command.requires_python_tools(args))
        with mock.patch.object(bazel_native, "native_platform", return_value="windows"):
            for policy in (check_command.requires_build_environment, check_command.requires_python_tools):
                with (
                    self.subTest(policy=policy.__name__),
                    self.assertRaises(SystemExit),
                    contextlib.redirect_stderr(io.StringIO()),
                ):
                    policy(args)

    def test_completion_rejects_sampling_and_non_windows_desktop_requests(self):
        with mock.patch.object(build_dir, "platform_profile", return_value=build_dir.LINUX_PROFILE):
            for request in (completion.Request(config="profile"), completion.Request(desktop_tests=True)):
                with self.subTest(request=request), self.assertRaises(bazel_native.BazelWorkspaceError):
                    completion.validate(request)

    def test_personal_rc_and_ambient_repo_selectors_cannot_filter_managed_checks(self):
        with (
            tempfile.TemporaryDirectory() as temp,
            mock.patch.dict(
                os.environ,
                {
                    "BAZELRC": "personal",
                    "AOBUS_BAZEL_CONFIGURATION": "unrelated",
                    "AOBUS_STATE_ROOT": str(Path(temp) / "state"),
                },
            ),
            mock.patch.object(bazel_workspace, "_linux_server_javabase", return_value=Path("jdk")),
        ):
            argv = bazel_workspace.invocation(Path("bazel"), Path("selected"), isolate_rc=True)
            env = bazel_workspace.launch_environment(isolate_rc=True)
        self.assertTrue({"--nosystem_rc", "--nohome_rc", "--noworkspace_rc"}.issubset(argv))
        self.assertNotIn("BAZELRC", env)
        self.assertNotIn("AOBUS_BAZEL_CONFIGURATION", env)


class CheckInvocationTest(unittest.TestCase):
    def setUp(self):
        root = Path(self.enterContext(tempfile.TemporaryDirectory())).resolve()
        self.tree = root / "tree"
        self.enterContext(
            mock.patch.dict(os.environ, {"AOBUS_STATE_ROOT": str(root / "state"), "AOBUS_BAZEL_DISK_CACHE": ""})
        )
        self.enterContext(mock.patch.object(bazel_native, "native_platform", return_value="linux"))
        self.enterContext(mock.patch.object(build_dir, "platform_profile", return_value=build_dir.LINUX_PROFILE))
        self.enterContext(mock.patch.object(bazel_workspace, "bazel_executable", return_value=Path("bazel")))
        self.enterContext(mock.patch.object(bazel_workspace, "_linux_server_javabase", return_value=root / "jdk"))
        self.bazel = self.enterContext(mock.patch.object(bazel_workspace.proc, "run"))
        self.enterContext(contextlib.redirect_stdout(io.StringIO()))

    def check(self, *arguments):
        args = parse_arguments(make_parser(), ["check", *arguments, "--path", str(self.tree)])
        return check_command.run_command(args)

    def test_native_only_checks_the_native_partition_and_propagates_test_failure(self):
        self.bazel.side_effect = [0, 3]
        self.assertEqual(
            self.check(
                "--native-only",
                "--extend",
                "--config",
                "release",
                "--clean",
                "--fresh",
                "--jobs",
                "3",
                "--remote-cache",
                "https://cache.invalid",
            ),
            3,
        )
        clean, test = (call.args[0] for call in self.bazel.call_args_list)
        self.assertEqual(clean[-1], "clean")
        command, separator = test.index("test"), test.index("--")
        self.assertIn("--nohome_rc", test[:command])
        self.assertEqual(test[separator:], ["--", "//...", "-" + REPOSITORY_SUITE])
        self.assertLessEqual(
            {
                "--keep_going",
                "--lockfile_mode=error",
                "--jobs=3",
                "--remote_cache=https://cache.invalid",
                "--nocache_test_results",
                "--nozip_undeclared_test_outputs",
            },
            set(test[command + 1 : separator]),
        )

    def test_full_lint_uses_all_targets_and_normal_caches(self):
        self.bazel.return_value = 3
        disk_cache = self.tree.parent / "disk-cache"
        with mock.patch.dict(os.environ, {"AOBUS_BAZEL_DISK_CACHE": str(disk_cache)}):
            for flags in ((), ("--extend",)):
                with (
                    self.subTest(flags=flags),
                    mock.patch.object(lint_scope, "plan", return_value=lint_scope.Plan(None, "unbounded")) as plan,
                ):
                    self.bazel.reset_mock()
                    self.assertEqual(self.check(*flags), 3)
                    self.bazel.assert_called_once()
                    argv = self.bazel.call_args.args[0]
                    options = argv[argv.index("test") + 1 :]
                    self.assertEqual([arg for arg in options if not arg.startswith("--")], ["//..."])
                    self.assertNotIn("--nocache_test_results", options)
                    self.assertNotIn("--keep_going", options)
                    if flags:
                        plan.assert_not_called()
                    else:
                        plan.assert_called_once_with(None)
        rc = (self.tree / "configuration" / "aobus.bazelrc").read_text(encoding="utf-8")
        self.assertIn(f'build "--disk_cache={disk_cache.as_posix()}"', rc.splitlines())

    def test_invalid_git_binding_stops_completion_before_bazel(self):
        with (
            mock.patch.dict(os.environ, {"AOBUS_GIT_DIR": str(self.tree.parent / "metadata")}),
            contextlib.redirect_stderr(io.StringIO()) as stderr,
            self.assertRaises(SystemExit) as error,
        ):
            os.environ.pop("AOBUS_GIT_WORK_TREE", None)
            self.check()
        self.assertNotEqual(error.exception.code, 0)
        self.assertIn("AOBUS_GIT_WORK_TREE", stderr.getvalue())
        self.bazel.assert_not_called()

    def test_scoped_lint_replaces_the_full_graph_gate_under_the_same_tree_lock(self):
        @contextlib.contextmanager
        def lock(tree):
            self.assertEqual(tree, self.tree)
            with build_tree_lock(tree):
                phases.append("lock")
                yield
            phases.append("unlock")

        phases = []
        files = (LINT_FILE,)
        self.bazel.side_effect = lambda *args, **kwargs: phases.append("bazel") or 0
        with (
            mock.patch.object(completion, "build_tree_lock", side_effect=lock),
            mock.patch.object(lint_scope, "plan", return_value=lint_scope.Plan(files, "scoped")) as plan,
            mock.patch.object(completion, "_verify_required_checks", side_effect=lambda *a, **kw: phases.append("bep")),
            mock.patch.object(bazel_lint, "run", side_effect=lambda *a, **kw: phases.append("lint") or 1) as lint,
        ):
            self.assertEqual(self.check("--native-only", "--lint-base", LINT_BASE, "--jobs", "3"), 1)
        self.assertEqual(phases, ["lock", "bazel", "bep", "lint", "unlock"])
        plan.assert_called_once_with(LINT_BASE)
        request = lint.call_args.args[0]
        lint.assert_called_once_with(request, SourceScope(files), lockfile_readonly=True)
        self.assertEqual((request.path, request.jobs), (str(self.tree), 3))
        argv = self.bazel.call_args.args[0]
        targets = argv[argv.index("--") + 1 :]
        self.assertEqual(targets[0], "//...")
        # The wildcard also builds the alias, generated gate and filegroups.
        self.assertLessEqual(
            {
                "-" + REPOSITORY_SUITE,
                "-//:production_lint",
                "-" + PRODUCTION_LINT,
                "-" + PRODUCTION_LINT + ".gate",
                "-" + PRODUCTION_LINT + ".gate_result",
                "-" + PRODUCTION_LINT + ".gate_log",
            },
            set(targets[1:]),
        )

    def test_no_scoped_lint_runs_with_no_files_or_a_failed_native_gate(self):
        for files, native_status in (((), 0), ((LINT_FILE,), 3)):
            self.bazel.reset_mock()
            self.bazel.return_value = native_status
            with (
                self.subTest(files=files, native_status=native_status),
                mock.patch.object(lint_scope, "plan", return_value=lint_scope.Plan(files, "scoped")),
                mock.patch.object(completion, "_verify_required_checks"),
                mock.patch.object(bazel_lint, "run") as lint,
            ):
                self.assertEqual(self.check(), native_status)
                self.bazel.assert_called_once()
                self.assertIn("-" + PRODUCTION_LINT, self.bazel.call_args.args[0])
                lint.assert_not_called()

    def test_windows_desktop_tests_enable_the_graph_and_require_interactive_evidence(self):
        self.bazel.return_value = 0
        with (
            mock.patch.object(bazel_native, "native_platform", return_value="windows"),
            mock.patch.object(build_dir, "platform_profile", return_value=build_dir.WINDOWS_PROFILE),
            mock.patch.object(bazel_workspace.providers, "vcpkg_prefix", return_value=self.tree / "sdk"),
            mock.patch.object(bazel_workspace.bazel_environment, "windows_compiler_selection", return_value={}),
            mock.patch.dict(os.environ, {"AOBUS_NUGET_PACKAGES_DIR": str(self.tree / "nuget")}),
            mock.patch.object(completion, "_verify_required_checks") as verify,
        ):
            self.assertEqual(self.check("--desktop-tests", "--extend"), 0)
        self.assertEqual(self.bazel.call_args.args[0][-1], "//...")
        rc = (self.tree / "configuration" / "aobus.bazelrc").read_text(encoding="utf-8")
        self.assertIn("build --//bazel:desktop_tests=true", rc.splitlines())
        selection = verify.call_args.args[1]
        self.assertLessEqual(
            {"//test:winui_uia_test", "//test:winui_uia_selection_test"}, selection.required - selection.optional
        )

    def test_clean_failure_stops_before_tests(self):
        self.bazel.return_value = 9
        self.assertEqual(self.check("--extend", "--clean"), 9)
        self.bazel.assert_called_once()
        self.assertEqual(self.bazel.call_args.args[0][-1], "clean")

    def test_repository_runs_only_its_suite_and_propagates_test_failure(self):
        self.bazel.return_value = 7
        self.assertEqual(self.check("repository", "--jobs", "3"), 7)
        self.bazel.assert_called_once()
        argv = self.bazel.call_args.args[0]
        options = argv[argv.index("test") + 1 :]
        self.assertEqual([arg for arg in options if not arg.startswith("--")], [REPOSITORY_SUITE])
        self.assertIn("--jobs=3", options)
        self.assertIn("--keep_going", options)

    def test_zero_bazel_exit_without_evidence_does_not_complete_the_check(self):
        self.bazel.return_value = 0
        with self.assertRaises(bazel_native.BazelWorkspaceError):
            completion.run(completion.Request(extend=True, path=str(self.tree)))


class RequiredScopeTest(unittest.TestCase):
    def test_native_only_partitions_repository_evidence_but_default_requires_both_sets(self):
        repository = completion._repository_selection()
        native_only = completion._selection("linux", completion.Request(native_only=True))
        default = completion._selection("linux", completion.Request())
        self.assertIn(REPOSITORY_SUITE, repository.suites)
        self.assertIn("//test/script:native_repository_checks", native_only.suites)
        self.assertIn("//test/script:repository_checks", default.suites)
        standalone = repository.required - repository.optional
        native = native_only.required - native_only.optional
        self.assertIn("//test/script:format_docs", standalone)
        self.assertIn("//test/script:format_sources", native)
        self.assertTrue(standalone.isdisjoint(native))
        self.assertEqual(standalone | native, default.required - default.optional)
        self.assertEqual(default.targets, ("//...",))

    def test_short_test_timeout_applies_only_to_github_hosted_windows_native_checks(self):
        for platform, native_only, runner, override in (
            ("windows", True, "github-hosted", True),
            ("windows", True, None, False),
            ("windows", True, "self-hosted", False),
            ("windows", False, "github-hosted", False),
            ("linux", True, "github-hosted", False),
            ("macos", True, "github-hosted", False),
        ):
            environment = {} if runner is None else {"RUNNER_ENVIRONMENT": runner}
            with (
                self.subTest(platform=platform, native_only=native_only, runner=runner),
                mock.patch.dict(os.environ, environment, clear=True),
            ):
                selection = completion._selection(platform, completion.Request(native_only=native_only))
                timeouts = tuple(option for option in selection.options if option.partition("=")[0] == "--test_timeout")
                self.assertEqual(timeouts, ("--test_timeout=300,300,900,3600",) if override else ())

    def test_scoped_lint_waives_only_full_lint_evidence(self):
        request = completion.Request(native_only=True, desktop_tests=True)
        full = completion._selection("windows", request)
        scoped = completion._selection("windows", request, scoped_lint=True)
        required = full.required - full.optional
        self.assertIn(PRODUCTION_LINT, required)
        self.assertEqual(scoped.suites, full.suites)
        self.assertEqual(scoped.required - scoped.optional, required - {PRODUCTION_LINT})
        self.assertLessEqual({"//test/lint:lint_test", "//test/script:bazel_lint"}, scoped.required - scoped.optional)

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
                selection = completion._selection(platform, completion.Request())
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

    def test_without_desktop_tests_neither_interactive_probe_is_required(self):
        probes = {"//test:winui_uia_test", "//test:winui_uia_selection_test"}
        for platform in ("windows", "linux", "macos"):
            for native_only in (False, True):
                with self.subTest(platform=platform, native_only=native_only):
                    selection = completion._selection(platform, completion.Request(native_only=native_only))
                    self.assertTrue(probes.isdisjoint(selection.required))

    def test_appkit_supervision_requires_evidence_in_both_macos_partitions(self):
        label = "//test/script:test_appkit_scenarios"
        for native_only in (False, True):
            with self.subTest(native_only=native_only):
                selection = completion._selection("macos", completion.Request(native_only=native_only))
                self.assertIn(label, selection.required - selection.optional)

    def test_winui_projection_is_required_only_on_windows(self):
        label = "//test/winui/projection:winui_projection_test"
        for platform in ("linux", "macos", "windows"):
            with self.subTest(platform=platform):
                selection = completion._selection(platform, completion.Request())
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

    LINUX_INTERPOSED_TESTS = ("//test:dictionary_publication_test", "//test:library_import_apply_cancellation_test")

    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.tree = Path(temp.name).resolve()
        self.path = self.tree / "tests.bep.json"
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
        completion._verify_required_checks(self.path, self.scope, tree=self.tree, fresh=fresh)

    def policy_result(self, labels, *, directory=None, status="PASSED", config=CHECKSUM):
        manifest = (directory or self.tree) / "registered-test-labels.json"
        manifest.write_text(json.dumps(labels), encoding="utf-8")
        return {
            "id": {"testResult": {"label": "//:native_policy", "configuration": {"id": config}}},
            "testResult": {
                "status": status,
                "testActionOutput": [{"name": "test.outputs/registered-test-labels.json", "uri": manifest.as_uri()}],
            },
        }

    def test_registered_owner_requires_its_public_pass_even_when_not_selected_by_a_suite(self):
        public = "//test:fixture_wrapper"
        self.scope = completion._CheckSelection({"//:suite"}, {"//:native_policy"}, {public, "//test:foreign"})
        base = [self.expansion, _summary(), self.policy_result([public]), _summary("//:native_policy")]
        # Even a passing inner test is not a run of the wrapper and its fixtures.
        for evidence in ([], [_summary("//test:inner.native")]):
            with self.subTest(evidence=evidence), self.assertRaises(bazel_native.BazelWorkspaceError):
                self.verify([*base, *evidence, self.finish, self.last])
        events = [*base, _summary(public, cached=1), self.finish, self.last]
        self.verify(events)
        with self.assertRaises(bazel_native.BazelWorkspaceError):
            self.verify(events, fresh=True)
        self.verify([*base, _summary(public), self.finish, self.last], fresh=True)

    def test_owner_and_audit_summaries_must_match_the_audit_results_configuration(self):
        self.scope = completion._CheckSelection(set(), {"//:native_policy"}, set())
        result = self.policy_result([UNIT_TEST])
        for other in (UNIT_TEST, "//:native_policy"):
            summaries = [
                _summary(label, config="other" if label == other else CHECKSUM)
                for label in (UNIT_TEST, "//:native_policy")
            ]
            with self.subTest(label=other), self.assertRaises(bazel_native.BazelWorkspaceError) as caught:
                self.verify([result, *summaries, self.finish, self.last])
            self.assertIn(other, str(caught.exception))

    def test_native_completion_requires_one_passing_ownership_result_with_its_output(self):
        self.scope = completion._CheckSelection(set(), {"//:native_policy"}, set())
        valid = self.policy_result([UNIT_TEST])
        missing_output = self.policy_result([UNIT_TEST])
        missing_output["testResult"]["testActionOutput"] = []
        summaries = [_summary("//:native_policy"), _summary()]
        self.verify([valid, *summaries, self.finish, self.last])
        for reason, evidence in (
            ("no result", []),
            ("no output", [missing_output]),
            ("failed result", [self.policy_result([UNIT_TEST], status="FAILED")]),
            ("ambiguous results", [valid, self.policy_result([UNIT_TEST], config="other")]),
        ):
            with self.subTest(reason=reason), self.assertRaises(bazel_native.BazelWorkspaceError):
                self.verify([*evidence, *summaries, self.finish, self.last])

    def test_incomplete_audit_evidence_identifies_the_missing_field(self):
        self.scope = completion._CheckSelection(set(), {"//:native_policy"}, set())
        for field, diagnostic in (("configuration", "configuration id"), ("testActionOutput", "testActionOutput")):
            result = self.policy_result([UNIT_TEST])
            section = result["id"]["testResult"] if field == "configuration" else result["testResult"]
            del section[field]
            with self.subTest(field=field), self.assertRaises(bazel_native.BazelWorkspaceError) as caught:
                self.verify([result, _summary("//:native_policy"), _summary(), self.finish, self.last])
            self.assertIn("//:native_policy", str(caught.exception))
            self.assertIn(diagnostic, str(caught.exception))

    def test_native_completion_requires_a_nonempty_list_of_labels_from_the_owned_tree(self):
        self.scope = completion._CheckSelection(set(), {"//:native_policy"}, set())
        outside = Path(self.enterContext(tempfile.TemporaryDirectory())).resolve()
        summaries = [_summary("//:native_policy"), _summary()]
        for reason, labels, directory in (
            ("empty list", [], self.tree),
            ("object instead of list", {UNIT_TEST: True}, self.tree),
            ("valid list from another tree", [UNIT_TEST], outside),
        ):
            result = self.policy_result(labels, directory=directory)
            with self.subTest(reason=reason), self.assertRaises(bazel_native.BazelWorkspaceError):
                self.verify([result, *summaries, self.finish, self.last])

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

    def test_interposed_tests_require_a_linux_pass_even_without_suite_discovery(self):
        self.scope = completion._selection("linux", completion.Request())
        for label in self.LINUX_INTERPOSED_TESTS:
            for discovered in ((), (label,)):
                events = [
                    self.expanded_check_suites(*discovered),
                    self.policy_result([UNIT_TEST]),
                    _summary(),
                    *(_summary(item) for item in self.scope.required - {label}),
                ]
                for evidence in ([], [_skipped(label, "skip-cfg")], [_summary(label, status="FAILED")]):
                    with (
                        self.subTest(label=label, discovered=discovered, evidence=evidence),
                        self.assertRaises(bazel_native.BazelWorkspaceError),
                    ):
                        self.verify([*events, *evidence, self.finish, self.last])
                self.verify([*events, _summary(label), self.finish, self.last])

    def test_foreign_linux_test_skips_do_not_hide_missing_or_skipped_required_checks(self):
        labels = self.LINUX_INTERPOSED_TESTS
        for platform in ("macos", "windows"):
            with self.subTest(platform=platform):
                self.scope = completion._selection(platform, completion.Request())
                base = [
                    self.expanded_check_suites(*labels),
                    self.policy_result([UNIT_TEST]),
                    _summary(),
                    *(_skipped(label, "skip-cfg") for label in labels),
                ]
                for gate in ("//:native_policy", "//test/lint:production_lint"):
                    others = [_summary(item) for item in self.scope.required - {gate}]
                    for evidence in ([], [_skipped(gate, "skip-cfg")]):
                        with (
                            self.subTest(gate=gate, evidence=evidence),
                            self.assertRaises(bazel_native.BazelWorkspaceError),
                        ):
                            self.verify([*base, *others, *evidence, self.finish, self.last])
                self.verify([*base, *(_summary(item) for item in self.scope.required), self.finish, self.last])

    def test_appkit_editor_skip_is_allowed_off_macos_after_suite_expansion(self):
        label = "//test:appkit_editor_test"
        for platform in ("linux", "windows"):
            with self.subTest(platform=platform):
                self.scope = completion._selection(platform, completion.Request())
                self.verify(
                    [
                        self.expanded_check_suites(label),
                        self.policy_result([UNIT_TEST]),
                        _summary(),
                        _skipped(label, "skip-cfg"),
                        *(_summary(item) for item in self.scope.required - {label}),
                        self.finish,
                        self.last,
                    ]
                )

    def test_appkit_editor_requires_a_macos_pass_even_without_suite_discovery(self):
        label = "//test:appkit_editor_test"
        self.scope = completion._selection("macos", completion.Request())
        events = [
            self.expanded_check_suites(),
            self.policy_result([UNIT_TEST]),
            _summary(),
            *(_summary(item) for item in self.scope.required - {label}),
        ]
        for evidence in ([], [_skipped(label, "skip-cfg")]):
            with self.subTest(evidence=evidence), self.assertRaises(bazel_native.BazelWorkspaceError) as caught:
                self.verify([*events, *evidence, self.finish, self.last])
            self.assertIn(label, str(caught.exception))
        self.verify([*events, _summary(label), self.finish, self.last])

    def test_windows_desktop_tests_require_a_passing_selection_probe_even_when_the_other_uia_probe_passes(self):
        label = "//test:winui_uia_selection_test"
        self.scope = completion._selection("windows", completion.Request(desktop_tests=True))
        events = [
            self.expanded_check_suites(),
            self.policy_result([UNIT_TEST]),
            _summary(),
            *(_summary(item) for item in self.scope.required - {label}),
        ]
        with self.assertRaisesRegex(bazel_native.BazelWorkspaceError, f"missing required passing checks: {label}"):
            self.verify([*events, self.finish, self.last])
        self.verify([*events, _summary(label), self.finish, self.last])

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
