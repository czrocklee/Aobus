"""Performance executable admission and no-build boundaries, not graph parity."""

import argparse
import contextlib
import json
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from workflow_support import executable_artifact

from ao.command import perf
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
        self.row["environment"] = {"PERFORMANCE_RESOURCE": "/declared/data", "TEST_SRCDIR": "/declared/elsewhere"}

        environment = self.parse().environment()

        self.assertEqual(environment["PERFORMANCE_RESOURCE"], "/declared/data")
        self.assertEqual(environment["TEST_SRCDIR"], str(self.artifact.runfiles_directory))

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
        self.compiler = self.artifact.execution_root / "tools/clang++"
        self.compiler.parent.mkdir()
        self.compiler.write_bytes(b"configured compiler; never executed")
        self.args = argparse.Namespace(
            config="debug", path=str(self.tree), jobs=3, vcpkg_prefix=None, remote_cache=None
        )
        self.actions = []
        self.ambiguous = False
        self.incompatible = False
        self.compiler_tool_only = False
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
            self.assertIn(label, argv)
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
                    output = json.dumps({"label": label, "incompatible": self.incompatible})
        elif action == "info":
            self.assertEqual(argv[2:], ["execution_root"])
            output = str(self.artifact.execution_root)
        elif action == "aquery":
            self.assertIn(f'mnemonic("CppCompile", deps({label}))', argv)
            self.assertIn("--include_param_files=true", argv)
            output = json.dumps(
                {
                    "targets": [{"id": 1, "label": label}],
                    "configuration": [
                        {"id": 1, "isTool": self.compiler_tool_only},
                        {"id": 2, "isTool": True},
                    ],
                    "actions": [
                        {
                            "mnemonic": "CppCompile",
                            "targetId": 1,
                            "configurationId": 1,
                            "arguments": ["tools/clang++", "@compile.params"],
                            "paramFiles": [{"execPath": "compile.params", "arguments": ["-c", "fixture.cpp"]}],
                        },
                        {
                            "mnemonic": "CppCompile",
                            "targetId": 1,
                            "configurationId": 2,
                            "arguments": ["/missing/host-tool-compiler", "-c", "tool.cpp"],
                        },
                    ],
                }
            )
        else:
            self.fail(f"unexpected subprocess: {argv}")
        stdout.write((output + "\n").encode())
        return subprocess.CompletedProcess(argv, 0)

    def test_no_build_queries_compiler_provenance_but_never_runs_a_build_action(self):
        with workflows.prepare_artifact(self.args, self.artifact.target, no_build=True) as artifact:
            self.assertEqual(artifact.executable, self.artifact.executable)
            self.assertEqual(
                artifact.compiler_identity,
                f"unverified existing artifact; currently configured compiler: "
                f"{self.compiler.resolve()} sha256={workflows.native.sha256(self.compiler)}",
            )
        self.assertIn("cquery", self.actions)
        self.assertIn("info", self.actions)
        self.assertIn("aquery", self.actions)
        self.assertNotIn("build", self.actions)
        (receipt,) = self.tree.glob("workflow-*/artifacts.json")
        metadata = json.loads(receipt.read_text(encoding="utf-8"))
        self.assertEqual(metadata["source_revision"], "unverified")
        self.assertEqual(metadata["artifact_origin"], "existing; build skipped")
        self.assertEqual(
            metadata["artifacts"][0]["executable_sha256"], workflows.native.sha256(self.artifact.executable)
        )
        self.assertEqual(metadata["artifacts"][0]["compiler"], artifact.compiler_identity)

    def test_normal_preparation_builds_exact_selected_label_and_records_compiler_before_launch(self):
        with workflows.prepare_artifact(self.args, self.artifact.target) as artifact:
            self.assertEqual(artifact.configuration, "a" * 64)
            self.assertEqual(
                artifact.compiler_identity, f"{self.compiler.resolve()} sha256={workflows.native.sha256(self.compiler)}"
            )
            self.assertEqual(self.actions.count("build"), 1)
        self.assertLess(self.actions.index("build"), self.actions.index("info"))
        self.assertLess(self.actions.index("info"), self.actions.index("aquery"))

    def test_artifact_context_holds_the_selected_tree_until_use_finishes(self):
        lifecycle = []

        @contextlib.contextmanager
        def lock(tree):
            self.assertEqual(tree, self.tree)
            lifecycle.append("enter")
            try:
                yield
            finally:
                lifecycle.append("exit")

        with mock.patch.object(workflows, "build_tree_lock", side_effect=lock):
            with workflows.prepare_artifact(self.args, self.artifact.target, no_build=True):
                self.assertEqual(lifecycle, ["enter"])
            self.assertEqual(lifecycle, ["enter", "exit"])

    def test_missing_current_output_does_not_admit_an_old_tree_executable(self):
        self.artifact.executable.unlink()
        stale = self.tree / "test/ao_perf_baseline"
        stale.parent.mkdir()
        stale.write_bytes(b"stale executable")
        with self.assertRaisesRegex(BazelWorkspaceError, "missing or not executable"):
            with workflows.prepare_artifact(self.args, self.artifact.target, no_build=True):
                self.fail("missing configured artifact must not launch")
        self.assertEqual(stale.read_bytes(), b"stale executable")
        self.assertNotIn("build", self.actions)

    def test_missing_configured_compiler_cannot_fall_back_to_unverified_provenance(self):
        self.compiler.unlink()
        with self.assertRaisesRegex(BazelWorkspaceError, "configured compiler is not available"):
            with workflows.prepare_artifact(self.args, self.artifact.target, no_build=True):
                self.fail("missing configured compiler must not launch")
        self.assertNotIn("build", self.actions)

    def test_host_tool_compilers_do_not_substitute_for_the_performance_compiler(self):
        self.compiler_tool_only = True
        with self.assertRaisesRegex(BazelWorkspaceError, r"no configured C\+\+ compiler"):
            with workflows.prepare_artifact(self.args, self.artifact.target, no_build=True):
                self.fail("host-only compiler provenance must not launch")

    def test_multiple_configurations_for_one_label_fail_before_build(self):
        self.ambiguous = True
        with self.assertRaisesRegex(BazelWorkspaceError, "multiple configurations"):
            with workflows.prepare_artifact(self.args, self.artifact.target):
                self.fail("ambiguous configured target must not launch")
        self.assertNotIn("build", self.actions)

    def test_incompatible_target_fails_before_build_or_provider_admission(self):
        self.incompatible = True
        with self.assertRaisesRegex(BazelWorkspaceError, "incompatible"):
            with workflows.prepare_artifact(self.args, self.artifact.target):
                self.fail("incompatible target must not launch")
        self.assertEqual(self.actions, ["cquery"])

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


