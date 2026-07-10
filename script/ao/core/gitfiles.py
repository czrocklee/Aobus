"""Source file discovery: git change sets and folder scans."""

import os
import subprocess

from .paths import PROJECT_ROOT
from .proc import die

CPP_SUFFIXES = (".cpp", ".h", ".hpp")
PYTHON_SUFFIXES = (".py",)
SOURCE_SUFFIXES = (*CPP_SUFFIXES, *PYTHON_SUFFIXES)

# Lint checker fixtures contain deliberate violations; batch scans must never pick them up.
LINT_INTEGRATION_DIR = "test/integration/lint"


def _git_command(*args: str, os_name: str | None = None) -> list[str]:
    command = ["git"]
    if (os.name if os_name is None else os_name) == "nt":
        # Windows cannot preserve Unix executable bits on ordinary or mapped
        # worktrees. Keep both overrides process-local; no global Git config is
        # changed, and safe.directory is limited to this checkout.
        command += [
            "-c",
            f"safe.directory={PROJECT_ROOT.as_posix()}",
            "-c",
            "core.filemode=false",
        ]
    return [*command, *args]


def _git_lines(*args: str) -> list[str]:
    result = subprocess.run(
        _git_command(*args),
        cwd=PROJECT_ROOT,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        detail = result.stderr.strip() or f"exit {result.returncode}"
        raise die(f"git {' '.join(args)} failed: {detail}")
    return [line for line in result.stdout.splitlines() if line.strip()]


def _git_ok(*args: str) -> bool:
    return (
        subprocess.run(
            _git_command(*args),
            cwd=PROJECT_ROOT,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        ).returncode
        == 0
    )


def diff_base(commit: str | None = None) -> str:
    """Default to the local main branch; when already on main, use the previous commit."""
    if commit:
        return commit
    branches = _git_lines("branch", "--show-current")
    current = branches[0] if branches else ""
    if current != "main" and _git_ok("rev-parse", "--verify", "--quiet", "main"):
        return "main"
    return "HEAD~1"


def changed_files(commit: str | None = None, *, suffixes: tuple[str, ...] = CPP_SUFFIXES) -> list[str]:
    """Repo-relative sources changed since the diff base, plus working tree, staged, untracked."""
    base = diff_base(commit)
    names: set[str] = set()
    names.update(_git_lines("diff", "--name-only", f"{base}..HEAD"))
    names.update(_git_lines("diff", "--name-only"))
    names.update(_git_lines("diff", "--name-only", "--cached"))
    names.update(_git_lines("ls-files", "--others", "--exclude-standard"))
    return sorted(name for name in names if name.endswith(suffixes))


def find_sources(folders: list[str], *, suffixes: tuple[str, ...] = CPP_SUFFIXES) -> list[str]:
    """All sources under the given repo-relative folders, excluding lint integration files."""
    found: set[str] = set()
    for folder in folders:
        base = PROJECT_ROOT / folder
        if not base.exists():
            continue
        for path in base.rglob("*"):
            if path.suffix not in suffixes or not path.is_file():
                continue
            rel = path.relative_to(PROJECT_ROOT).as_posix()
            if rel.startswith(f"{LINT_INTEGRATION_DIR}/"):
                continue
            found.add(rel)
    return sorted(found)
