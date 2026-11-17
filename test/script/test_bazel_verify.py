"""CLI boundaries of native compile-contract checking."""

import io
import json
import subprocess
import sys
import tempfile
import unittest
from contextlib import contextmanager
from pathlib import Path
from unittest.mock import patch

from ao.__main__ import make_parser
from ao.command import check as verify_command
from ao.core import bazel_native as native
from ao.core import jobs
from ao.portal import bazel_verify, contract_checks
from ao.portal import bazel_workspace as workspace


class VerifyParserTests(unittest.TestCase):
    def test_verify_defaults_to_fastbuild_analysis_with_default_workers_and_no_forwarded_scope(self):
        args = make_parser().parse_args(["check", "contracts"])
        self.assertIs(args.func, verify_command.run_command)
        self.assertTrue(verify_command.requires_build_environment(args))
        self.assertEqual(args.config, "fastbuild")
        self.assertEqual(args.jobs, jobs.default_workers())
        self.assertIsInstance(args.jobs, int)
        self.assertIsNone(args.path)
        self.assertIsNone(args.vcpkg_prefix)

    def test_verify_accepts_explicit_config_path_prefix_and_positive_jobs(self):
        args = make_parser().parse_args(
            [
                "check",
                "contracts",
                "--config",
                "release",
                "--path",
                "/tmp/bazel-verify",
                "--jobs",
                "3",
                "--vcpkg-prefix",
                "/tmp/installed-prefix",
            ]
        )
        self.assertEqual(args.config, "release")
        self.assertEqual(args.path, "/tmp/bazel-verify")
        self.assertEqual(args.jobs, 3)
        self.assertEqual(args.vcpkg_prefix, "/tmp/installed-prefix")
        for config in ("fastbuild", "debug", "profile", "asan", "tsan"):
            self.assertEqual(make_parser().parse_args(["check", "contracts", "--config", config]).config, config)

    def test_verify_rejects_targets_forwarded_options_and_remote_cache(self):
        for arguments in (
            ["//..."],
            ["--", "//test:query_test"],
            ["--", "--output=jsonproto"],
            ["--remote-cache", "https://cache.example"],
            ["--", "--remote_cache=https://cache.example"],
            ["--config", "unknown"],
            ["--jobs", "0"],
            ["--jobs", "-1"],
            ["--jobs", "many"],
        ):
            with self.subTest(arguments=arguments), patch("sys.stderr"), self.assertRaises(SystemExit) as exited:
                make_parser().parse_args(["check", "contracts", *arguments])
            self.assertEqual(exited.exception.code, 2)


class VerifyDispatchTests(unittest.TestCase):
    def test_verify_prepares_and_analyzes_under_the_tree_lock_without_build_execution(self):
        # The ordinary fastbuild default and an explicitly requested Debug
        # analysis both pass their actual configuration through unchanged.
        for config in ("fastbuild", "debug"):
            with self.subTest(config=config):
                self._run_verify_preparation(config, "linux")

    def _run_verify_preparation(self, config, host):
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

            def prepare(selected, mode, *, prefix=None, graph_options=()):
                self.assertTrue(locked)
                self.assertTrue(tree.is_dir())
                self.assertEqual((selected, mode, prefix, graph_options), (tree, config, None, ()))
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
            args = make_parser().parse_args(
                ["check", "contracts", "--config", config, "--path", str(tree), "--jobs", "3"]
            )
            with (
                patch.object(native, "native_platform", return_value=host),
                patch.object(workspace, "workspace_path", return_value=tree) as select,
                patch.object(contract_checks, "build_tree_lock", side_effect=lock),
                patch.object(workspace, "prepare_workspace", side_effect=prepare),
                patch.object(workspace, "bazel_executable", side_effect=lookup),
                patch.object(workspace, "execute") as execute,
                patch.object(bazel_verify, "verify", side_effect=verify) as analyze,
                patch("sys.stdout", output),
                patch("sys.stderr", diagnostics),
            ):
                self.assertEqual(verify_command.run_command(args), 7)
            select.assert_called_once_with(str(tree), config)
            analyze.assert_called_once_with(executable, tree, prepared, config, jobs=3, compiler_include_roots=())
            execute.assert_not_called()
            self.assertEqual(events, ["lock", "prepare", "lookup", "verify", "unlock"])
            self.assertEqual(output.getvalue(), "")
            self.assertIn(f"Prepared Bazel configuration: {prepared}", diagnostics.getvalue())

    def test_preparation_failure_prevents_verify_and_tool_lookup(self):
        with tempfile.TemporaryDirectory(prefix="aobus-verify-cli-") as temporary:
            args = make_parser().parse_args(["check", "contracts", "--path", temporary])
            with (
                patch.object(native, "native_platform", return_value="linux"),
                patch.object(workspace, "prepare_workspace", side_effect=native.BazelWorkspaceError("stale input")),
                patch.object(workspace, "bazel_executable") as executable,
                patch.object(workspace, "execute") as execute,
                patch.object(bazel_verify, "verify") as analyze,
                patch("sys.stderr", new_callable=io.StringIO) as diagnostics,
                self.assertRaises(SystemExit),
            ):
                verify_command.run_command(args)
            self.assertIn("stale input", diagnostics.getvalue())
            executable.assert_not_called()
            execute.assert_not_called()
            analyze.assert_not_called()


