"""Documentation body ownership and diagnostic URL presentation boundaries."""

import tempfile
import unittest
from pathlib import Path

from ao.portal import doc_check


class DocCheckBoundaryTest(unittest.TestCase):
    def test_front_matter_never_supplies_links_or_anchors_in_discovered_locations(self):
        for relative in (
            "README.md",
            "AGENTS.md",
            "CONTRIBUTING.md",
            "tool/example/README.md",
            "app/example/README.md",
            "asset/example/README.md",
            ".agents/skills/example/SKILL.md",
        ):
            with self.subTest(relative=relative), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary).resolve()
                (root / "doc").mkdir()
                (root / "doc/README.md").write_text("# Documentation\n", encoding="utf-8")
                path = root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                # This owner's metadata need not obey doc/'s flat scalar schema.
                path.write_text(
                    "---\nname: example\ndescription: >-\n"
                    "  Literal [Missing](absent.md).\n---\n# Body\n\n"
                    "[Fake](#name-example-description--literal-missing)\n[Real](missing.md)\n",
                    encoding="utf-8",
                )

                issues = doc_check.check_tree(root)

                self.assertEqual(
                    [(issue.path, issue.line, issue.kind) for issue in issues],
                    [(path, 8, "broken-anchor"), (path, 9, "broken-link")],
                )

    def test_metadata_cannot_make_a_document_reachable(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            (root / "doc").mkdir()
            (root / "doc/README.md").write_text("# Documentation\n\n[Repository](/README.md)\n", encoding="utf-8")
            target = root / "doc/hidden.md"
            target.write_text("# Hidden\n", encoding="utf-8")
            (root / "README.md").write_text(
                "---\nsource: [Hidden](doc/hidden.md)\n---\n# Repository\n", encoding="utf-8"
            )

            issues = doc_check.check_tree(root)

            self.assertEqual([(issue.path, issue.kind) for issue in issues], [(target, "orphan")])

    def test_hub_link_is_direct_and_uses_the_rendered_reference_destination(self):
        for hub, topic in (
            ("user/README.md", "user/tasks/play.md"),
            ("reference/README.md", "reference/media/README.md"),
            ("decision/README.md", "decision/format.md"),
            ("system/README.md", "system/overview.md"),
            ("system/playback/README.md", "system/playback/audio/session.md"),
            ("development/README.md", "development/build.md"),
            ("development/test.md", "development/test/scope.md"),
            ("development/linting.md", "development/lint/naming.md"),
        ):
            with self.subTest(hub=hub), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary).resolve()
                docs = root / "doc"
                target = docs / topic
                target.parent.mkdir(parents=True)
                target.write_text("# Details\n", encoding="utf-8")
                (docs / "README.md").write_text(f"[Hub]({hub})\n", encoding="utf-8")
                (docs / "bridge.md").write_text(f"[Topic](/doc/{topic})\n", encoding="utf-8")
                entry = docs / hub
                entry.write_text(
                    f"# Hub\n\n[Bridge](/doc/bridge.md)\n![Image](/doc/{topic}) and [Here](#hub)\n",
                    encoding="utf-8",
                )

                issues = doc_check.check_tree(root)

                self.assertEqual([(issue.path, issue.kind) for issue in issues], [(target, "hub-link")])
                with entry.open("a", encoding="utf-8") as stream:
                    stream.write(f"\n[Topic][details]\n\n[details]: /doc/{topic}#details\n")
                self.assertEqual(doc_check.check_tree(root), [])

    def test_related_documents_accepts_annotated_and_grouped_list_links(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            docs = root / "doc"
            docs.mkdir()
            (docs / "README.md").write_text(
                "# Index\n\n```markdown\n## Related documents\n```\n\n> ## Related documents\n\n"
                "Body before the real section.\n\nRelated documents\n-----------------\n\n"
                "1. [Index][home] owns the\n   documentation entrance.\n"
                "2. [Home](#index) and [this section](#related-documents)\n"
                "   - [Nested entry][home]\n\n[home]: #index\n",
                encoding="utf-8",
            )

            self.assertEqual(doc_check.check_tree(root), [])

    def test_related_documents_reports_first_non_link_list_content_on_its_physical_line(self):
        for tail in (
            "A key can be rebound.\n\nAnother rule.",
            "See [the index](#index).",
            "  Keep the selection on failure. See [Home](#index).",
            "### More",
            "```md\n- [Example](#index)\n```",
            "- `not [a link](#index)`",
            "- ![Image](README.md)",
            "  > Quoted paragraphs are not link lists.",
        ):
            with self.subTest(tail=tail), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary).resolve()
                docs = root / "doc"
                docs.mkdir()
                path = docs / "README.md"
                path.write_text(
                    "---\nid: docs\n---\n# Index\n\n## Related documents\n\n- [Home](#index)\n\n" + tail + "\n",
                    encoding="utf-8",
                )

                issues = doc_check.check_tree(root)

                self.assertEqual(
                    [(issue.path, issue.line, issue.kind) for issue in issues], [(path, 10, "related-documents")]
                )

    def test_related_documents_requires_a_rendered_link_list(self):
        for tail in ("", "\n[unused]: #index\n"):
            with self.subTest(tail=tail), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary).resolve()
                path = root / "doc/README.md"
                path.parent.mkdir()
                path.write_text("# Index\n\n## Related documents\n" + tail, encoding="utf-8")

                issues = doc_check.check_tree(root)

                self.assertEqual([(issue.line, issue.kind) for issue in issues], [(3, "related-documents")])

    def test_diagnostics_display_readable_local_targets(self):
        for source, expected in (
            ("[Missing](<not here.md>)", "not here.md"),
            ("[Missing](不存在.md)", "不存在.md"),
            ("![Missing](<not here.png>)", "not here.png"),
            ("[Missing][ref]\n\n[ref]: <not here.md>", "not here.md"),
            ("[Missing](literal%2520name.md)", "literal%20name.md"),
        ):
            with self.subTest(source=source), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary).resolve()
                (root / "doc").mkdir()
                path = root / "doc/README.md"
                path.write_text("# Documentation\n\n" + source + "\n", encoding="utf-8")

                issues = doc_check.check_tree(root)

                self.assertEqual(
                    [(issue.line, issue.kind, issue.message) for issue in issues],
                    [(3, "broken-link", f"target does not exist: {expected}")],
                )

    def test_anchor_diagnostics_decode_only_the_destination_path(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            (root / "doc").mkdir()
            (root / "doc/README.md").write_text(
                "# Documentation\n\n[Missing](hash%23name.md#absent)\n", encoding="utf-8"
            )
            (root / "doc/hash#name.md").write_text("# Target\n", encoding="utf-8")

            issues = doc_check.check_tree(root)

            self.assertEqual(
                [(issue.line, issue.kind, issue.message) for issue in issues],
                [(3, "broken-anchor", "anchor '#absent' does not exist in hash#name.md")],
            )

    def test_display_decoding_does_not_rewrite_resolution_urls(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            (root / "doc").mkdir()
            (root / "doc/README.md").write_text(
                "# Documentation\n\n[Hash](hash%23name.md)\n[Percent](literal%2520name.md)\n[Space](space%20name.md)\n",
                encoding="utf-8",
            )
            for name in ("hash#name.md", "literal%20name.md", "space name.md"):
                (root / "doc" / name).write_text("# Target\n", encoding="utf-8")

            self.assertEqual(doc_check.check_tree(root), [])
