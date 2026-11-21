"""CLI boundaries of native compile-contract checking."""

import io
import json
import os
import subprocess
import sys
import tempfile
import unittest
from contextlib import contextmanager
from pathlib import Path
from unittest.mock import patch

from ao.__main__ import make_parser, parse_arguments
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
    def test_verify_prepares_and_analyzes_under_the_selected_tree_lock(self):
        with tempfile.TemporaryDirectory(prefix="aobus-bazel-verify-cli-") as temporary:
            tree = Path(temporary).resolve() / "tree"
            events = []

            @contextmanager
            def lock(selected):
                self.assertEqual(selected, tree)
                events.append("lock")
                try:
                    yield
                finally:
                    events.append("unlock")

            def prepare(selected, config, **kwargs):
                self.assertTrue(tree.is_dir())
                self.assertEqual((selected, config), (tree, "fastbuild"))
                events.append("prepare")
                return tree / "configuration"

            def lookup():
                events.append("lookup")
                return Path("bazel")

            def analyze(executable, selected, prepared, config, **kwargs):
                self.assertEqual(
                    (executable, selected, prepared, config), (Path("bazel"), tree, tree / "configuration", "fastbuild")
                )
                events.append("analyze")
                return 7

            args = parse_arguments(make_parser(), ["check", "contracts", "--path", str(tree)])
            with (
                patch.object(native, "native_platform", return_value="linux"),
                patch.object(contract_checks, "build_tree_lock", side_effect=lock),
                patch.object(workspace, "prepare_workspace", side_effect=prepare),
                patch.object(workspace, "bazel_executable", side_effect=lookup),
                patch.object(bazel_verify, "verify", side_effect=analyze),
                patch.object(workspace, "execute") as execute,
                patch("sys.stderr", new_callable=io.StringIO),
            ):
                self.assertEqual(verify_command.run_command(args), 7)
            execute.assert_not_called()
            self.assertEqual(events, ["lock", "prepare", "lookup", "analyze", "unlock"])

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
    def test_standalone_contracts_isolates_the_native_child_and_preserves_failed_binary_streams(self):
        with tempfile.TemporaryDirectory(prefix="aobus-bazel-capture-") as temporary:
            root = Path(temporary).resolve()
            tree = root / "tree"
            personal_rc = root / ".bazelrc"
            personal_rc.write_text("build:debug --config=personal-only\n", encoding="utf-8")
            child = root / "bazel-fixture.py"
            child.write_text(
                f"""
import json, os, pathlib, sys

root = pathlib.Path(__file__).parent
repository = pathlib.Path({str(native.PROJECT_ROOT)!r})
assert pathlib.Path.cwd().samefile(repository)
command = sys.argv.index('build')
startup = sys.argv[1:command]
assert {{'--nosystem_rc', '--nohome_rc', '--noworkspace_rc'}} <= set(startup), startup
assert not any(key.casefold() == 'bazelrc' for key in os.environ)
rcs = [pathlib.Path(arg.removeprefix('--bazelrc=')) for arg in startup if arg.startswith('--bazelrc=')]
assert rcs == [repository / '.bazelrc', root / 'tree/configuration/aobus.bazelrc'], rcs
assert '--config=debug' in rcs[-1].read_text(encoding='utf-8')
assert sys.argv[command:] == ['build', '--jobs=3', '//:native_policy_inputs'], sys.argv
receipt = next((root / 'tree/contracts/debug').glob('verify-*/receipt.json'))
facts = json.loads(receipt.read_text(encoding='utf-8'))['facts']
assert facts['status'] == 'running' and facts['exit'] is None
sys.stdout.buffer.write(b'\\x00facts\\xff\\n')
sys.stderr.buffer.write(b'\\x00diagnostic\\xfe\\n')
sys.exit(23)
""",
                encoding="utf-8",
            )
            run = subprocess.run

            def launch(argv, **kwargs):
                self.assertEqual(argv[0], str(child))
                # Keep the actual process cwd, environment and binary handles.
                return run([sys.executable, "-E", str(child), *argv[1:]], **kwargs)

            args = parse_arguments(
                make_parser(), ["check", "contracts", "--config", "debug", "--path", str(tree), "--jobs", "3"]
            )
            with (
                patch.dict(
                    os.environ,
                    {
                        "HOME": str(root),
                        "USERPROFILE": str(root),
                        "BAZELRC": str(personal_rc),
                        "AOBUS_STATE_ROOT": str(root / "state"),
                        "AOBUS_BAZEL_DISK_CACHE": "",
                    },
                ),
                patch.object(native, "native_platform", return_value="linux"),
                patch.object(workspace, "bazel_executable", return_value=child),
                patch.object(workspace, "_linux_server_javabase", return_value=root / "jdk"),
                patch.object(bazel_verify.subprocess, "run", side_effect=launch) as process,
                patch("sys.stdout", new_callable=io.StringIO) as output,
                patch("sys.stderr", new_callable=io.StringIO),
            ):
                status = args.func(args)
            process.assert_called_once()
            receipt_path = next((tree / "contracts/debug").glob("verify-*/receipt.json"))
            receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
            facts = receipt["facts"]
            stderr = Path(facts["stderr"]).read_bytes()
            self.assertEqual(status, 23, stderr.decode(errors="replace"))
            self.assertEqual((receipt["exit"], facts["exit"]), (status, status))
            self.assertEqual(Path(facts["stdout"]).read_bytes(), b"\x00facts\xff\n")
            self.assertEqual(stderr, b"\x00diagnostic\xfe\n")
            self.assertEqual(output.getvalue(), "")


if __name__ == "__main__":
    unittest.main()
