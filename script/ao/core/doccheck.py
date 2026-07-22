"""Mechanical validation for the Aobus documentation system."""

from __future__ import annotations

import html
import re
import urllib.parse
from collections import Counter, deque
from collections.abc import Iterable
from dataclasses import dataclass
from pathlib import Path

from .paths import PROJECT_ROOT

GOVERNED_ROOT_TYPES: dict[str, frozenset[str]] = {
    "user": frozenset({"index", "user-guide"}),
    "architecture": frozenset({"index", "architecture"}),
    "spec": frozenset({"index", "spec"}),
    "reference": frozenset({"index", "reference"}),
    "development": frozenset({"index", "development"}),
    "decision": frozenset({"index", "decision"}),
    "rfc": frozenset({"index", "rfc"}),
}

TYPE_STATUSES: dict[str, frozenset[str]] = {
    "index": frozenset({"draft", "current", "deprecated"}),
    "user-guide": frozenset({"draft", "current", "deprecated"}),
    "architecture": frozenset({"draft", "current", "deprecated"}),
    "spec": frozenset({"draft", "current", "deprecated"}),
    "reference": frozenset({"draft", "current", "deprecated"}),
    "development": frozenset({"draft", "current", "deprecated"}),
    "decision": frozenset({"proposed", "accepted", "superseded", "rejected"}),
    "rfc": frozenset({"draft", "in-review", "accepted"}),
}

REQUIRED_METADATA = ("id", "type", "status", "domain", "summary")
RFC_DEPENDENCY_KEY = "depends-on"
RFC_DEPENDENCY_CATEGORIES = ("Hard", "Conditional", "Integration")
RFC_DEPENDENCY_INDEX_HEADER = "| RFC | Hard | Conditional | Integration |"
ALLOWED_METADATA = frozenset((*REQUIRED_METADATA, RFC_DEPENDENCY_KEY))
ID_RE = re.compile(r"^[a-z0-9]+(?:[.-][a-z0-9]+)*$")
DOMAIN_RE = re.compile(r"^[a-z0-9]+(?:-[a-z0-9]+)*$")
LINK_RE = re.compile(r"!?\[[^\]]*\]\(([^)]+)\)")
REFERENCE_DEFINITION_RE = re.compile(r"^\s{0,3}\[([^\]]+)\]:\s*(?:<([^>]+)>|(\S+))(?:\s+.*)?$")
REFERENCE_LINK_RE = re.compile(r"!?\[([^\]]+)\]\[([^\]]*)\]")
SHORTCUT_REFERENCE_RE = re.compile(r"!?\[([^\]]+)\](?![\[(])")
HEADING_RE = re.compile(r"^#{1,6}\s+(.+?)\s*#*\s*$")
HTML_ANCHOR_RE = re.compile(r"<a\s+(?:id|name)=[\"']([^\"']+)[\"'][^>]*>", re.IGNORECASE)
INLINE_CODE_RE = re.compile(r"`+[^`]*`+")
REACHABILITY_ROOTS = frozenset(GOVERNED_ROOT_TYPES)
PLURAL_DOCUMENT_DIRECTORIES = {
    "architectures": "architecture",
    "decisions": "decision",
    "developments": "development",
    "references": "reference",
    "rfcs": "rfc",
    "specs": "spec",
    "templates": "template",
    "users": "user",
}
PLACEHOLDER_IDS = frozenset(
    {
        "architecture.concern",
        "decision.0001.concern",
        "development.workflow",
        "domain.contract",
        "domain.index",
        "domain.task",
        "reference.surface",
        "rfc.0001.concern",
    }
)
PLACEHOLDER_SUMMARIES = frozenset(
    {
        "Defines one system boundary or cross-cutting architecture concern.",
        "Records one consequential Aobus design decision and its tradeoffs.",
        "Defines one contributor policy or repository workflow.",
        "Defines one normative Aobus behavior contract.",
        "Routes one Aobus documentation area and defines its ownership boundaries.",
        "Helps users accomplish one concrete Aobus task.",
        "Enumerates one exact Aobus command, schema, grammar, protocol, or format surface.",
        "Proposes one consequential change for review before implementation.",
    }
)
PLACEHOLDER_TITLES = frozenset(
    {
        "# Architecture title",
        "# Area title",
        "# Decision 0001: title",
        "# Development guide title",
        "# Reference title",
        "# RFC 0001: title",
        "# Specification title",
        "# Task title",
    }
)

REQUIRED_SECTIONS: dict[str, tuple[str, ...]] = {
    "architecture": (
        "Scope",
        "System context",
        "Responsibilities",
        "Boundaries and dependency direction",
        "Data and control flow",
        "Structural constraints",
        "Failure, cancellation, and lifetime boundaries",
        "Implementation map",
        "Test map",
        "Related documents",
    ),
    "spec": ("Scope", "Code boundary", "Implementation map", "Test map", "Related documents"),
    "reference": (
        "Scope and version",
        "Code boundary",
        "Implementation authority",
        "Test authority",
        "Related documents",
    ),
    "user-guide": ("Outcome", "Steps", "Verify the result"),
    "decision": ("Context", "Decision", "Alternatives considered", "Consequences", "Current authorities"),
}

ARCHITECTURE_ROLE_SECTION = "Portfolio roles"
ARCHITECTURE_RELATIONSHIP_SECTION = "Architecture relationships"
ARCHITECTURE_COVERAGE_SECTION = "Capability coverage"
ARCHITECTURE_COVERAGE_STATES = frozenset({"Current", "Partial"})


@dataclass(frozen=True)
class Issue:
    path: Path
    line: int
    kind: str
    message: str

    def format(self, root: Path = PROJECT_ROOT) -> str:
        try:
            relative = self.path.relative_to(root)
        except ValueError:
            relative = self.path
        return f"{relative.as_posix()}:{self.line}: {self.kind}: {self.message}"


