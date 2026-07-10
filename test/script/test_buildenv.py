"""Tests for ao.core.buildenv — per-command native build environment needs."""

import argparse
import contextlib
import io
import unittest

from ao.__main__ import make_parser
from ao.command import COMMAND_MODULES
from ao.core import buildenv


class BuildEnvTest(unittest.TestCase):
    def test_every_command_module_declares_its_metadata(self):
        names = [module.NAME for module in COMMAND_MODULES]
        self.assertEqual(sorted(names), sorted(set(names)))
        for module in COMMAND_MODULES:
            self.assertIsInstance(module.NAME, str, module)
            self.assertIsInstance(module.REQUIRES_BUILD_ENV, bool, module)

    def test_registered_subparsers_match_declared_names(self):
        parser = make_parser()
        subparsers = next(action for action in parser._actions if isinstance(action, argparse._SubParsersAction))
        self.assertEqual(set(subparsers.choices), {module.NAME for module in COMMAND_MODULES})

    def test_native_toolchain_commands_require_the_build_environment(self):
        self.assertEqual(
            {module.NAME for module in COMMAND_MODULES if module.REQUIRES_BUILD_ENV},
            {"analyze", "build", "check", "deps", "format", "hygiene", "run", "test", "tidy"},
        )

    def test_requires_build_env_answers_by_command_name(self):
        self.assertTrue(buildenv.requires_build_env("build"))
        self.assertFalse(buildenv.requires_build_env("council"))
        self.assertFalse(buildenv.requires_build_env("no-such-command"))
        self.assertFalse(buildenv.requires_build_env(""))

    def test_main_prints_a_batch_consumable_flag(self):
        for arguments, expected in (["check"], "1"), (["name-audit"], "0"), ([], "0"):
            stdout = io.StringIO()
            with contextlib.redirect_stdout(stdout):
                status = buildenv.main(arguments)
            self.assertEqual(status, 0)
            self.assertEqual(stdout.getvalue().strip(), expected)


if __name__ == "__main__":
    unittest.main()
