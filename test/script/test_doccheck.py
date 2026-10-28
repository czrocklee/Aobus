"""Tests for documentation metadata, links, anchors, and reachability."""

import tempfile
import unittest
from pathlib import Path

from doccheck_support import parse_links

from ao.core import doccheck


class DocCheckTest(unittest.TestCase):
    def _write(self, root: Path, relative: str, text: str) -> Path:
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        return path

    def _metadata(self, **fields: str) -> str:
        lines = ["---", *(f"{key}: {value}" for key, value in fields.items()), "---"]
        return "\n".join(lines) + "\n"

    def _valid_tree(self, root: Path) -> tuple[Path, Path]:
        self._write(root, "doc/README.md", "# Documentation\n\n[System guide](system/guide.md)\n")
        guide = self._write(
            root,
            "doc/system/guide.md",
            "# System guide\n\n[Playback details](architecture/playback.md#storage--compatibility)\n",
        )
        topic = self._write(
            root,
            "doc/system/architecture/playback.md",
            "# Playback\n\n## Storage & compatibility\n\nCurrent details.\n",
        )
        return guide, topic

    def test_accepts_metadata_free_system_topics_free_sections_and_indirect_reachability(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            self._valid_tree(root)

            issues = doccheck.check_tree(root)

        self.assertEqual(issues, [])

    def test_accepts_unrestricted_directory_names_and_no_direct_index_registration(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            guide, _ = self._valid_tree(root)
            guide.write_text(
                guide.read_text(encoding="utf-8") + "\n[Plural area](specifications/player.md)\n",
                encoding="utf-8",
            )
            self._write(
                root,
                "doc/system/specifications/player.md",
                "# Player behavior\n\n## Any useful section\n\nThe guide is its only incoming link.\n",
            )

            issues = doccheck.check_tree(root)

        self.assertEqual(issues, [])

    def test_accepts_optional_and_historical_metadata_without_schema_gates(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            guide, topic = self._valid_tree(root)
            topic.write_text(
                self._metadata(
                    id="playback.topic",
                    type="architecture",
                    status="implemented",
                    domain="Historical.Domain",
                    summary="Historical summary without punctuation",
                )
                + "# Playback\n\n## Storage & compatibility\n\nCurrent details.\n",
                encoding="utf-8",
            )
            guide.write_text(guide.read_text(encoding="utf-8") + "\n[Minimal](minimal.md)\n", encoding="utf-8")
            self._write(root, "doc/system/minimal.md", self._metadata(id="system.minimal") + "Body without an H1.\n")

            issues = doccheck.check_tree(root)

        self.assertEqual(issues, [])

    def test_accepts_all_supported_status_markers(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            statuses = sorted(doccheck.VALID_STATUSES)
            links = "".join(f"- [{status}](status/{status}.md)\n" for status in statuses)
            self._write(root, "doc/README.md", "# Documentation\n\n" + links)
            for status in statuses:
                self._write(
                    root,
                    f"doc/status/{status}.md",
                    self._metadata(status=status) + f"# {status}\n",
                )

            issues = doccheck.check_tree(root)

        self.assertEqual(issues, [])

    def test_accepts_ordinary_proposal_and_legacy_rfc_without_dependency_contract(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            self._write(
                root,
                "doc/README.md",
                "# Documentation\n\n[Proposal](system/proposal.md)\n[Legacy RFC](rfc/0042-legacy.md)\n",
            )
            self._write(
                root,
                "doc/system/proposal.md",
                "# Proposal\n\n## Motivation\n\nA normal proposal with no dependency section.\n",
            )
            self._write(
                root,
                "doc/rfc/0042-legacy.md",
                self._metadata(id="rfc.0042.legacy", type="rfc", status="implemented")
                + "# Legacy RFC\n\nHistorical proposal text with no dependency metadata or category list.\n",
            )

            issues = doccheck.check_tree(root)

        self.assertEqual(issues, [])

    def test_reports_optional_invalid_id_and_status(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            _, topic = self._valid_tree(root)
            topic.write_text(
                self._metadata(id="Invalid ID", status="archived") + "# Playback\n",
                encoding="utf-8",
            )

            issues = doccheck.check_tree(root)

        messages = [issue.message for issue in issues]
        self.assertIn("invalid document id 'Invalid ID'", messages)
        self.assertTrue(any("invalid document status 'archived'" in message for message in messages))

    def test_rejects_empty_quoted_id_and_status(self):
        for field in ("id", "status"):
            with self.subTest(field=field), tempfile.TemporaryDirectory() as temp_dir:
                root = Path(temp_dir)
                _, topic = self._valid_tree(root)
                topic.write_text(self._metadata(**{field: '""'}) + "# Playback\n", encoding="utf-8")

                issues = doccheck.check_tree(root)

                self.assertTrue(any(issue.kind == "metadata" for issue in issues))

    def test_reports_front_matter_parser_errors_without_requiring_front_matter(self):
        cases = {
            "unclosed": (
                "---\nid: system.topic\n# Topic\n",
                "front matter has no closing '---' delimiter",
            ),
            "empty entry": (
                "---\nid:\n---\n# Topic\n",
                "metadata entries must be non-empty 'key: value' scalars",
            ),
            "missing separator": (
                "---\nid system.topic\n---\n# Topic\n",
                "metadata entries must be non-empty 'key: value' scalars",
            ),
            "duplicate key": (
                "---\nid: system.topic\nid: system.other\n---\n# Topic\n",
                "duplicate metadata key 'id'",
            ),
            "structured value": (
                "---\nid: [system.topic]\n---\n# Topic\n",
                "metadata values must use the flat scalar subset",
            ),
        }
        for name, (content, expected) in cases.items():
            with self.subTest(name=name), tempfile.TemporaryDirectory() as temp_dir:
                root = Path(temp_dir)
                _, topic = self._valid_tree(root)
                topic.write_text(content, encoding="utf-8")

                messages = [issue.message for issue in doccheck.check_tree(root)]

                self.assertIn(expected, messages)

    def test_reports_duplicate_optional_ids(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            guide, topic = self._valid_tree(root)
            topic.write_text(self._metadata(id="system.shared") + "# Playback\n", encoding="utf-8")
            guide.write_text(guide.read_text(encoding="utf-8") + "\n[Peer](peer.md)\n", encoding="utf-8")
            self._write(root, "doc/system/peer.md", self._metadata(id="system.shared") + "# Peer\n")

            issues = doccheck.check_tree(root)

        self.assertEqual(sum(issue.kind == "duplicate-id" for issue in issues), 2)

    def test_reports_broken_links_anchors_references_and_plan_links(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            guide, _ = self._valid_tree(root)
            self._write(root, "doc/plan/local.md", "# Local plan\n")
            guide.write_text(
                guide.read_text(encoding="utf-8")
                + "\n[Missing](absent.md)\n"
                + "[Bad anchor](architecture/playback.md#absent)\n"
                + "[Plan](../plan/local.md)\n"
                + "[Missing reference][unknown]\n",
                encoding="utf-8",
            )

            issues = doccheck.check_tree(root)

        kinds = {issue.kind for issue in issues}
        self.assertTrue({"broken-link", "broken-anchor", "broken-reference", "plan-link"}.issubset(kinds))

    def test_wrapped_inline_link_reports_missing_target_at_its_opening_line(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir).resolve()
            guide, _ = self._valid_tree(root)
            original = guide.read_text(encoding="utf-8")
            guide.write_text(
                original + "\nSee [library change\npublication](architecture/runtime/playback.md).\n",
                encoding="utf-8",
            )

            issues = doccheck.check_tree(root)

            self.assertEqual(
                [(issue.path, issue.line, issue.kind, issue.message) for issue in issues],
                [(guide, 5, "broken-link", "target does not exist: architecture/runtime/playback.md")],
            )
            guide.write_text(
                original + "\nSee [library change\npublication](architecture/playback.md).\n",
                encoding="utf-8",
            )
            self.assertEqual(doccheck.check_tree(root), [])

    def test_wrapped_links_and_reference_labels_preserve_reachability(self):
        for link in (
            "[System\nguide](system/guide.md)",
            "[System\nguide][system guide]",
            "[Guide][system\nguide]",
            "[System\nguide][]",
            "[System\nguide]",
        ):
            with self.subTest(link=link), tempfile.TemporaryDirectory() as temp_dir:
                root = Path(temp_dir)
                self._write(root, "doc/README.md", f"# Documentation\n\n{link}\n\n[system guide]: system/guide.md\n")
                self._write(root, "doc/system/guide.md", "# Guide\n")

                self.assertEqual(doccheck.check_tree(root), [])

    def test_wrapped_undefined_reference_reports_its_opening_line(self):
        links, undefined = parse_links(("# Guide", "", "See [missing", "guide][unknown", "label]."))

        self.assertEqual(links, [])
        self.assertEqual(undefined, [(3, "unknown label")])

    def test_multiline_code_spans_do_not_supply_links_or_shift_following_line_numbers(self):
        links, undefined = parse_links(
            (
                "# Guide",
                "",
                "`[Code",
                "example](ignored.md)` then [actual",
                "link](target.md).",
                "A `[code]` [next",
                "link](other.md).",
            )
        )

        self.assertEqual(links, [(4, "target.md"), (6, "other.md")])
        self.assertEqual(undefined, [])

    def test_only_equal_backtick_runs_hide_links_within_one_paragraph(self):
        for source, expected in (
            ("`unclosed\n[Visible](absent.md)", [(2, "absent.md")]),
            ("`unclosed\n[Visible](absent.md)\n``different delimiter", [(2, "absent.md")]),
            ("``code with a ` character\n[Hidden](absent.md)\n``", []),
            ("`valid multiline\n[Hidden](absent.md)\ncode`", []),
            (r"\`literal [Visible](absent.md)`", [(1, "absent.md")]),
        ):
            with self.subTest(source=source):
                self.assertEqual(parse_links(tuple(source.splitlines())), (expected, []))

    def test_inline_spans_do_not_cross_list_quote_or_setext_block_boundaries(self):
        for source in (
            "- [Draft\n- notes](absent.md)",
            "1. [Draft\n2. notes](absent.md)",
            "[Draft\n> notes](absent.md)",
            "> [Draft\n- notes](absent.md)",
            "[Draft\n---\nnotes](absent.md)",
            "[Draft\n===\nnotes](absent.md)",
        ):
            with self.subTest(source=source):
                self.assertEqual(parse_links(tuple(source.splitlines())), ([], []))
        for source, expected in (
            ("- [Wrapped\n  label](target.md)", [(1, "target.md")]),
            ("> [Wrapped\n> label](target.md)", [(1, "target.md")]),
            ("> [Wrapped\nlabel](target.md)", [(1, "target.md")]),
            ("- `unclosed\n- [Visible](target.md)`", [(2, "target.md")]),
            ("`unclosed\n> [Visible](target.md)`", [(2, "target.md")]),
        ):
            with self.subTest(source=source):
                self.assertEqual(parse_links(tuple(source.splitlines())), (expected, []))

    def test_wrapped_link_cannot_cross_blank_lines_or_fences(self):
        for boundary in ("", "   ", "```markdown\n```", "~~~markdown\n~~~"):
            with self.subTest(boundary=boundary):
                lines = tuple(f"[not a link\n{boundary}\nlabel](absent.md)".splitlines())

                self.assertEqual(parse_links(lines), ([], []))

    def test_definition_like_text_cannot_interrupt_an_existing_paragraph(self):
        # CommonMark 4.7: reference definitions cannot interrupt a paragraph.
        lines = ("[not a link", "[target]: other.md", "label](absent.md)")

        self.assertEqual(parse_links(lines), ([(1, "absent.md")], []))

    def test_heading_links_do_not_join_surrounding_paragraphs(self):
        lines = ("[Unclosed prose", "# [Heading](target.md)", "label](absent.md)")

        self.assertEqual(parse_links(lines), ([(2, "target.md")], []))

    def test_empty_link_destinations_do_not_crash_or_invent_missing_targets(self):
        for link in ("[Here]( )", "[Here](\n)"):
            with self.subTest(link=link), tempfile.TemporaryDirectory() as temp_dir:
                root = Path(temp_dir).resolve()
                self._write(root, "doc/README.md", f"# Documentation\n\n{link}\n")

                self.assertEqual(doccheck.check_tree(root), [])

    def test_wrapped_image_targets_are_checked_but_excluded_from_navigation(self):
        lines = ("# Guide", "", "![Missing", "image](absent.png)")

        self.assertEqual(parse_links(lines), ([(3, "absent.png")], []))
        self.assertEqual(parse_links(lines, include_images=False), ([], []))

    def test_wrapped_links_in_fenced_examples_are_ignored(self):
        for marker in ("```", "~~~"):
            with self.subTest(marker=marker):
                links, undefined = parse_links(
                    (f"{marker}markdown", "[Ignored", "link](absent.md)", marker, "[Visible", "link](target.md)")
                )

                self.assertEqual(links, [(5, "target.md")])
                self.assertEqual(undefined, [])

    def test_reference_style_links_and_supported_anchors_preserve_reachability(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            self._write(
                root,
                "doc/README.md",
                "# Documentation\n\n[Guide][system guide]\n\n[system guide]: system/guide.md\n",
            )
            self._write(
                root,
                "doc/system/guide.md",
                "# Guide\n\n[Topic][]\n\n[Topic]: topic.md#repeated-1\n",
            )
            self._write(
                root,
                "doc/system/topic.md",
                "# Topic\n\n## Repeated\n\nFirst.\n\n## Repeated\n\nSecond.\n\n"
                '<a id="custom-anchor"></a>\n\n'
                '<a class="anchor" id="multi-attr-anchor"></a>\n\n'
                "[Custom](#custom-anchor)\n[Multi](#multi-attr-anchor)\n[Root absolute](/doc/README.md)\n[External](https://example.com/)\n",
            )

            issues = doccheck.check_tree(root)

        self.assertEqual(issues, [])

    def test_heading_anchors_use_rendered_text_without_decoding_code_or_entities_twice(self):
        lines = ("# &amp;copy;", "# `&amp;copy;`", r"# A \*literal\* _word_", "# Caf&eacute;", r"# \_word\_")

        self.assertEqual(
            doccheck._parse_markdown(lines).anchors,
            frozenset({"copy", "ampcopy", "a-literal-word", "café", "_word_"}),
        )

    def test_unknown_images_report_one_reference_error_without_navigation_errors(self):
        lines = ("![Missing", "image][unknown]", r"\![Visible][missing]")

        self.assertEqual(parse_links(lines), ([], [(1, "unknown"), (3, "missing")]))
        # Undefined reference diagnostics are independent of navigation edges:
        # include_images=False drops image link edges but keeps image
        # reference-definition diagnostics, unlike the previous seam.
        self.assertEqual(parse_links(lines, include_images=False), ([], [(1, "unknown"), (3, "missing")]))

    def test_image_alternative_text_does_not_supply_links_or_undefined_references(self):
        lines = ("![An [inner](ignored.md) and [unknown][missing]](image.png)",)

        self.assertEqual(parse_links(lines), ([(1, "image.png")], []))
        self.assertEqual(parse_links(lines, include_images=False), ([], []))
        self.assertEqual(doccheck._parse_markdown(('![Banner <a id="fake"></a>](image.png)',)).anchors, frozenset())

    def test_html_anchors_preserve_id_and_name_with_quoted_or_multiline_attributes(self):
        for markup in (
            '<a id="real" name="legacy"></a>',
            "<a title='display > cutoff' name='legacy' data-note='id=\"ignored\"' id='real'></a>",
            '<A\n class="anchor"\n ID = "real"\n NAME = "legacy"></A>',
        ):
            with self.subTest(markup=markup), tempfile.TemporaryDirectory() as temp_dir:
                root = Path(temp_dir)
                self._write(
                    root,
                    "doc/README.md",
                    f"# Documentation\n\n{markup}\n\n[Id](#real)\n[Name](#legacy)\n",
                )

                self.assertEqual(doccheck.check_tree(root), [])

    def test_html_anchors_do_not_come_from_attribute_values_or_code_examples(self):
        for markup in (
            "<a title=\"see id='fake'\"></a>",
            '<a data-id="fake"></a>',
            '<!-- <a id="fake"></a> -->',
            '`<a id="fake"></a>`',
            '```html\n<a id="fake"></a>\n```',
        ):
            with self.subTest(markup=markup), tempfile.TemporaryDirectory() as temp_dir:
                root = Path(temp_dir)
                self._write(root, "doc/README.md", f"# Documentation\n\n{markup}\n\n[Fake](#fake)\n")

                issues = doccheck.check_tree(root)

                self.assertEqual([issue.kind for issue in issues], ["broken-anchor"])
                self.assertIn("'#fake'", issues[0].message)

    def test_ignores_links_and_headings_in_fenced_blocks(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            guide, topic = self._valid_tree(root)
            topic.write_text(
                "# Playback\n\n## Visible heading\n\n"
                "```markdown\n[Ignored missing link](absent.md)\n## Ignored heading\n```\n",
                encoding="utf-8",
            )
            guide.write_text(
                guide.read_text(encoding="utf-8").replace(
                    "playback.md#storage--compatibility", "playback.md#visible-heading"
                ),
                encoding="utf-8",
            )

            issues = doccheck.check_tree(root)

        self.assertEqual(issues, [])

    def test_shortcut_references_and_inline_code_keep_their_existing_link_behavior(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            self._write(root, "doc/README.md", "# Documentation\n\n[Guide]\n\n[Guide]: system/guide.md\n")
            self._write(
                root,
                "doc/system/guide.md",
                "# Guide\n\n`[Not a link](absent.md)`\n\n[Missing][target]\n\n[target]: absent.md\n",
            )

            issues = doccheck.check_tree(root)

        self.assertEqual([issue.kind for issue in issues], ["broken-link"])

    def test_plan_links_cannot_bypass_validation_through_encoded_or_parent_paths(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            self._valid_tree(root)
            self._write(root, "doc/plan/local.md", "# Private plan\n")
            self._write(
                root,
                ".agents/skills/example/SKILL.md",
                "# Skill\n\n[Private](/doc/system/../%70lan/local.md)\n",
            )

            issues = doccheck.check_tree(root)

        self.assertEqual([issue.kind for issue in issues], ["plan-link"])

    def test_reference_links_use_the_first_normalized_definition(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            self._write(
                root,
                "doc/README.md",
                "# Documentation\n\n[Guide][Topic Label]\n\n[topic label]: absent.md\n[TOPIC   LABEL]: guide.md\n",
            )
            self._write(root, "doc/guide.md", "# Guide\n")

            issues = doccheck.check_tree(root)

        self.assertEqual({issue.kind for issue in issues}, {"broken-link", "orphan"})

    def test_images_are_checked_but_do_not_provide_document_navigation(self):
        for image in (
            "![Hidden](hidden.md)",
            "![Hidden\nimage](hidden.md)",
            "![Hidden\nimage][target]\n\n[target]: hidden.md",
            "![Hidden\nimage][]\n\n[Hidden image]: hidden.md",
            "![Hidden\nimage]\n\n[Hidden image]: hidden.md",
            "![Hidden][target]\n\n[target]: hidden.md",
            "![Hidden][]\n\n[Hidden]: hidden.md",
            "![Hidden]\n\n[Hidden]: hidden.md",
        ):
            with self.subTest(image=image), tempfile.TemporaryDirectory() as temp_dir:
                root = Path(temp_dir)
                self._write(root, "doc/README.md", "# Documentation\n\n" + image + "\n![Missing](absent.png)\n")
                self._write(root, "doc/hidden.md", "# Hidden\n")

                issues = doccheck.check_tree(root)

                self.assertEqual({issue.kind for issue in issues}, {"broken-link", "orphan"})

    def test_escaped_image_markers_preserve_document_navigation(self):
        for link in (
            "![Guide](user/guide.md)",
            "![System\nguide](user/guide.md)",
            "![System\nguide][target]\n\n[target]: user/guide.md",
            "![System\nguide][]\n\n[System guide]: user/guide.md",
            "![System\nguide]\n\n[System guide]: user/guide.md",
            "![Guide][target]\n\n[target]: user/guide.md",
            "![Guide][]\n\n[Guide]: user/guide.md",
            "![Guide]\n\n[Guide]: user/guide.md",
        ):
            for backslashes in range(5):
                with self.subTest(link=link, backslashes=backslashes), tempfile.TemporaryDirectory() as temp_dir:
                    root = Path(temp_dir).resolve()
                    prefix = "\\" * backslashes
                    self._write(root, "doc/README.md", "# Documentation\n\n" + prefix + link + "\n")
                    guide = self._write(root, "doc/user/guide.md", "# Guide\n")

                    issues = doccheck.check_tree(root)

                    expected = [] if backslashes % 2 else [(guide, "orphan")]
                    self.assertEqual([(issue.path, issue.kind) for issue in issues], expected)

    def test_local_links_cannot_depend_on_files_outside_the_checkout(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            parent = Path(temp_dir)
            root = parent / "repository"
            self._valid_tree(root)
            self._write(parent, "outside.md", "# Machine-local file\n")
            index = root / "doc/README.md"
            index.write_text(
                index.read_text(encoding="utf-8") + "\n[External local](../../outside.md)\n",
                encoding="utf-8",
            )

            issues = doccheck.check_tree(root)

        self.assertEqual([issue.kind for issue in issues], ["external-local-link"])

    def test_reports_every_unreachable_document_under_doc(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            self._valid_tree(root)
            self._write(root, "doc/misc/orphan.md", "# Orphan\n")

            issues = doccheck.check_tree(root)

        self.assertIn("orphan", {issue.kind for issue in issues})

    def test_repository_root_and_skill_markdown_are_not_subject_to_reachability(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            self._valid_tree(root)
            self._write(root, "README.md", "# Repository\n")
            self._write(
                root,
                ".agents/skills/example/SKILL.md",
                "---\nname: example\ndescription: >-\n  A multiline skill description.\n---\n# Skill\n",
            )

            issues = doccheck.check_tree(root)

        self.assertEqual(issues, [])

    def test_skill_front_matter_is_not_markdown_and_body_diagnostics_keep_physical_lines(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir).resolve()
            self._valid_tree(root)
            skill = self._write(
                root,
                ".agents/skills/example/SKILL.md",
                "---\nname: example\ndescription: >-\n"
                "  Literal [example](absent.md).\n---\n# Skill\n\n[Real](missing.md)\n",
            )

            issues = doccheck.check_tree(root)

            self.assertEqual([(issue.path, issue.line, issue.kind) for issue in issues], [(skill, 8, "broken-link")])

    def test_skill_front_matter_cannot_supply_a_heading_anchor(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir).resolve()
            self._valid_tree(root)
            skill = self._write(
                root,
                ".agents/skills/example/SKILL.md",
                "---\nname: example\n---\n# Skill\n\n[Fake](#name-example)\n",
            )

            issues = doccheck.check_tree(root)

            self.assertEqual([(issue.path, issue.line, issue.kind) for issue in issues], [(skill, 6, "broken-anchor")])

    def test_component_and_brand_markdown_are_discovered_and_link_checked(self):
        for folder in ("tool", "app", "asset"):
            with self.subTest(folder=folder), tempfile.TemporaryDirectory() as temp_dir:
                root = Path(temp_dir).resolve()
                self._valid_tree(root)
                page = self._write(root, f"{folder}/nested/README.md", "# Component\n\n[Missing](absent.md)\n")
                self._write(root, f"{folder}/nested/implementation.cpp", "[Ignored](absent.md)\n")

                self.assertIn(page, doccheck.discover_markdown(root))
                issues = doccheck.check_tree(root)

                self.assertEqual([(issue.path, issue.kind) for issue in issues], [(page, "broken-link")])
                page.write_text("# Component\n\n[Overview](/doc/README.md#documentation)\n", encoding="utf-8")
                self.assertEqual(doccheck.check_tree(root), [])

    def test_component_and_brand_anchor_targets_are_checked(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            self._valid_tree(root)
            index = root / "doc/README.md"
            for folder in ("tool", "app", "asset"):
                self._write(root, f"{folder}/nested/README.md", "# Component\n")
            index.write_text(
                "# Documentation\n\n[Guide](system/guide.md)\n"
                + "".join(f"[{folder}](/{folder}/nested/README.md#missing)\n" for folder in ("tool", "app", "asset")),
                encoding="utf-8",
            )

            issues = doccheck.check_tree(root)

        self.assertEqual([issue.kind for issue in issues], ["broken-anchor"] * 3)

    def test_local_plan_markdown_is_excluded_from_discovery(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            self._valid_tree(root)
            plan = self._write(root, "doc/plan/nested/draft.md", "# Private plan\n\n[Missing](absent.md)\n")

            self.assertNotIn(plan, doccheck.discover_markdown(root))
            self.assertEqual(doccheck.check_tree(root), [])

    def test_reports_missing_documentation_root_index(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            self._write(root, "doc/system/topic.md", "# Topic\n")

            messages = [issue.message for issue in doccheck.check_tree(root)]

        self.assertIn("documentation root index is missing", messages)

    def test_accepts_quoted_metadata_and_angle_links(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            self._write(root, "doc/README.md", "# Documentation\n\n[Topic](<system/topic.md>)\n")
            self._write(
                root,
                "doc/system/topic.md",
                self._metadata(id="'system.topic'", status='"current"') + "# Topic\n",
            )

            issues = doccheck.check_tree(root)

        self.assertEqual(issues, [])

    def test_issue_format_uses_relative_paths_and_preserves_external_paths(self):
        root = Path("/repo")
        relative = doccheck.Issue(root / "doc/system/example.md", 7, "kind", "message")
        external = doccheck.Issue(Path("/outside/example.md"), 3, "kind", "message")

        self.assertEqual(relative.format(root), "doc/system/example.md:7: kind: message")
        self.assertEqual(external.format(root), "/outside/example.md:3: kind: message")


if __name__ == "__main__":
    unittest.main()