@dataclass(frozen=True)
class Document:
    path: Path
    lines: tuple[str, ...]
    metadata: dict[str, str]
    body_line: int


def discover_markdown(root: Path = PROJECT_ROOT) -> list[Path]:
    doc_root = root / "doc"
    files = {path for path in doc_root.rglob("*.md") if path.relative_to(doc_root).parts[:1] != ("plan",)}
    files.update(path for path in (root / ".agents" / "skills").rglob("*.md"))
    files.update(path for name in ("README.md", "AGENTS.md", "CONTRIBUTING.md") if (path := root / name).is_file())
    return sorted(files)


def check_tree(root: Path = PROJECT_ROOT) -> list[Issue]:
    paths = discover_markdown(root)
    documents: dict[Path, Document] = {}
    issues: list[Issue] = []

    for path in paths:
        document, metadata_issues = _read_document(path, root)
        documents[path.resolve()] = document
        issues.extend(metadata_issues)

    issues.extend(_check_directory_names(paths, root))
    issues.extend(_check_unique_ids(documents.values()))
    issues.extend(_check_required_sections(documents.values()))
    issues.extend(_check_index_ownership(documents, root))
    issues.extend(_check_architecture_portfolio(documents, root))
    issues.extend(_check_rfc_dependencies(documents, root))
    issues.extend(_check_links(documents, root))
    issues.extend(_check_reachability(documents, root))
    return sorted(issues, key=lambda issue: (issue.path.as_posix(), issue.line, issue.kind, issue.message))


def _check_directory_names(paths: Iterable[Path], root: Path) -> list[Issue]:
    issues: list[Issue] = []
    doc_root = root / "doc"
    for path in paths:
        try:
            parts = path.relative_to(doc_root).parts[:-1]
        except ValueError:
            continue
        for part in parts:
            if singular := PLURAL_DOCUMENT_DIRECTORIES.get(part):
                issues.append(Issue(path, 1, "directory-name", f"use singular documentation directory '{singular}/'"))
                break
    return issues


def _read_document(path: Path, root: Path) -> tuple[Document, list[Issue]]:
    lines = tuple(path.read_text(encoding="utf-8", errors="replace").splitlines())
    try:
        relative = path.relative_to(root / "doc")
    except ValueError:
        relative = Path()
    governed_root = relative.parts[0] if relative.parts else ""
    governed = governed_root in GOVERNED_ROOT_TYPES
    metadata: dict[str, str] = {}
    body_line = 1
    issues: list[Issue] = []

    if governed:
        metadata, body_line, metadata_issues = _parse_metadata(path, lines)
        issues.extend(metadata_issues)
        issues.extend(_validate_metadata(path, relative, metadata))
        issues.extend(_validate_title(path, lines, body_line))
        if metadata.get("type") == "rfc":
            issues.extend(_validate_rfc_structure(path, lines, body_line, metadata))

    return Document(path=path, lines=lines, metadata=metadata, body_line=body_line), issues


def _parse_metadata(path: Path, lines: tuple[str, ...]) -> tuple[dict[str, str], int, list[Issue]]:
    if not lines or lines[0] != "---":
        return {}, 1, [Issue(path, 1, "metadata", "governed documents must start with YAML front matter")]

    try:
        closing_index = lines.index("---", 1)
    except ValueError:
        return {}, 1, [Issue(path, 1, "metadata", "front matter has no closing '---' delimiter")]

    metadata: dict[str, str] = {}
    issues: list[Issue] = []
    for index, line in enumerate(lines[1:closing_index], 2):
        key, separator, raw_value = line.partition(":")
        key = key.strip()
        value = raw_value.strip()
        if not separator or not key or not value:
            issues.append(Issue(path, index, "metadata", "metadata entries must be non-empty 'key: value' scalars"))
            continue
        if key in metadata:
            issues.append(Issue(path, index, "metadata", f"duplicate metadata key '{key}'"))
            continue
        if value.startswith(("[", "{", "|", ">")):
            issues.append(Issue(path, index, "metadata", "metadata values must use the flat scalar subset"))
            continue
        if len(value) >= 2 and value[0] == value[-1] and value[0] in {'"', "'"}:
            value = value[1:-1]
        metadata[key] = value

    return metadata, closing_index + 2, issues


