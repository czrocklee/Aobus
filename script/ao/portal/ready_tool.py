"""A host command and the provider inputs that admitted it."""

from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class ReadyTool:
    command: tuple[str, ...]
    files: tuple[Path, ...]
    directories: tuple[Path, ...] = ()
    trees: tuple[Path, ...] = ()


class ToolMissing(Exception):
    """A missing provider input that explicit setup can create."""

    def __init__(self, message: str, *files: Path):
        super().__init__(message)
        self.files = files
