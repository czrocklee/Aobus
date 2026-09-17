"""Mechanical validation for the Aobus documentation system."""

from __future__ import annotations

import html
import re
import urllib.parse
from collections import Counter, deque
from collections.abc import Iterable
from dataclasses import dataclass
from html.parser import HTMLParser
from pathlib import Path

from . import nameaudit
from .paths import PROJECT_ROOT, absolute_path

VALID_STATUSES = frozenset(
    {
        "accepted",
        "current",
        "deprecated",
        "draft",
        "implemented",
        "in-review",
        "proposed",
        "rejected",
        "superseded",
    }
)
ID_RE = re.compile(r"^[a-z0-9]+(?:[.-][a-z0-9]+)*$")
LINK_RE = re.compile(r"!?\[[^\]]*\]\(([^)]+)\)")
REFERENCE_DEFINITION_RE = re.compile(r"^\s{0,3}\[([^\]]+)\]:\s*(?:<([^>]+)>|(\S+))(?:\s+.*)?$")
REFERENCE_LINK_RE = re.compile(r"!?\[([^\]]+)\]\[([^\]]*)\]")
SHORTCUT_REFERENCE_RE = re.compile(r"!?\[([^\]]+)\](?![\[(])")
HEADING_RE = re.compile(r"^#{1,6}\s+(.+?)\s*#*\s*$")
INLINE_CODE_RE = re.compile(r"`+[^`]*`+")


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
    for folder in ("tool", "app", "asset"):
        if (root / folder).is_dir():
            files.update((root / folder).rglob("*.md"))
    return sorted(files)


def check_tree(root: Path = PROJECT_ROOT) -> list[Issue]:
    root = absolute_path(root)
    paths = discover_markdown(root)
    documents: dict[Path, Document] = {}
    issues: list[Issue] = []

    for path in paths:
        document, metadata_issues = _read_document(path, root)
        documents[absolute_path(path)] = document
        issues.extend(metadata_issues)

    issues.extend(_check_unique_ids(documents.values()))
    issues.extend(_check_links(documents, root))
    issues.extend(_check_reachability(documents, root))
    issues.extend(_check_naming_contract(documents, root))
    return sorted(issues, key=lambda issue: (issue.path.as_posix(), issue.line, issue.kind, issue.message))


def _check_naming_contract(documents: dict[Path, Document], root: Path) -> list[Issue]:
    """Validate executable naming vocabulary even in Markdown-only CI."""
    path = root / "doc/development/lint/naming-checks.md"
    document = documents.get(path)
    checks = root / "tool/lint/check"
    if document is None:
        if checks.is_dir():
            return [Issue(path, 1, "naming-contract", "naming checks reference document is missing")]
        return []

    text = "\n".join(document.lines)
    bullets = [
        " ".join(bullet.splitlines()) for bullet in re.findall(r"^- .*?(?:\n  .*?)*(?=\n(?!  )|\Z)", text, re.MULTILINE)
    ]
    issues = []
    for phrase, expected in (
        ("Layer placement for role suffixes", set(nameaudit.ROLE_ALLOWED_PREFIXES)),
        ("catch-all file name suffixes", set(nameaudit.GENERIC_SUFFIXES)),
        ("must live under `test/`", {"Fake*", "Mock*", "Spy*", "Stub*", "test/"}),
    ):
        bullet = next((item for item in bullets if phrase in item), "")
        actual = set(re.findall(r"`([^`]+)`", bullet))
        if actual != expected:
            issues.append(
                Issue(path, 1, "naming-contract", f"{phrase}: expected {sorted(expected)}, found {sorted(actual)}")
            )

    referenced_checks = {name for name in re.findall(r"`([^`]+)`", text) if name.endswith("Check")}
    if not referenced_checks:
        issues.append(Issue(path, 1, "naming-contract", "reference must name the naming lint checks"))
    for name in sorted(referenced_checks):
        if not (checks / f"{name}.h").is_file():
            issues.append(Issue(path, 1, "naming-contract", f"unknown naming lint check: {name}"))

    module_path = root / "tool/lint/AobusLintModule.cpp"
    if module_path.is_file():
        registrations = re.findall(
            r'registerCheck<(?:[A-Za-z_][A-Za-z0-9_]*::)*([A-Za-z_][A-Za-z0-9_]*Check)>\s*\(\s*"([^"]+)"',
            module_path.read_text(encoding="utf-8", errors="replace"),
        )
        registered_naming_checks = {class_name for class_name, alias in registrations if "naming" in alias}
        if referenced_checks != registered_naming_checks:
            issues.append(
                Issue(
                    path,
                    1,
                    "naming-contract",
                    "documented naming checks: "
                    f"expected {sorted(registered_naming_checks)}, found {sorted(referenced_checks)}",
                )
            )
    return issues


