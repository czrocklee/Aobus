"""CLI boundaries for native Bazel compile-contract analysis."""

import io
import json
import subprocess
import tempfile
import unittest
from contextlib import contextmanager
from pathlib import Path
from unittest.mock import patch

from ao.__main__ import make_parser
from ao.command import bazel
from ao.core import bazel_contract
from ao.core import bazel_native as native
from ao.core import bazel_workspace as workspace


class BazelVerifyParserTests(unittest.TestCase):
    def test_verify_defaults_to_debug_analysis_with_two_jobs_and_no_forwarded_scope(self):
        args = make_parser().parse_args(["bazel", "verify"])
        self.assertIs(args.func, bazel.run)
        self.assertTrue(bazel.requires_build_environment(args))
        self.assertEqual(args.config, "debug")
        self.assertEqual(args.jobs, 2)
        self.assertIsInstance(args.jobs, int)
        self.assertIsNone(args.path)
        self.assertEqual(args.targets, [])
        self.assertIsNone(args.remote_cache)

    def test_verify_accepts_explicit_config_path_and_positive_jobs(self):
        args = make_parser().parse_args(
            ["bazel", "verify", "--config", "release", "--path", "/tmp/bazel-verify", "--jobs", "3"]
        )
        self.assertEqual(args.config, "release")
        self.assertEqual(args.path, "/tmp/bazel-verify")
        self.assertEqual(args.jobs, 3)

    def test_verify_rejects_targets_forwarded_options_and_remote_cache(self):
        for arguments in (
            ["//..."],
            ["--", "//test:query_test"],
            ["--", "--output=jsonproto"],
            ["--remote-cache", "https://cache.example"],
            ["--", "--remote_cache=https://cache.example"],
            ["--config", "profile"],
            ["--jobs", "0"],
            ["--jobs", "-1"],
            ["--jobs", "many"],
        ):
            with self.subTest(arguments=arguments), patch("sys.stderr"), self.assertRaises(SystemExit) as exited:
                make_parser().parse_args(["bazel", "verify", *arguments])
            self.assertEqual(exited.exception.code, 2)


class BazelVerifyDispatchTests(unittest.TestCase):
    def test_verify_prepares_and_analyzes_under_the_tree_lock_without_build_execution(self):
        for host, config in (("linux", "debug"), ("linux", "release"), ("macos", "debug"), ("windows", "release")):
            with self.subTest(host=host, config=config):
                self._assert_verify_dispatch(host, config)

    def _assert_verify_dispatch(self, host, config):
        with tempfile.TemporaryDirectory(prefix="aobus-bazel-verify-cli-") as temporary:
            tree = Path(temporary) / "tree"
            prepared = tree / "configuration"
            executable = Path("mock-bazel")
            events = []
            locked = False

            @contextmanager
            def lock(selected):
                nonlocal locked
                self.assertEqual(selected, tree)
                locked = True
                events.append("lock")
                try:
                    yield
                finally:
                    events.append("unlock")
                    locked = False

            def prepare(selected, mode, *, prefix):
                self.assertTrue(locked)
                self.assertTrue(tree.is_dir())
                self.assertEqual((selected, mode, prefix), (tree, config, None))
                events.append("prepare")
                return prepared

            def lookup():
                self.assertTrue(locked)
                events.append("lookup")
                return executable

            def verify(*args, **kwargs):
                self.assertTrue(locked)
                events.append("verify")
                return 7

            output, diagnostics = io.StringIO(), io.StringIO()
            args = make_parser().parse_args(["bazel", "verify", "--config", config, "--path", str(tree), "--jobs", "3"])
            with (
                patch.object(native, "native_platform", return_value=host),
                patch.object(workspace, "workspace_path", return_value=tree) as select,
                patch.object(bazel, "build_tree_lock", side_effect=lock),
                patch.object(workspace, "prepare_workspace", side_effect=prepare),
                patch.object(workspace, "bazel_executable", side_effect=lookup),
                patch.object(workspace, "execute") as execute,
                patch.object(bazel_contract, "verify", side_effect=verify) as analyze,
                patch("sys.stdout", output),
                patch("sys.stderr", diagnostics),
            ):
                self.assertEqual(bazel.run(args), 7)
            select.assert_called_once_with(str(tree), config)
            analyze.assert_called_once_with(executable, tree, prepared, config, jobs=3)
            execute.assert_not_called()
            self.assertEqual(events, ["lock", "prepare", "lookup", "verify", "unlock"])
            self.assertEqual(output.getvalue(), "")
            self.assertIn(f"Prepared Bazel configuration: {prepared}", diagnostics.getvalue())

    def test_verify_rejects_an_unsupported_host_before_tree_preparation_or_tool_lookup(self):
        args = make_parser().parse_args(["bazel", "verify"])
        output, diagnostics = io.StringIO(), io.StringIO()
        with (
            patch.object(native, "native_platform", return_value="unsupported"),
            patch.object(workspace, "workspace_path") as select,
            patch.object(bazel, "build_tree_lock") as lock,
            patch.object(workspace, "prepare_workspace") as prepare,
            patch.object(workspace, "bazel_executable") as executable,
            patch.object(workspace, "execute") as execute,
            patch.object(bazel_contract, "verify") as analyze,
            patch("sys.stdout", output),
            patch("sys.stderr", diagnostics),
            self.assertRaises(SystemExit) as exited,
        ):
            bazel.run(args)
        self.assertNotEqual(exited.exception.code, 0)
        self.assertIn("verify", diagnostics.getvalue())
        self.assertEqual(output.getvalue(), "")
        for operation in (select, lock, prepare, executable, execute, analyze):
            operation.assert_not_called()

    def test_preparation_failure_prevents_verify_and_tool_lookup(self):
        with tempfile.TemporaryDirectory(prefix="aobus-bazel-verify-cli-") as temporary:
            args = make_parser().parse_args(["bazel", "verify", "--path", temporary])
            with (
                patch.object(native, "native_platform", return_value="linux"),
                patch.object(workspace, "prepare_workspace", side_effect=native.BazelWorkspaceError("stale input")),
                patch.object(workspace, "bazel_executable") as executable,
                patch.object(workspace, "execute") as execute,
                patch.object(bazel_contract, "verify") as analyze,
                patch("sys.stderr", new_callable=io.StringIO) as diagnostics,
                self.assertRaises(SystemExit),
            ):
                bazel.run(args)
            self.assertIn("stale input", diagnostics.getvalue())
            executable.assert_not_called()
            execute.assert_not_called()
            analyze.assert_not_called()


