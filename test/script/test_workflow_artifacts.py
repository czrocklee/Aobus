"""Configured performance artifacts and the analysis-only launch boundary."""

import json
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from workflow_support import executable_artifact

from ao.core.bazel_native import BazelWorkspaceError
from ao.portal import bazel_workflows as workflows
from ao.portal import build_dir


class ArtifactAdmissionTest(unittest.TestCase):
    def setUp(self):
        self.root = Path(self.enterContext(tempfile.TemporaryDirectory(prefix="aobus-workflow-")))
        self.artifact = executable_artifact(self.root / "tree")
        relative = self.artifact.executable.relative_to(self.artifact.execution_root).as_posix()
        self.row = {
            "label": self.artifact.target,
            "incompatible": False,
            "executable": relative,
            "files": [{"path": relative, "directory": False}],
            "manifest": None,
            "runfiles": [{"path": "fixture/data.txt", "short_path": "fixture/data.txt", "directory": False}],
            "environment": {},
        }

    def parse(self):
        return workflows.parse_artifact(
            self.row,
            target=self.artifact.target,
            checksum="a" * 64,
            config="debug",
            tree=self.artifact.tree,
            execution_root=self.artifact.execution_root,
        )

    def test_declared_executable_wins_over_other_default_outputs(self):
        self.row["files"].insert(0, {"path": "bazel-out/native/bin/fixture/not-the-executable", "directory": False})
        self.assertEqual(self.parse().executable, self.artifact.executable)

    def test_no_runfiles_directory_requires_a_manifest_that_resolves_the_declared_input(self):
        shutil.rmtree(self.artifact.runfiles_directory)
        with self.assertRaises(BazelWorkspaceError):
            self.parse()
        manifest = self.artifact.executable.with_suffix(".manifest")
        data = self.artifact.execution_root / "fixture/data.txt"
        manifest.write_text(f"_main/fixture/data.txt {data}\n", encoding="utf-8")
        self.row["manifest"] = manifest.relative_to(self.artifact.execution_root).as_posix()
        environment = self.parse().environment()
        self.assertEqual(environment["RUNFILES_MANIFEST_FILE"], str(manifest))
        self.assertEqual(environment["TEST_SRCDIR"], "")

    def test_declared_run_environment_cannot_override_the_selected_runfiles_context(self):
        self.row["environment"] = {"PERFORMANCE_RESOURCE": "/declared/data", "TEST_SRCDIR": "/declared/elsewhere"}
        environment = self.parse().environment()
        self.assertEqual(environment["PERFORMANCE_RESOURCE"], "/declared/data")
        self.assertEqual(environment["TEST_SRCDIR"], str(self.artifact.runfiles_directory))

    def test_missing_configured_output_does_not_fall_back_to_another_executable(self):
        binary = self.artifact.executable
        stale = binary.with_name("previous-build")
        binary.rename(stale)
        self.row["files"].append(
            {"path": stale.relative_to(self.artifact.execution_root).as_posix(), "directory": False}
        )
        with self.assertRaisesRegex(BazelWorkspaceError, "configured executable is missing"):
            self.parse()

    def test_no_build_prepares_the_configured_executable_without_running_a_build(self):
        artifact = self.artifact
        base = self.root / "configuration"
        base.mkdir()
        (base / "aobus.bazelrc").touch()
        compiler = artifact.execution_root / "fixture/compiler"
        compiler.touch()
        bazel = str(self.root / "bazel")

        def run(argv, *, stdout, **kwargs):
            self.assertEqual(argv[0], bazel)
            action, *arguments = argv[1:]
            if action == "cquery" and artifact.target in arguments:
                if "--output=jsonproto" in arguments:
                    output = {
                        "results": [
                            {
                                "target": {"type": "RULE", "rule": {"name": artifact.target, "ruleClass": "cc_binary"}},
                                "configurationId": 1,
                            }
                        ],
                        "configurations": [{"id": 1, "checksum": "b" * 64}],
                    }
                elif any("files_to_run" in argument for argument in arguments):
                    output = self.row
                else:
                    self.assertTrue(any("IncompatiblePlatformProvider" in argument for argument in arguments))
                    output = {"label": artifact.target, "incompatible": False}
            elif action == "info" and arguments == ["execution_root"]:
                stdout.write(str(artifact.execution_root).encode())
                return subprocess.CompletedProcess(argv, 0)
            elif action == "aquery" and f'mnemonic("CppCompile", deps({artifact.target}))' in arguments:
                output = {
                    "targets": [{"id": 1, "label": artifact.target}],
                    "configuration": [{"id": 1, "isTool": False}],
                    "actions": [
                        {
                            "mnemonic": "CppCompile",
                            "targetId": 1,
                            "configurationId": 1,
                            "arguments": [str(compiler), "-c", "fixture.cpp"],
                        }
                    ],
                }
            else:
                self.fail(f"unexpected Bazel command: {argv}")
            stdout.write(json.dumps(output).encode())
            return subprocess.CompletedProcess(argv, 0)

        request = workflows.Request(config="debug", path=str(artifact.tree))
        with (
            mock.patch.object(build_dir, "platform_profile", return_value=build_dir.LINUX_PROFILE),
            mock.patch.object(workflows.native, "require_supported_source"),
            mock.patch.object(workflows.bazel_workspace, "bazel_executable", return_value=Path(bazel)),
            mock.patch.object(workflows.bazel_workspace, "prepare_workspace", return_value=base),
            mock.patch.object(workflows.bazel_workspace, "invocation", return_value=[bazel]),
            mock.patch.object(
                workflows.bazel_workspace, "execute", side_effect=AssertionError("--no-build ran Bazel build")
            ),
            mock.patch.object(workflows.subprocess, "run", side_effect=run),
            workflows.prepare_artifact(request, artifact.target, no_build=True) as selected,
        ):
            self.assertEqual(selected.executable, artifact.executable)
            self.assertEqual(selected.configuration, "b" * 64)


if __name__ == "__main__":
    unittest.main()
