"""Tests for ao.core.dedup — diagnostic block de-duplication."""

import io
import tempfile
import unittest
from pathlib import Path

from ao.core.dedup import deduplicate

ROOT = Path("/repo")

HEADER_DIAGNOSTIC = """\
/repo/include/aobus/Foo.h:10:5: warning: do not use magic numbers [readability-magic-numbers]
    int x = 42;
        ^
/repo/include/aobus/Foo.h:10:5: note: expanded from macro
1 warning generated.
"""

EXTERNAL_DIAGNOSTIC = """\
/nix/store/abc/gtkmm/box.h:5:1: warning: something external [bugprone-foo]
  external line;
"""


class DedupTest(unittest.TestCase):
    def _write_logs(self, *contents: str) -> list[Path]:
        temp_dir = Path(tempfile.mkdtemp(prefix="ao-dedup-test-"))
        logs = []
        for index, content in enumerate(contents):
            log = temp_dir / f"{index}.log"
            log.write_text(content, encoding="utf-8")
            logs.append(log)
        return logs

    def test_same_header_diagnostic_appears_once(self):
        logs = self._write_logs(HEADER_DIAGNOSTIC, HEADER_DIAGNOSTIC)
        out = io.StringIO()
        count = deduplicate(logs, out, ROOT)
        self.assertEqual(count, 1)
        self.assertEqual(out.getvalue().count("do not use magic numbers"), 1)

    def test_notes_and_context_stay_attached_to_their_block(self):
        logs = self._write_logs(HEADER_DIAGNOSTIC)
        out = io.StringIO()
        deduplicate(logs, out, ROOT)
        text = out.getvalue()
        self.assertIn("int x = 42;", text)
        self.assertIn("note: expanded from macro", text)

    def test_noise_lines_are_filtered(self):
        logs = self._write_logs(HEADER_DIAGNOSTIC + "Suppressed 12 warnings (10 in non-user code).\n")
        out = io.StringIO()
        deduplicate(logs, out, ROOT)
        self.assertNotIn("warning generated", out.getvalue())
        self.assertNotIn("Suppressed", out.getvalue())

    def test_distinct_diagnostics_are_both_kept(self):
        other = HEADER_DIAGNOSTIC.replace(":10:", ":20:")
        logs = self._write_logs(HEADER_DIAGNOSTIC, other)
        out = io.StringIO()
        count = deduplicate(logs, out, ROOT)
        self.assertEqual(count, 2)

    def test_external_blocks_dropped_unless_included(self):
        logs = self._write_logs(EXTERNAL_DIAGNOSTIC)
        excluded = io.StringIO()
        self.assertEqual(deduplicate(logs, excluded, ROOT, include_external=False), 0)
        self.assertNotIn("something external", excluded.getvalue())

        included = io.StringIO()
        self.assertEqual(deduplicate(logs, included, ROOT, include_external=True), 1)
        self.assertIn("something external", included.getvalue())

    def test_relative_paths_resolve_against_project_root(self):
        relative = HEADER_DIAGNOSTIC.replace("/repo/include", "include")
        logs = self._write_logs(HEADER_DIAGNOSTIC, relative)
        out = io.StringIO()
        self.assertEqual(deduplicate(logs, out, ROOT), 1)


if __name__ == "__main__":
    unittest.main()
