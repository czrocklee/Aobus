"""Tests for documentation metadata, links, anchors, and reachability."""

import tempfile
import unittest
from pathlib import Path

from ao.core import doccheck


class DocCheckTest(unittest.TestCase):
    def _write(self, root: Path, relative: str, text: str) -> Path:
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        return path

    def _metadata(
        self,
        *,
        document_id="spec.example",
        document_type="spec",
        status="current",
        depends_on="none",
    ) -> str:
        metadata = (
            "---\n"
            f"id: {document_id}\n"
            f"type: {document_type}\n"
            f"status: {status}\n"
            "domain: example\n"
            "summary: Defines an example contract.\n"
        )
        if document_type == "rfc":
            metadata += f"depends-on: {depends_on}\n"
        return metadata + "---\n"

    def _rfc_body(
        self,
        title: str,
        hard: str = "None.",
        conditional: str = "None.",
        integration: str = "None.",
    ) -> str:
        return (
            f"# {title}\n\n"
            "## Problem\n\n"
            "Describe the problem.\n\n"
            "## Dependencies\n\n"
            f"- Hard: {hard}\n"
            f"- Conditional: {conditional}\n"
            f"- Integration: {integration}\n\n"
            "## Goals\n\n"
            "Define the goal.\n"
        )

    def _rfc_index(self, rows: str) -> str:
        return (
            self._metadata(document_id="rfc.index", document_type="index") + "# RFCs\n\n"
            "## Dependency map\n\n"
            "| RFC | Hard | Conditional | Integration |\n"
            "|---|---|---|---|\n"
            f"{rows}"
        )

    def _architecture_body(self, title: str) -> str:
        return (
            f"# {title}\n\n"
            "## Scope\n\nDefines one structural question.\n\n"
            "## System context\n\nLinks the portfolio and system.\n\n"
            "## Responsibilities\n\nOwns the example structure.\n\n"
            "## Boundaries and dependency direction\n\nDependencies point toward the system.\n\n"
            "## Data and control flow\n\nInput becomes output.\n\n"
            "## Structural constraints\n\nOne owner exists.\n\n"
            "## Failure, cancellation, and lifetime boundaries\n\nThe owner controls lifetime.\n\n"
            "## Implementation map\n\n- `example.cpp`\n\n"
            "## Test map\n\n- `example-test.cpp`\n\n"
            "## Related documents\n\n- [Architecture landscape](README.md)\n"
        )

    def _valid_architecture_tree(self, root: Path) -> Path:
        self._write(root, "doc/README.md", "# Documentation\n\n[Architecture](architecture/README.md)\n")
        index = self._write(
            root,
            "doc/architecture/README.md",
            self._metadata(document_id="architecture.index", document_type="index") + "# Architecture landscape\n\n"
            "## Portfolio roles\n\n"
            "| Document | Owns |\n"
            "|---|---|\n"
            "| [System](system.md) | Layers. |\n"
            "| [Example](example.md) | Example structure. |\n\n"
            "## Architecture relationships\n\n"
            "| Architecture | Consumes or refines | Feeds or constrains |\n"
            "|---|---|---|\n"
            "| [System](system.md) | Build graph | Example |\n"
            "| [Example](example.md) | System | Consumers |\n\n"
            "## Capability coverage\n\n"
            "| Capability | Current structural owner | Coverage | Remaining documentation question |\n"
            "|---|---|---|---|\n"
            "| Layering | [System](system.md) | Current | None. |\n"
            "| Example capability | [Example](example.md) | Current | None. |\n",
        )
        self._write(
            root,
            "doc/architecture/system.md",
            self._metadata(document_id="architecture.system", document_type="architecture")
            + self._architecture_body("System architecture"),
        )
        self._write(
            root,
            "doc/architecture/example.md",
            self._metadata(document_id="architecture.example", document_type="architecture")
            + self._architecture_body("Example architecture"),
        )
        return index

    def _valid_tree(self, root: Path) -> None:
        self._write(root, "doc/README.md", "# Documentation\n\n[Specifications](spec/README.md)\n")
        self._write(
            root,
            "doc/spec/README.md",
            self._metadata(document_id="spec.index", document_type="index")
            + "# Specifications\n\n[Example](example.md#storage--compatibility)\n",
        )
        self._write(
            root,
            "doc/spec/example.md",
            self._metadata() + "# Example\n\n"
            "## Scope\n\nDefines the example behavior.\n\n"
            "## Code boundary\n\nOwned by the example subsystem.\n\n"
            "## Storage & compatibility\n\nDefines compatibility details.\n\n"
            "## Implementation map\n\n- `example.cpp`\n\n"
            "## Test map\n\n- `example-test.cpp`\n\n"
            "## Related documents\n\n- [Specifications](README.md)\n",
        )

    def _valid_rfc_pair(self, root: Path) -> tuple[Path, Path, Path]:
        self._valid_tree(root)
        doc_index = root / "doc/README.md"
        doc_index.write_text(doc_index.read_text(encoding="utf-8") + "\n[RFCs](rfc/README.md)\n", encoding="utf-8")
        rfc_index = self._write(
            root,
            "doc/rfc/README.md",
            self._rfc_index(
                "| [Base](0001-base.md) | None | None | None |\n"
                "| [Consumer](0002-consumer.md) | [Base](0001-base.md) | None | None |\n"
            ),
        )
        base = self._write(
            root,
            "doc/rfc/0001-base.md",
            self._metadata(document_id="rfc.0001.base", document_type="rfc", status="draft")
            + self._rfc_body("RFC 0001: Base"),
        )
        consumer = self._write(
            root,
            "doc/rfc/0002-consumer.md",
            self._metadata(
                document_id="rfc.0002.consumer",
                document_type="rfc",
                status="draft",
                depends_on="rfc.0001.base",
            )
            + self._rfc_body("RFC 0002: Consumer", "[Base](0001-base.md) provides the boundary."),
        )
        return rfc_index, base, consumer

    def test_accepts_valid_metadata_links_github_anchors_and_reachability(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            self._valid_tree(root)

            issues = doccheck.check_tree(root)

        self.assertEqual(issues, [])

    def test_reports_missing_metadata_and_wrong_type_status(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            self._valid_tree(root)
            self._write(root, "doc/spec/missing.md", "# Missing metadata\n")
            self._write(
                root,
                "doc/spec/wrong.md",
                self._metadata(document_id="wrong", document_type="decision", status="current") + "# Wrong\n",
            )
            index = root / "doc/spec/README.md"
            index.write_text(
                index.read_text(encoding="utf-8") + "\n[Missing](missing.md)\n[Wrong](wrong.md)\n",
                encoding="utf-8",
            )

            issues = doccheck.check_tree(root)

        messages = [(issue.kind, issue.message) for issue in issues]
        self.assertIn(("metadata", "governed documents must start with YAML front matter"), messages)
        self.assertTrue(any("does not belong under spec/" in message for _, message in messages))

    def test_rejects_terminal_rfc_statuses(self):
        for terminal_status in ("implemented", "rejected"):
            with self.subTest(status=terminal_status), tempfile.TemporaryDirectory() as temp_dir:
                root = Path(temp_dir)
                _, base, _ = self._valid_rfc_pair(root)
                base.write_text(
                    base.read_text(encoding="utf-8").replace("status: draft", f"status: {terminal_status}"),
                    encoding="utf-8",
                )

                messages = [issue.message for issue in doccheck.check_tree(root)]

            self.assertIn(
                f"status '{terminal_status}' is invalid for rfc (accepted, draft, in-review)",
                messages,
            )

    def test_reports_duplicate_ids_broken_links_anchors_and_plan_links(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            self._valid_tree(root)
            self._write(
                root,
                "doc/spec/duplicate.md",
                self._metadata(document_id="spec.example")
                + "# Duplicate\n\n[Missing](absent.md)\n[Anchor](example.md#absent)\n[Plan](../plan/local.md)\n",
            )
            self._write(root, "doc/plan/local.md", "# Local plan\n")
            index = root / "doc/spec/README.md"
            index.write_text(index.read_text(encoding="utf-8") + "\n[Duplicate](duplicate.md)\n", encoding="utf-8")

            issues = doccheck.check_tree(root)

        kinds = {issue.kind for issue in issues}
        self.assertTrue({"duplicate-id", "broken-link", "broken-anchor", "plan-link"}.issubset(kinds))

    def test_reports_governed_documents_not_reachable_from_root_index(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            self._valid_tree(root)
            self._write(root, "doc/spec/orphan.md", self._metadata(document_id="spec.orphan") + "# Orphan\n")

            issues = doccheck.check_tree(root)

        self.assertIn("orphan", {issue.kind for issue in issues})

    def test_requires_direct_index_ownership_for_governed_documents(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            self._valid_tree(root)
            self._write(
                root,
                "doc/spec/unindexed.md",
                self._metadata(document_id="spec.unindexed") + "# Unindexed\n\n"
                "## Scope\n\nUnindexed behavior.\n\n"
                "## Code boundary\n\nExample subsystem.\n\n"
                "## Implementation map\n\n- `example.cpp`\n\n"
                "## Test map\n\n- `example-test.cpp`\n\n"
                "## Related documents\n\n- [Example](example.md)\n",
            )

            issues = doccheck.check_tree(root)

        self.assertIn("index-ownership", {issue.kind for issue in issues})

    def test_accepts_complete_architecture_portfolio(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            self._valid_architecture_tree(root)

            issues = doccheck.check_tree(root)

        self.assertEqual(issues, [])

    def test_reports_architecture_portfolio_and_coverage_mismatches(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            index = self._valid_architecture_tree(root)
            index.write_text(
                index.read_text(encoding="utf-8")
                .replace(
                    "| [Example](example.md) | Example structure. |\n",
                    "| [Example](example.md) | Example structure. |\n"
                    "| [Example again](example.md) | Duplicate role. |\n",
                )
                .replace(
                    "| Example capability | [Example](example.md) | Current | None. |",
                    "| Example capability | [System](system.md), [Example](example.md) | Current | None. |",
                ),
                encoding="utf-8",
            )

            issues = doccheck.check_tree(root)

        kinds = {issue.kind for issue in issues}
        self.assertIn("architecture-portfolio", kinds)
        self.assertIn("architecture-coverage", kinds)

    def test_requires_core_user_guide_sections(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            self._write(root, "doc/README.md", "# Documentation\n\n[Users](user/README.md)\n")
            self._write(
                root,
                "doc/user/README.md",
                self._metadata(document_id="user.index", document_type="index")
                + "# User documentation\n\n[Incomplete](incomplete.md)\n",
            )
            self._write(
                root,
                "doc/user/incomplete.md",
                self._metadata(document_id="user.incomplete", document_type="user-guide")
                + "# Incomplete guide\n\n## Outcome\n\nComplete a task.\n",
            )

            issues = doccheck.check_tree(root)

        self.assertIn("document-structure", {issue.kind for issue in issues})

    def test_reference_style_links_participate_in_validation_and_reachability(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            self._write(
                root,
                "doc/README.md",
                "# Documentation\n\n[Specifications][spec-index]\n\n[spec-index]: spec/README.md\n",
            )
            self._write(
                root,
                "doc/spec/README.md",
                self._metadata(document_id="spec.index", document_type="index")
                + "# Specifications\n\n[Example][]\n\n[Example]: example.md#storage--compatibility\n",
            )
            self._write(
                root,
                "doc/spec/example.md",
                self._metadata() + "# Example\n\n"
                "## Scope\n\nDefines the example behavior.\n\n"
                "## Code boundary\n\nOwned by the example subsystem.\n\n"
                "## Storage & compatibility\n\nDefines compatibility details.\n\n"
                "## Implementation map\n\n- `example.cpp`\n\n"
                "## Test map\n\n- `example-test.cpp`\n\n"
                "## Related documents\n\n- [Specifications](README.md)\n",
            )

            issues = doccheck.check_tree(root)

        self.assertEqual(issues, [])

    def test_reports_broken_reference_targets_and_undefined_labels(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            self._valid_tree(root)
            index = root / "doc/spec/README.md"
            index.write_text(
                index.read_text(encoding="utf-8")
                + "\n[Missing target][missing]\n[Undefined][unknown]\n\n[missing]: absent.md\n",
                encoding="utf-8",
            )

            issues = doccheck.check_tree(root)

        kinds = {issue.kind for issue in issues}
        self.assertIn("broken-link", kinds)
        self.assertIn("broken-reference", kinds)

    def test_rejects_unchanged_template_placeholders(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            self._valid_tree(root)
            self._write(
                root,
                "doc/spec/copied-template.md",
                "---\n"
                "id: domain.contract\n"
                "type: spec\n"
                "status: draft\n"
                "domain: domain\n"
                "summary: Defines one normative Aobus behavior contract.\n"
                "---\n"
                "# Specification title\n",
            )
            index = root / "doc/spec/README.md"
            index.write_text(
                index.read_text(encoding="utf-8") + "\n[Copied template](copied-template.md)\n",
                encoding="utf-8",
            )

            issues = doccheck.check_tree(root)

        self.assertIn("placeholder", {issue.kind for issue in issues})

    def test_rejects_plural_document_directory_names(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            self._valid_tree(root)
            self._write(root, "doc/templates/example.md", "# Plural directory\n")

            issues = doccheck.check_tree(root)

        self.assertIn("directory-name", {issue.kind for issue in issues})

    def test_issue_format_uses_relative_paths_and_preserves_external_paths(self):
        root = Path("/repo")

        relative = doccheck.Issue(root / "doc/spec/example.md", 7, "kind", "message")
        external = doccheck.Issue(Path("/outside/example.md"), 3, "kind", "message")

        self.assertEqual(relative.format(root), "doc/spec/example.md:7: kind: message")
        self.assertEqual(external.format(root), "/outside/example.md:3: kind: message")

    def test_reports_front_matter_scalar_and_delimiter_errors_independently(self):
        cases = {
            "unclosed": (
                "---\nid: spec.example\ntype: spec\nstatus: current\ndomain: example\n",
                "front matter has no closing '---' delimiter",
            ),
            "empty entry": (
                "---\nid: spec.example\ntype: spec\nstatus: current\ndomain: example\nsummary:\n---\n# Example\n",
                "metadata entries must be non-empty 'key: value' scalars",
            ),
            "duplicate key": (
                self._metadata().replace("type: spec\n", "type: spec\ntype: spec\n") + "# Example\n",
                "duplicate metadata key 'type'",
            ),
            "structured value": (
                self._metadata().replace("domain: example\n", "domain: [example]\n") + "# Example\n",
                "metadata values must use the flat scalar subset",
            ),
        }
        for name, (content, expected) in cases.items():
            with self.subTest(name=name), tempfile.TemporaryDirectory() as temp_dir:
                root = Path(temp_dir)
                self._valid_tree(root)
                (root / "doc/spec/example.md").write_text(content, encoding="utf-8")

                messages = [issue.message for issue in doccheck.check_tree(root)]

                self.assertIn(expected, messages)

    def test_reports_metadata_value_and_document_naming_errors_independently(self):
        cases = {
            "unknown key": (
                "summary: Defines an example contract.\n",
                "summary: Defines an example contract.\nextra: value\n",
                "unknown metadata key 'extra'",
            ),
            "invalid id": ("id: spec.example\n", "id: Spec Example\n", "invalid document id 'Spec Example'"),
            "invalid domain": ("domain: example\n", "domain: Bad.Domain\n", "invalid domain 'Bad.Domain'"),
            "summary punctuation": (
                "summary: Defines an example contract.\n",
                "summary: Missing punctuation\n",
                "summary must be one sentence ending in punctuation",
            ),
        }
        for name, (old, new, expected) in cases.items():
            with self.subTest(name=name), tempfile.TemporaryDirectory() as temp_dir:
                root = Path(temp_dir)
                self._valid_tree(root)
                example = root / "doc/spec/example.md"
                example.write_text(example.read_text(encoding="utf-8").replace(old, new), encoding="utf-8")

                messages = [issue.message for issue in doccheck.check_tree(root)]

                self.assertIn(expected, messages)

    def test_reports_index_filename_readme_type_and_title_contracts(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            self._valid_tree(root)
            spec_index = root / "doc/spec/README.md"
            spec_index.write_text(
                spec_index.read_text(encoding="utf-8").replace("type: index", "type: spec"), encoding="utf-8"
            )
            self._write(
                root,
                "doc/spec/not-readme.md",
                self._metadata(document_id="spec.other-index", document_type="index") + "# Other index\n",
            )
            self._write(root, "doc/spec/no-title.md", self._metadata(document_id="spec.no-title"))
            self._write(
                root,
                "doc/spec/bad-title.md",
                self._metadata(document_id="spec.bad-title") + "Body before title.\n",
            )
            spec_index.write_text(
                spec_index.read_text(encoding="utf-8")
                + "\n[Other](not-readme.md)\n[No title](no-title.md)\n[Bad title](bad-title.md)\n",
                encoding="utf-8",
            )

            messages = [issue.message for issue in doccheck.check_tree(root)]

        self.assertIn("README.md documents must use type 'index'", messages)
        self.assertIn("index documents must be named README.md", messages)
        self.assertIn("document has no H1 title", messages)
        self.assertIn("the first body element must be one H1 title", messages)

    def test_ignores_fenced_links_and_headings_and_accepts_supported_link_forms(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            self._valid_tree(root)
            example = root / "doc/spec/example.md"
            example.write_text(
                self._metadata() + "# Example\n\n"
                "## Scope\n\nDefines the example behavior.\n\n"
                "## Code boundary\n\nOwned by the example subsystem.\n\n"
                '<a id="custom-anchor"></a>\n\n'
                "## Visible heading\n\n"
                "```markdown\n[Ignored missing link](absent.md)\n## Ignored heading\n```\n\n"
                "[Self](#visible-heading)\n"
                "[Custom](#custom-anchor)\n"
                "[Root absolute](/doc/spec/README.md)\n"
                "[External](https://example.com/)\n\n"
                "## Implementation map\n\n- `example.cpp`\n\n"
                "## Test map\n\n- `example-test.cpp`\n\n"
                "## Related documents\n\n- [Specifications](README.md)\n",
                encoding="utf-8",
            )
            spec_index = root / "doc/spec/README.md"
            spec_index.write_text(
                spec_index.read_text(encoding="utf-8").replace(
                    "example.md#storage--compatibility", "example.md#visible-heading"
                ),
                encoding="utf-8",
            )

            issues = doccheck.check_tree(root)

        self.assertEqual(issues, [])

    def test_accepts_quoted_metadata_shortcut_references_and_angle_links(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            self._valid_tree(root)
            example = root / "doc/spec/example.md"
            example.write_text(
                example.read_text(encoding="utf-8")
                .replace("domain: example", "domain: 'example'")
                .replace("summary: Defines an example contract.", 'summary: "Defines an example contract."')
                .replace("---\n# Example", "---\n\n# Example")
                + "\n[Specification index]\n"
                "[Angle target](<README.md>)\n\n"
                "[Specification index]: README.md\n",
                encoding="utf-8",
            )

            issues = doccheck.check_tree(root)

        self.assertEqual(issues, [])

    def test_non_documentation_files_are_not_subject_to_governed_reachability(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            self._valid_tree(root)
            self._write(root, "README.md", "# Repository\n")
            self._write(root, ".agents/skills/example/SKILL.md", "# Skill\n")

            issues = doccheck.check_tree(root)

        self.assertEqual(issues, [])

    def test_reports_missing_documentation_root_index(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            self._write(
                root,
                "doc/spec/README.md",
                self._metadata(document_id="spec.index", document_type="index") + "# Specifications\n",
            )

            messages = [issue.message for issue in doccheck.check_tree(root)]

        self.assertIn("documentation root index is missing", messages)

    def test_accepts_rfc_dependency_metadata_section_and_link(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            self._valid_tree(root)
            doc_index = root / "doc/README.md"
            doc_index.write_text(doc_index.read_text(encoding="utf-8") + "\n[RFCs](rfc/README.md)\n", encoding="utf-8")
            self._write(
                root,
                "doc/rfc/README.md",
                self._rfc_index(
                    "| [Base](0001-base.md) | None | None | None |\n"
                    "| [Consumer](0002-consumer.md) | [Base](0001-base.md) | None | None |\n"
                ),
            )
            self._write(
                root,
                "doc/rfc/0001-base.md",
                self._metadata(document_id="rfc.0001.base", document_type="rfc", status="draft")
                + self._rfc_body("RFC 0001: Base"),
            )
            self._write(
                root,
                "doc/rfc/0002-consumer.md",
                self._metadata(
                    document_id="rfc.0002.consumer",
                    document_type="rfc",
                    status="draft",
                    depends_on="rfc.0001.base",
                )
                + self._rfc_body("RFC 0002: Consumer", "[RFC 0001](0001-base.md) provides the boundary."),
            )

            issues = doccheck.check_tree(root)

        self.assertEqual(issues, [])

    def test_reports_each_dependency_section_shape_error(self):
        cases = {
            "missing section": (
                lambda text: text.replace("## Dependencies", "## Relationship notes"),
                "RFCs must contain exactly one '## Dependencies' section",
            ),
            "duplicate hard entry": (
                lambda text: text.replace(
                    "- Hard: [Base](0001-base.md) provides the boundary.\n",
                    "- Hard: [Base](0001-base.md) provides the boundary.\n- Hard: None.\n",
                ),
                "Dependencies must contain exactly one '- Hard:' entry",
            ),
            "missing conditional entry": (
                lambda text: text.replace("- Conditional: None.\n", ""),
                "Dependencies must contain exactly one '- Conditional:' entry",
            ),
            "none with link": (
                lambda text: text.replace("- Conditional: None.", "- Conditional: None. [Base](0001-base.md)"),
                "'- Conditional: None.' cannot contain links",
            ),
            "reason without link": (
                lambda text: text.replace("- Integration: None.", "- Integration: A reason without a link."),
                "'- Integration:' must be 'None.' or link at least one RFC",
            ),
            "metadata dependency with none entry": (
                lambda text: text.replace("- Hard: [Base](0001-base.md) provides the boundary.", "- Hard: None."),
                "'- Hard:' must explain and link every metadata dependency",
            ),
            "hard entry with none metadata": (
                lambda text: text.replace("depends-on: rfc.0001.base", "depends-on: none"),
                "'- Hard:' must be 'None.' when 'depends-on' is 'none'",
            ),
        }
        for name, (mutate, expected) in cases.items():
            with self.subTest(name=name), tempfile.TemporaryDirectory() as temp_dir:
                root = Path(temp_dir)
                _, _, consumer = self._valid_rfc_pair(root)
                consumer.write_text(mutate(consumer.read_text(encoding="utf-8")), encoding="utf-8")

                messages = [issue.message for issue in doccheck.check_tree(root)]

                self.assertIn(expected, messages)

    def test_ignores_fenced_dependency_headings_and_accepts_reference_style_dependency_links(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            rfc_index, _, consumer = self._valid_rfc_pair(root)
            self._write(
                root,
                "doc/rfc/0003-peer.md",
                self._metadata(document_id="rfc.0003.peer", document_type="rfc", status="draft")
                + self._rfc_body("RFC 0003: Peer"),
            )
            consumer.write_text(
                consumer.read_text(encoding="utf-8")
                .replace(
                    "Describe the problem.\n",
                    "Describe the problem.\n\n```markdown\n## Dependencies\n- Hard: None.\n```\n",
                )
                .replace("- Integration: None.", "- Integration: [Peer][peer].\n\n[peer]: 0003-peer.md"),
                encoding="utf-8",
            )
            rfc_index.write_text(
                rfc_index.read_text(encoding="utf-8").replace(
                    "| [Consumer](0002-consumer.md) | [Base](0001-base.md) | None | None |\n",
                    "| [Consumer](0002-consumer.md) | [Base](0001-base.md) | None | "
                    "[Peer](0003-peer.md) |\n"
                    "| [Peer](0003-peer.md) | None | None | None |\n",
                ),
                encoding="utf-8",
            )

            issues = doccheck.check_tree(root)

        self.assertEqual(issues, [])

    def test_rejects_empty_items_in_comma_separated_hard_dependency_metadata(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            _, _, consumer = self._valid_rfc_pair(root)
            consumer.write_text(
                consumer.read_text(encoding="utf-8").replace("depends-on: rfc.0001.base", "depends-on: rfc.0001.base,"),
                encoding="utf-8",
            )

            messages = [issue.message for issue in doccheck.check_tree(root)]

        self.assertIn("'depends-on' must be 'none' or comma-separated stable RFC document ids", messages)

    def test_reports_each_dependency_index_shape_and_drift_error(self):
        consumer_row = "| [Consumer](0002-consumer.md) | [Base](0001-base.md) | None | None |\n"
        cases = {
            "missing section": (
                lambda text: text.replace("## Dependency map", "## Relationships"),
                "RFC index must contain '## Dependency map'",
            ),
            "wrong header": (
                lambda text: text.replace(
                    "| RFC | Hard | Conditional | Integration |",
                    "| RFC | Required | Conditional | Integration |",
                ),
                "dependency map must contain the exact header",
            ),
            "wrong separator": (
                lambda text: text.replace("|---|---|---|---|", "|---|---|---|"),
                "dependency map has no four-column separator",
            ),
            "wrong column count": (
                lambda text: text.replace(
                    consumer_row, "| [Consumer](0002-consumer.md) | [Base](0001-base.md) | None |\n"
                ),
                "dependency row must contain four columns",
            ),
            "owner without link": (
                lambda text: text.replace("[Consumer](0002-consumer.md)", "Consumer", 1),
                "RFC column must link exactly one RFC",
            ),
            "duplicate owner": (
                lambda text: text.replace(consumer_row, consumer_row + consumer_row),
                "duplicate dependency row for 'rfc.0002.consumer'",
            ),
            "missing owner": (
                lambda text: text.replace(consumer_row, ""),
                "dependency map is missing RFC 'rfc.0002.consumer'",
            ),
            "garbage cell": (
                lambda text: text.replace(consumer_row, "| [Consumer](0002-consumer.md) | garbage | None | None |\n"),
                "dependency cells must be 'None' or RFC links",
            ),
            "cell prose": (
                lambda text: text.replace(
                    consumer_row,
                    "| [Consumer](0002-consumer.md) | [Base](0001-base.md) because required | None | None |\n",
                ),
                "dependency cells may contain only RFC links",
            ),
            "non-rfc target": (
                lambda text: text.replace(
                    consumer_row,
                    "| [Consumer](0002-consumer.md) | [Spec](../spec/example.md) | None | None |\n",
                ),
                "dependency cell target is not an RFC: ../spec/example.md",
            ),
            "duplicate dependency link": (
                lambda text: text.replace(
                    consumer_row,
                    "| [Consumer](0002-consumer.md) | [Base](0001-base.md), [Base](0001-base.md) | None | None |\n",
                ),
                "duplicate dependency link 'rfc.0001.base'",
            ),
            "relation drift": (
                lambda text: text.replace(consumer_row, "| [Consumer](0002-consumer.md) | None | None | None |\n"),
                "rfc.0002.consumer hard dependencies do not match its RFC section",
            ),
        }
        for name, (mutate, expected) in cases.items():
            with self.subTest(name=name), tempfile.TemporaryDirectory() as temp_dir:
                root = Path(temp_dir)
                rfc_index, _, _ = self._valid_rfc_pair(root)
                rfc_index.write_text(mutate(rfc_index.read_text(encoding="utf-8")), encoding="utf-8")

                messages = [issue.message for issue in doccheck.check_tree(root)]

                self.assertTrue(any(expected in message for message in messages), messages)

    def test_reports_missing_rfc_index_when_rfc_documents_exist(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            rfc_index, _, _ = self._valid_rfc_pair(root)
            rfc_index.unlink()

            messages = [issue.message for issue in doccheck.check_tree(root)]

        self.assertIn("RFC index is missing", messages)

    def test_reports_wrong_type_and_external_dependency_targets(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            _, _, consumer = self._valid_rfc_pair(root)
            spec = root / "doc/spec/example.md"
            spec.write_text(
                spec.read_text(encoding="utf-8").replace("id: spec.example", "id: rfc.not-an-rfc"),
                encoding="utf-8",
            )
            consumer.write_text(
                consumer.read_text(encoding="utf-8")
                .replace("depends-on: rfc.0001.base", "depends-on: rfc.not-an-rfc")
                .replace(
                    "- Hard: [Base](0001-base.md) provides the boundary.",
                    "- Hard: [Spec](../spec/example.md) is the wrong type.",
                )
                .replace("- Conditional: None.", "- Conditional: [Remote](https://example.com/rfc) is external."),
                encoding="utf-8",
            )

            messages = [issue.message for issue in doccheck.check_tree(root)]

        self.assertIn("hard dependency is not an RFC: rfc.not-an-rfc", messages)
        self.assertIn("hard dependency target is not an RFC: ../spec/example.md", messages)
        self.assertIn("conditional dependency must use a repository RFC link: https://example.com/rfc", messages)

    def test_rejects_missing_invalid_and_unlinked_rfc_dependencies(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            self._valid_tree(root)
            doc_index = root / "doc/README.md"
            doc_index.write_text(doc_index.read_text(encoding="utf-8") + "\n[RFCs](rfc/README.md)\n", encoding="utf-8")
            self._write(
                root,
                "doc/rfc/README.md",
                self._metadata(document_id="rfc.index", document_type="index")
                + "# RFCs\n\n[Missing](0001-missing.md)\n[Invalid](0002-invalid.md)\n"
                "[Base](0003-base.md)\n[Unlinked](0004-unlinked.md)\n",
            )
            self._write(
                root,
                "doc/rfc/0001-missing.md",
                "---\n"
                "id: rfc.0001.missing\n"
                "type: rfc\n"
                "status: draft\n"
                "domain: example\n"
                "summary: Defines a missing dependency field.\n"
                "---\n" + self._rfc_body("RFC 0001: Missing"),
            )
            self._write(
                root,
                "doc/rfc/0002-invalid.md",
                self._metadata(
                    document_id="rfc.0002.invalid",
                    document_type="rfc",
                    status="draft",
                    depends_on="rfc.missing.target",
                )
                + self._rfc_body("RFC 0002: Invalid", "A target is named but not linked."),
            )
            self._write(
                root,
                "doc/rfc/0003-base.md",
                self._metadata(document_id="rfc.0003.base", document_type="rfc", status="draft")
                + self._rfc_body("RFC 0003: Base"),
            )
            self._write(
                root,
                "doc/rfc/0004-unlinked.md",
                self._metadata(
                    document_id="rfc.0004.unlinked",
                    document_type="rfc",
                    status="draft",
                    depends_on="rfc.0003.base",
                )
                + self._rfc_body("RFC 0004: Unlinked", "The base RFC is required but not linked."),
            )

            issues = doccheck.check_tree(root)

        messages = [issue.message for issue in issues if issue.kind == "rfc-dependency"]
        self.assertTrue(any("missing required metadata key 'depends-on'" in message for message in messages))
        self.assertTrue(any("hard dependency does not exist" in message for message in messages))
        self.assertTrue(any("must link dependency" in message for message in messages))

    def test_rejects_rfc_dependency_duplicates_self_reference_cycles_and_bad_section_order(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            self._valid_tree(root)
            doc_index = root / "doc/README.md"
            doc_index.write_text(doc_index.read_text(encoding="utf-8") + "\n[RFCs](rfc/README.md)\n", encoding="utf-8")
            self._write(
                root,
                "doc/rfc/README.md",
                self._metadata(document_id="rfc.index", document_type="index")
                + "# RFCs\n\n[A](0001-a.md)\n[B](0002-b.md)\n[Self](0003-self.md)\n[Order](0004-order.md)\n",
            )
            self._write(
                root,
                "doc/rfc/0001-a.md",
                self._metadata(
                    document_id="rfc.0001.a",
                    document_type="rfc",
                    status="draft",
                    depends_on="rfc.0002.b, rfc.0002.b",
                )
                + self._rfc_body("RFC 0001: A", "[RFC 0002](0002-b.md) is required."),
            )
            self._write(
                root,
                "doc/rfc/0002-b.md",
                self._metadata(
                    document_id="rfc.0002.b",
                    document_type="rfc",
                    status="draft",
                    depends_on="rfc.0001.a",
                )
                + self._rfc_body("RFC 0002: B", "[RFC 0001](0001-a.md) is required."),
            )
            self._write(
                root,
                "doc/rfc/0003-self.md",
                self._metadata(
                    document_id="rfc.0003.self",
                    document_type="rfc",
                    status="draft",
                    depends_on="rfc.0003.self",
                )
                + self._rfc_body("RFC 0003: Self", "[This RFC](0003-self.md) is required."),
            )
            self._write(
                root,
                "doc/rfc/0004-order.md",
                self._metadata(document_id="rfc.0004.order", document_type="rfc", status="draft")
                + "# RFC 0004: Order\n\n## Dependencies\n\n"
                "- Hard: None.\n- Conditional: None.\n- Integration: None.\n\n"
                "## Problem\n\nProblem.\n\n## Goals\n\nGoal.\n",
            )

            issues = doccheck.check_tree(root)

        kinds = {issue.kind for issue in issues}
        messages = [issue.message for issue in issues]
        self.assertIn("rfc-dependency-cycle", kinds)
        self.assertTrue(any("duplicate hard dependency" in message for message in messages))
        self.assertTrue(any("cannot depend on itself" in message for message in messages))
        self.assertTrue(any("immediately between" in message for message in messages))

    def test_reports_one_cycle_when_a_repeated_edge_rediscovers_the_same_cycle(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            self._valid_tree(root)
            doc_index = root / "doc/README.md"
            doc_index.write_text(doc_index.read_text(encoding="utf-8") + "\n[RFCs](rfc/README.md)\n", encoding="utf-8")
            self._write(
                root,
                "doc/rfc/README.md",
                self._rfc_index(
                    "| [A](0001-a.md) | [B](0002-b.md) | None | None |\n"
                    "| [B](0002-b.md) | [C](0003-c.md) | None | None |\n"
                    "| [C](0003-c.md) | [A](0001-a.md), [A](0001-a.md) | None | None |\n"
                ),
            )
            self._write(
                root,
                "doc/rfc/0001-a.md",
                self._metadata(document_id="rfc.0001.a", document_type="rfc", status="draft", depends_on="rfc.0002.b")
                + self._rfc_body("RFC 0001: A", "[B](0002-b.md) is required."),
            )
            self._write(
                root,
                "doc/rfc/0002-b.md",
                self._metadata(document_id="rfc.0002.b", document_type="rfc", status="draft", depends_on="rfc.0003.c")
                + self._rfc_body("RFC 0002: B", "[C](0003-c.md) is required."),
            )
            self._write(
                root,
                "doc/rfc/0003-c.md",
                self._metadata(
                    document_id="rfc.0003.c",
                    document_type="rfc",
                    status="draft",
                    depends_on="rfc.0001.a, rfc.0001.a",
                )
                + self._rfc_body("RFC 0003: C", "[A](0001-a.md) and [A again](0001-a.md) repeat one edge."),
            )

            issues = doccheck.check_tree(root)

        cycles = [issue for issue in issues if issue.kind == "rfc-dependency-cycle"]
        self.assertEqual(len(cycles), 1)

    def test_accepts_conditional_and_cyclic_integration_dependencies_when_index_matches(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            self._valid_tree(root)
            doc_index = root / "doc/README.md"
            doc_index.write_text(doc_index.read_text(encoding="utf-8") + "\n[RFCs](rfc/README.md)\n", encoding="utf-8")
            self._write(
                root,
                "doc/rfc/README.md",
                self._rfc_index(
                    "| [Base](0001-base.md) | None | None | None |\n"
                    "| [Consumer](0002-consumer.md) | None | [Base](0001-base.md) | "
                    "[Peer](0003-peer.md) |\n"
                    "| [Peer](0003-peer.md) | None | None | [Consumer](0002-consumer.md) |\n"
                ),
            )
            self._write(
                root,
                "doc/rfc/0001-base.md",
                self._metadata(document_id="rfc.0001.base", document_type="rfc", status="draft")
                + self._rfc_body("RFC 0001: Base"),
            )
            self._write(
                root,
                "doc/rfc/0002-consumer.md",
                self._metadata(document_id="rfc.0002.consumer", document_type="rfc", status="draft")
                + self._rfc_body(
                    "RFC 0002: Consumer",
                    conditional="[Base](0001-base.md) supplies one optional phase.",
                    integration="[Peer](0003-peer.md) shares one contract.",
                ),
            )
            self._write(
                root,
                "doc/rfc/0003-peer.md",
                self._metadata(document_id="rfc.0003.peer", document_type="rfc", status="draft")
                + self._rfc_body(
                    "RFC 0003: Peer",
                    integration="[Consumer](0002-consumer.md) shares one contract.",
                ),
            )

            issues = doccheck.check_tree(root)

        self.assertEqual(issues, [])

    def test_rejects_optional_dependency_errors_and_rfc_index_drift(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            self._valid_tree(root)
            doc_index = root / "doc/README.md"
            doc_index.write_text(doc_index.read_text(encoding="utf-8") + "\n[RFCs](rfc/README.md)\n", encoding="utf-8")
            self._write(
                root,
                "doc/rfc/README.md",
                self._rfc_index(
                    "| [Base](0001-base.md) | None | None | None |\n"
                    "| [Consumer](0002-consumer.md) | None | None | None |\n"
                ),
            )
            self._write(
                root,
                "doc/rfc/0001-base.md",
                self._metadata(document_id="rfc.0001.base", document_type="rfc", status="draft")
                + self._rfc_body("RFC 0001: Base"),
            )
            self._write(
                root,
                "doc/rfc/0002-consumer.md",
                self._metadata(document_id="rfc.0002.consumer", document_type="rfc", status="draft")
                + self._rfc_body(
                    "RFC 0002: Consumer",
                    conditional="[Base](0001-base.md) and [Base again](0001-base.md) are duplicated.",
                    integration=(
                        "[Base](0001-base.md), [Self](0002-consumer.md), and "
                        "[a specification](../spec/example.md) are invalid together."
                    ),
                ),
            )

            issues = doccheck.check_tree(root)

        messages = [issue.message for issue in issues]
        self.assertTrue(any("duplicate conditional dependency" in message for message in messages))
        self.assertTrue(any("appears in both Conditional and Integration" in message for message in messages))
        self.assertTrue(any("cannot depend on itself in integration" in message for message in messages))
        self.assertTrue(any("integration dependency target is not an RFC" in message for message in messages))
        self.assertTrue(any("conditional dependencies do not match" in message for message in messages))

    def test_rejects_malformed_rfc_dependency_and_field_on_non_rfc(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            self._valid_tree(root)
            example = root / "doc/spec/example.md"
            example.write_text(
                example.read_text(encoding="utf-8").replace(
                    "summary: Defines an example contract.\n",
                    "summary: Defines an example contract.\ndepends-on: none\n",
                ),
                encoding="utf-8",
            )
            doc_index = root / "doc/README.md"
            doc_index.write_text(doc_index.read_text(encoding="utf-8") + "\n[RFCs](rfc/README.md)\n", encoding="utf-8")
            self._write(
                root,
                "doc/rfc/README.md",
                self._metadata(document_id="rfc.index", document_type="index")
                + "# RFCs\n\n[Malformed](0001-malformed.md)\n",
            )
            self._write(
                root,
                "doc/rfc/0001-malformed.md",
                self._metadata(
                    document_id="rfc.0001.malformed",
                    document_type="rfc",
                    status="draft",
                    depends_on="0002-not-an-rfc-id",
                )
                + self._rfc_body("RFC 0001: Malformed"),
            )

            issues = doccheck.check_tree(root)

        messages = [issue.message for issue in issues]
        self.assertTrue(any("valid only for RFCs" in message for message in messages))
        self.assertTrue(any("invalid RFC dependency id" in message for message in messages))


if __name__ == "__main__":
    unittest.main()
