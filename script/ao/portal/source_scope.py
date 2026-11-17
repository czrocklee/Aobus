"""Resolve source-operation selections once, retaining explicit-file provenance."""

from __future__ import annotations

import os
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path

from ao.core.paths import PROJECT_ROOT
from ao.portal import git_files

TOP_DIRS = (
    "app",
    "asset",
    "bazel",
    "cmake",
    "config",
    "doc",
    "include",
    "lib",
    "script",
    "test",
    "tool",
    ".agents/skills",
    ".github",
)
PRETTIER_CONTROL_FILES = (".prettierrc.json", ".prettierignore", ".gitignore", "script/ao/prettier.json")


@dataclass(frozen=True)
class Selection:
    paths: tuple[str, ...] = ()
    all: bool = False
    commit: str | None = None


@dataclass(frozen=True)
class SourceScope:
    files: tuple[str, ...] = ()
    explicit_files: tuple[str, ...] = ()


def validate(selection: Selection) -> None:
    """Admit selectors before discovery or native/tool preparation."""
    if sum((bool(selection.paths), selection.all, selection.commit is not None)) > 1:
        raise ValueError("paths, --all and --commit are mutually exclusive")
    if selection.commit is not None and (not selection.commit or "\0" in selection.commit):
        raise ValueError("--commit requires a nonempty revision without NUL characters")
    for name in selection.paths:
        if not name or "\0" in name:
            raise ValueError("source paths must be nonempty and contain no NUL characters")
        path = PROJECT_ROOT / name
        if not (path.is_file() or path.is_dir()):
            raise ValueError(f"explicitly selected path does not exist: {name}")


def file_exists(name: str) -> bool:
    return (PROJECT_ROOT / name).is_file()


def spelling(name: str | Path) -> str:
    """Normalize lexical paths without hiding a text formatter's escaping symlink."""
    path = Path(os.path.abspath(PROJECT_ROOT / name))
    try:
        return path.relative_to(PROJECT_ROOT).as_posix()
    except ValueError:
        return path.as_posix()


def _scan(folders: Sequence[str], *, include_root: bool = False) -> list[str]:
    internal: list[str] = []
    external: list[Path] = []
    for folder in folders:
        path = Path(os.path.abspath(PROJECT_ROOT / folder))
        if path.is_relative_to(PROJECT_ROOT):
            internal.append(folder)
        else:
            external.append(path)
    files = [
        name
        for name in git_files.find_sources(internal, suffixes=git_files.SOURCE_SUFFIXES)
        if not git_files.is_prettier_file(name)
    ]
    files.extend(git_files.prettier_sources(internal, include_root=include_root))
    # Explicit external directories obey the same downstream containment and
    # compile-owner admission as individually named external files. They never
    # acquire explicit-file provenance merely because the directory was named.
    for directory in external:
        files.extend(
            path.as_posix()
            for path in directory.rglob("*")
            if path.is_file() and git_files.matches_source(path.as_posix(), git_files.SOURCE_SUFFIXES)
        )
    if include_root:
        files.extend(git_files.root_bazel_files())
    return sorted(set(files))


def resolve(selection: Selection, *, top_dirs: Sequence[str] = TOP_DIRS) -> SourceScope:
    """Select files, directory scans, all sources or the default Git comparison.

    Explicit files may include intentional lint fixtures; directory, diff and
    whole-tree discovery do not promote those fixtures to explicit selections.
    A prepared SourceScope can be serialized and passed straight to execution.
    """
    validate(selection)
    if selection.paths:
        selected: set[str] = set()
        explicit: set[str] = set()
        folders: list[str] = []
        # Admit every path before starting any scan.
        for name in selection.paths:
            path = PROJECT_ROOT / name
            normalized = spelling(name)
            if path.is_file():
                selected.add(normalized)
                explicit.add(normalized)
            elif path.is_dir():
                folders.append(normalized)
            else:
                raise ValueError(f"explicitly selected path does not exist: {name}")
        if folders:
            selected.update(_scan(folders))
        return SourceScope(tuple(sorted(selected)), tuple(sorted(explicit)))
    if selection.all:
        return SourceScope(tuple(_scan(top_dirs, include_root=True)))
    changed = git_files.changed_paths(selection.commit)
    files = [
        name
        for name in changed
        if git_files.matches_source(name, git_files.SOURCE_SUFFIXES)
        and (
            name.startswith(tuple(f"{top}/" for top in top_dirs))
            or (
                Path(name).parent == Path(".")
                and (
                    git_files.is_bazel_file(name)
                    or git_files.is_bazel_rc_file(name)
                    or git_files.is_prettier_file(name)
                )
            )
            or git_files.is_workflow_file(name)
        )
        and not (name.startswith(f"{git_files.LINT_INTEGRATION_DIR}/") and not git_files.is_bazel_file(name))
        and file_exists(name)
    ]
    if any(name in PRETTIER_CONTROL_FILES for name in changed):
        files.extend(git_files.prettier_sources(list(top_dirs), include_root=True))
    return SourceScope(tuple(sorted(set(files))))


def prepared(files: Sequence[str], explicit_files: Sequence[str] = ()) -> SourceScope:
    """Rehydrate prepared paths and per-file provenance without rediscovery."""
    selected = tuple(dict.fromkeys(spelling(name) for name in files))
    explicit = {spelling(name) for name in explicit_files}
    if not explicit.issubset(selected):
        raise ValueError("explicit source provenance names a file outside the prepared scope")
    return SourceScope(selected, tuple(name for name in selected if name in explicit))


def require_files(scope: SourceScope) -> None:
    """Fail if a frozen input disappeared instead of silently shrinking a check."""
    for name in scope.files:
        if not file_exists(name):
            raise ValueError(f"selected source file does not exist: {name}")


def subset(scope: SourceScope, files: Sequence[str]) -> SourceScope:
    selected = tuple(files)
    explicit = set(scope.explicit_files)
    return SourceScope(selected, tuple(name for name in selected if name in explicit))
