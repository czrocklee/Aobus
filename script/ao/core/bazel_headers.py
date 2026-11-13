"""Read selected vcpkg ownership inventories without walking the include tree.

The caller selects the reviewed public-header closure and validates raw installed
status (including port revisions). This module accepts upstream versions only;
.list filenames do not carry the port revision. It neither resolves dependencies
nor interprets CMake exports or header contents.
"""

from __future__ import annotations

from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path


class HeaderInventoryError(ValueError):
    """The installed vcpkg metadata does not match the selected ports."""


@dataclass(frozen=True)
class HeaderInventory:
    # Files retain their provider-relative spelling, including symlinks, so a
    # sparse overlay can preserve relative includes between nested roots.
    files: tuple[Path, ...]
    manifests: tuple[Path, ...]
    directories: tuple[Path, ...]


def vcpkg_header_inventory(prefix: Path, ports: Mapping[str, str], include_roots: Sequence[Path]) -> HeaderInventory:
    """Return owned files and the metadata watches for an installed triplet.

    All listed non-directory files beneath the selected roots are header inputs,
    without suffix filtering. Unlisted physical files and lib/share payloads are
    not imported or enumerated; the include tree is never walked.

    ``files`` are absolute provider paths, not canonical symlink targets. Keep
    their paths relative to ``prefix/include`` when materializing an overlay:
    flattening roots would break sibling-relative includes.

    ``manifests`` includes every selected .list, even a port with no retained
    files. ``directories`` contains the info directory whose membership decides
    missing/stale/ambiguous inventory selection. The caller owns watching these
    metadata inputs and the header payloads; there is no freshness cache here.
    """
    prefix = prefix.resolve()
    triplet = prefix.name
    roots = [root.absolute() for root in include_roots]
    info = prefix.parent / "vcpkg" / "info"
    files: dict[str, Path] = {}
    manifests: list[Path] = []
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
        manifests.append(manifest)
        for line in manifest.read_text(encoding="utf-8").splitlines():
            if line.endswith("/"):
                continue
            # Each entry is "<triplet>/<path below the installed prefix>".
            source = prefix.joinpath(*line.split("/")[1:])
            if any(source.is_relative_to(root) for root in roots):
                files[source.as_posix()] = source
    return HeaderInventory(tuple(sorted(files.values())), tuple(manifests), (info,))
