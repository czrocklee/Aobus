"""Fixture-derived custom-check inventory and symmetric plugin validation."""

import argparse
import json
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao.command import tidy
from ao.core import bazel_lint_action as action
from ao.core import linttest


class TidyInventoryTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.fixtures = self.root / "test/lint/fixture"
        self.fixtures.mkdir(parents=True)
        self.enterContext(mock.patch.object(linttest, "FIXTURE_DIR", self.fixtures))

    def fixture(self, name, source="// POSITIVE\nint value;\n"):
        path = self.fixtures / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(source, encoding="utf-8")

    def test_inventory_uses_nonblank_supported_custom_fixtures_on_every_host(self):
        for name in (
            "aobus-first/Basic.cpp",
            "aobus-first/Other.cpp",
            "aobus-header/Basic.h",
            "aobus-darwin/Basic.mm",
            "aobus-Legacy_Alias/Basic.cpp",
            "modernize-use-starts-ends-with/Basic.cpp",
            "aobus-unsupported/README.txt",
        ):
            self.fixture(name)
        self.fixture("aobus-empty/Basic.cpp", "")
        self.fixture("aobus-whitespace/Basic.cpp", " \t\n")
        # Clean, marker-free inputs are valid negative controls, not evidence
        # that the checker has positive coverage (the lint suite owns that).
        self.fixture("aobus-clean/Basic.h", "// SPDX-License-Identifier: MIT\n")
        (self.fixtures / "aobus-empty-directory").mkdir()
        self.assertEqual(
            tidy.expected_aobus_checks(),
            frozenset({"aobus-first", "aobus-header", "aobus-darwin", "aobus-Legacy_Alias", "aobus-clean"}),
        )

    def prepare(self):
        configuration = self.root / "configuration.json"
        configuration.write_text(
            json.dumps(
                {
                    "schema": 1,
                    "source_root": str(self.root),
                    "alpha": False,
                    "check": None,
                    "header_filter": None,
                    "clang_tidy_args": [],
                }
            ),
            encoding="utf-8",
        )
        policy = self.root / "policy.json"
        action.prepare(argparse.Namespace(configuration=configuration, output=policy))
        return policy

    def tools(self):
        policy = self.prepare()
        sdk = self.root / "sdk.json"
        sdk.write_text(
            json.dumps(
                {
                    "schema": 1,
                    "platform": "linux",
                    "version": "22.1.0",
                    "host_kind": "plugin",
                    "system_arguments": [],
                    "analysis_arguments": [],
                }
            ),
            encoding="utf-8",
        )
        host, plugin = self.root / "clang-tidy", self.root / "plugin.so"
        host.touch()
        plugin.touch()
        return argparse.Namespace(
            policy=policy,
            sdk=sdk,
            tidy=host,
            plugin=plugin,
            output=self.root / "verified.json",
            log=self.root / "verification.log",
        )

    def test_registry_reports_both_fixture_and_toolchain_mismatches(self):
        self.fixture("aobus-first/Basic.cpp")
        self.fixture("aobus-second/Basic.cpp")
        self.fixture("aobus-whitespace/Basic.cpp", " \t\n")
        tools = self.tools()
        for registered, errors in (
            (("aobus-first", "aobus-second"), ()),
            (("aobus-first",), ("fixture checks absent from the selected toolchain: aobus-second",)),
            (
                ("aobus-first", "aobus-second", "aobus-stale", "aobus-whitespace"),
                ("registered checks without non-blank supported fixtures: aobus-stale, aobus-whitespace",),
            ),
            (
                ("aobus-stale",),
                (
                    "fixture checks absent from the selected toolchain: aobus-first, aobus-second",
                    "registered checks without non-blank supported fixtures: aobus-stale",
                ),
            ),
            (("aobus-first", "aobus-second", "aobus-first"), ("duplicate registered Aobus checks",)),
        ):

            def invoke(command, registered=registered, **_kwargs):
                if command == [str(tools.tidy), "--version"]:
                    return subprocess.CompletedProcess(command, 0, "LLVM version 22.1.0\n")
                self.assertEqual(
                    command, [str(tools.tidy), f"-load={tools.plugin}", "-checks=-*,aobus-*", "-list-checks"]
                )
                return subprocess.CompletedProcess(command, 0, "Enabled checks:\n" + "\n".join(registered))

            with self.subTest(registered=registered), mock.patch.object(action.subprocess, "run", side_effect=invoke):
                if not errors:
                    self.assertEqual(action.verify(tools), 0)
                    self.assertTrue(json.loads(tools.output.read_text())["verified"])
                else:
                    with self.assertRaisesRegex(
                        ValueError, "registry differs from the working fixture registry"
                    ) as failure:
                        action.verify(tools)
                    self.assertFalse(tools.output.exists())
                    for error in errors:
                        self.assertIn(error, str(failure.exception))
                        self.assertIn(error, tools.log.read_text())
                    self.assertIn("\n".join(registered), tools.log.read_text())