class BazelVerifyCaptureTests(unittest.TestCase):
    def test_native_child_observes_running_checkpoint_and_preserves_failed_binary_streams(self):
        with tempfile.TemporaryDirectory(prefix="aobus-bazel-capture-") as temporary:
            root = Path(temporary).resolve()
            tree = root / "tree"
            prepared = root / "configuration"
            prepared.mkdir()
            (prepared / "aobus.bazelrc").write_text("# fixture configuration\n", encoding="utf-8")
            child = root / "bazel-fixture.py"
            child.write_text(
                "import json, pathlib, sys\n"
                "root = pathlib.Path(__file__).parent\n"
                "assert sys.argv[2] == 'build' and '//:native_policy_inputs' in sys.argv\n"
                "assert pathlib.Path.cwd() == root\n"
                "receipt = next((root / 'tree/contracts/debug').glob('verify-*/receipt.json'))\n"
                "facts = json.loads(receipt.read_text(encoding='utf-8'))['facts']\n"
                "assert facts['status'] == 'running' and facts['exit'] is None\n"
                "sys.stdout.buffer.write(b'\\x00facts\\xff\\n')\n"
                "sys.stderr.buffer.write(b'\\x00diagnostic\\xfe\\n')\n"
                "sys.exit(23)\n",
                encoding="utf-8",
            )
            run = subprocess.run

            def launch(argv, **kwargs):
                self.assertEqual(argv[0], str(child))
                # Substitute only the external tool: the native Python child uses
                # verify's actual cwd, environment and separate binary handles.
                return run([sys.executable, str(child), *argv[1:]], **kwargs)

            with (
                patch.object(bazel_verify, "PROJECT_ROOT", root),
                patch.object(native, "PROJECT_ROOT", root),
                patch.object(bazel_verify.subprocess, "run", side_effect=launch) as process,
                patch("sys.stdout", new_callable=io.StringIO) as output,
                patch("sys.stderr", new_callable=io.StringIO),
            ):
                self.assertEqual(bazel_verify.verify(child, tree, prepared, "debug"), 23)
            process.assert_called_once()
            receipt_path = next((tree / "contracts/debug").glob("verify-*/receipt.json"))
            receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
            self.assertEqual((receipt["status"], receipt["exit"]), ("facts build failed", 23))
            facts = receipt["facts"]
            self.assertEqual((facts["status"], facts["exit"]), ("failed", 23))
            self.assertEqual(Path(facts["stdout"]).read_bytes(), b"\x00facts\xff\n")
            self.assertEqual(Path(facts["stderr"]).read_bytes(), b"\x00diagnostic\xfe\n")
            for stage in (receipt["files"], receipt["execution_root"], receipt["driver"]):
                self.assertIn(stage["status"], {"unrun", "not applicable"})
                self.assertIsNone(stage["exit"])
            self.assertEqual(output.getvalue(), "")


if __name__ == "__main__":
    unittest.main()
