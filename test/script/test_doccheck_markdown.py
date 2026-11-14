"""Focused CommonMark and GFM regressions for the documentation link checker.

These regressions cover the doccheck parser findings from the second review of
the markdown link rewrite: stray opening brackets, GFM table row barriers,
anchors after fenced and setext headings, multiline images, and malformed
trailing constructs.

The contract is standard CommonMark plus the GFM table extension. Where exact
line numbers matter, the test-only `parse_links` fact-snapshot projection
reports the opening line of each link; public `check_tree` fixtures cover
target validity, navigation reachability, and anchor existence. Expectations
marked as controls exercise behavior the shared block and inline
interpretation already handles for links, definitions, and anchors.
"""

import tempfile
import unittest
from pathlib import Path

from doccheck_support import parse_links

from ao.core import doccheck


class DocCheckMarkdownRegressionTest(unittest.TestCase):
    def _write(self, root: Path, relative: str, text: str) -> Path:
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        return path

    def _parse(self, source: str) -> tuple[list[tuple[int, str]], list[tuple[int, str]]]:
        return parse_links(tuple(source.splitlines()))

    def test_stray_opening_bracket_cannot_capture_a_following_reference(self):
        # A reference label may not contain an unescaped '[', so a stray
        # opening bracket must stay literal text instead of swallowing the
        # next collapsed or shortcut reference, and it must not be reported
        # as an undefined reference either.
        for source in (
            "[stray\n[Guide][]\n\n[Guide]: guide.md",
            "[stray\n[Guide]\n\n[Guide]: guide.md",
        ):
            with self.subTest(source=source):
                self.assertEqual(self._parse(source), ([(2, "guide.md")], []))

    def test_gfm_table_rows_are_separate_and_cell_links_are_extracted(self):
        # GFM table rows are separate blocks: a '[' in one row cannot join
        # with '](destination)' in another row. A link that stays within one
        # cell is a real link at its own line, including reference links that
        # resolve a definition placed after the table.
        for source, expected in (
            ("| Label | Value |\n| --- | --- |\n| Open [ | A |\n| Close ](missing.md) | B |", []),
            ("| See | Guide |\n| --- | --- |\n| [Guide](guide.md) | value |", [(3, "guide.md")]),
            ("| Guide |\n| --- |\n| [Visible][Guide] |\n\n[Guide]: guide.md", [(3, "guide.md")]),
        ):
            with self.subTest(source=source):
                self.assertEqual(self._parse(source), (expected, []))

    def test_setext_and_post_fence_headings_supply_anchors(self):
        # A heading after a list-prefixed fence, and a setext heading, are
        # real anchors; the '# Fake' heading inside the fenced code is not.
        # The fake anchor is the only broken one in the tree.
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            self._write(
                root,
                "doc/README.md",
                "# Documentation\n"
                "\n"
                "- ```text\n"
                "  # Fake\n"
                "  ```\n"
                "## Real\n"
                "\n"
                "Setext Heading\n"
                "===\n"
                "\n"
                "[Real](#real)\n"
                "[Setext](#setext-heading)\n"
                "[Fake](#fake)\n",
            )

            issues = doccheck.check_tree(root)

        self.assertEqual([(issue.line, issue.kind) for issue in issues], [(13, "broken-anchor")])
        self.assertIn("'#fake'", issues[0].message)

    def test_multiline_image_keeps_the_surrounding_real_link_visible(self):
        # Control: a wrapped image is checked for its own target but must not
        # hide the real link that follows it; only that real link provides
        # navigation to the guide document.
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            self._write(
                root,
                "doc/README.md",
                "# Documentation\n\n![Wrapped\nlogo](missing.png) and [Guide](system/guide.md).\n",
            )
            self._write(root, "doc/system/guide.md", "# Guide\n")

            issues = doccheck.check_tree(root)

        self.assertEqual([(issue.line, issue.kind) for issue in issues], [(3, "broken-link")])
        self.assertIn("missing.png", issues[0].message)

    def test_malformed_later_constructs_do_not_corrupt_earlier_facts(self):
        # Control: an unclosed bracket, a definition-shaped line without a
        # closing bracket, a stray closer, and an unclosed fence at the end of
        # the document are literal text or trailing code; the earlier inline
        # link, the earlier definition, and the shortcut that uses it must
        # all survive, keeping the guide reachable and the tree clean.
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            self._write(
                root,
                "doc/README.md",
                "# Documentation\n"
                "\n"
                "[Good](system/guide.md)\n"
                "\n"
                "[Good]: system/guide.md\n"
                "\n"
                "[Bad: not-a-definition\n"
                "\n"
                "[Good]\n"
                "\n"
                "[unclosed(\n"
                "\n"
                "] (stray closer)\n"
                "\n"
                "```text\n"
                "never closed\n",
            )
            self._write(root, "doc/system/guide.md", "# Guide\n")

            issues = doccheck.check_tree(root)

        self.assertEqual(issues, [])


if __name__ == "__main__":
    unittest.main()
