"""Tests that keep doc/development/naming-convention.md and the naming tooling in sync."""

import contextlib
import io
import unittest
from pathlib import Path
from unittest import mock

from ao.command import docs
from ao.core.paths import PROJECT_ROOT

DOC_PATH = PROJECT_ROOT / "doc" / "development" / "naming-convention.md"


class NamingDocTest(unittest.TestCase):
    def test_documentation_gate_accepts_current_naming_contract(self):
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()) as errors:
            self.assertEqual(docs.run_check(None), 0, errors.getvalue())

    def test_documentation_gate_rejects_semantic_drift(self):
        original_read = Path.read_text
        original = DOC_PATH.read_text(encoding="utf-8")
        for before, after in (
            ("`Service`", "`BogusRole`"),
            ("`Utils`", "`BogusSuffix`"),
            ("`Spy*`", "`Bogus*`"),
            ("`IdentifierNamingExtensionsCheck`", "`MissingNamingCheck`"),
            ("`ResultNamingConventionCheck`", "`IdentifierNamingExtensionsCheck`"),
            ("## Enforcement", "## Other enforcement"),
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
