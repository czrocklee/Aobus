"""Configured executable selection and no-build boundaries, not graph parity."""

import argparse
import contextlib
import io
import json
import shutil
import subprocess
import tempfile
import unittest
from dataclasses import replace
from pathlib import Path
from unittest import mock

from workflow_support import executable_artifact, lint_fixture_document

from ao.command import test as test_command
from ao.core import bazel_workflows as workflows
from ao.core import builddir
from ao.core.bazel_native import BazelWorkspaceError


class ArtifactAdmissionTest(unittest.TestCase):
    def setUp(self):
        self.tree = Path(self.enterContext(tempfile.TemporaryDirectory(prefix="aobus-workflow-")))
        self.artifact = executable_artifact(self.tree)
        self.row = {
            "label": "//fixture:program",
            "incompatible": False,
            "executable": self.artifact.executable.relative_to(self.artifact.execution_root).as_posix(),
            "files": [
                {
                    "path": self.artifact.executable.relative_to(self.artifact.execution_root).as_posix(),
                    "directory": False,
                }
            ],
            "manifest": None,
            "runfiles": [{"path": "fixture/data.txt", "short_path": "fixture/data.txt", "directory": False}],
            "environment": {},
        }

    def parse(self):
        return workflows.parse_artifact(
            self.row,
            target="//fixture:program",
            checksum="a" * 64,
            config="debug",
            tree=self.tree,
            execution_root=self.artifact.execution_root,
        )

    def test_declared_executable_wins_over_other_default_outputs(self):
        self.row["files"].insert(0, {"path": "bazel-out/native/bin/fixture/not-the-executable", "directory": False})
        self.assertEqual(self.parse().executable, self.artifact.executable)

    def test_required_runfiles_cannot_silently_disappear(self):
        shutil.rmtree(self.artifact.runfiles_directory)
        with self.assertRaisesRegex(BazelWorkspaceError, "declared runfiles"):
            self.parse()
        manifest = self.artifact.execution_root / "bazel-out/native/bin/fixture/current.manifest"
        manifest.write_text(
            f"_main/fixture/data.txt {self.artifact.execution_root / 'fixture/data.txt'}\n", encoding="utf-8"
        )
        self.row["manifest"] = manifest.relative_to(self.artifact.execution_root).as_posix()
        artifact = self.parse()
        self.assertIsNone(artifact.runfiles_directory)
        self.assertEqual(artifact.environment()["RUNFILES_MANIFEST_FILE"], str(manifest))
        self.assertEqual(artifact.environment()["TEST_SRCDIR"], "")

    def test_declared_run_environment_reaches_the_launch_beneath_the_owned_context(self):
        self.row["environment"] = {
            "AOBUS_TEST_DBUS_DAEMON": "/native/dbus-daemon",
            "TEST_SRCDIR": "/declared/elsewhere",
            "ASAN_OPTIONS": "declared",
        }
        artifact = replace(self.parse(), runtime_environment=(("ASAN_OPTIONS", "runtime"),))

        environment = artifact.environment()

        self.assertEqual(environment["AOBUS_TEST_DBUS_DAEMON"], "/native/dbus-daemon")
        self.assertEqual(environment["TEST_SRCDIR"], str(self.artifact.runfiles_directory))
        self.assertEqual(environment["ASAN_OPTIONS"], "runtime")

    def test_malformed_declared_run_environment_is_not_admitted(self):
        for environment in (None, [], {"": "value"}, {"NAME=VALUE": "value"}, {"NAME": 1}):
            with self.subTest(environment=environment):
                self.row["environment"] = environment
                with self.assertRaisesRegex(BazelWorkspaceError, "malformed declared run environment"):
                    self.parse()


