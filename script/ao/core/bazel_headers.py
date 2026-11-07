"""Read selected vcpkg ownership inventories without walking the include tree.

The caller selects the reviewed public-header closure and validates raw installed
status (including port revisions). This module accepts upstream versions only;
.list filenames do not carry the port revision. It neither resolves dependencies
nor interprets CMake exports or header contents.
"""

from __future__ import annotations

import re
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path

_NAME = re.compile(r"[a-z0-9]+(?:-[a-z0-9]+)*\Z")
_VERSION = re.compile(r"[A-Za-z0-9][A-Za-z0-9._+-]*\Z")


class HeaderInventoryError(ValueError):
    """Invalid selection, ownership metadata, or selected header source."""


@dataclass(frozen=True)
class HeaderInventory:
    # Files retain their provider-relative spelling, including safe symlinks,
    # so a sparse overlay can preserve relative includes between nested roots.
    files: tuple[Path, ...]
    manifests: tuple[Path, ...]
    directories: tuple[Path, ...]


def _resolve(path: Path) -> Path:
    try:
        return path.resolve(strict=True)
    except (OSError, RuntimeError, ValueError) as exc:
        raise HeaderInventoryError(f"cannot resolve inventory path: {path}: {exc}") from exc


def _entry(line: str, triplet: str, manifest: Path, number: int) -> tuple[tuple[str, ...], bool]:
    directory = line.endswith("/")
    parts = tuple((line[:-1] if directory else line).split("/"))
    if (
        any(character in line for character in '\\:<>"|?*')
        or any(ord(character) < 32 or ord(character) == 127 for character in line)
        or any(not part or part in {".", ".."} or part.endswith((".", " ")) for part in parts)
    ):
        raise HeaderInventoryError(f"unsafe inventory entry: {manifest}:{number}: {line!r}")
    if parts[0] != triplet:
        raise HeaderInventoryError(f"wrong triplet in inventory entry: {manifest}:{number}: {line!r}")
    return parts[1:], directory


def vcpkg_header_inventory(prefix: Path, ports: Mapping[str, str], include_roots: Sequence[Path]) -> HeaderInventory:
    """Return owned files and the metadata watches for an installed triplet.

    Roots must be existing directories under the resolved ``prefix/include``.
    Each root must contain at least one selected owned file; individual ports
    may own no headers. All listed non-directory files beneath those roots are
    treated as header inputs, without suffix filtering. Unlisted physical files
    and lib/share payloads are not imported or enumerated.

    ``files`` are absolute provider paths, not canonical symlink targets. Keep
    their paths relative to ``prefix/include`` when materializing an overlay:
    flattening roots would break sibling-relative includes. Canonical targets
    must remain within the prefix and the selected canonical header roots.
    Casefold destination collisions fail on every host, not just Windows.

    ``manifests`` includes every selected .list, even a port with no retained
    files. ``directories`` contains the info directory whose membership decides
    missing/stale/ambiguous inventory selection. The caller owns watching these
    metadata inputs and the header payloads; there is no freshness cache here.
    """
    if not isinstance(ports, Mapping) or not ports:
        raise HeaderInventoryError("ports must be a nonempty mapping of base port names to upstream versions")
    for port, version in ports.items():
        if not isinstance(port, str) or not _NAME.fullmatch(port):
            raise HeaderInventoryError(f"invalid base port name: {port!r}")
        if not isinstance(version, str) or not _VERSION.fullmatch(version):
            raise HeaderInventoryError(f"invalid upstream version for {port}: {version!r}; do not include #revision")
    if not include_roots:
        raise HeaderInventoryError("include_roots must not be empty")

    prefix = _resolve(prefix)
    triplet = prefix.name
    if not _NAME.fullmatch(triplet) or not prefix.is_dir():
        raise HeaderInventoryError(f"invalid installed triplet prefix: {prefix}")
    include = prefix / "include"
    canonical_include = _resolve(include)
    if not canonical_include.is_relative_to(prefix) or not canonical_include.is_dir():
        raise HeaderInventoryError(f"header directory escapes the prefix or is not a directory: {include}")
    roots: dict[Path, Path] = {}
    for root in include_roots:
        root = root.absolute()
        if ".." in root.parts or not root.is_relative_to(include):
            raise HeaderInventoryError(f"include root is outside prefix/include: {root}")
        canonical = _resolve(root)
        if not canonical.is_relative_to(canonical_include) or not canonical.is_dir():
            raise HeaderInventoryError(f"include root escapes the header directory or is not a directory: {root}")
        roots[root] = canonical

    info = prefix.parent / "vcpkg" / "info"
    canonical_info = _resolve(info)
    if not canonical_info.is_relative_to(prefix.parent / "vcpkg") or not canonical_info.is_dir():
        raise HeaderInventoryError(f"inventory directory escapes vcpkg metadata or is not a directory: {info}")
    files: dict[str, Path] = {}
    manifests: list[Path] = []
    covered: set[Path] = set()
    try:
        # Only metadata membership is enumerated. Never recurse into include.
        names = [path.name for path in info.iterdir()]
        for port, version in sorted(ports.items()):
            expected = f"{port}_{version}_{triplet}.list"
            candidates = sorted(
                name
                for name in names
                if name.casefold().startswith(f"{port}_") and name.casefold().endswith(f"_{triplet}.list")
            )
            if candidates != [expected]:
                raise HeaderInventoryError(
                    f"missing, stale, or ambiguous inventory for {port}: expected {info / expected}; found {candidates}"
                )
            manifest = info / expected
            canonical_manifest = _resolve(manifest)
            if not canonical_manifest.is_relative_to(canonical_info) or not canonical_manifest.is_file():
                raise HeaderInventoryError(f"inventory source escapes info or is not a file: {manifest}")
            manifests.append(manifest)
            lines = manifest.read_text(encoding="utf-8").splitlines()
            if not lines:
                raise HeaderInventoryError(f"empty inventory: {manifest}")
            for number, line in enumerate(lines, 1):
                parts, directory = _entry(line, triplet, manifest, number)
                if directory:
                    continue
                source = prefix.joinpath(*parts)
                matching_roots = [root for root in roots if source.is_relative_to(root)]
                if not matching_roots:
                    continue
                target = _resolve(source)
                if not target.is_relative_to(prefix) or not any(
                    target.is_relative_to(canonical) for canonical in roots.values()
                ):
                    raise HeaderInventoryError(f"header source escapes selected header roots: {source}")
                if not target.is_file():
                    raise HeaderInventoryError(f"listed header source is not a file: {source}")
                destination = source.relative_to(include).as_posix().casefold()
                previous = files.get(destination)
                if previous is not None and previous.as_posix() != source.as_posix():
                    raise HeaderInventoryError(f"casefold header destination conflict: {previous} and {source}")
                files[destination] = source
                covered.update(matching_roots)
    except (OSError, UnicodeError) as exc:
        raise HeaderInventoryError(f"cannot read vcpkg header inventory in {info}: {exc}") from exc
    missing = roots.keys() - covered
    if missing:
        raise HeaderInventoryError(f"include roots have no selected owned files: {sorted(map(str, missing))}")
    return HeaderInventory(tuple(sorted(files.values())), tuple(manifests), (info,))
