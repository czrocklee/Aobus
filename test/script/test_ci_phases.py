"""Portal and workflow consumers of opt-in phase measurements."""

import contextlib
import io
import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao import __main__ as portal
from ao.core import phases, tooltest, workspace_cache
from ao.core.paths import PROJECT_ROOT


class PortalPhaseTest(unittest.TestCase):
    def test_root_command_records_returned_failure_without_changing_it(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            arguments = mock.Mock(command="check", func=mock.Mock(return_value=7))
            with (
                mock.patch.dict(os.environ, {phases.LOG_DIR_ENV: temporary}),
                mock.patch.object(portal, "parse_arguments", return_value=arguments),
                mock.patch.object(portal.buildenv, "load_source_scope"),
                mock.patch.object(portal.buildenv, "requires_parsed_build_env", return_value=False),
                mock.patch.object(portal.workspace_cache, "command_uses_context", return_value=False),
                mock.patch.object(portal.workspace_cache, "command_context", return_value=contextlib.nullcontext()),
            ):
                self.assertEqual(portal.main(["check"]), 7)
            rows = [json.loads(line) for file in directory.glob("*.jsonl") for line in file.read_text().splitlines()]
            self.assertEqual([row["kind"] for row in rows], ["BEGIN", "END"])
            self.assertEqual(rows[0]["command"], "check")
            self.assertEqual(rows[1]["status"], 7)
            self.assertEqual(rows[1]["outcome"], "fail")
            arguments.func.assert_called_once_with(arguments)

    def test_caught_workspace_failure_is_recorded_as_failure(self):
        with tempfile.TemporaryDirectory() as temporary:
            arguments = mock.Mock(command="build")
            arguments.func.side_effect = workspace_cache.WorkspaceCacheError("private diagnostic")
            with (
                mock.patch.dict(os.environ, {phases.LOG_DIR_ENV: temporary}),
                mock.patch.object(portal, "parse_arguments", return_value=arguments),
                mock.patch.object(portal.buildenv, "load_source_scope"),
                mock.patch.object(portal.buildenv, "requires_parsed_build_env", return_value=False),
                mock.patch.object(portal.workspace_cache, "command_uses_context", return_value=False),
                mock.patch.object(portal.workspace_cache, "command_context", return_value=contextlib.nullcontext()),
                contextlib.redirect_stderr(io.StringIO()),
            ):
                self.assertEqual(portal.main(["build"]), 1)
            text = "".join(file.read_text() for file in Path(temporary).glob("*.jsonl"))
            self.assertNotIn("private diagnostic", text)
            self.assertEqual(json.loads(text.splitlines()[-1])["status"], 1)


class ToolingPhaseIsolationTest(unittest.TestCase):
    def test_unit_processes_cannot_pollute_the_real_command_trace(self):
        result = mock.Mock(returncode=0, stdout="Ran 1 test\nOK\n")
        with (
            mock.patch.dict(os.environ, {phases.LOG_DIR_ENV: "/private/real-command-trace"}),
            mock.patch.object(tooltest.pythoncheck, "run_paths", return_value=0),
            mock.patch.object(tooltest, "test_modules", return_value=["test_example"]),
            mock.patch.object(tooltest, "_run_modules", return_value=[result]) as run_modules,
            contextlib.redirect_stdout(io.StringIO()),
        ):
            self.assertEqual(tooltest.run(), 0)
            child_environment = run_modules.call_args.args[1]
            self.assertNotIn(phases.LOG_DIR_ENV, child_environment)
            self.assertEqual(os.environ[phases.LOG_DIR_ENV], "/private/real-command-trace")
            self.assertIn("PYTHONPATH", child_environment)


class CiPhaseWorkflowTest(unittest.TestCase):
    def test_reports_use_each_platforms_governed_python(self):
        workflow = (PROJECT_ROOT / ".github/workflows/ci.yml").read_text()
        summary_steps = [
            block.split("- name:", 1)[0] for block in workflow.split("- name: Summarize native phases")[1:]
        ]
        self.assertEqual(len(summary_steps), 3)
        linux = summary_steps[0]
        shell_preparation = 'NIX_BUILD_SHELL="$(nix-build shell.nix -A portalShell --no-out-link)/bin/bash"'
        self.assertIn(shell_preparation, linux)
        self.assertIn("export NIX_BUILD_SHELL", linux)
        self.assertLess(linux.index(shell_preparation), linux.index("nix-shell --run"))
        self.assertIn('"$AO_NIX_PORTAL_PYTHON" -m ao.core.phases', linux)
        self.assertIn('aobus_macos_prepare_python "$PWD"', summary_steps[1])
        self.assertIn('"$AOBUS_PYTHON" -m ao.core.phases', summary_steps[1])
        self.assertIn("script/ao/toolchain.json", summary_steps[2])
        self.assertIn(r"tools\python\$($contract.python)\python.exe", summary_steps[2])
        self.assertIn("$env:AOBUS_PYTHON", summary_steps[2])

    def test_only_reporting_steps_are_best_effort_not_native_gates(self):
        workflow = (PROJECT_ROOT / ".github/workflows/ci.yml").read_text()
        summaries = [block.split("- name:", 1)[0] for block in workflow.split("- name: Summarize native phases")[1:]]
        self.assertEqual(len(summaries), 3)
        for step in summaries:
            self.assertIn("continue-on-error: true", step)
        for name in ("Build and test", "Run hygiene"):
            gates = [block.split("- name:", 1)[0] for block in workflow.split(f"- name: {name}")[1:]]
            self.assertEqual(len(gates), 3)
            for step in gates:
                self.assertNotIn("continue-on-error:", step)

    def test_job_measurement_paths_use_available_run_contexts(self):
        workflow = (PROJECT_ROOT / ".github/workflows/ci.yml").read_text()
        directories = [line for line in workflow.splitlines() if "AOBUS_PHASE_LOG_DIR:" in line]
        self.assertEqual(len(directories), 3)
        for directory in directories:
            # runner is unavailable in jobs.<job>.env, unlike individual steps.
            self.assertNotIn("runner.", directory)
            self.assertIn("${{ github.run_id }}", directory)
            self.assertIn("${{ github.run_attempt }}", directory)

    def test_native_jobs_collect_only_public_phase_measurements(self):
        workflow = (PROJECT_ROOT / ".github/workflows/ci.yml").read_text()
        self.assertEqual(workflow.count("AOBUS_PHASE_LOG_DIR:"), 3)
        self.assertEqual(workflow.count("- name: Summarize native phases"), 3)
        self.assertEqual(workflow.count("--github-summary"), 3)
        self.assertNotIn("--msbuild-capture", workflow)
        self.assertNotIn("/v:diag", workflow)
        for block in workflow.split("- name: Summarize native phases")[1:]:
            summary_step = block.split("- name:", 1)[0]
            self.assertIn("always() && steps.build.outcome != 'skipped'", summary_step)
            self.assertIn("PYTHONPATH: script", summary_step)
            self.assertIn("-m ao.core.phases", summary_step)


if __name__ == "__main__":
    unittest.main()