class PreparationBoundaryTest(unittest.TestCase):
    def setUp(self):
        self.tree = Path(self.enterContext(tempfile.TemporaryDirectory(prefix="aobus-preparation-")))
        self.artifact = executable_artifact(self.tree)
        self.args = argparse.Namespace(
            config="debug", path=str(self.tree), jobs=3, vcpkg_prefix=None, remote_cache=None
        )
        self.actions = []
        self.ambiguous = False
        self.enterContext(mock.patch.object(builddir, "platform_profile", return_value=builddir.LINUX_PROFILE))
        self.enterContext(mock.patch.object(workflows.native, "require_supported_source"))
        self.enterContext(
            mock.patch.object(workflows.bazel_workspace, "bazel_executable", return_value=Path("/fixture/bazel"))
        )
        self.enterContext(mock.patch.object(workflows.bazel_workspace, "prepare_workspace", side_effect=self.prepare))
        self.enterContext(mock.patch.object(workflows.bazel_workspace, "invocation", return_value=["/fixture/bazel"]))
        self.enterContext(mock.patch.object(workflows.subprocess, "run", side_effect=self.process))
        self.enterContext(mock.patch.object(workflows.bazel_workspace, "execute", side_effect=self.build))

    def build(self, executable, tree, prepared, action, config, targets, *, jobs, remote_cache, isolate_rc):
        self.assertEqual(
            (executable, tree, action, config, targets, jobs, remote_cache, isolate_rc),
            (
                Path("/fixture/bazel"),
                self.tree,
                "build",
                "debug",
                [self.artifact.target],
                3,
                None,
                True,
            ),
        )
        self.assertTrue((prepared / "aobus.bazelrc").is_file())
        self.actions.append("build")
        (tree / "build.log").write_bytes(b"governed build log")
        return 0

    def prepare(self, tree, config, *, prefix):
        self.assertEqual((tree, config, prefix), (self.tree, "debug", None))
        prepared = self.tree / "configuration"
        prepared.mkdir(exist_ok=True)
        (prepared / "aobus.bazelrc").write_text("build --config=debug\n", encoding="utf-8")
        return prepared

    def process(self, argv, *, cwd, env, stdout, stderr, check):
        self.assertEqual(argv[0], "/fixture/bazel")
        self.assertFalse(check)
        action = argv[1]
        self.actions.append(action)
        label = self.artifact.target
        if action == "cquery":
            self.assertIn(f"set({label})", argv)
            if "--output=jsonproto" in argv:
                targets = [
                    {
                        "target": {"type": "RULE", "rule": {"name": label, "ruleClass": "cc_binary"}},
                        "configurationId": 1,
                    }
                ]
                if self.ambiguous:
                    targets.append({**targets[0], "configurationId": 2})
                output = json.dumps(
                    {
                        "results": targets,
                        "configurations": [
                            {"id": 1, "checksum": "a" * 64},
                            {"id": 2, "checksum": "b" * 64},
                        ],
                    }
                )
            else:
                (expression,) = [argument for argument in argv if argument.startswith("--starlark:expr=")]
                if "files_to_run" in expression:
                    relative = self.artifact.executable.relative_to(self.artifact.execution_root).as_posix()
                    output = json.dumps(
                        {
                            "label": label,
                            "incompatible": False,
                            "executable": relative,
                            "files": [{"path": relative, "directory": False}],
                            "runfiles": [],
                            "manifest": None,
                            "environment": {},
                        }
                    )
                else:
                    self.assertIn("IncompatiblePlatformProvider", expression)
                    output = json.dumps({"label": label, "incompatible": False})
        elif action == "info":
            self.assertEqual(argv[2:], ["execution_root"])
            output = str(self.artifact.execution_root)
        else:
            self.fail(f"unexpected subprocess: {argv}")
        stdout.write((output + "\n").encode())
        return subprocess.CompletedProcess(argv, 0)

    def test_no_build_may_query_but_never_runs_a_build_action(self):
        with workflows.prepare_artifacts(self.args, [self.artifact.target], no_build=True) as artifacts:
            self.assertEqual([artifact.executable for artifact in artifacts], [self.artifact.executable])
        self.assertIn("cquery", self.actions)
        self.assertIn("info", self.actions)
        self.assertNotIn("build", self.actions)
        (receipt,) = self.tree.glob("workflow-*/artifacts.json")
        metadata = json.loads(receipt.read_text(encoding="utf-8"))
        self.assertEqual(metadata["source_revision"], "unverified")
        self.assertEqual(metadata["artifact_origin"], "existing; build skipped")
        self.assertEqual(
            metadata["artifacts"][0]["executable_sha256"], workflows.native.sha256(self.artifact.executable)
        )

    def test_normal_preparation_builds_exact_selected_label_before_launch(self):
        with workflows.prepare_artifacts(self.args, [self.artifact.target]) as artifacts:
            self.assertEqual(artifacts[0].configuration, "a" * 64)
            self.assertEqual(self.actions.count("build"), 1)
        self.assertLess(self.actions.index("build"), self.actions.index("info"))

    def test_missing_current_output_does_not_admit_an_old_tree_executable(self):
        self.artifact.executable.unlink()
        stale = self.tree / "test/ao_core_test"
        stale.parent.mkdir()
        stale.write_bytes(b"stale executable")
        with self.assertRaisesRegex(BazelWorkspaceError, "missing or not executable"):
            with workflows.prepare_artifacts(self.args, [self.artifact.target], no_build=True):
                self.fail("missing configured artifact must not launch")
        self.assertEqual(stale.read_bytes(), b"stale executable")
        self.assertNotIn("build", self.actions)

    def test_multiple_configurations_for_one_label_fail_before_build(self):
        self.ambiguous = True
        with self.assertRaisesRegex(BazelWorkspaceError, "multiple configurations"):
            with workflows.prepare_artifacts(self.args, [self.artifact.target]):
                self.fail("ambiguous configured target must not launch")
        self.assertNotIn("build", self.actions)

    def test_feature_switches_are_exact_graph_settings_and_invalid_values_fail(self):
        self.args.gtk, self.args.system_media = "off", True
        self.assertEqual(
            workflows.feature_options(self.args),
            (
                "--//bazel:gtk_enabled=false",
                "--//bazel:system_media_enabled=true",
            ),
        )
        self.args.system_media = "maybe"
        with self.assertRaisesRegex(BazelWorkspaceError, "on/off or a boolean"):
            workflows.validate_request(self.args)


