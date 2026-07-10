"""Repository layout anchors shared by every command."""

import os
from pathlib import Path


def absolute_path(path: str | Path, *, os_name: str | None = None) -> Path:
    """Return an absolute normalized path without resolving Windows filesystem links."""
    candidate = Path(path)
    if (os.name if os_name is None else os_name) == "nt":
        return Path(os.path.abspath(candidate))
    return candidate.resolve()


PROJECT_ROOT = absolute_path(__file__).parents[3]
