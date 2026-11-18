"""Content-addressed vcpkg binary archives for CI caches.

CI moves vcpkg's files-backend archive directory between runners with
actions/cache. A restored directory can match its cache key while holding no
archive for the current package ABIs, for example after a runner image changes
the compiler or CMake. This helper keeps only the archives the prepared build
tree installed and derives the save key from those ABIs, so CI saves exactly
when the reusable content changes.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import sys
from collections.abc import Iterable, Sequence
from dataclasses import dataclass
from pathlib import Path

_ARCHIVE_NAME = re.compile(r"^([0-9a-f]{64})\.zip$")
_INSTALLED_DIRECTORY = "vcpkg_installed"


@dataclass(frozen=True)
class PruneResult:
    kept: tuple[str, ...]
    removed: int
    missing: tuple[str, ...]
    bytes: int


def _paragraphs(text: str) -> Iterable[dict[str, str]]:
    fields: dict[str, str] = {}
    for line in text.splitlines():
        if not line.strip():
            if fields:
                yield fields
                fields = {}
            continue
        if line[0].isspace():
            # Continuation lines belong to multi-line descriptions only.
            continue
        name, separator, value = line.partition(":")
        if separator:
            fields[name.strip()] = value.strip()
    if fields:
        yield fields


def _database_files(installed: Path) -> list[Path]:
    database = installed / "vcpkg"
    files = [database / "status"] if (database / "status").is_file() else []
    updates = database / "updates"
    if updates.is_dir():
        # Update files are numbered; order by value, not by padding width.
        numbered = [path for path in updates.iterdir() if path.is_file() and path.name.isdigit()]
        files.extend(sorted(numbered, key=lambda path: int(path.name)))
    return files


def installed_abis(installed: Path) -> set[str]:
    """Return package ABIs recorded as installed in one vcpkg_installed tree."""
    # Later update files supersede earlier paragraphs for the same package row.
    rows: dict[tuple[str, str, str], dict[str, str]] = {}
    for path in _database_files(installed):
        for fields in _paragraphs(path.read_text(encoding="utf-8")):
            identity = (fields.get("Package", ""), fields.get("Feature", ""), fields.get("Architecture", ""))
            rows[identity] = fields
    return {
        fields["Abi"]
        for fields in rows.values()
        if fields.get("Abi") and fields.get("Status", "").endswith(" installed")
    }


def find_installed_trees(roots: Iterable[Path]) -> list[Path]:
    trees: list[Path] = []
    for root in roots:
        for current, directories, _files in os.walk(root):
            if Path(current).name == _INSTALLED_DIRECTORY:
                trees.append(Path(current))
                directories.clear()
    return sorted(trees)


def prune(cache_dir: Path, abis: set[str]) -> PruneResult:
    """Remove archives for ABIs outside ``abis`` and report the retained set."""
    kept: set[str] = set()
    removed = 0
    size = 0
    if cache_dir.is_dir():
        for path in sorted(cache_dir.rglob("*.zip")):
            match = _ARCHIVE_NAME.match(path.name)
            if not match or not path.is_file():
                continue
            if match.group(1) in abis:
                kept.add(match.group(1))
                size += path.stat().st_size
            else:
                path.unlink()
                removed += 1
        for directory in sorted((path for path in cache_dir.rglob("*") if path.is_dir()), reverse=True):
            if not any(directory.iterdir()):
                directory.rmdir()
    return PruneResult(
        kept=tuple(sorted(kept)),
        removed=removed,
        missing=tuple(sorted(abis - kept)),
        bytes=size,
    )


def content_fingerprint(abis: Iterable[str]) -> str:
    return hashlib.sha256("\n".join(sorted(abis)).encode("ascii")).hexdigest()[:32]


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Aobus CI vcpkg binary-cache helper")
    subparsers = parser.add_subparsers(dest="command", required=True)
    seal = subparsers.add_parser("seal", help="prune stale archives and emit a content-addressed cache key")
    seal.add_argument("--cache-dir", type=Path, required=True)
    seal.add_argument("--build-root", type=Path, action="append", required=True)
    seal.add_argument("--key-prefix", required=True)
    seal.add_argument("--github-output", type=Path, required=True)
    return parser


def _seal(args: argparse.Namespace) -> dict[str, str]:
    trees = find_installed_trees(args.build_root)
    abis: set[str] = set()
    for tree in trees:
        abis |= installed_abis(tree)
    if not abis:
        print("No installed vcpkg packages were recorded; skipping cache save.", file=sys.stderr)
        return {"ready": "false"}
    result = prune(args.cache_dir, abis)
    print(
        f"vcpkg binary archives: {len(result.kept)} kept ({result.bytes} bytes), "
        f"{result.removed} stale removed, {len(result.missing)} installed without an archive"
    )
    if not result.kept:
        print("No reusable vcpkg archives remain; skipping cache save.", file=sys.stderr)
        return {"ready": "false"}
    return {"ready": "true", "key": args.key_prefix + content_fingerprint(result.kept)}


def main(argv: Sequence[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        outputs = _seal(args)
        with args.github_output.open("a", encoding="utf-8", newline="\n") as stream:
            for key, value in outputs.items():
                stream.write(f"{key}={value}\n")
        return 0
    except (OSError, UnicodeDecodeError) as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
