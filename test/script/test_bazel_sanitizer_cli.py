"""Sanitizer CLI admission and the compile-contract analysis handoff."""

import contextlib
import io
import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao.__main__ import make_parser, parse_arguments
from ao.portal import bazel_compiler_probe, bazel_verify, bazel_workspace, build_dir, windows_portal


class SanitizerCliTests(unittest.TestCase):
    def test_contracts_rejects_a_conflicting_profile_instead_of_using_the_last_one(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as exited:
            parse_arguments(make_parser(), ["check", "contracts", "--config", "asan", "--config", "tsan"])
        self.assertEqual(exited.exception.code, 2)

    def test_windows_rejects_tsan_before_visual_studio_or_dispatch(self):
        with (
            mock.patch.dict(os.environ),
            mock.patch.object(build_dir, "platform_profile", return_value=build_dir.WINDOWS_PROFILE),
            mock.patch.object(windows_portal, "prepare_build_environment", side_effect=AssertionError("VS setup")),
            mock.patch.object(windows_portal, "dispatch", side_effect=AssertionError("native dispatch")),
            contextlib.redirect_stdout(io.StringIO()),
            contextlib.redirect_stderr(io.StringIO()),
            self.assertRaises(SystemExit) as exited,
        ):
            os.environ.pop("_AOBUS_WINDOWS_SCOPE_FILE", None)
            windows_portal.main(["check", "contracts", "--config", "tsan"])
        self.assertEqual(exited.exception.code, 1)

    def test_tsan_contracts_inspect_debug_with_the_selected_sdk_include_root(self):
        with tempfile.TemporaryDirectory(prefix="aobus-sanitizer-cli-") as temporary:
            tree = Path(temporary).resolve() / "tree"
            sdk_include = tree / "compiler-rt/include"
            ready = tree / "configuration"
            bazel = tree / "bazel"

            def prepare(selected, config, **kwargs):
                self.assertEqual((selected, config), (tree, "tsan"))
                return ready

            def compiler_metadata(compiler):
                self.assertEqual(compiler, "clang")
                return {"sanitizerIncludeDirectory": str(sdk_include)}

            command = ["check", "contracts", "--config", "tsan", "--compiler", "clang", "--path", str(tree)]
            with (
                mock.patch.object(build_dir, "platform_profile", return_value=build_dir.LINUX_PROFILE),
                mock.patch.object(bazel_workspace, "prepare_workspace", side_effect=prepare),
                mock.patch.object(bazel_workspace, "bazel_executable", return_value=bazel),
                mock.patch.object(bazel_compiler_probe, "nix_cc_wrapper", side_effect=compiler_metadata),
                mock.patch.object(bazel_verify, "verify", return_value=7) as analyze,
                contextlib.redirect_stderr(io.StringIO()),
            ):
                args = parse_arguments(make_parser(), command)
                self.assertEqual(args.func(args), 7)
            self.assertEqual(analyze.call_args.args, (bazel, tree, ready, "debug"))
            self.assertEqual(analyze.call_args.kwargs["compiler_include_roots"], (str(sdk_include),))


if __name__ == "__main__":
    unittest.main()