def _validate_metadata(path: Path, relative: Path, metadata: dict[str, str]) -> list[Issue]:
    issues: list[Issue] = []
    for key in REQUIRED_METADATA:
        if key not in metadata:
            issues.append(Issue(path, 1, "metadata", f"missing required metadata key '{key}'"))

    for key in sorted(set(metadata) - ALLOWED_METADATA):
        issues.append(Issue(path, 1, "metadata", f"unknown metadata key '{key}'"))

    document_id = metadata.get("id", "")
    if document_id and ID_RE.fullmatch(document_id) is None:
        issues.append(Issue(path, 1, "metadata", f"invalid document id '{document_id}'"))
    if document_id in PLACEHOLDER_IDS:
        issues.append(Issue(path, 1, "placeholder", f"replace template document id '{document_id}'"))

    document_type = metadata.get("type", "")
    allowed_types = GOVERNED_ROOT_TYPES[relative.parts[0]]
    if document_type and document_type not in allowed_types:
        choices = ", ".join(sorted(allowed_types))
        issues.append(
            Issue(path, 1, "metadata", f"type '{document_type}' does not belong under {relative.parts[0]}/ ({choices})")
        )

    status = metadata.get("status", "")
    if document_type in TYPE_STATUSES and status and status not in TYPE_STATUSES[document_type]:
        choices = ", ".join(sorted(TYPE_STATUSES[document_type]))
        issues.append(Issue(path, 1, "metadata", f"status '{status}' is invalid for {document_type} ({choices})"))

    domain = metadata.get("domain", "")
    if domain and DOMAIN_RE.fullmatch(domain) is None:
        issues.append(Issue(path, 1, "metadata", f"invalid domain '{domain}'"))
    if domain == "domain":
        issues.append(Issue(path, 1, "placeholder", "replace template domain 'domain'"))

    summary = metadata.get("summary", "")
    if summary and summary[-1] not in ".?!":
        issues.append(Issue(path, 1, "metadata", "summary must be one sentence ending in punctuation"))
    if summary in PLACEHOLDER_SUMMARIES:
        issues.append(Issue(path, 1, "placeholder", "replace the template summary"))

    dependency_value = metadata.get(RFC_DEPENDENCY_KEY)
    if document_type == "rfc":
        if dependency_value is None:
            issues.append(Issue(path, 1, "rfc-dependency", f"missing required metadata key '{RFC_DEPENDENCY_KEY}'"))
        elif error := _rfc_dependency_syntax_error(dependency_value):
            issues.append(Issue(path, 1, "rfc-dependency", error))
    elif dependency_value is not None:
        issues.append(Issue(path, 1, "metadata", f"metadata key '{RFC_DEPENDENCY_KEY}' is valid only for RFCs"))

    if document_type == "index" and path.name != "README.md":
        issues.append(Issue(path, 1, "metadata", "index documents must be named README.md"))
    if path.name == "README.md" and document_type and document_type != "index":
        issues.append(Issue(path, 1, "metadata", "README.md documents must use type 'index'"))
    return issues


def _rfc_dependency_syntax_error(value: str) -> str | None:
    if value == "none":
        return None
    dependency_ids = [part.strip() for part in value.split(",")]
    if not dependency_ids or any(not dependency_id for dependency_id in dependency_ids):
        return f"'{RFC_DEPENDENCY_KEY}' must be 'none' or comma-separated stable RFC document ids"
    for dependency_id in dependency_ids:
        if ID_RE.fullmatch(dependency_id) is None or not dependency_id.startswith("rfc."):
            return f"invalid RFC dependency id '{dependency_id}'"
    return None


def _rfc_dependency_ids(metadata: dict[str, str]) -> tuple[str, ...]:
    value = metadata.get(RFC_DEPENDENCY_KEY, "")
    if not value or value == "none" or _rfc_dependency_syntax_error(value):
        return ()
    return tuple(part.strip() for part in value.split(","))


def _validate_rfc_structure(
    path: Path, lines: tuple[str, ...], body_line: int, metadata: dict[str, str]
) -> list[Issue]:
    headings = _h2_headings(lines, body_line)
    by_title: dict[str, list[int]] = {}
    for index, title in headings:
        by_title.setdefault(title, []).append(index)

    issues: list[Issue] = []
    dependency_headings = by_title.get("Dependencies", [])
    if len(dependency_headings) != 1:
        issues.append(
            Issue(
                path,
                body_line,
                "rfc-dependency",
                "RFCs must contain exactly one '## Dependencies' section",
            )
        )
        return issues

    problem_headings = by_title.get("Problem", [])
    goals_headings = by_title.get("Goals", [])
    dependency_index = dependency_headings[0]
    heading_titles = [title for _, title in headings]
    if (
        len(problem_headings) != 1
        or len(goals_headings) != 1
        or heading_titles.index("Dependencies") != heading_titles.index("Problem") + 1
        or heading_titles.index("Goals") != heading_titles.index("Dependencies") + 1
    ):
        issues.append(
            Issue(
                path,
                dependency_index + 1,
                "rfc-dependency",
                "'## Dependencies' must appear immediately between '## Problem' and '## Goals'",
            )
        )

    next_h2 = next((index for index, _ in headings if index > dependency_index), len(lines))
    section = lines[dependency_index + 1 : next_h2]
    category_lines: dict[str, list[tuple[int, str]]] = {category: [] for category in RFC_DEPENDENCY_CATEGORIES}
    for offset, line in enumerate(section, dependency_index + 2):
        for category in category_lines:
            prefix = f"- {category}:"
            if line.startswith(prefix):
                category_lines[category].append((offset, line[len(prefix) :].strip()))

    for category, entries in category_lines.items():
        if len(entries) != 1:
            issues.append(
                Issue(
                    path,
                    dependency_index + 1,
                    "rfc-dependency",
                    f"Dependencies must contain exactly one '- {category}:' entry",
                )
            )

    links_by_line: dict[int, list[str]] = {}
    for line_number, target in _links(lines):
        links_by_line.setdefault(line_number, []).append(target)

    for category, entries in category_lines.items():
        if len(entries) != 1:
            continue
        line_number, value = entries[0]
        dependency_links = links_by_line.get(line_number, [])
        if value.startswith("None.") and dependency_links:
            issues.append(Issue(path, line_number, "rfc-dependency", f"'- {category}: None.' cannot contain links"))
        elif value != "None." and not dependency_links:
            issues.append(
                Issue(
                    path,
                    line_number,
                    "rfc-dependency",
                    f"'- {category}:' must be 'None.' or link at least one RFC",
                )
            )

    hard_entries = category_lines["Hard"]
    dependency_ids = _rfc_dependency_ids(metadata)
    if hard_entries:
        line_number, hard_value = hard_entries[0]
        if not dependency_ids and hard_value != "None.":
            issues.append(
                Issue(path, line_number, "rfc-dependency", "'- Hard:' must be 'None.' when 'depends-on' is 'none'")
            )
        if dependency_ids and hard_value == "None.":
            issues.append(
                Issue(path, line_number, "rfc-dependency", "'- Hard:' must explain and link every metadata dependency")
            )
    return issues


