"""Tests for ao.command.setup — explicit, scoped host setup actions."""

import contextlib
import io
import os
import unittest
from pathlib import Path
from unittest import mock

from ao.__main__ import make_parser, parse_arguments
from ao.command import setup
from ao.command import tidy as tidy_command
from ao.core import builddir, buildenv, tidyengine


class SetupCommandTest(unittest.TestCase):
    def parse(self, arguments):
        return parse_arguments(make_parser(), arguments)

    def test_llvm_sdk_is_a_registered_public_component(self):
        self.assertEqual(self.parse(["setup", "llvm-sdk"]).component, "llvm-sdk")
        with self.assertRaises(SystemExit):
            with contextlib.redirect_stderr(io.StringIO()):
                self.parse(["setup", "not-a-component"])

    def test_llvm_sdk_requests_the_native_build_environment_only_on_windows(self):
        for profile, expected in (
            (builddir.WINDOWS_PROFILE, True),
            (builddir.LINUX_PROFILE, False),
            (builddir.MACOS_PROFILE, False),
        ):
            with (
                self.subTest(profile=profile.name),
                mock.patch.object(builddir, "platform_profile", return_value=profile),
            ):
                self.assertEqual(buildenv.requires_build_env("setup", ["llvm-sdk"]), expected)

    def test_other_setup_components_keep_needing_no_build_environment(self):
        with mock.patch.object(builddir, "platform_profile", return_value=builddir.WINDOWS_PROFILE):
            for component in ("compiler-cache", "git-hooks", "winui-runtime"):
                self.assertFalse(buildenv.requires_build_env("setup", [component]), component)
        self.assertFalse(setup.REQUIRES_BUILD_ENV)

    def test_setup_help_needs_no_build_environment(self):
        self.assertFalse(buildenv.requires_build_env("setup", ["--help"]))
        self.assertFalse(buildenv.requires_build_env("setup", ["llvm-sdk", "--help"]))

    def test_llvm_sdk_fails_clearly_on_other_hosts_without_provisioning(self):
        for profile in (builddir.LINUX_PROFILE, builddir.MACOS_PROFILE):
            with (
                self.subTest(profile=profile.name),
                mock.patch.object(builddir, "platform_profile", return_value=profile),
                mock.patch.object(tidyengine, "ensure_windows_llvm_sdk") as ensure,
            ):
                with contextlib.redirect_stderr(io.StringIO()) as stderr:
                    with self.assertRaises(SystemExit):
                        setup.run_command(self.parse(["setup", "llvm-sdk"]))
            ensure.assert_not_called()
            self.assertIn("only on native Windows hosts", stderr.getvalue())

    def test_llvm_sdk_provisions_the_default_host_without_a_checker_build(self):
        host = builddir.WINDOWS_PROFILE.build_root / "windows-tidy"
        with (
            mock.patch.object(builddir, "platform_profile", return_value=builddir.WINDOWS_PROFILE),
            mock.patch.dict(os.environ, {}, clear=False),
        ):
            os.environ.pop("BUILD_DIR", None)
            with (
                mock.patch.object(tidyengine, "ensure_windows_llvm_sdk") as ensure,
                mock.patch.object(
                    tidyengine, "llvm_sdk_version", return_value=tidyengine.windows_llvm_sdk_manifest()[0]
                ) as version,
                mock.patch.object(tidyengine, "llvm_sdk_root", return_value=Path("C:/cache/llvm")) as root,
                mock.patch.object(tidy_command, "prepare_toolchain") as prepare,
            ):
                with contextlib.redirect_stdout(io.StringIO()) as stdout:
                    self.assertEqual(setup.run_command(self.parse(["setup", "llvm-sdk"])), 0)
        ensure.assert_called_once_with(host)
        version.assert_called_once_with(host)
        root.assert_called_once_with(host)
        prepare.assert_not_called()
        self.assertIn(tidyengine.windows_llvm_sdk_manifest()[0], stdout.getvalue())

    def test_llvm_sdk_derives_the_build_dir_override_sibling_and_skips_the_product_root(self):
        override = Path("C:/state/primary")
        with (
            mock.patch.object(builddir, "platform_profile", return_value=builddir.WINDOWS_PROFILE),
            mock.patch.dict(os.environ, {"BUILD_DIR": str(override)}, clear=False),
            mock.patch.object(tidyengine, "ensure_windows_llvm_sdk") as ensure,
            mock.patch.object(tidyengine, "llvm_sdk_version", return_value=tidyengine.windows_llvm_sdk_manifest()[0]),
            mock.patch.object(tidyengine, "llvm_sdk_root", return_value=Path("C:/cache/llvm")),
        ):
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(setup.run_command(self.parse(["setup", "llvm-sdk"])), 0)
        ensure.assert_called_once_with(override.with_name(override.name + "-tidy"))

    def test_llvm_sdk_rejects_compiler_cache_options(self):
        for option in ("--shared-workspaces", "--no-shared-workspaces"):
            with self.subTest(option=option):
                with (
                    mock.patch.object(builddir, "platform_profile", return_value=builddir.WINDOWS_PROFILE),
                    mock.patch.object(tidyengine, "ensure_windows_llvm_sdk") as ensure,
                ):
                    with contextlib.redirect_stderr(io.StringIO()) as stderr:
                        with self.assertRaises(SystemExit):
                            setup.run_command(self.parse(["setup", "llvm-sdk", option]))
                ensure.assert_not_called()
                self.assertIn("apply only to setup compiler-cache", stderr.getvalue())

    def test_other_components_still_dispatch_their_own_actions(self):
        with (
            mock.patch.object(builddir, "platform_profile", return_value=builddir.WINDOWS_PROFILE),
            mock.patch.object(setup.winui, "setup_runtime") as setup_runtime,
            mock.patch.object(tidyengine, "ensure_windows_llvm_sdk") as ensure,
        ):
            installed = mock.Mock(version="1.0", architecture="x64")
            setup_runtime.return_value = installed
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(setup.run_command(self.parse(["setup", "winui-runtime"])), 0)
        setup_runtime.assert_called_once_with()
        ensure.assert_not_called()


if __name__ == "__main__":
    unittest.main()
