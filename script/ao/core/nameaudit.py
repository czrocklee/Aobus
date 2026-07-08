"""Mechanical naming checks for class/file role vocabulary."""

import re
from collections.abc import Iterable, Sequence
from dataclasses import dataclass
from pathlib import Path

from . import gitfiles
from .paths import PROJECT_ROOT

AUDIT_FOLDERS = ("app", "include", "lib", "test", "tool")
LINT_FIXTURE_PARTS = ("test", "integration", "lint", "fixture")

ROLE_ALLOWED_PREFIXES: dict[str, tuple[str, ...]] = {
    "ViewModel": ("app/include/ao/uimodel/", "app/uimodel/", "test/unit/uimodel/"),
    "Service": ("app/include/ao/rt/", "app/runtime/", "test/unit/runtime/"),
    "Component": ("app/linux-gtk/layout/", "test/unit/linux-gtk/layout/"),
    "Dialog": ("app/linux-gtk/", "test/unit/linux-gtk/"),
    "Widget": ("app/linux-gtk/", "test/unit/linux-gtk/"),
    "Panel": ("app/linux-gtk/", "app/tui/", "test/unit/linux-gtk/", "test/unit/tui/"),
    "Controller": ("app/linux-gtk/", "app/tui/", "test/unit/linux-gtk/", "test/unit/tui/"),
    "Coordinator": ("app/linux-gtk/", "app/tui/", "test/unit/linux-gtk/", "test/unit/tui/"),
    "Host": ("app/linux-gtk/", "app/tui/", "test/unit/linux-gtk/", "test/unit/tui/"),
    "Bridge": ("app/linux-gtk/", "app/tui/", "test/unit/linux-gtk/", "test/unit/tui/"),
}

ROLE_SUFFIXES = tuple(sorted(ROLE_ALLOWED_PREFIXES, key=len, reverse=True))
GENERIC_SUFFIXES = ("Utils", "Util", "Utility", "Types")
LEGACY_GENERIC_FILES = frozenset({"test/unit/TestUtils.h"})

RECORD_DEFINITION_RE = re.compile(r"\b(?:class|struct)\s+([A-Z][A-Za-z0-9_]*)\b[^;{}]*\{", re.DOTALL)
TEST_DOUBLE_RE = re.compile(r"^(?:Fake|Mock|Stub)[A-Z]")


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


@dataclass(frozen=True)
class Record:
    path: Path
    line: int
    name: str


def discover_files(root: Path = PROJECT_ROOT) -> list[Path]:
    files: list[Path] = []
    for folder in AUDIT_FOLDERS:
        base = root / folder
        if not base.exists():
            continue
        for path in base.rglob("*"):
            try:
                relative = path.relative_to(root)
            except ValueError:
                continue
            if path.is_file() and _is_auditable_file(relative):
                files.append(path)
    return sorted(files)


def resolve_files(paths: Iterable[str], root: Path = PROJECT_ROOT) -> list[Path]:
    selected = list(paths)
    if not selected:
        return discover_files(root)

    files: list[Path] = []
    seen: set[Path] = set()
    for name in selected:
        path = Path(name)
        if not path.is_absolute():
            path = root / path

        if path.is_dir():
            candidates = sorted(candidate for candidate in path.rglob("*") if candidate.suffix in gitfiles.CPP_SUFFIXES)
        elif path.is_file() and path.suffix in gitfiles.CPP_SUFFIXES:
            candidates = [path]
        else:
            candidates = []

        for candidate in candidates:
            try:
                relative = candidate.relative_to(root)
            except ValueError:
                continue
            if candidate in seen or not _is_auditable_file(relative):
                continue
            seen.add(candidate)
            files.append(candidate)

    return files


def parse_records(path: Path) -> list[Record]:
    source = path.read_text(encoding="utf-8")
    return [
        Record(path=path, line=source.count("\n", 0, match.start()) + 1, name=match.group(1))
        for match in RECORD_DEFINITION_RE.finditer(source)
    ]


def audit_paths(paths: Iterable[str], root: Path = PROJECT_ROOT) -> list[Issue]:
    return audit_files(resolve_files(paths, root), root)


def audit_files(files: Iterable[Path], root: Path = PROJECT_ROOT) -> list[Issue]:
    issues: list[Issue] = []
    for path in files:
        issues.extend(_audit_file_name(path, root))
        issues.extend(_audit_records(parse_records(path), root))
    return issues


def _audit_file_name(path: Path, root: Path) -> list[Issue]:
    relative = _relative(path, root)
    rel = relative.as_posix()
    stem = relative.stem
    issues: list[Issue] = []

    if stem.endswith(GENERIC_SUFFIXES) and rel not in LEGACY_GENERIC_FILES:
        issues.append(
            Issue(
                path,
                1,
                "generic-file",
                "file names must use a concrete domain concept, not Utils/Util/Utility/Types",
            )
        )

    if stem.endswith("Helper"):
        issues.append(
            Issue(path, 1, "generic-file", "use a concrete name or plural Helpers for scoped helper collections")
        )

    if stem.endswith("Helpers") and not _helpers_file_is_scoped(relative):
        issues.append(
            Issue(path, 1, "generic-file", "Helpers files must live in test, tool, or a detail implementation area")
        )

    return issues


def _audit_records(records: Sequence[Record], root: Path) -> list[Issue]:
    issues: list[Issue] = []
    for record in records:
        relative = _relative(record.path, root)
        rel = relative.as_posix()

        if TEST_DOUBLE_RE.match(record.name) is not None and not rel.startswith("test/"):
            issues.append(Issue(record.path, record.line, "test-double", "Fake/Mock/Stub types belong in tests"))

        suffix = _role_suffix(record.name)
        if suffix is None:
            continue

        if not rel.startswith(ROLE_ALLOWED_PREFIXES[suffix]):
            prefixes = ", ".join(ROLE_ALLOWED_PREFIXES[suffix])
            issues.append(
                Issue(
                    record.path,
                    record.line,
                    "role-location",
                    f"{suffix} types must live under one of: {prefixes}",
                )
            )

    return issues


def _role_suffix(name: str) -> str | None:
    for suffix in ROLE_SUFFIXES:
        if name.endswith(suffix) and name != suffix:
            return suffix
    return None


def _helpers_file_is_scoped(path: Path) -> bool:
    rel = path.as_posix()
    return rel.startswith("test/") or rel.startswith("tool/") or "/detail/" in rel


def _is_auditable_file(path: Path) -> bool:
    return path.suffix in gitfiles.CPP_SUFFIXES and path.parts[: len(LINT_FIXTURE_PARTS)] != LINT_FIXTURE_PARTS


def _relative(path: Path, root: Path) -> Path:
    try:
        return path.relative_to(root)
    except ValueError:
        return path