def _h2_headings(lines: tuple[str, ...], body_line: int = 1) -> list[tuple[int, str]]:
    headings: list[tuple[int, str]] = []
    fence: str | None = None
    for index, line in enumerate(lines[max(body_line - 1, 0) :], max(body_line - 1, 0)):
        stripped = line.lstrip()
        if stripped.startswith(("```", "~~~")):
            marker = stripped[:3]
            if fence is None:
                fence = marker
            elif fence == marker:
                fence = None
            continue
        if fence is None and line.startswith("## ") and not line.startswith("### "):
            headings.append((index, line[3:].strip().rstrip("#").rstrip()))
    return headings


def _validate_title(path: Path, lines: tuple[str, ...], body_line: int) -> list[Issue]:
    for index in range(max(body_line - 1, 0), len(lines)):
        if not lines[index].strip():
            continue
        if lines[index] in PLACEHOLDER_TITLES:
            return [Issue(path, index + 1, "placeholder", "replace the template title")]
        if lines[index].startswith("# "):
            return []
        return [Issue(path, index + 1, "title", "the first body element must be one H1 title")]
    return [Issue(path, body_line, "title", "document has no H1 title")]


def _check_unique_ids(documents: Iterable[Document]) -> list[Issue]:
    by_id: dict[str, list[Document]] = {}
    for document in documents:
        if document_id := document.metadata.get("id"):
            by_id.setdefault(document_id, []).append(document)

    issues: list[Issue] = []
    for document_id, owners in sorted(by_id.items()):
        if len(owners) < 2:
            continue
        paths = ", ".join(owner.path.as_posix() for owner in owners)
        for owner in owners:
            issues.append(Issue(owner.path, 1, "duplicate-id", f"document id '{document_id}' is also used by: {paths}"))
    return issues


def _check_required_sections(documents: Iterable[Document]) -> list[Issue]:
    issues: list[Issue] = []
    for document in documents:
        document_type = document.metadata.get("type", "")
        if document_type not in REQUIRED_SECTIONS:
            continue
        headings = _h2_headings(document.lines, document.body_line)
        by_title: dict[str, list[int]] = {}
        for line_index, title in headings:
            by_title.setdefault(title, []).append(line_index)

        previous_index = -1
        for title in REQUIRED_SECTIONS[document_type]:
            matches = by_title.get(title, [])
            if len(matches) != 1:
                issues.append(
                    Issue(
                        document.path,
                        document.body_line,
                        "document-structure",
                        f"{document_type} documents must contain exactly one '## {title}' section",
                    )
                )
                continue
            current_index = matches[0]
            if current_index < previous_index:
                issues.append(
                    Issue(
                        document.path,
                        current_index + 1,
                        "document-structure",
                        f"'## {title}' appears out of required template order",
                    )
                )
            previous_index = max(previous_index, current_index)
    return issues


def _check_index_ownership(documents: dict[Path, Document], root: Path) -> list[Issue]:
    governed_documents = [
        document
        for document in documents.values()
        if document.metadata.get("type") in set().union(*GOVERNED_ROOT_TYPES.values())
    ]
    index_documents = [document for document in governed_documents if document.metadata.get("type") == "index"]
    directly_indexed: set[Path] = set()

    for index in index_documents:
        try:
            index_root = index.path.relative_to(root / "doc").parts[0]
        except (ValueError, IndexError):
            continue
        for _, target in _links(index.lines):
            resolved, _ = _resolve_link(index.path, target, root)
            if resolved is None:
                continue
            target_document = documents.get(resolved.resolve())
            if target_document is None:
                continue
            try:
                target_root = target_document.path.relative_to(root / "doc").parts[0]
            except (ValueError, IndexError):
                continue
            if target_root == index_root:
                directly_indexed.add(target_document.path.resolve())

    issues: list[Issue] = []
    for document in governed_documents:
        if document.metadata.get("type") == "index" or document.path.resolve() in directly_indexed:
            continue
        issues.append(
            Issue(
                document.path,
                1,
                "index-ownership",
                "governed documents must be linked directly from an index in the same documentation root",
            )
        )
    return issues


