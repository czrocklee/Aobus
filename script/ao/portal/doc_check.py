"""Mechanical validation for the Aobus documentation system."""

from __future__ import annotations

import re
import urllib.parse
from collections import Counter, deque
from collections.abc import Callable, Iterable
from dataclasses import dataclass
from html.parser import HTMLParser
from pathlib import Path

from markdown_it import MarkdownIt
from markdown_it.common.utils import normalizeReference
from markdown_it.rules_inline import StateInline, image, link
from markdown_it.token import Token

from ao.core.paths import PROJECT_ROOT, absolute_path
from ao.portal import name_audit
from ao.portal.source_scope import discover_markdown

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
REFERENCE_LABEL_RE = re.compile(r"(?:\\[^\n]|[^\[\]\\])*")


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


def check_tree(root: Path = PROJECT_ROOT) -> list[Issue]:
    root = absolute_path(root)
    paths = discover_markdown(root)
    documents: dict[Path, Document] = {}
    issues: list[Issue] = []

    for path in paths:
        document, metadata_issues = _read_document(path, root)
        documents[absolute_path(path)] = document
        issues.extend(metadata_issues)

    parsed = {
        path: _parse_markdown(("",) * (document.body_line - 1) + document.lines[document.body_line - 1 :])
        for path, document in documents.items()
    }
    issues.extend(_check_unique_ids(documents.values()))
    link_issues, edges = _check_links(documents, parsed, root)
    issues.extend(link_issues)
    issues.extend(_check_navigation(edges, root))
    for path, facts in parsed.items():
        if facts.invalid_related_line is not None:
            issues.append(
                Issue(path, facts.invalid_related_line, "related-documents", "only link lists belong in this section")
            )
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
        ("Layer placement for role suffixes", set(name_audit.ROLE_ALLOWED_PREFIXES)),
        ("catch-all file name suffixes", set(name_audit.GENERIC_SUFFIXES)),
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

    if lines and lines[0] == "---":
        if path.is_relative_to(root / "doc"):
            metadata, body_line, metadata_issues = _parse_metadata(path, lines)
            issues.extend(metadata_issues)
            issues.extend(_validate_metadata(path, metadata))
        else:
            # Other discovered locations own their metadata schemas. Extract the
            # body consistently without imposing doc/'s flat scalar validation.
            try:
                body_line = lines.index("---", 1) + 2
            except ValueError:
                pass

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


def _check_links(
    documents: dict[Path, Document], parsed: dict[Path, _MarkdownFacts], root: Path
) -> tuple[list[Issue], dict[Path, set[Path]]]:
    issues: list[Issue] = []
    edges: dict[Path, set[Path]] = {key: set() for key in parsed}
    for key, facts in parsed.items():
        path = documents[key].path
        for line_number, label in facts.undefined_references:
            issues.append(Issue(path, line_number, "broken-reference", f"reference label is not defined: {label}"))
        for line_number, target, is_image in facts.links:
            resolved, fragment = _resolve_link(path, target, root)
            if resolved is None:
                continue
            if not _is_under(resolved, root):
                issues.append(Issue(path, line_number, "external-local-link", "local target is outside the repository"))
                continue
            if _is_under(resolved, root / "doc" / "plan"):
                issues.append(Issue(path, line_number, "plan-link", "tracked documentation must not link to doc/plan"))
                continue
            if not resolved.exists():
                issues.append(
                    Issue(
                        path,
                        line_number,
                        "broken-link",
                        f"target does not exist: {urllib.parse.unquote(target)}",
                    )
                )
                continue
            resolved_key = absolute_path(resolved)
            if resolved_key not in parsed:
                continue
            if not is_image and resolved_key != key:
                edges[key].add(resolved_key)
            if fragment and fragment not in parsed[resolved_key].anchors:
                target_name = urllib.parse.unquote(target.split("#", 1)[0]) or path.name
                issues.append(
                    Issue(
                        path,
                        line_number,
                        "broken-anchor",
                        f"anchor '#{fragment}' does not exist in {target_name}",
                    )
                )
    return issues, edges