class FreshLintWorkflowTest(unittest.TestCase):
    def setUp(self):
        self.tree = Path(self.enterContext(tempfile.TemporaryDirectory(prefix="aobus-fresh-lint-")))
        self.execution_root = self.tree / "state/output/execroot/_main"
        self.document = lint_fixture_document(self.tree)
        self.original_argv = tuple(self.document["actions"][0]["arguments"])
        self.args = argparse.Namespace(
            config="debug", path=str(self.tree), jobs=16, vcpkg_prefix=None, remote_cache=None
        )
        self.helper_calls = []
        self.builds = []
        self.fixture_status = 0
        self.helper_exit = 0
        self.write_report = True
        self.report_override = None
        self.enterContext(contextlib.redirect_stdout(io.StringIO()))
        self.enterContext(contextlib.redirect_stderr(io.StringIO()))
        self.enterContext(mock.patch.object(builddir, "platform_profile", return_value=builddir.LINUX_PROFILE))
        self.enterContext(mock.patch.object(workflows.native, "require_supported_source"))
        self.enterContext(mock.patch.object(workflows.bazel_workspace, "workspace_path", return_value=self.tree))
        self.enterContext(
            mock.patch.object(workflows.bazel_workspace, "bazel_executable", return_value=Path("/fixture/bazel"))
        )
        self.enterContext(mock.patch.object(workflows.bazel_workspace, "prepare_workspace", side_effect=self.prepare))
        self.enterContext(mock.patch.object(workflows.bazel_workspace, "invocation", return_value=["/fixture/bazel"]))
        self.enterContext(mock.patch.object(workflows.bazel_workspace, "execute", side_effect=self.build))
        self.enterContext(mock.patch.object(workflows.subprocess, "run", side_effect=self.process))

    def prepare(self, tree, config, *, prefix):
        self.assertEqual((tree, config, prefix), (self.tree, "debug", None))
        prepared = tree / "configuration"
        prepared.mkdir(exist_ok=True)
        (prepared / "aobus.bazelrc").write_text("build --config=debug\n", encoding="utf-8")
        return prepared

    def build(self, executable, tree, prepared, action, config, targets, *, jobs, remote_cache, isolate_rc):
        self.assertEqual(
            (executable, tree, action, config, targets, jobs, remote_cache, isolate_rc),
            (
                Path("/fixture/bazel"),
                self.tree,
                "build",
                "debug",
                ["//tool/lint:verification"],
                16,
                None,
                True,
            ),
        )
        self.builds.append(targets)
        (tree / "build.log").write_bytes(b"verification build only\n")
        return 0

    def process(self, argv, *, cwd, env, stdout, stderr, check):
        self.assertFalse(check)
        if argv[0] == "/fixture/bazel":
            action = argv[1]
            if action == "cquery" and "//test/lint:lint_test" in argv:
                self.assertIn("--output=jsonproto", argv)
                output = {
                    "results": [
                        {
                            "target": {
                                "type": "RULE",
                                "rule": {
                                    "name": "//test/lint:lint_test",
                                    "ruleClass": "lint_fixture_test",
                                },
                            },
                            "configurationId": 2,
                        }
                    ],
                    "configurations": [{"id": 2, "checksum": "a" * 64}],
                }
            elif action == "cquery" and "//tool/lint:verification" in argv:
                self.assertTrue(any("DefaultInfo" in argument for argument in argv))
                output = {
                    "label": "//tool/lint:verification",
                    "files": [self.original_argv[self.original_argv.index("--verification") + 1]],
                }
            elif action == "aquery":
                self.assertIn('mnemonic("AobusLintFixtures", //test/lint:lint_test)', argv)
                self.assertIn("--include_artifacts=true", argv)
                self.assertIn("--include_param_files=true", argv)
                output = self.document
            elif action == "info":
                self.assertEqual(argv[2:], ["execution_root"])
                stdout.write(str(self.execution_root).encode())
                return subprocess.CompletedProcess(argv, 0)
            else:
                self.fail(f"unexpected Bazel boundary: {argv}")
            stdout.write(json.dumps(output).encode())
            return subprocess.CompletedProcess(argv, 0)
        self.assertEqual(argv[:7], list(self.original_argv[:7]))
        self.assertEqual(cwd, self.execution_root)
        self.assertEqual(env, {"PRIVATE_ACTION_ENV": "selected"})
        self.assertEqual(argv[-2:], ["--jobs", "2"], "build jobs must never leak into fixture scheduling")
        paths = {name: Path(argv[argv.index("--" + name) + 1]) for name in ("report", "log", "work")}
        self.assertTrue(all(not path.exists() for path in paths.values()), "each helper launch must own fresh outputs")
        self.helper_calls.append(tuple(argv))
        paths["work"].mkdir()
        paths["log"].write_text("new assertion log\n", encoding="utf-8")
        if self.write_report:
            payload = (
                self.report_override
                if self.report_override is not None
                else {
                    "schema": 1,
                    "status": self.fixture_status,
                    "platform": "linux",
                }
            )
            paths["report"].write_text(json.dumps(payload), encoding="utf-8")
        stdout.write(b"new assertions\n")
        stderr.write(b"diagnostic retained\n")
        return subprocess.CompletedProcess(argv, self.helper_exit)

    def command_arguments(self, *options):
        parser = argparse.ArgumentParser()
        test_command.register(parser.add_subparsers(dest="command", required=True))
        return parser.parse_args(["test", "--lint", "--jobs", "16", "--path", str(self.tree), *options])

    def test_normal_manual_lint_builds_verification_only_then_runs_new_assertions(self):
        self.assertEqual(test_command.run_command(self.command_arguments()), 0)
        self.assertEqual(self.builds, [["//tool/lint:verification"]])
        self.assertEqual(len(self.helper_calls), 1)
        receipts = list(self.tree.glob("workflow-*/lint-run-*/receipt.json"))
        self.assertEqual(len(receipts), 1)
        receipt = json.loads(receipts[0].read_text(encoding="utf-8"))
        self.assertEqual(
            (receipt["helper_exit"], receipt["fixture_status"], receipt["configuration"]), (0, 0, "a" * 64)
        )
        self.assertEqual(Path(receipt["stdout"]).read_bytes(), b"new assertions\n")
        self.assertEqual(Path(receipt["stderr"]).read_bytes(), b"diagnostic retained\n")

    def test_no_build_repeats_new_assertions_and_replaces_only_owned_output_roles(self):
        cached = self.execution_root / self.original_argv[self.original_argv.index("--report") + 1]
        cached.parent.mkdir(parents=True, exist_ok=True)
        cached.write_bytes(b"old cached result must not be read or modified")
        self.assertEqual(test_command.run_command(self.command_arguments("--no-build", "--repeat", "2")), 0)
        self.assertEqual(self.builds, [])
        self.assertEqual(len(self.helper_calls), 2)
        reports = []
        for call in self.helper_calls:
            normalized = list(call)
            reports.append(normalized[normalized.index("--report") + 1])
            for name in ("report", "log", "work"):
                index = normalized.index("--" + name) + 1
                normalized[index] = self.original_argv[index]
            self.assertEqual(tuple(normalized), self.original_argv)
        self.assertNotEqual(*reports)
        self.assertEqual(cached.read_bytes(), b"old cached result must not be read or modified")

    def test_completed_assertion_failure_is_nonzero_even_when_helper_exits_zero(self):
        self.fixture_status = 1
        self.assertEqual(test_command.run_command(self.command_arguments("--no-build", "--repeat", "3")), 1)
        self.assertEqual(len(self.helper_calls), 1)
        (receipt,) = self.tree.glob("workflow-*/lint-run-*/receipt.json")
        self.assertEqual(json.loads(receipt.read_text(encoding="utf-8"))["helper_exit"], 0)

    def test_helper_failure_is_captured_even_when_it_wrote_a_passing_report(self):
        self.helper_exit = 9
        self.assertEqual(test_command.run_command(self.command_arguments("--no-build")), 9)
        (receipt,) = self.tree.glob("workflow-*/lint-run-*/receipt.json")
        observed = json.loads(receipt.read_text(encoding="utf-8"))
        self.assertEqual((observed["helper_exit"], observed["fixture_status"]), (9, None))
        self.assertTrue(Path(observed["stderr"]).is_file())

    def test_missing_new_report_cannot_replay_a_cached_passing_report(self):
        self.write_report = False
        cached = self.execution_root / self.original_argv[self.original_argv.index("--report") + 1]
        cached.parent.mkdir(parents=True, exist_ok=True)
        cached.write_text(json.dumps({"schema": 1, "status": 0, "platform": "linux"}), encoding="utf-8")
        with self.assertRaises(SystemExit):
            test_command.run_command(self.command_arguments("--no-build"))
        self.assertEqual(len(self.helper_calls), 1)
        self.assertEqual(self.builds, [])

    def test_declared_aquery_param_expansion_keeps_the_exact_producer_command(self):
        action = self.document["actions"][0]
        action["paramFiles"] = [{"execPath": "bazel-out/fixture.params", "arguments": action["arguments"][1:]}]
        action["arguments"] = [action["arguments"][0], "@bazel-out/fixture.params"]
        self.assertEqual(test_command.run_command(self.command_arguments("--no-build")), 0)
        normalized = list(self.helper_calls[0])
        for name in ("report", "log", "work"):
            index = normalized.index("--" + name) + 1
            normalized[index] = self.original_argv[index]
        self.assertEqual(tuple(normalized), self.original_argv)


if __name__ == "__main__":
    unittest.main()