def _check_architecture_portfolio(documents: dict[Path, Document], root: Path) -> list[Issue]:
    index_path = (root / "doc" / "architecture" / "README.md").resolve()
    index = documents.get(index_path)
    architecture_documents = {
        document.path.resolve(): document
        for document in documents.values()
        if document.metadata.get("type") == "architecture"
    }
    if index is None:
        if not architecture_documents:
            return []
        return [
            Issue(
                root / "doc" / "architecture" / "README.md",
                1,
                "architecture-portfolio",
                "architecture index is missing",
            )
        ]

    current_documents = {
        path: document
        for path, document in architecture_documents.items()
        if document.metadata.get("status") == "current"
    }
    issues: list[Issue] = []
    role_rows = _section_table_rows(index, ARCHITECTURE_ROLE_SECTION)
    relationship_rows = _section_table_rows(index, ARCHITECTURE_RELATIONSHIP_SECTION)
    coverage_rows = _section_table_rows(index, ARCHITECTURE_COVERAGE_SECTION)

    role_counts: Counter[Path] = Counter()
    for line_number, cells in role_rows:
        if len(cells) not in {2, 3}:
            continue
        target = _first_governed_link_target(index, cells[0], documents, root)
        if target is None:
            continue
        target_document = documents[target]
        if target_document.metadata.get("type") != "architecture":
            issues.append(
                Issue(index.path, line_number, "architecture-portfolio", "portfolio role target is not an architecture")
            )
            continue
        if target_document.metadata.get("status") != "current":
            issues.append(
                Issue(
                    index.path,
                    line_number,
                    "architecture-portfolio",
                    "portfolio roles may contain only current architectures",
                )
            )
        role_counts[target] += 1

    relationship_counts: Counter[Path] = Counter()
    for line_number, cells in relationship_rows:
        if len(cells) != 3:
            continue
        target = _first_governed_link_target(index, cells[0], documents, root)
        if target is None:
            continue
        target_document = documents[target]
        if target_document.metadata.get("type") != "architecture":
            issues.append(
                Issue(
                    index.path,
                    line_number,
                    "architecture-portfolio",
                    "relationship row target is not an architecture",
                )
            )
            continue
        relationship_counts[target] += 1

    for path, document in current_documents.items():
        document_id = document.metadata.get("id", path.name)
        if role_counts[path] != 1:
            issues.append(
                Issue(
                    index.path,
                    1,
                    "architecture-portfolio",
                    f"current architecture '{document_id}' must appear in exactly one portfolio role row",
                )
            )
        if relationship_counts[path] != 1:
            issues.append(
                Issue(
                    index.path,
                    1,
                    "architecture-portfolio",
                    f"current architecture '{document_id}' must appear in exactly one relationship row",
                )
            )

    capability_names: Counter[str] = Counter()
    for line_number, cells in coverage_rows:
        if len(cells) != 4:
            continue
        capability, owner_cell, state, question = cells
        capability_names[capability] += 1
        if state not in ARCHITECTURE_COVERAGE_STATES:
            issues.append(
                Issue(
                    index.path,
                    line_number,
                    "architecture-coverage",
                    f"unknown architecture coverage state '{state}'",
                )
            )
            continue
        owner_paths = _governed_link_targets(index, owner_cell, documents, root)
        current_owner_paths = [path for path in owner_paths if path in current_documents]
        if state == "Current" and len(set(current_owner_paths)) != 1:
            issues.append(
                Issue(
                    index.path,
                    line_number,
                    "architecture-coverage",
                    "Current capabilities must link exactly one current architecture owner",
                )
            )
        if state == "Partial" and not current_owner_paths:
            issues.append(
                Issue(
                    index.path,
                    line_number,
                    "architecture-coverage",
                    "Partial capabilities must link at least one current endpoint owner",
                )
            )
        if state == "Partial" and not question.strip():
            issues.append(
                Issue(
                    index.path,
                    line_number,
                    "architecture-coverage",
                    "Partial capabilities must state the remaining documentation question",
                )
            )

    for capability, count in capability_names.items():
        if count > 1:
            issues.append(
                Issue(index.path, 1, "architecture-coverage", f"duplicate capability coverage row '{capability}'")
            )
    return issues


def _section_table_rows(document: Document, section_title: str) -> list[tuple[int, list[str]]]:
    headings = _h2_headings(document.lines, document.body_line)
    section_index = next((line for line, title in headings if title == section_title), None)
    if section_index is None:
        return []
    next_h2 = next((line for line, _ in headings if line > section_index), len(document.lines))
    rows: list[tuple[int, list[str]]] = []
    for index in range(section_index + 1, next_h2):
        line = document.lines[index].strip()
        if not line.startswith("|"):
            continue
        cells = [cell.strip() for cell in line.strip("|").split("|")]
        if not cells or all(re.fullmatch(r":?-{3,}:?", cell) for cell in cells):
            continue
        if cells[0] in {"Document", "Architecture", "Capability"}:
            continue
        rows.append((index + 1, cells))
    return rows


def _first_governed_link_target(
    source: Document, text: str, documents: dict[Path, Document], root: Path
) -> Path | None:
    targets = _governed_link_targets(source, text, documents, root)
    return targets[0] if targets else None


def _governed_link_targets(source: Document, text: str, documents: dict[Path, Document], root: Path) -> list[Path]:
    targets: list[Path] = []
    for _, target in _links((text,)):
        resolved, _ = _resolve_link(source.path, target, root)
        if resolved is None:
            continue
        key = resolved.resolve()
        if key in documents:
            targets.append(key)
    return targets


def _check_rfc_dependencies(documents: dict[Path, Document], root: Path) -> list[Issue]:
    by_id = {document.metadata["id"]: document for document in documents.values() if "id" in document.metadata}
    rfc_documents = {
        document_id: document for document_id, document in by_id.items() if document.metadata.get("type") == "rfc"
    }
    graph: dict[str, tuple[str, ...]] = {}
    relations: dict[str, dict[str, tuple[str, ...]]] = {}
    issues: list[Issue] = []

    for document_id, document in sorted(rfc_documents.items()):
        metadata_ids = _rfc_dependency_ids(document.metadata)
        graph[document_id] = metadata_ids
        duplicates = sorted(dependency_id for dependency_id, count in Counter(metadata_ids).items() if count > 1)
        for dependency_id in duplicates:
            issues.append(Issue(document.path, 1, "rfc-dependency", f"duplicate hard dependency '{dependency_id}'"))
        for dependency_id in dict.fromkeys(metadata_ids):
            if dependency_id == document_id:
                issues.append(Issue(document.path, 1, "rfc-dependency", "an RFC cannot depend on itself"))
                continue
            target = by_id.get(dependency_id)
            if target is None:
                issues.append(
                    Issue(document.path, 1, "rfc-dependency", f"hard dependency does not exist: {dependency_id}")
                )
            elif target.metadata.get("type") != "rfc":
                issues.append(
                    Issue(document.path, 1, "rfc-dependency", f"hard dependency is not an RFC: {dependency_id}")
                )

        document_relations, relation_issues = _rfc_dependency_relations(document, documents, root)
        relations[document_id] = document_relations
        issues.extend(relation_issues)

        section_hard = set(document_relations["Hard"])
        metadata_hard = set(metadata_ids)
        for dependency_id in sorted(metadata_hard - section_hard):
            issues.append(
                Issue(document.path, 1, "rfc-dependency", f"'- Hard:' must link dependency '{dependency_id}'")
            )
        for dependency_id in sorted(section_hard - metadata_hard):
            issues.append(
                Issue(
                    document.path,
                    1,
                    "rfc-dependency",
                    f"'- Hard:' links dependency not declared by '{RFC_DEPENDENCY_KEY}': {dependency_id}",
                )
            )

    issues.extend(_check_rfc_dependency_cycles(graph, rfc_documents))
    issues.extend(_check_rfc_dependency_index(documents, root, rfc_documents, relations))
    return issues


