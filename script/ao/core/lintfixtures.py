"""Supported fixture discovery shared by lint policy and native fixture execution."""

from dataclasses import dataclass
from pathlib import Path

from .paths import PROJECT_ROOT

FIXTURE_DIR = PROJECT_ROOT / "test/lint/fixture"


@dataclass(frozen=True)
class Fixture:
    path: Path
    check: str


def discover_fixtures(fixture_dir: Path = FIXTURE_DIR) -> list[Fixture]:
    paths = sorted(path for path in fixture_dir.glob("*/*") if path.is_file() and path.suffix in {".cpp", ".h", ".mm"})
    return [Fixture(path, path.parent.name) for path in paths]


def expected_aobus_checks(fixture_dir: Path | None = None) -> frozenset[str]:
    """Read the nonblank custom-check fixture inventory, including new fixtures."""
    root = FIXTURE_DIR if fixture_dir is None else fixture_dir
    return frozenset(
        fixture.check
        for fixture in discover_fixtures(root)
        if fixture.check.startswith("aobus-") and fixture.path.read_text(encoding="utf-8", errors="replace").strip()
    )
