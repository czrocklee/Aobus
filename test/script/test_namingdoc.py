"""Tests that keep the naming-check reference and naming tooling in sync."""

import contextlib
import io
import re
import unittest
from pathlib import Path
from unittest import mock

from ao.command import docs
from ao.core import doccheck
from ao.core.paths import PROJECT_ROOT

DOC_PATH = PROJECT_ROOT / "doc" / "development" / "lint" / "naming-checks.md"


class NamingDocTest(unittest.TestCase):
    def test_documentation_gate_accepts_current_naming_contract(self):
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()) as errors:
            self.assertEqual(docs.run_check(None), 0, errors.getvalue())

    def test_naming_contract_does_not_depend_on_section_headings(self):
        original = DOC_PATH.read_text(encoding="utf-8")
        without_headings = re.sub(r"^#{1,6} .*$", "", original, flags=re.MULTILINE)
        document = doccheck.Document(
            path=DOC_PATH,
            lines=tuple(without_headings.splitlines()),
            metadata={},
            body_line=1,
        )

        issues = doccheck._check_naming_contract({DOC_PATH: document}, PROJECT_ROOT)

        self.assertEqual(issues, [])

    def test_documentation_gate_rejects_semantic_drift(self):
        original_read = Path.read_text
        original = DOC_PATH.read_text(encoding="utf-8")
        for before, after in (
            ("`Service`", "`BogusRole`"),
            ("`Utils`", "`BogusSuffix`"),
            ("`Spy*`", "`Bogus*`"),
            ("`IdentifierNamingExtensionsCheck`", "`MissingNamingCheck`"),
            ("`ResultNamingConventionCheck`", "`IdentifierNamingExtensionsCheck`"),
        ):
            altered = original.replace(before, after)
            self.assertNotEqual(altered, original)

            def read_overlay(path, *args, replacement=altered, **kwargs):
                return replacement if path == DOC_PATH else original_read(path, *args, **kwargs)

            with self.subTest(before=before), mock.patch.object(Path, "read_text", read_overlay):
                with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()) as errors:
                    self.assertEqual(docs.run_check(None), 1)
                self.assertIn("naming-contract", errors.getvalue())


if __name__ == "__main__":
    unittest.main()