def _rfc_dependency_relations(
    document: Document, documents: dict[Path, Document], root: Path
) -> tuple[dict[str, tuple[str, ...]], list[Issue]]:
    headings = _h2_headings(document.lines, document.body_line)
    dependency_index = next((index for index, title in headings if title == "Dependencies"), None)
    if dependency_index is None:
        return {category: () for category in RFC_DEPENDENCY_CATEGORIES}, []
    next_h2 = next((index for index, _ in headings if index > dependency_index), len(document.lines))
    dependency_lines: dict[str, int] = {}
    for line_number in range(dependency_index + 2, next_h2 + 1):
        line = document.lines[line_number - 1]
        for category in RFC_DEPENDENCY_CATEGORIES:
            if line.startswith(f"- {category}:") and category not in dependency_lines:
                dependency_lines[category] = line_number

    links_by_line: dict[int, list[str]] = {}
    for line_number, target in _links(document.lines):
        links_by_line.setdefault(line_number, []).append(target)

    document_id = document.metadata.get("id", "")
    relations: dict[str, tuple[str, ...]] = {}
    issues: list[Issue] = []
    owner_by_dependency: dict[str, str] = {}
    for category in RFC_DEPENDENCY_CATEGORIES:
        category_line_number = dependency_lines.get(category)
        dependency_ids: list[str] = []
        if category_line_number is not None:
            for target in links_by_line.get(category_line_number, []):
                resolved, _ = _resolve_link(document.path, target, root)
                if resolved is None:
                    issues.append(
                        Issue(
                            document.path,
                            category_line_number,
                            "rfc-dependency",
                            f"{category.lower()} dependency must use a repository RFC link: {target}",
                        )
                    )
                    continue
                target_document = documents.get(resolved.resolve())
                if target_document is None or target_document.metadata.get("type") != "rfc":
                    issues.append(
                        Issue(
                            document.path,
                            category_line_number,
                            "rfc-dependency",
                            f"{category.lower()} dependency target is not an RFC: {target}",
                        )
                    )
                    continue
                dependency_id = target_document.metadata.get("id", "")
                if dependency_id == document_id:
                    issues.append(
                        Issue(
                            document.path,
                            category_line_number,
                            "rfc-dependency",
                            f"an RFC cannot depend on itself in {category.lower()} dependencies",
                        )
                    )
                    continue
                dependency_ids.append(dependency_id)

        for dependency_id, count in Counter(dependency_ids).items():
            if count > 1:
                issues.append(
                    Issue(
                        document.path,
                        category_line_number or document.body_line,
                        "rfc-dependency",
                        f"duplicate {category.lower()} dependency '{dependency_id}'",
                    )
                )
        for dependency_id in dict.fromkeys(dependency_ids):
            if previous_category := owner_by_dependency.get(dependency_id):
                issues.append(
                    Issue(
                        document.path,
                        category_line_number or document.body_line,
                        "rfc-dependency",
                        f"dependency '{dependency_id}' appears in both {previous_category} and {category}",
                    )
                )
            else:
                owner_by_dependency[dependency_id] = category
        relations[category] = tuple(dependency_ids)
    return relations, issues


def _check_rfc_dependency_index(
    documents: dict[Path, Document],
    root: Path,
    rfc_documents: dict[str, Document],
    relations: dict[str, dict[str, tuple[str, ...]]],
) -> list[Issue]:
    index_path = (root / "doc" / "rfc" / "README.md").resolve()
    index = documents.get(index_path)
    if index is None:
        if not rfc_documents:
            return []
        return [Issue(root / "doc" / "rfc" / "README.md", 1, "rfc-dependency-index", "RFC index is missing")]

    headings = _h2_headings(index.lines, index.body_line)
    section_index = next((line for line, title in headings if title == "Dependency map"), None)
    if section_index is None:
        return [
            Issue(index.path, index.body_line, "rfc-dependency-index", "RFC index must contain '## Dependency map'")
        ]
    next_h2 = next((line for line, _ in headings if line > section_index), len(index.lines))
    header_index = next(
        (
            line
            for line in range(section_index + 1, next_h2)
            if index.lines[line].strip() == RFC_DEPENDENCY_INDEX_HEADER
        ),
        None,
    )
    if header_index is None:
        return [
            Issue(
                index.path,
                section_index + 1,
                "rfc-dependency-index",
                f"dependency map must contain the exact header '{RFC_DEPENDENCY_INDEX_HEADER}'",
            )
        ]

    issues: list[Issue] = []
    separator_index = header_index + 1
    if separator_index >= len(index.lines) or not _is_four_column_markdown_separator(index.lines[separator_index]):
        issues.append(
            Issue(
                index.path,
                separator_index + 1,
                "rfc-dependency-index",
                "dependency map has no four-column separator",
            )
        )
        return issues

    rows: dict[str, dict[str, tuple[str, ...]]] = {}
    line_index = separator_index + 1
    while line_index < next_h2 and index.lines[line_index].lstrip().startswith("|"):
        line_number = line_index + 1
        cells = [cell.strip() for cell in index.lines[line_index].strip().strip("|").split("|")]
        if len(cells) != 4:
            issues.append(
                Issue(index.path, line_number, "rfc-dependency-index", "dependency row must contain four columns")
            )
            line_index += 1
            continue
        owner_ids, owner_issues = _rfc_index_cell_ids(index, cells[0], line_number, documents, root, allow_none=False)
        issues.extend(owner_issues)
        if len(owner_ids) != 1:
            issues.append(
                Issue(index.path, line_number, "rfc-dependency-index", "RFC column must link exactly one RFC")
            )
            line_index += 1
            continue
        owner_id = owner_ids[0]
        if owner_id in rows:
            issues.append(
                Issue(index.path, line_number, "rfc-dependency-index", f"duplicate dependency row for '{owner_id}'")
            )
            line_index += 1
            continue

        row_relations: dict[str, tuple[str, ...]] = {}
        for category, cell in zip(RFC_DEPENDENCY_CATEGORIES, cells[1:], strict=True):
            ids, cell_issues = _rfc_index_cell_ids(index, cell, line_number, documents, root, allow_none=True)
            issues.extend(cell_issues)
            row_relations[category] = ids
        rows[owner_id] = row_relations
        line_index += 1

    for document_id in sorted(set(rfc_documents) - set(rows)):
        issues.append(
            Issue(
                index.path,
                section_index + 1,
                "rfc-dependency-index",
                f"dependency map is missing RFC '{document_id}'",
            )
        )
    for document_id in sorted(set(rows) & set(relations)):
        for category in RFC_DEPENDENCY_CATEGORIES:
            expected = set(relations[document_id][category])
            actual = set(rows[document_id][category])
            if expected != actual:
                issues.append(
                    Issue(
                        index.path,
                        section_index + 1,
                        "rfc-dependency-index",
                        f"{document_id} {category.lower()} dependencies do not match its RFC section",
                    )
                )
    return issues