def _read_document(path: Path, root: Path) -> tuple[Document, list[Issue]]:
    lines = tuple(path.read_text(encoding="utf-8", errors="replace").splitlines())
    metadata: dict[str, str] = {}
    body_line = 1
    issues: list[Issue] = []

    # Skills have their own metadata schema, including multiline descriptions.
    if path.is_relative_to(root / "doc") and lines and lines[0] == "---":
        metadata, body_line, metadata_issues = _parse_metadata(path, lines)
        issues.extend(metadata_issues)
        issues.extend(_validate_metadata(path, metadata))

    return Document(path=path, lines=lines, metadata=metadata, body_line=body_line), issues


def _parse_metadata(path: Path, lines: tuple[str, ...]) -> tuple[dict[str, str], int, list[Issue]]:
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


def _validate_metadata(path: Path, metadata: dict[str, str]) -> list[Issue]:
    issues: list[Issue] = []
    document_id = metadata.get("id", "")
    if "id" in metadata and ID_RE.fullmatch(document_id) is None:
        issues.append(Issue(path, 1, "metadata", f"invalid document id '{document_id}'"))

    status = metadata.get("status", "")
    if "status" in metadata and status not in VALID_STATUSES:
        choices = ", ".join(sorted(VALID_STATUSES))
        issues.append(Issue(path, 1, "metadata", f"invalid document status '{status}' ({choices})"))
    return issues


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
            if not _is_under(resolved, root):
                issues.append(
                    Issue(document.path, line_number, "external-local-link", "local target is outside the repository")
                )
                continue
            if _is_under(resolved, root / "doc" / "plan"):
                issues.append(
                    Issue(document.path, line_number, "plan-link", "tracked documentation must not link to doc/plan")
                )
                continue
            if not resolved.exists():
                issues.append(Issue(document.path, line_number, "broken-link", f"target does not exist: {target}"))
                continue
            resolved_key = absolute_path(resolved)
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
    start = absolute_path(root / "doc" / "README.md")
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
        for _, target in _links(document.lines, include_images=False):
            resolved, _ = _resolve_link(document.path, target, root)
            if resolved is None:
                continue
            key = absolute_path(resolved)
            if key in documents and key not in reachable:
                pending.append(key)

    issues: list[Issue] = []
    doc_root = root / "doc"
    for path, document in documents.items():
        try:
            relative = path.relative_to(doc_root)
        except ValueError:
            continue
        if relative.parts[:1] == ("plan",):
            continue
        if path not in reachable:
            issues.append(Issue(document.path, 1, "orphan", "document is not reachable from doc/README.md"))
    return issues


def _links(lines: tuple[str, ...], *, include_images: bool = True) -> list[tuple[int, str]]:
    return _parse_links(lines, include_images=include_images)[0]


def _parse_links(
    lines: tuple[str, ...], *, include_images: bool = True
) -> tuple[list[tuple[int, str]], list[tuple[int, str]]]:
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
            if include_images or not _is_image_link(match):
                result.append((line_number, _inline_target(match.group(1))))
            occupied.append(match.span())
        for match in REFERENCE_LINK_RE.finditer(searchable):
            label = _normalize_reference_label(match.group(2) or match.group(1))
            occupied.append(match.span())
            if not include_images and _is_image_link(match):
                continue
            if label in definitions:
                result.append((line_number, definitions[label]))
            else:
                undefined_references.append((line_number, label))
        for match in SHORTCUT_REFERENCE_RE.finditer(searchable):
            if any(_spans_overlap(match.span(), span) for span in occupied):
                continue
            if not include_images and _is_image_link(match):
                continue
            label = _normalize_reference_label(match.group(1))
            if label in definitions:
                result.append((line_number, definitions[label]))
    return result, undefined_references


def _is_image_link(match: re.Match[str]) -> bool:
    if not match.group(0).startswith("!"):
        return False
    start = match.start()
    position = start
    while position > 0 and match.string[position - 1] == "\\":
        position -= 1
    # An odd backslash run escapes the marker, leaving an ordinary link.
    return (start - position) % 2 == 0


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
            # Markdown resolves repeated reference labels using the first definition.
            definitions.setdefault(label, html.unescape(match.group(2) or match.group(3)))
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
        return absolute_path(source), fragment
    if path_text.startswith("/"):
        resolved = root / path_text.lstrip("/")
    else:
        resolved = source.parent / path_text
    return resolved, fragment


class _HtmlAnchorParser(HTMLParser):
    def __init__(self) -> None:
        super().__init__()
        self.anchors: set[str] = set()

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        if tag == "a":
            self.anchors.update(value.lower() for name, value in attrs if name in {"id", "name"} and value)


def _anchors(lines: tuple[str, ...]) -> frozenset[str]:
    anchors: set[str] = set()
    custom = _HtmlAnchorParser()
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
        custom.feed(INLINE_CODE_RE.sub("", line) + "\n")
        if match := HEADING_RE.match(line):
            base = _heading_slug(match.group(1))
            index = counts[base]
            counts[base] += 1
            anchors.add(base if index == 0 else f"{base}-{index}")
    custom.close()
    return frozenset(anchors | custom.anchors)


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
        absolute_path(path).relative_to(absolute_path(parent))
    except ValueError:
        return False
    return True
