"""Tests that keep doc/dev/naming-conventions.md and the naming tooling in sync."""

import re
import unittest

from ao.core import nameaudit
from ao.core.paths import PROJECT_ROOT

DOC_PATH = PROJECT_ROOT / "doc" / "dev" / "naming-conventions.md"
LINT_CHECK_DIR = PROJECT_ROOT / "tool" / "lint" / "check"


def _enforcement_section(text: str) -> str:
    match = re.search(r"^## Enforcement$(.*?)^## ", text, re.MULTILINE | re.DOTALL)
    if match is None:
        raise AssertionError("naming-conventions.md has no top-level '## Enforcement' section")
    return match.group(1)


def _bullets(section: str) -> list[str]:
    """Return bullet items with wrapped continuation lines joined."""
    bullets: list[str] = []
    current: list[str] = []
    for line in section.splitlines():
        if line.startswith("- "):
            if current:
                bullets.append(" ".join(current))
            current = [line[2:].strip()]
        elif line.startswith("  ") and current:
            current.append(line.strip())
        else:
            if current:
                bullets.append(" ".join(current))
            current = []
    if current:
        bullets.append(" ".join(current))
    return bullets


def _bullet_containing(section: str, phrase: str) -> str:
    for bullet in _bullets(section):
        if phrase in bullet:
            return bullet
    raise AssertionError(f"Enforcement section has no bullet containing {phrase!r}")


def _backticked(text: str) -> set[str]:
    return set(re.findall(r"`([^`]+)`", text))


class NamingDocTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.enforcement = _enforcement_section(DOC_PATH.read_text(encoding="utf-8"))

    def test_doc_lists_exactly_the_audited_role_suffixes(self):
        bullet = _bullet_containing(self.enforcement, "Layer placement for role suffixes")
        self.assertEqual(_backticked(bullet), set(nameaudit.ROLE_ALLOWED_PREFIXES))

    def test_doc_lists_exactly_the_banned_generic_file_suffixes(self):
        bullet = _bullet_containing(self.enforcement, "catch-all file name suffixes")
        self.assertEqual(_backticked(bullet), set(nameaudit.GENERIC_SUFFIXES))

    def test_doc_documents_the_audited_test_double_prefixes(self):
        bullet = _bullet_containing(self.enforcement, "must live under `test/`")
        for prefix in ("Fake*", "Mock*", "Spy*", "Stub*"):
            self.assertIn(prefix, _backticked(bullet))

    def test_enforcement_references_only_existing_lint_checks(self):
        check_names = {name for name in _backticked(self.enforcement) if name.endswith("Check")}
        self.assertTrue(check_names, "Enforcement section should reference the naming clang-tidy checks")
        for check in sorted(check_names):
            header = LINT_CHECK_DIR / f"{check}.h"
            self.assertTrue(header.exists(), f"doc references {check} but {header} does not exist")


if __name__ == "__main__":
    unittest.main()
