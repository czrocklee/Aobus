"""Fixture-derived custom-check inventory and symmetric plugin validation."""

import contextlib
import io
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao.command import tidy
from ao.core import linttest


class TidyInventoryTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.fixtures = Path(temporary.name) / "fixtures"
        self.fixtures.mkdir()
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

    def test_empty_or_missing_custom_inventory_fails_before_running_a_tool(self):
        self.fixture("modernize-use-starts-ends-with/Basic.cpp")
        toolchain = tidy.TidyToolchain("clang-tidy", Path("plugin.so"), None)
        with (
            mock.patch.object(tidy.subprocess, "run") as run,
            contextlib.redirect_stderr(io.StringIO()) as error,
            self.assertRaises(SystemExit),
        ):
            tidy.verify_tidy_toolchain(toolchain)
        run.assert_not_called()
        self.assertIn("no non-blank Aobus checker fixtures", error.getvalue())
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            tidy.expected_aobus_checks(self.fixtures / "missing")

    def test_registry_reports_both_fixture_and_toolchain_mismatches(self):
        self.fixture("aobus-first/Basic.cpp")
        self.fixture("aobus-Legacy_Alias/Basic.cpp")
        self.fixture("aobus-whitespace/Basic.cpp", " \t\n")
        toolchain = tidy.TidyToolchain("clang-tidy", Path("plugin.so"), None)
        for registered, expected_errors in (
            (("aobus-first", "aobus-Legacy_Alias"), ()),
            (("aobus-first",), ("fixture checks absent from the selected toolchain: aobus-Legacy_Alias",)),
            (
                ("aobus-first", "aobus-Legacy_Alias", "aobus-stale", "aobus-whitespace"),
                ("registered checks without non-blank supported fixtures: aobus-stale, aobus-whitespace",),
            ),
            (
                ("aobus-stale",),
                (
                    "fixture checks absent from the selected toolchain: aobus-Legacy_Alias, aobus-first",
                    "registered checks without non-blank supported fixtures: aobus-stale",
                ),
            ),
        ):
            with self.subTest(registered=registered), contextlib.redirect_stderr(io.StringIO()) as error:
                result = mock.Mock(returncode=0, stdout="Enabled checks:\n" + "\n".join(registered))
                with mock.patch.object(tidy.subprocess, "run", return_value=result):
                    if not expected_errors:
                        tidy.verify_tidy_toolchain(toolchain)
                    else:
                        with self.assertRaises(SystemExit):
                            tidy.verify_tidy_toolchain(toolchain)
                        for expected_error in expected_errors:
                            self.assertIn(expected_error, error.getvalue())
                        self.assertIn(str(self.fixtures), error.getvalue())
                        self.assertIn("including untracked files", error.getvalue())

    def test_failed_registry_query_is_not_reported_as_a_fixture_mismatch(self):
        self.fixture("aobus-first/Basic.cpp")
        with (
            mock.patch.object(tidy.subprocess, "run", return_value=mock.Mock(returncode=2, stdout="")),
            contextlib.redirect_stderr(io.StringIO()) as error,
            self.assertRaises(SystemExit),
        ):
            tidy.verify_tidy_toolchain(tidy.TidyToolchain("clang-tidy", Path("plugin.so"), None))
        self.assertIn("cannot list the selected clang-tidy check registry (exit 2)", error.getvalue())
        self.assertNotIn("working-tree fixtures", error.getvalue())

    def test_fixture_read_failure_names_the_inventory_root(self):
        self.fixture("aobus-first/Basic.cpp")
        with (
            mock.patch.object(Path, "read_text", side_effect=OSError("read denied")),
            contextlib.redirect_stderr(io.StringIO()) as error,
            self.assertRaises(SystemExit),
        ):
            tidy.expected_aobus_checks()
        self.assertIn(f"cannot read Aobus checker fixtures under {self.fixtures}", error.getvalue())