class PerformanceProfileAdmissionTest(unittest.TestCase):
    def setUp(self):
        self.enterContext(mock.patch.object(builddir, "platform_profile", return_value=builddir.LINUX_PROFILE))

    def test_sanitizer_artifact_requests_fail_before_selecting_or_preparing_a_tree(self):
        for config in ("asan", "tsan"):
            args = argparse.Namespace(config=config)
            with self.subTest(config=config), mock.patch.object(workflows.bazel_workspace, "workspace_path") as path:
                self.assertEqual(workflows.validate_request(args), (config, "linux"))
                with self.assertRaisesRegex(BazelWorkspaceError, "perf does not support sanitizer"):
                    with workflows.prepare_artifact(args, perf.TARGET_LABEL):
                        self.fail("performance review has no supported sanitizer runtime")
                path.assert_not_called()

    def test_perf_sanitizer_preflight_fails_before_native_environment_selection(self):
        parser = argparse.ArgumentParser()
        perf.register(parser.add_subparsers(dest="command", required=True))
        for switch in ("--asan", "--tsan"):
            with self.subTest(switch=switch):
                args = parser.parse_args(["perf", "debug", switch])
                with self.assertRaisesRegex(BazelWorkspaceError, "perf does not support sanitizer"):
                    perf.requires_build_environment(args)

    def test_debug_release_and_profile_remain_available_for_performance_review(self):
        for config in ("debug", "release", "profile"):
            with self.subTest(config=config):
                self.assertEqual(
                    workflows.validate_performance_request(argparse.Namespace(config=config)), (config, "linux")
                )


if __name__ == "__main__":
    unittest.main()
