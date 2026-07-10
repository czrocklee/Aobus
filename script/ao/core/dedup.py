"""De-duplication of clang-tidy diagnostic blocks across per-file logs.

Header diagnostics surface once per translation unit that includes the header; this
collapses them to one block keyed by location and message. Used by both the tidy and
analyze commands (formerly two divergent copies embedded in the shell scripts).
"""

import os
import re
from collections.abc import Iterable
from pathlib import Path
from typing import IO

from .paths import absolute_path

DIAGNOSTIC_RE = re.compile(r"^(.+):([0-9]+):([0-9]+):\s+(warning|error|note):\s+(.*)")
NOISE_RE = re.compile(
    r"^([0-9]+ warnings? generated\.|Suppressed [0-9]+ warnings?(?: \([^)]+\))?\.?|Use -header-filter=.*)$"
)


def deduplicate(
    log_paths: Iterable[Path],
    out: IO[str],
    project_root: Path,
    *,
    include_external: bool = True,
) -> int:
    """Write unique diagnostic blocks from the logs to `out`; return the unique count.

    With include_external=False, blocks whose primary location resolves outside
    project_root are dropped entirely (analyzer reports on third-party headers).
    """
    root = absolute_path(project_root)
    seen: set[str] = set()
    block: list[str] = []
    cid: str | None = None
    skip_block = False
    count = 0

    def normalized_path(path: str) -> Path:
        p = Path(path)
        if not p.is_absolute():
            p = root / p
        try:
            return absolute_path(p)
        except OSError:
            return p

    def normalize(path: str) -> str:
        return os.path.normcase(str(normalized_path(path)))

    def is_project_path(path: str) -> bool:
        try:
            normalized_path(path).relative_to(root)
            return True
        except ValueError:
            return False

    def flush() -> None:
        nonlocal block, cid, count
        if block and cid and cid not in seen:
            out.write("".join(block))
            seen.add(cid)
            count += 1
        block = []
        cid = None

    for log_path in log_paths:
        with open(log_path, encoding="utf-8", errors="replace") as log:
            for line in log:
                match = DIAGNOSTIC_RE.match(line)
                if match:
                    if match.group(4) in ("warning", "error"):
                        flush()
                        if not include_external and not is_project_path(match.group(1)):
                            skip_block = True
                            cid = None
                            continue
                        skip_block = False
                        location = normalize(match.group(1))
                        cid = f"{location}:{match.group(2)}:{match.group(3)}:{match.group(4)}:{match.group(5)}"
                        block.append(line)
                    elif cid is not None:
                        block.append(line)
                elif line.startswith("In file included from") or line.strip().startswith("from "):
                    pass
                elif skip_block:
                    continue
                elif NOISE_RE.match(line):
                    pass
                elif cid is not None:
                    block.append(line)
                elif line.strip() and line not in seen:
                    out.write(line)
                    seen.add(line)
    flush()
    return count