def _topic_hubs(relative: Path) -> tuple[Path, ...]:
    """Return eligible hubs relative to doc/, excluding the entry pages themselves."""
    pillar = Path(relative.parts[0])
    hubs: tuple[Path, ...]
    if pillar.name == "system" and len(relative.parts) > 2:
        hubs = (pillar / relative.parts[1] / "README.md",)
    elif pillar.name == "development":
        hubs = tuple(pillar / name for name in ("README.md", "test.md", "linting.md"))
    elif pillar.name in {"user", "reference", "decision", "system"}:
        hubs = (pillar / "README.md",)
    else:
        return ()
    return () if relative in hubs else hubs


def _check_navigation(edges: dict[Path, set[Path]], root: Path) -> list[Issue]:
    doc_root = root / "doc"
    start = absolute_path(doc_root / "README.md")
    if start not in edges:
        return [Issue(start, 1, "index", "documentation root index is missing")]

    reachable: set[Path] = set()
    pending = deque([start])
    while pending:
        current = pending.popleft()
        if current in reachable:
            continue
        reachable.add(current)
        pending.extend(edges[current] - reachable)

    issues: list[Issue] = []
    for path in edges:
        if not path.is_relative_to(doc_root):
            continue
        if path not in reachable:
            issues.append(Issue(path, 1, "orphan", "document is not reachable from doc/README.md"))
        hubs = _topic_hubs(path.relative_to(doc_root))
        if hubs and not any(path in edges.get(doc_root / hub, ()) for hub in hubs):
            choices = " or ".join(f"doc/{hub.as_posix()}" for hub in hubs)
            issues.append(Issue(path, 1, "hub-link", f"document needs a direct link from {choices}"))
    return issues


@dataclass(frozen=True)
class _MarkdownFacts:
    # The boolean distinguishes checked image targets from navigation edges.
    links: tuple[tuple[int, str, bool], ...]
    undefined_references: tuple[tuple[int, str], ...]
    anchors: frozenset[str]
    invalid_related_line: int | None


def _undefined_reference(state: StateInline, start: int) -> str | None:
    """Recognize only the checker's explicit-reference diagnostic, not Markdown blocks."""
    label_end = state.md.helpers.parseLinkLabel(state, start, True)
    if label_end < 0 or state.src[label_end + 1 : label_end + 2] != "[":
        return None
    reference_end = state.md.helpers.parseLinkLabel(state, label_end + 1)
    if reference_end < 0:
        return None
    label = state.src[label_end + 2 : reference_end] or state.src[start + 1 : label_end]
    if not label.strip() or REFERENCE_LABEL_RE.fullmatch(label) is None:
        return None
    if normalizeReference(label) in state.env.get("references", {}):
        return None
    return " ".join(label.split()).casefold()


