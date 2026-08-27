"""Source file discovery: git change sets and folder scans."""

import functools
import os
import subprocess
import sys
from pathlib import Path

from .paths import PROJECT_ROOT
from .proc import die

CPP_TRANSLATION_UNIT_SUFFIXES = (".cpp",)
CPP_HEADER_SUFFIXES = (".h", ".hpp")
CPP_INCLUDE_FRAGMENT_SUFFIXES = (*CPP_HEADER_SUFFIXES, ".def")
CPP_SUFFIXES = (*CPP_TRANSLATION_UNIT_SUFFIXES, *CPP_INCLUDE_FRAGMENT_SUFFIXES)
PYTHON_SUFFIXES = (".py",)
SOURCE_SUFFIXES = (*CPP_SUFFIXES, *PYTHON_SUFFIXES)

# Lint checker fixtures contain deliberate violations; batch scans must never pick them up.
LINT_INTEGRATION_DIR = "test/integration/lint"


def _path_is_on_smb_mount(path: Path, mount_output: str) -> bool:
    """Return whether a Darwin mount table places path on an smbfs mount."""
    resolved_path = path.resolve()
    for line in mount_output.splitlines():
        _source, separator, mount_description = line.rpartition(" on ")
        if not separator:
            continue
        mount_point_text, options_separator, options = mount_description.rpartition(" (")
        if not options_separator or options.removesuffix(")").split(",", 1)[0] != "smbfs":
            continue
        try:
            resolved_path.relative_to(Path(mount_point_text).resolve())
        except ValueError:
            continue
        return True
    return False


@functools.cache
def _darwin_checkout_uses_smb() -> bool:
    try:
        result = subprocess.run(
            ["/sbin/mount", "-t", "smbfs"],
            capture_output=True,
            text=True,
        )
    except OSError:
        return False
    return result.returncode == 0 and _path_is_on_smb_mount(PROJECT_ROOT, result.stdout)


def _git_command(
    *args: str,
    os_name: str | None = None,
    platform_name: str | None = None,
    checkout_uses_smb: bool | None = None,
) -> list[str]:
    command = ["git"]
    resolved_os_name = os.name if os_name is None else os_name
    resolved_platform_name = sys.platform if platform_name is None else platform_name
    if resolved_os_name == "nt":
        # Windows cannot preserve Unix executable bits on ordinary or mapped
        # worktrees. Keep both overrides process-local; no global Git config is
        # changed, and safe.directory is limited to this checkout.
        command += [
            "-c",
            f"safe.directory={PROJECT_ROOT.as_posix()}",
            "-c",
            "core.filemode=false",
        ]
    elif resolved_platform_name == "darwin" and (
        _darwin_checkout_uses_smb() if checkout_uses_smb is None else checkout_uses_smb
    ):
        # SMB presents ordinary files as executable on macOS. Ignore that
        # transport artifact without hiding meaningful mode changes on APFS.
        command += ["-c", "core.filemode=false"]
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
