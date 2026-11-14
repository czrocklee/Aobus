"""CLI boundaries of the dedicated native compile-contract verify command."""

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
from ao.command import verify as verify_command
from ao.core import bazel_contract, jobs
from ao.core import bazel_native as native
from ao.core import bazel_workspace as workspace


class VerifyParserTests(unittest.TestCase):
    def test_verify_defaults_to_debug_analysis_with_default_workers_and_no_forwarded_scope(self):
        args = make_parser().parse_args(["verify"])
        self.assertIs(args.func, verify_command.run_command)
        self.assertTrue(verify_command.requires_build_environment(args))
        self.assertEqual(args.config, "debug")
        self.assertEqual(args.jobs, jobs.default_workers())
        self.assertIsInstance(args.jobs, int)
        self.assertIsNone(args.path)
        self.assertIsNone(args.vcpkg_prefix)

    def test_verify_accepts_explicit_config_path_prefix_and_positive_jobs(self):
        args = make_parser().parse_args(
            [
                "verify",
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
        self.assertEqual(make_parser().parse_args(["verify", "--config", "profile"]).config, "profile")
        self.assertEqual(make_parser().parse_args(["verify", "--config", "asan"]).config, "asan")
        self.assertEqual(make_parser().parse_args(["verify", "--config", "tsan"]).config, "tsan")

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
                make_parser().parse_args(["verify", *arguments])
            self.assertEqual(exited.exception.code, 2)


class VerifyDispatchTests(unittest.TestCase):
    def test_verify_prepares_and_analyzes_under_the_tree_lock_without_build_execution(self):
        host, config = "linux", "debug"
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

            def prepare(selected, mode, *, prefix=None):
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
            args = make_parser().parse_args(["verify", "--config", config, "--path", str(tree), "--jobs", "3"])
            with (
                patch.object(native, "native_platform", return_value=host),
                patch.object(workspace, "workspace_path", return_value=tree) as select,
                patch.object(verify_command, "build_tree_lock", side_effect=lock),
                patch.object(workspace, "prepare_workspace", side_effect=prepare),
                patch.object(workspace, "bazel_executable", side_effect=lookup),
                patch.object(workspace, "execute") as execute,
                patch.object(bazel_contract, "verify", side_effect=verify) as analyze,
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
            args = make_parser().parse_args(["verify", "--path", temporary])
            with (
                patch.object(native, "native_platform", return_value="linux"),
                patch.object(workspace, "prepare_workspace", side_effect=native.BazelWorkspaceError("stale input")),
                patch.object(workspace, "bazel_executable") as executable,
                patch.object(workspace, "execute") as execute,
                patch.object(bazel_contract, "verify") as analyze,
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
                "assert sys.argv[2] == 'query' and '--output=label' in sys.argv\n"
                "assert pathlib.Path.cwd() == root\n"
                "receipt = next((root / 'tree/contracts/debug').glob('verify-*/receipt.json'))\n"
                "selection = json.loads(receipt.read_text(encoding='utf-8'))['selection']\n"
                "assert selection['status'] == 'running' and selection['exit'] is None\n"
                "sys.stdout.buffer.write(b'\\x00selection\\xff\\n')\n"
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
                patch.object(bazel_contract, "PROJECT_ROOT", root),
                patch.object(native, "PROJECT_ROOT", root),
                patch.object(
                    bazel_contract,
                    "_expected_hardening_environment",
                    return_value={"NIX_HARDENING_ENABLE_fixture": "stackprotector"},
                ),
                patch.object(bazel_contract.subprocess, "run", side_effect=launch) as process,
                patch("sys.stdout", new_callable=io.StringIO) as output,
                patch("sys.stderr", new_callable=io.StringIO),
            ):
                self.assertEqual(bazel_contract.verify(child, tree, prepared, "debug"), 23)
            process.assert_called_once()
            receipt_path = next((tree / "contracts/debug").glob("verify-*/receipt.json"))
            receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
            self.assertEqual((receipt["status"], receipt["exit"]), ("selection failed", 23))
            selection = receipt["selection"]
            self.assertEqual((selection["status"], selection["exit"]), ("exited", 23))
            self.assertEqual(Path(selection["stdout"]).read_bytes(), b"\x00selection\xff\n")
            self.assertEqual(Path(selection["stderr"]).read_bytes(), b"\x00diagnostic\xfe\n")
            self.assertTrue(all(query["status"] == "unrun" and query["exit"] is None for query in receipt["queries"]))
            self.assertEqual(output.getvalue(), "")


if __name__ == "__main__":
    unittest.main()
