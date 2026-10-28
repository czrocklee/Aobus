"""Documentation body ownership and diagnostic URL presentation boundaries."""

import tempfile
import unittest
from pathlib import Path

from ao.core import doccheck


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

                issues = doccheck.check_tree(root)

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

            issues = doccheck.check_tree(root)

            self.assertEqual([(issue.path, issue.kind) for issue in issues], [(target, "orphan")])

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

                issues = doccheck.check_tree(root)

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

            issues = doccheck.check_tree(root)

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

            self.assertEqual(doccheck.check_tree(root), [])
