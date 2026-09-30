"""Tests for the concise tooling test runner."""

import contextlib
import io
import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao.core import doccheck, tooltest


class ToolTestRunnerTest(unittest.TestCase):
    def _run(self, completed, *, static_status=0, log=None, modules=("test_example",)):
        outcomes = completed if isinstance(completed, dict) else {module: completed for module in modules}

        def run_module(command, **_kwargs):
            return outcomes[command[-1]]

        with (
            mock.patch.object(tooltest.pythoncheck, "run_paths", return_value=static_status) as static,
            mock.patch.object(tooltest, "test_modules", return_value=list(outcomes)),
            mock.patch.object(tooltest.subprocess, "run", side_effect=run_module),
        ):
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                status = tooltest.run(log=log)
        static.assert_called_once_with([], log=log)
        return status, output.getvalue()

    def test_success_prints_only_a_concise_summary(self):
        completed = tooltest.subprocess.CompletedProcess(
            [],
            0,
            stdout="expected argparse noise\nRan 61 tests in 0.032s\n\nOK\n",
        )

        status, output = self._run(completed)

        self.assertEqual(status, 0)
        self.assertEqual(output, "Tooling tests passed (61 tests).\n")

    def test_failure_prints_the_captured_test_output(self):
        completed = tooltest.subprocess.CompletedProcess([], 1, stdout="FAILED: test_example\n")

        status, output = self._run(completed)

        self.assertEqual(status, 1)
        self.assertEqual(output, "Tooling tests failed.\n== test_example\nFAILED: test_example\n")

    def test_static_check_failure_fails_the_suite(self):
        completed = tooltest.subprocess.CompletedProcess([], 0, stdout="Ran 1 test in 0.001s\n\nOK\n")

        status, _ = self._run(completed, static_status=1)

        self.assertEqual(status, 1)

    def test_unit_test_failure_takes_precedence_over_static_status(self):
        completed = tooltest.subprocess.CompletedProcess([], 2, stdout="FAILED: test_example\n")

        status, _ = self._run(completed, static_status=1)

        self.assertEqual(status, 2)

    def test_tooling_runner_does_not_repeat_the_documentation_gate(self):
        completed = tooltest.subprocess.CompletedProcess([], 0, stdout="Ran 1 test in 0.001s\n\nOK\n")

        with mock.patch.object(doccheck, "check_tree") as docs:
            status, _ = self._run(completed)

        self.assertEqual(status, 0)
        docs.assert_not_called()

    def test_captured_output_is_appended_to_the_gate_log(self):
        completed = tooltest.subprocess.CompletedProcess([], 0, stdout="Ran 2 tests in 0.001s\n\nOK\n")

        with tempfile.TemporaryDirectory() as temp_dir:
            log = Path(temp_dir) / "build.log"
            status, _ = self._run(completed, log=log)

            self.assertEqual(status, 0)
            self.assertEqual(log.read_text(encoding="utf-8"), f"== test_example\n{completed.stdout}")

    def test_modules_are_summarized_together_with_their_skips(self):
        completed = {
            "test_alpha": tooltest.subprocess.CompletedProcess([], 0, stdout="Ran 3 tests in 0.1s\n\nOK\n"),
            "test_beta": tooltest.subprocess.CompletedProcess([], 0, stdout="Ran 4 tests in 0.1s\n\nOK (skipped=2)\n"),
            "test_helpers": tooltest.subprocess.CompletedProcess(
                [], tooltest.NO_TESTS_RAN, stdout="Ran 0 tests in 0.0s\n\nNO TESTS RAN\n"
            ),
        }

        status, output = self._run(completed)

        self.assertEqual(status, 0)
        self.assertEqual(output, "Tooling tests passed (7 tests, 2 skipped).\n")

    def test_failure_prints_only_failing_module_output(self):
        completed = {
            "test_alpha": tooltest.subprocess.CompletedProcess([], 0, stdout="passing module noise\n"),
            "test_beta": tooltest.subprocess.CompletedProcess([], 1, stdout="FAILED: test_beta_case\n"),
            "test_gamma": tooltest.subprocess.CompletedProcess([], 1, stdout="FAILED: test_gamma_case\n"),
        }

        status, output = self._run(completed)

        self.assertEqual(status, 1)
        self.assertEqual(
            output,
            "Tooling tests failed.\n== test_beta\nFAILED: test_beta_case\n== test_gamma\nFAILED: test_gamma_case\n",
        )

    def test_each_module_runs_in_its_own_interpreter_from_the_project_root(self):
        completed = tooltest.subprocess.CompletedProcess([], 0, stdout="Ran 1 test in 0.001s\n\nOK\n")
        with (
            mock.patch.object(tooltest.pythoncheck, "run_paths", return_value=0),
            mock.patch.object(tooltest, "test_modules", return_value=["test_alpha", "test_beta"]),
            mock.patch.object(tooltest.subprocess, "run", return_value=completed) as run,
            contextlib.redirect_stdout(io.StringIO()),
        ):
            self.assertEqual(tooltest.run(), 0)

        commands = sorted(call.args[0][-1] for call in run.call_args_list)
        self.assertEqual(commands, ["test_alpha", "test_beta"])
        for call in run.call_args_list:
            self.assertEqual(call.kwargs["cwd"], tooltest.PROJECT_ROOT)
            search_path = call.kwargs["env"]["PYTHONPATH"].split(os.pathsep)
            self.assertEqual(search_path[:2], [str(tooltest.PROJECT_ROOT / "script"), str(tooltest.TEST_DIR)])

    def test_host_shared_workspace_activation_is_not_inherited_by_unit_tests(self):
        completed = tooltest.subprocess.CompletedProcess([], 0, stdout="Ran 1 test in 0.001s\n\nOK\n")
        inherited = {
            tooltest.compiler_cache.SHARED_WORKSPACES_EFFECTIVE: "1",
            tooltest.compiler_cache.SHARED_WORKSPACES_CCACHE: "/cache/ccache",
            tooltest.compiler_cache.SHARED_WORKSPACES_NAMESPACE_PREFIX: "personal",
        }
        with (
            mock.patch.dict(os.environ, inherited, clear=False),
            mock.patch.object(tooltest.pythoncheck, "run_paths", return_value=0),
            mock.patch.object(tooltest, "test_modules", return_value=["test_example"]),
            mock.patch.object(tooltest.subprocess, "run", return_value=completed) as run,
            contextlib.redirect_stdout(io.StringIO()),
        ):
            self.assertEqual(tooltest.run(), 0)

        child_environment = run.call_args.kwargs["env"]
        for key in inherited:
            self.assertNotIn(key, child_environment)


if __name__ == "__main__":
    unittest.main()