def _rfc_index_cell_ids(
    index: Document,
    cell: str,
    line_number: int,
    documents: dict[Path, Document],
    root: Path,
    *,
    allow_none: bool,
) -> tuple[tuple[str, ...], list[Issue]]:
    if allow_none and cell == "None":
        return (), []
    links = _links((cell,))
    issues: list[Issue] = []
    if not links:
        return (), [
            Issue(index.path, line_number, "rfc-dependency-index", "dependency cells must be 'None' or RFC links")
        ]
    residue = LINK_RE.sub("", cell).replace(",", "").strip()
    if residue:
        issues.append(
            Issue(index.path, line_number, "rfc-dependency-index", "dependency cells may contain only RFC links")
        )
    dependency_ids: list[str] = []
    for _, target in links:
        resolved, _ = _resolve_link(index.path, target, root)
        target_document = documents.get(resolved.resolve()) if resolved is not None else None
        if target_document is None or target_document.metadata.get("type") != "rfc":
            issues.append(
                Issue(
                    index.path,
                    line_number,
                    "rfc-dependency-index",
                    f"dependency cell target is not an RFC: {target}",
                )
            )
            continue
        dependency_ids.append(target_document.metadata["id"])
    for dependency_id, count in Counter(dependency_ids).items():
        if count > 1:
            issues.append(
                Issue(index.path, line_number, "rfc-dependency-index", f"duplicate dependency link '{dependency_id}'")
            )
    return tuple(dependency_ids), issues


def _is_four_column_markdown_separator(line: str) -> bool:
    cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
    return len(cells) == 4 and all(re.fullmatch(r":?-{3,}:?", cell) for cell in cells)


def _check_rfc_dependency_cycles(graph: dict[str, tuple[str, ...]], rfc_documents: dict[str, Document]) -> list[Issue]:
    state: dict[str, int] = {}
    stack: list[str] = []
    reported: set[tuple[str, ...]] = set()
    issues: list[Issue] = []

    def visit(document_id: str) -> None:
        state[document_id] = 1
        stack.append(document_id)
        for dependency_id in graph.get(document_id, ()):
            if dependency_id not in graph or dependency_id == document_id:
                continue
            if state.get(dependency_id, 0) == 0:
                visit(dependency_id)
                continue
            if state.get(dependency_id) != 1:
                continue
            cycle_start = stack.index(dependency_id)
            cycle = [*stack[cycle_start:], dependency_id]
            key = tuple(sorted(cycle[:-1]))
            if key in reported:
                continue
            reported.add(key)
            issues.append(
                Issue(
                    rfc_documents[cycle[0]].path,
                    1,
                    "rfc-dependency-cycle",
                    "hard dependency cycle: " + " -> ".join(cycle),
                )
            )
        stack.pop()
        state[document_id] = 2

    for document_id in sorted(graph):
        if state.get(document_id, 0) == 0:
            visit(document_id)
    return issues


def _check_links(documents: dict[Path, Document], root: Path) -> list[Issue]:
    issues: list[Issue] = []
    anchor_cache = {path: _anchors(document.lines) for path, document in documents.items()}

    for document in documents.values():
        links, undefined_references = _parse_links(document.lines)
        for line_number, label in undefined_references:
            issues.append(
                Issue(document.path, line_number, "broken-reference", f"reference label is not defined: {label}")
            )
        for line_number, target in links:
            resolved, fragment = _resolve_link(document.path, target, root)
            if resolved is None:
                continue
            if _is_under(resolved, root / "doc" / "plan"):
                issues.append(
                    Issue(document.path, line_number, "plan-link", "tracked documentation must not link to doc/plan")
                )
                continue
            if not resolved.exists():
                issues.append(Issue(document.path, line_number, "broken-link", f"target does not exist: {target}"))
                continue
            resolved_key = resolved.resolve()
            if fragment and resolved_key in anchor_cache and fragment not in anchor_cache[resolved_key]:
                target_name = target.split("#", 1)[0] or document.path.name
                issues.append(
                    Issue(
                        document.path,
                        line_number,
                        "broken-anchor",
                        f"anchor '#{fragment}' does not exist in {target_name}",
                    )
                )
    return issues