class BazelAnalysisOutputTests(unittest.TestCase):
    def test_cli_analysis_preserves_one_json_document_and_reports_failure_diagnostics_only_once(self):
        with tempfile.TemporaryDirectory(prefix="aobus-bazel-analysis-output-") as temporary:
            tree = Path(temporary)
            prepared = tree / "configuration"
            payload = b'{"results": [{"label": "//test:query_test"}]}\n'
            diagnostic = b"native analysis diagnostic\n"
            for action in ("query", "cquery", "aquery"):
                for status in (0, 7):
                    output, diagnostics = io.StringIO(), io.StringIO()
                    args = make_parser().parse_args(
                        ["bazel", action, "--path", str(tree), "--", "//...", "--output=jsonproto"]
                    )
                    with (
                        self.subTest(action=action, status=status),
                        patch.object(native, "native_platform", return_value="linux"),
                        patch.object(workspace, "prepare_workspace", return_value=prepared),
                        patch.object(workspace, "bazel_executable", return_value=Path("mock-bazel")),
                        patch.object(
                            workspace.subprocess,
                            "run",
                            return_value=subprocess.CompletedProcess([], status, payload, diagnostic),
                        ) as run,
                        patch("sys.stdout", output),
                        patch("sys.stderr", diagnostics),
                    ):
                        self.assertEqual(bazel.run(args), status)
                    self.assertEqual(output.getvalue(), payload.decode("utf-8"))
                    self.assertEqual(json.loads(output.getvalue()), {"results": [{"label": "//test:query_test"}]})
                    self.assertEqual(diagnostics.getvalue().count(diagnostic.decode("utf-8")), int(status != 0))
                    self.assertNotIn(payload.decode("utf-8"), diagnostics.getvalue())
                    self.assertIn("Prepared Bazel configuration:", diagnostics.getvalue())
                    self.assertEqual((tree / "build.log").read_bytes(), payload + diagnostic)
                    run.assert_called_once()
                    self.assertTrue(run.call_args.kwargs["capture_output"])
                    self.assertEqual(run.call_args.args[0][2], action)


if __name__ == "__main__":
    unittest.main()