def _parse_markdown(lines: tuple[str, ...]) -> _MarkdownFacts:
    """Project links, navigation and anchors from one CommonMark + table parse."""
    parser = MarkdownIt("commonmark").enable("table")
    # Block parsing supplies the actual inline containers and their physical maps.
    # Resolve references first, then observe the standard inline rules in each
    # container. No second fence, list, definition or code-span scanner is used.
    parser.core.ruler.disable("inline")
    image_depth = 0
    links: list[tuple[int, str, bool]] = []
    undefined: list[tuple[int, str]] = []
    block_links: list[tuple[int, str, bool]] = []
    block_undefined: dict[int, str] = {}

    def observe(rule: Callable[[StateInline, bool], bool], *, is_image: bool) -> Callable[[StateInline, bool], bool]:
        def observed(state: StateInline, silent: bool) -> bool:
            nonlocal image_depth
            start = state.pos
            token_start = len(state.tokens)
            if is_image:
                image_depth += 1
            try:
                matched = rule(state, silent)
            finally:
                if is_image:
                    image_depth -= 1
            if silent or image_depth:
                return matched
            if matched:
                kind, attribute = ("image", "src") if is_image else ("link_open", "href")
                target = next(token.attrGet(attribute) for token in state.tokens[token_start:] if token.type == kind)
                assert isinstance(target, str)
                block_links.append((start, target, is_image))
            elif state.src.startswith("![" if is_image else "[", start):
                label = _undefined_reference(state, start + int(is_image))
                if label is not None:
                    # An unresolved image is left as ordinary text by Markdown.
                    # Its following '[' must not emit a second, non-image error.
                    block_undefined.setdefault(start + int(is_image), label)
            return matched

        return observed

    parser.inline.ruler.at("link", observe(link, is_image=False))
    parser.inline.ruler.at("image", observe(image, is_image=True))
    environment: dict[str, object] = {}
    tokens = parser.parse("\n".join(lines), environment)
    anchors: set[str] = set()
    counts: Counter[str] = Counter()
    custom = _HtmlAnchorParser()
    heading = False
    for token in tokens:
        if token.type == "heading_open":
            heading = True
        elif token.type == "heading_close":
            heading = False
        elif token.type == "html_block":
            custom.feed(token.content)
        elif token.type == "inline":
            assert token.map is not None
            first_line = token.map[0] + 1
            block_links.clear()
            block_undefined.clear()
            children: list[Token] = []
            parser.inline.parse(token.content, parser, environment, children)
            token.children = children
            links.extend(
                (first_line + token.content.count("\n", 0, offset), target, is_image)
                for offset, target, is_image in sorted(block_links)
            )
            undefined.extend(
                (first_line + token.content.count("\n", 0, offset), label)
                for offset, label in sorted(block_undefined.items())
            )
            for child in children:
                if child.type == "html_inline":
                    custom.feed(child.content)
            if heading:
                base = _heading_slug(_inline_text(children))
                index = counts[base]
                counts[base] += 1
                anchors.add(base if index == 0 else f"{base}-{index}")
    custom.close()
    return _MarkdownFacts(
        tuple(links), tuple(undefined), frozenset(anchors | custom.anchors), _invalid_related_line(tokens)
    )


def _invalid_related_line(tokens: list[Token]) -> int | None:
    # Headings consist of heading_open, inline, heading_close. Quoted headings
    # and examples are not page sections; reference definitions are not blocks
    # of rendered content, so they may supply links even after the list.
    for heading_index, token in enumerate(tokens):
        if token.type == "heading_open" and token.tag == "h2" and token.level == 0:
            title = _inline_text(tokens[heading_index + 1].children or [])
            if " ".join(title.split()).casefold() == "related documents":
                break
    else:
        return None

    tail_start = heading_index + 3
    if tail_start == len(tokens):
        heading_lines = tokens[heading_index].map
        assert heading_lines is not None
        return heading_lines[0] + 1

    for index in range(tail_start, len(tokens)):
        token = tokens[index]
        if token.nesting < 0 or token.type in {"bullet_list_open", "ordered_list_open"}:
            continue
        if token.type == "list_item_open" and tokens[index + 1].type == "paragraph_open":
            continue
        if token.type == "paragraph_open" and tokens[index - 1].type == "list_item_open":
            continue
        if token.type == "inline" and any(child.type == "link_open" for child in token.children or []):
            continue
        assert token.map is not None
        return token.map[0] + 1
    return None


def _inline_text(tokens: list[Token]) -> str:
    return "".join(
        _inline_text(token.children or [])
        if token.type == "image"
        else "\n"
        if token.type in {"softbreak", "hardbreak"}
        else token.content
        if token.type in {"text", "text_special", "code_inline"}
        else ""
        for token in tokens
    )


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


def _heading_slug(text: str) -> str:
    text = text.lower().strip()
    text = re.sub(r"[^\w\-\s]", "", text, flags=re.UNICODE)
    return re.sub(r"\s", "-", text)


def _is_under(path: Path, parent: Path) -> bool:
    try:
        absolute_path(path).relative_to(absolute_path(parent))
    except ValueError:
        return False
    return True
