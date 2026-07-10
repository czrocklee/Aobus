"""Advisory checks for Catch2 test names and tags."""

import re
from collections.abc import Iterable
from dataclasses import dataclass
from pathlib import Path

from .paths import PROJECT_ROOT

LINT_FIXTURE_PARTS = ("integration", "lint", "fixture")

KNOWN_LAYERS = frozenset(
    {
        "audio",
        "cli",
        "core",
        "council",
        "gtk",
        "library",
        "lmdb",
        "media",
        "perf",
        "query",
        "runtime",
        "tag",
        "tui",
        "uimodel",
        "utility",
    }
)

KNOWN_TYPES = frozenset({"integration", "regression", "smoke", "unit", "workflow"})

TAG_RE = re.compile(r"\[([^\[\]]+)\]")
TEST_CASE_RE = re.compile(r"\bTEST_CASE\s*\(")
STRING_LITERAL_RE = re.compile(r'"((?:\\.|[^"\\])*)"')
# Catch2 treats a leading dot as an opt-in hidden tag. The tag body still uses
# the project's kebab-case convention (for example, [.manual]).
TAG_STYLE_RE = re.compile(r"\.?[a-z0-9]+(?:-[a-z0-9]+)*")
FUNCTION_LEVEL_NAME_RE = re.compile(r"(?:[a-z][A-Za-z0-9_]*|operator\S+)\s+[a-z].+")


@dataclass(frozen=True)
class TestCase:
    path: Path
    line: int
    name: str
    tags: tuple[str, ...]


@dataclass(frozen=True)
class Issue:
    path: Path
    line: int
    kind: str
    message: str

    def format(self, root: Path = PROJECT_ROOT) -> str:
        try:
            path = self.path.relative_to(root).as_posix()
        except ValueError:
            path = self.path.as_posix()
        return f"{path}:{self.line}: {self.kind}: {self.message}"


def discover_test_files(root: Path = PROJECT_ROOT) -> list[Path]:
    test_root = root / "test"
    return sorted(path for path in test_root.rglob("*Test.cpp") if _is_auditable_test_file(path, test_root))


def resolve_files(paths: Iterable[str], root: Path = PROJECT_ROOT) -> list[Path]:
    selected = list(paths)
    if not selected:
        return discover_test_files(root)

    files: list[Path] = []
    seen: set[Path] = set()
    test_root = root / "test"
    for name in selected:
        path = Path(name)
        if not path.is_absolute():
            path = root / path
        if path.is_dir():
            candidates = sorted(path.rglob("*Test.cpp"))
        elif path.is_file() and path.name.endswith("Test.cpp"):
            candidates = [path]
        else:
            candidates = []

        for candidate in candidates:
            resolved = candidate.resolve()
            if resolved in seen or not _is_auditable_test_file(resolved, test_root):
                continue
            seen.add(resolved)
            files.append(resolved)

    return files


def parse_test_cases(path: Path) -> list[TestCase]:
    source = path.read_text(encoding="utf-8")
    cases: list[TestCase] = []

    for match in TEST_CASE_RE.finditer(source):
        open_paren = match.end() - 1
        close_paren = _matching_paren(source, open_paren)
        if close_paren is None:
            continue

        invocation = source[open_paren + 1 : close_paren]
        literals = [m.group(1) for m in STRING_LITERAL_RE.finditer(invocation)]
        if not literals:
            continue

        tags = tuple(TAG_RE.findall(literals[1])) if len(literals) > 1 else ()
        cases.append(TestCase(path=path, line=source.count("\n", 0, match.start()) + 1, name=literals[0], tags=tags))

    return cases


def audit_files(files: Iterable[Path]) -> list[Issue]:
    issues: list[Issue] = []
    for path in files:
        for case in parse_test_cases(path):
            issues.extend(_audit_case(case))
    return issues


def audit_paths(paths: Iterable[str], root: Path = PROJECT_ROOT) -> list[Issue]:
    return audit_files(resolve_files(paths, root))


def _audit_case(case: TestCase) -> list[Issue]:
    issues: list[Issue] = []
    if not _valid_name(case.name):
        issues.append(
            Issue(
                case.path,
                case.line,
                "name",
                'test name should use "Component - behavior" or the documented function-level form',
            )
        )

    if not case.tags:
        issues.append(Issue(case.path, case.line, "tag", "test case should include Catch2 tags"))
        return issues

    if case.tags[0] not in KNOWN_LAYERS:
        issues.append(
            Issue(
                case.path,
                case.line,
                "tag-order",
                f"first tag should be a known layer tag, got [{case.tags[0]}]",
            )
        )

    if len(case.tags) < 2:
        issues.append(Issue(case.path, case.line, "tag-order", "second tag should identify the test type"))
    elif case.tags[1] not in KNOWN_TYPES:
        issues.append(
            Issue(
                case.path,
                case.line,
                "tag-order",
                f"second tag should be a known test type tag, got [{case.tags[1]}]",
            )
        )

    if len(case.tags) > 4:
        issues.append(Issue(case.path, case.line, "tag-count", "prefer three or four tags"))

    for tag in case.tags:
        if TAG_STYLE_RE.fullmatch(tag) is None:
            issues.append(Issue(case.path, case.line, "tag-style", f"tag [{tag}] should use kebab case"))

    return issues


def _valid_name(name: str) -> bool:
    if " - " in name:
        component, behavior = name.split(" - ", 1)
        return bool(component.strip()) and bool(behavior.strip())
    return FUNCTION_LEVEL_NAME_RE.fullmatch(name) is not None


def _is_auditable_test_file(path: Path, test_root: Path) -> bool:
    try:
        relative = path.relative_to(test_root)
    except ValueError:
        return False
    return relative.parts[: len(LINT_FIXTURE_PARTS)] != LINT_FIXTURE_PARTS


def _matching_paren(source: str, open_paren: int) -> int | None:
    depth = 0
    index = open_paren
    while index < len(source):
        char = source[index]

        if char == '"':
            index = _skip_string(source, index)
            continue

        if char == "'":
            index = _skip_char(source, index)
            continue

        if source.startswith("//", index):
            newline = source.find("\n", index + 2)
            if newline == -1:
                return None
            index = newline + 1
            continue

        if source.startswith("/*", index):
            end = source.find("*/", index + 2)
            if end == -1:
                return None
            index = end + 2
            continue

        if char == "(":
            depth += 1
        elif char == ")":
            depth -= 1
            if depth == 0:
                return index

        index += 1

    return None


def _skip_string(source: str, start: int) -> int:
    index = start + 1
    while index < len(source):
        if source[index] == "\\":
            index += 2
            continue
        if source[index] == '"':
            return index + 1
        index += 1
    return index


def _skip_char(source: str, start: int) -> int:
    index = start + 1
    while index < len(source):
        if source[index] == "\\":
            index += 2
            continue
        if source[index] == "'":
            return index + 1
        index += 1
    return index
