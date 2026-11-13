"""UIModel organization root and layout contracts of the shared audit backend."""

import tempfile
import unittest
from pathlib import Path

from ao.core import guardrails


class UimodelOrganizationTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus organization ")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.roots = {name: self.root / name for name in ("public", "source", "test")}
        for root in self.roots.values():
            root.mkdir()

    def audit(self, kind="uimodel-organization", **overrides):
        roots = {**self.roots, **overrides}
        return guardrails.audit_uimodel(kind, roots["public"], roots["source"], roots["test"])

    def put(self, name, relative, source):
        path = self.roots[name] / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(source, encoding="utf-8")
        return path

    def test_valid_roots_still_enforce_layout_and_namespaces(self):
        for name in self.roots:
            self.put(name, "field/Example.h", "namespace ao::uimodel {}\n")
        self.assertTrue(self.audit().ok)

        # Test files declare the test namespace; public headers declare detail.
        for name in ("public", "test"):
            with self.subTest(root=name):
                self.put(name, "field/Example.h", "namespace ao::uimodel::feature {}\n")
                result = self.audit()
                self.assertFalse(result.ok, result.errors)
                self.assertIn("declares a feature namespace", result.errors[0].detail)
                self.put(name, "field/Example.h", "namespace ao::uimodel {}\n")

        # Source files outside the final folder layout are rejected in every root.
        for name in self.roots:
            with self.subTest(root=name):
                self.put(name, "Outside.cpp", "")
                result = self.audit()
                self.assertFalse(result.ok, result.errors)
                self.assertIn("outside the final UIModel folder layout", result.errors[0].detail)
                self.assertIn("Outside.cpp", result.errors[0].path)
                (self.roots[name] / "Outside.cpp").unlink()

    def test_narrow_suffixes_are_the_organization_audit_scope(self):
        # The organization audit scans .h/.hpp/.cpp only; a violating .mm file
        # is the neutrality kind's contract, not this one.
        self.put("source", "field/Example.mm", "native code\n")
        self.assertTrue(self.audit().ok)


if __name__ == "__main__":
    unittest.main()
