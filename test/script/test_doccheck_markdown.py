"""Focused CommonMark and GFM regressions for the documentation link checker.

These regressions cover the doccheck parser findings from the second review of
the markdown link rewrite: code-span reference labels, stray opening brackets,
one shared block interpretation for fenced code, container reference
definitions, and GFM table row barriers, plus adversarial controls for
destinations, titles, images, anchors, and malformed trailing constructs.

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

    def test_code_span_reference_labels_resolve_independently(self):
        # CommonMark normalizes reference labels by case and whitespace only;
        # code-span characters belong to the label text and must not make
        # distinct labels collide into one definition key.
        for source, expected in (
            (
                "[`alpha`][]\n[`beta`][]\n\n[`alpha`]: alpha.md\n[`beta`]: beta.md",
                [(1, "alpha.md"), (2, "beta.md")],
            ),
            (
                "[Use `alpha`][]\n[Use `beta`][]\n\n[Use `alpha`]: alpha.md\n[Use `beta`]: beta.md",
                [(1, "alpha.md"), (2, "beta.md")],
            ),
        ):
            with self.subTest(source=source):
                self.assertEqual(self._parse(source), (expected, []))

    def test_repeated_code_span_reference_labels_use_the_first_definition(self):
        # Control: repeated definitions of one normalized label keep the first.
        source = "[Ref][`alpha`]\n\n[`alpha`]: alpha.md\n[`alpha`]: shadowed.md"

        self.assertEqual(self._parse(source), ([(1, "alpha.md")], []))

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

    def test_definitions_inside_fenced_code_stay_hidden_in_every_container(self):
        # Fenced code content is never a reference definition, and the closing
        # fence must at least match the opening run length. The list variant
        # and the four-backtick variant are review regressions; the fully
        # quoted fence and inline code span are controls.
        for source, expected in (
            (
                "- ```markdown\n  [Guide]: absent.md\n  ```\n[Visible][Guide]\n\n[Guide]: guide.md",
                [(4, "guide.md")],
            ),
            (
                "> ```markdown\n> [Guide]: absent.md\n> ```\n[Visible][Guide]\n\n[Guide]: guide.md",
                [(4, "guide.md")],
            ),
            (
                "````markdown\n[Guide]: absent.md\n```\nstill code\n````\n[Visible][Guide]\n\n[Guide]: guide.md",
                [(6, "guide.md")],
            ),
            (
                "- `code [Guide]: absent.md` inline\n\n[Visible][Guide]\n\n[Guide]: guide.md",
                [(3, "guide.md")],
            ),
        ):
            with self.subTest(source=source):
                self.assertEqual(self._parse(source), (expected, []))

    def test_fenced_code_definition_cannot_shadow_a_real_reference(self):
        # Public oracle for the list-prefixed fence disagreement: the real
        # definition outside the fence must win, so the tree stays clean.
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            self._write(
                root,
                "doc/README.md",
                "# Documentation\n\n- ```markdown\n  [Guide]: missing.md\n  ```\n[Visible][Guide]\n\n"
                "[Guide]: guide.md\n",
            )
            self._write(root, "doc/guide.md", "# Guide\n")

            issues = doccheck.check_tree(root)

        self.assertEqual(issues, [])

    def test_container_reference_definitions_are_registered(self):
        # Reference definitions inside list items and block quotes are valid
        # document-wide definitions; the following shortcut must resolve and
        # keep the target reachable from the documentation root index.
        for definition in ("- [Guide]: system/guide.md\n", "> [Guide]: system/guide.md\n"):
            with self.subTest(definition=definition), tempfile.TemporaryDirectory() as temp_dir:
                root = Path(temp_dir)
                self._write(root, "doc/README.md", "# Documentation\n\n" + definition + "\n[Guide]\n")
                self._write(root, "doc/system/guide.md", "# Guide\n")

                issues = doccheck.check_tree(root)

                self.assertEqual(issues, [])

    def test_invalid_definition_tail_keeps_the_visible_inline_link(self):
        # '- [note]: prose [Actual](missing.md)' is not a reference definition:
        # the trailing prose is not a valid title, so the inline link must stay
        # visible and broken, and the valid container definition on the next
        # line must still resolve the shortcut reference.
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            self._write(
                root,
                "doc/README.md",
                "# Documentation\n\n- [note]: prose [Actual](missing.md)\n- [Guide]: system/guide.md\n\n[Guide]\n",
            )
            self._write(root, "doc/system/guide.md", "# Guide\n")

            issues = doccheck.check_tree(root)

        self.assertEqual([(issue.line, issue.kind) for issue in issues], [(3, "broken-link")])
        self.assertIn("missing.md", issues[0].message)

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

    def test_indented_code_and_escaped_brackets_are_not_links(self):
        # An indented code block is code content, and an escaped '[' is a
        # literal character, so neither construct is an inline link.
        for source in (
            "Intro paragraph.\n\n    [Guide](absent.md)",
            "\\[Guide\\](absent.md)",
        ):
            with self.subTest(source=source):
                self.assertEqual(self._parse(source), ([], []))

    def test_link_destinations_allow_balanced_parentheses(self):
        # Unescaped parentheses are allowed in a destination only as a
        # balanced pair; an unbalanced inner opening parenthesis consumes the
        # closing parenthesis, so the construct is not a link at all.
        for source, expected in (
            ("[link](foo(and(bar)).md)", [(1, "foo(and(bar)).md")]),
            ("[link](foo(and(bar).md)", []),
        ):
            with self.subTest(source=source):
                self.assertEqual(self._parse(source), (expected, []))

    def test_title_bearing_links_keep_their_destination(self):
        # Controls: double-quoted, single-quoted, and parenthesized titles are
        # part of the inline link, not part of its destination.
        for link, destination in (
            ('[Guide](guide.md "System guide")', "guide.md"),
            ("[Guide](guide.md 'System guide')", "guide.md"),
            ("[link](target.md (parenthetical title))", "target.md"),
        ):
            with self.subTest(link=link):
                self.assertEqual(self._parse(link), ([(1, destination)], []))

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
