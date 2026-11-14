"""Tests for ao.command.setup — explicit, scoped host setup actions."""

import contextlib
import io
import unittest
from pathlib import Path
from unittest import mock

from ao.__main__ import make_parser, parse_arguments
from ao.command import setup
from ao.core import bazel_provision, bazel_workspace, builddir, buildenv, windows_llvm_sdk


class SetupCommandTest(unittest.TestCase):
    def parse(self, arguments):
        return parse_arguments(make_parser(), arguments)

    def test_bazel_component_only_initializes_the_pinned_executable(self):
        # The explicit setup route never prepares a workspace or runs Bazel.
        args = self.parse(["setup", "bazel"])
        with (
            mock.patch.object(bazel_workspace, "bazel_executable", return_value=Path("bazel")) as executable,
            mock.patch.object(bazel_workspace, "prepare_workspace") as prepare,
            mock.patch.object(bazel_workspace, "execute") as execute,
            contextlib.redirect_stdout(io.StringIO()) as stdout,
        ):
            self.assertEqual(setup.run_command(args), 0)
        executable.assert_called_once_with(setup=True)
        prepare.assert_not_called()
        execute.assert_not_called()
        self.assertIn("Bazel ready: bazel", stdout.getvalue())

    def test_llvm_sdk_is_a_registered_public_component(self):
        self.assertEqual(self.parse(["setup", "llvm-sdk"]).component, "llvm-sdk")
        with self.assertRaises(SystemExit):
            with contextlib.redirect_stderr(io.StringIO()):
                self.parse(["setup", "not-a-component"])

    def test_setup_never_requires_the_native_build_environment(self):
        # requires_build_environment is an unconditional False that never
        # consults the profile; one component proves the constant.
        self.assertFalse(buildenv.requires_build_env("setup", ["llvm-sdk"]))

    def test_setup_help_needs_no_build_environment(self):
        self.assertFalse(buildenv.requires_build_env("setup", ["--help"]))
        self.assertFalse(buildenv.requires_build_env("setup", ["llvm-sdk", "--help"]))

    def test_llvm_sdk_fails_clearly_on_other_hosts_without_provisioning(self):
        for profile in (builddir.LINUX_PROFILE, builddir.MACOS_PROFILE):
            with (
                self.subTest(profile=profile.name),
                mock.patch.object(builddir, "platform_profile", return_value=profile),
                mock.patch.object(bazel_provision, "install_llvm_sdk") as install,
            ):
                with contextlib.redirect_stderr(io.StringIO()) as stderr:
                    with self.assertRaises(SystemExit):
                        setup.run_command(self.parse(["setup", "llvm-sdk"]))
            install.assert_not_called()
            self.assertIn("only on native Windows hosts", stderr.getvalue())

    def test_llvm_sdk_provisions_through_the_native_installer(self):
        manifest = mock.Mock()
        manifest.version = "31.2.3"
        with (
            mock.patch.object(builddir, "platform_profile", return_value=builddir.WINDOWS_PROFILE),
            mock.patch.object(bazel_provision, "install_llvm_sdk", return_value="C:/state/llvm") as install,
            mock.patch.object(windows_llvm_sdk, "read_manifest", return_value=manifest) as read_manifest,
        ):
            with contextlib.redirect_stdout(io.StringIO()) as stdout:
                self.assertEqual(setup.run_command(self.parse(["setup", "llvm-sdk"])), 0)
        install.assert_called_once_with()
        read_manifest.assert_called_once_with(windows_llvm_sdk.MANIFEST)
        self.assertIn("Pinned LLVM SDK 31.2.3 ready at C:/state/llvm.", stdout.getvalue())

    def test_other_components_still_dispatch_their_own_actions(self):
        with (
            mock.patch.object(builddir, "platform_profile", return_value=builddir.WINDOWS_PROFILE),
            mock.patch.object(setup.winui, "setup_runtime") as setup_runtime,
            mock.patch.object(bazel_provision, "install_llvm_sdk") as install,
        ):
            installed = mock.Mock(version="1.0", architecture="x64")
            setup_runtime.return_value = installed
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(setup.run_command(self.parse(["setup", "winui-runtime"])), 0)
        setup_runtime.assert_called_once_with()
        install.assert_not_called()


if __name__ == "__main__":
    unittest.main()
