"""Managed profile admission precedes native SDK and tree preparation."""

import contextlib
import io
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao.__main__ import make_parser
from ao.command import check as check_command
from ao.command import verify as verify_command
from ao.core import (
    bazel_contract,
    bazel_native,
    bazel_sanitizers,
    bazel_workflows,
    bazel_workspace,
    builddir,
    windows_portal,
)


def parse(*arguments):
    return make_parser().parse_args(list(arguments))


class SanitizerCliAdmissionTest(unittest.TestCase):
    def test_native_commands_select_named_profiles_without_inventing_compilation_modes(self):
        for arguments, profile in (
            (("check", "--asan"), "asan"),
            (("check", "--tsan"), "tsan"),
            (("perf", "debug", "--asan"), "asan"),
            (("perf", "debug", "--tsan"), "tsan"),
        ):
            with self.subTest(arguments=arguments):
                self.assertEqual(bazel_workflows.configuration(parse(*arguments)), profile)
        # The flavor spellings map to the same governed configurations; the
        # ordinary check default is fastbuild and perf keeps explicit Release.
        self.assertEqual(bazel_workflows.configuration(parse("check")), "fastbuild")
        self.assertEqual(bazel_workflows.configuration(parse("check", "fastbuild")), "fastbuild")
        self.assertEqual(bazel_workflows.configuration(parse("check", "debug")), "debug")
        self.assertEqual(bazel_workflows.configuration(parse("check", "release")), "release")
        self.assertEqual(bazel_workflows.configuration(parse("perf")), "release")
        self.assertEqual(bazel_workflows.configuration(parse("verify")), "fastbuild")
        self.assertEqual(bazel_workflows.configuration(parse("verify", "--config", "asan")), "asan")

    def test_sanitizer_switches_admit_the_default_and_debug_flavors_and_reject_release_and_profile(self):
        # The shared registration admits one sanitizer request per invocation.
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            parse("check", "--asan", "--tsan")
        # The default fastbuild normal completion and an explicitly requested
        # Debug one are both admitted; the instrumented subset stays dbg-based.
        for arguments in (
            ("check", "--asan"),
            ("check", "fastbuild", "--asan"),
            ("check", "debug", "--asan"),
            ("check", "debug", "--tsan"),
        ):
            with self.subTest(arguments=arguments):
                self.assertEqual(bazel_workflows.configuration(parse(*arguments)), arguments[-1].lstrip("-"))
        # Release and profile are incompatible with sanitizer instrumentation.
        for arguments in (("check", "release", "--asan"), ("perf", "profile", "--tsan")):
            with (
                self.subTest(arguments=arguments),
                self.assertRaisesRegex(
                    bazel_native.BazelWorkspaceError, "incompatible with the release and profile flavors"
                ),
            ):
                bazel_workflows.configuration(parse(*arguments))

    def test_verify_rejects_duplicate_or_conflicting_profile_requests(self):
        # Verify keeps its own single-configuration contract: a later repeat
        # cannot replace the admitted request in either order.
        for profiles in (
            ("asan", "tsan"),
            ("tsan", "asan"),
            ("release", "asan"),
            ("asan", "release"),
            ("debug", "release"),
            ("release", "debug"),
            ("debug", "debug"),
        ):
            with (
                self.subTest(profiles=profiles),
                contextlib.redirect_stderr(io.StringIO()),
                self.assertRaises(SystemExit),
            ):
                parse("verify", "--config", profiles[0], "--config", profiles[1])

    def test_completion_lint_and_provisioning_keep_their_normal_profile_contract(self):
        for arguments in (
            ("tidy", "--config", "asan"),
            ("tidy", "--config", "profile"),
            ("hygiene", "--config", "asan"),
            ("deps", "provision", "--config", "asan"),
            ("deps", "report", "--config", "asan"),
        ):
            with (
                self.subTest(arguments=arguments),
                contextlib.redirect_stderr(io.StringIO()),
                self.assertRaises(SystemExit),
            ):
                parse(*arguments)
        # The completion gate admits sanitizer execution but never the sampling
        # profile: profile is a build/performance workflow, refused at admission.
        with (
            mock.patch.object(builddir, "platform_profile", return_value=builddir.LINUX_PROFILE),
            mock.patch("sys.stderr", io.StringIO()) as stderr,
            self.assertRaises(SystemExit),
        ):
            check_command.requires_build_environment(parse("check", "profile"))
        self.assertIn("sampling is a build/performance profile", stderr.getvalue())

    def test_windows_tsan_is_refused_before_visual_studio_or_tree_setup(self):
        args = parse("verify", "--config", "tsan")
        stderr = io.StringIO()
        status = None
        with (
            mock.patch.object(windows_portal, "parse_invocation", return_value=args),
            mock.patch.object(builddir, "platform_profile", return_value=builddir.WINDOWS_PROFILE),
            mock.patch.object(bazel_native, "native_platform", return_value="windows"),
            mock.patch.object(windows_portal, "prepare_build_environment") as sdk,
            mock.patch.object(windows_portal, "dispatch") as dispatch,
            mock.patch.object(bazel_workspace, "workspace_path") as tree,
            contextlib.redirect_stderr(stderr),
        ):
            try:
                status = windows_portal.main([])
            except SystemExit, bazel_native.BazelWorkspaceError:
                status = None  # the refusal is an admission failure in either spelling
        self.assertNotEqual(status, 0)
        self.assertIn("Windows does not support ThreadSanitizer", stderr.getvalue())
        sdk.assert_not_called()
        dispatch.assert_not_called()
        tree.assert_not_called()

    def test_sanitizer_verify_returns_the_debug_compile_contract_of_the_profile_graph(self):
        # Instrumentation is proven by the sanitizer tests; verify adds no second oracle.
        for profile, roots in (("asan", ()), ("tsan", ("/selected/compiler-rt/include",))):
            with self.subTest(profile=profile), tempfile.TemporaryDirectory(prefix="aobus-sanitizer-cli-") as temporary:
                root = Path(temporary)
                args = parse("verify", "--config", profile, "--path", str(root), "--jobs", "16")
                executable = root / "selected-bazel"
                prepared = root / "configuration"
                with (
                    mock.patch.object(bazel_native, "native_platform", return_value="linux"),
                    mock.patch.object(builddir, "platform_profile", return_value=builddir.LINUX_PROFILE),
                    mock.patch.object(bazel_workspace, "workspace_path", return_value=root),
                    mock.patch.object(verify_command, "build_tree_lock", return_value=contextlib.nullcontext()),
                    mock.patch.object(bazel_workspace, "prepare_workspace", return_value=prepared) as prepare,
                    mock.patch.object(bazel_workspace, "bazel_executable", return_value=executable),
                    mock.patch.object(
                        bazel_sanitizers, "_selected_tsan_interface_root", return_value="/selected/compiler-rt/include"
                    ),
                    mock.patch.object(bazel_contract, "verify", return_value=7) as base,
                    contextlib.redirect_stderr(io.StringIO()),
                ):
                    self.assertEqual(verify_command.run_command(args), 7)
                self.assertEqual(prepare.call_args.args, (root, profile))
                base.assert_called_once_with(executable, root, prepared, "debug", jobs=16, compiler_include_roots=roots)


if __name__ == "__main__":
    unittest.main()