def _check_reachability(documents: dict[Path, Document], root: Path) -> list[Issue]:
    start = (root / "doc" / "README.md").resolve()
    if start not in documents:
        return [Issue(root / "doc" / "README.md", 1, "index", "documentation root index is missing")]

    reachable: set[Path] = set()
    pending = deque([start])
    while pending:
        current = pending.popleft()
        if current in reachable:
            continue
        reachable.add(current)
        document = documents[current]
        for _, target in _links(document.lines):
            resolved, _ = _resolve_link(document.path, target, root)
            if resolved is None:
                continue
            key = resolved.resolve()
            if key in documents and key not in reachable:
                pending.append(key)

    issues: list[Issue] = []
    for path, document in documents.items():
        try:
            relative = path.relative_to(root / "doc")
        except ValueError:
            continue
        if relative.parts and relative.parts[0] in REACHABILITY_ROOTS and path not in reachable:
            issues.append(Issue(document.path, 1, "orphan", "document is not reachable from doc/README.md"))
    return issues


def _links(lines: tuple[str, ...]) -> list[tuple[int, str]]:
    return _parse_links(lines)[0]


def _parse_links(lines: tuple[str, ...]) -> tuple[list[tuple[int, str]], list[tuple[int, str]]]:
    definitions = _reference_definitions(lines)
    result: list[tuple[int, str]] = []
    undefined_references: list[tuple[int, str]] = []
    fence: str | None = None
    for line_number, line in enumerate(lines, 1):
        stripped = line.lstrip()
        if stripped.startswith(("```", "~~~")):
            marker = stripped[:3]
            if fence is None:
                fence = marker
            elif fence == marker:
                fence = None
            continue
        if fence is not None:
            continue
        searchable = INLINE_CODE_RE.sub("", line)
        if REFERENCE_DEFINITION_RE.match(searchable):
            continue
        occupied: list[tuple[int, int]] = []
        for match in LINK_RE.finditer(searchable):
            result.append((line_number, _inline_target(match.group(1))))
            occupied.append(match.span())
        for match in REFERENCE_LINK_RE.finditer(searchable):
            label = _normalize_reference_label(match.group(2) or match.group(1))
            occupied.append(match.span())
            if label in definitions:
                result.append((line_number, definitions[label]))
            else:
                undefined_references.append((line_number, label))
        for match in SHORTCUT_REFERENCE_RE.finditer(searchable):
            if any(_spans_overlap(match.span(), span) for span in occupied):
                continue
            label = _normalize_reference_label(match.group(1))
            if label in definitions:
                result.append((line_number, definitions[label]))
    return result, undefined_references


def _reference_definitions(lines: tuple[str, ...]) -> dict[str, str]:
    definitions: dict[str, str] = {}
    fence: str | None = None
    for line in lines:
        stripped = line.lstrip()
        if stripped.startswith(("```", "~~~")):
            marker = stripped[:3]
            if fence is None:
                fence = marker
            elif fence == marker:
                fence = None
            continue
        if fence is not None:
            continue
        if match := REFERENCE_DEFINITION_RE.match(INLINE_CODE_RE.sub("", line)):
            label = _normalize_reference_label(match.group(1))
            definitions[label] = html.unescape(match.group(2) or match.group(3))
    return definitions


def _inline_target(raw: str) -> str:
    raw = raw.strip()
    if raw.startswith("<") and ">" in raw:
        target = raw[1 : raw.index(">")]
    else:
        target = raw.split(maxsplit=1)[0]
    return html.unescape(target)


def _normalize_reference_label(label: str) -> str:
    return " ".join(label.split()).casefold()


def _spans_overlap(left: tuple[int, int], right: tuple[int, int]) -> bool:
    return left[0] < right[1] and right[0] < left[1]


def _resolve_link(source: Path, target: str, root: Path) -> tuple[Path | None, str]:
    parsed = urllib.parse.urlsplit(target)
    if parsed.scheme or parsed.netloc:
        return None, ""
    path_text = urllib.parse.unquote(parsed.path)
    fragment = urllib.parse.unquote(parsed.fragment).lower()
    if not path_text:
        return source.resolve(), fragment
    if path_text.startswith("/"):
        resolved = root / path_text.lstrip("/")
    else:
        resolved = source.parent / path_text
    return resolved, fragment


def _anchors(lines: tuple[str, ...]) -> frozenset[str]:
    anchors: set[str] = set()
    counts: Counter[str] = Counter()
    fence: str | None = None
    for line in lines:
        stripped = line.lstrip()
        if stripped.startswith(("```", "~~~")):
            marker = stripped[:3]
            if fence is None:
                fence = marker
            elif fence == marker:
                fence = None
            continue
        if fence is not None:
            continue
        for custom in HTML_ANCHOR_RE.findall(line):
            anchors.add(custom.lower())
        if match := HEADING_RE.match(line):
            base = _heading_slug(match.group(1))
            index = counts[base]
            counts[base] += 1
            anchors.add(base if index == 0 else f"{base}-{index}")
    return frozenset(anchors)


def _heading_slug(text: str) -> str:
    text = html.unescape(text).lower().strip()
    text = re.sub(r"\[([^\]]+)\]\([^)]+\)", r"\1", text)
    text = re.sub(r"\[([^\]]+)\]\[[^\]]*\]", r"\1", text)
    text = re.sub(r"\[([^\]]+)\]", r"\1", text)
    text = re.sub(r"<[^>]+>", "", text)
    text = text.replace("`", "").replace("*", "").replace("_", "").replace("~", "")
    text = re.sub(r"[^\w\-\s]", "", text, flags=re.UNICODE)
    return re.sub(r"\s", "-", text)


def _is_under(path: Path, parent: Path) -> bool:
    try:
        path.resolve().relative_to(parent.resolve())
    except ValueError:
        return False
    return True
