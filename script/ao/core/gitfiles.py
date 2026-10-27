"""Source file discovery: git change sets and folder scans."""

import functools
import os
import subprocess
import sys
from pathlib import Path

from .paths import PROJECT_ROOT, absolute_path
from .proc import die

CPP_TRANSLATION_UNIT_SUFFIXES = (".cpp", ".mm")
CPP_HEADER_SUFFIXES = (".h", ".hpp")
CPP_INCLUDE_FRAGMENT_SUFFIXES = (*CPP_HEADER_SUFFIXES, ".def")
CPP_SUFFIXES = (*CPP_TRANSLATION_UNIT_SUFFIXES, *CPP_INCLUDE_FRAGMENT_SUFFIXES)
PYTHON_SUFFIXES = (".py",)
SOURCE_SUFFIXES = (*CPP_SUFFIXES, *PYTHON_SUFFIXES)

# Lint checker fixtures contain deliberate violations; batch scans must never pick them up.
LINT_INTEGRATION_DIR = "test/lint"


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


def git_command(
    *args: str,
    os_name: str | None = None,
    platform_name: str | None = None,
    checkout_uses_smb: bool | None = None,
    source_discovery: bool = False,
) -> list[str]:
    """Build checkout-scoped argv; discovery alone opts into read-only bindings.

    Generic callers such as hook setup and perf provenance ignore discovery
    selectors, including malformed ones, and retain their own write semantics.
    """
    command = ["git"]
    if source_discovery:
        # git diff can refresh stat-only changes even when optional locks are
        # disabled. Source discovery must not rewrite a shared checkout's index.
        command += ["--no-optional-locks", "-c", "diff.autoRefreshIndex=false"]
        git_directory = os.environ.get("AOBUS_GIT_DIR")
        work_tree = os.environ.get("AOBUS_GIT_WORK_TREE")
        if git_directory is not None or work_tree is not None:
            if not git_directory or not work_tree:
                raise die("AOBUS_GIT_DIR and AOBUS_GIT_WORK_TREE must be set together")
            if not Path(git_directory).is_absolute() or not Path(work_tree).is_absolute():
                raise die("AOBUS_GIT_DIR and AOBUS_GIT_WORK_TREE must be absolute paths")
            # Bind discovery only: setup mutations and disposable repositories
            # must not inherit an alternate repository selection.
            if absolute_path(work_tree) == absolute_path(PROJECT_ROOT):
                command += ["--git-dir", git_directory, "--work-tree", str(PROJECT_ROOT)]
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


def _git_lines(*args: str, null_terminated: bool = False) -> list[str]:
    result = subprocess.run(
        git_command(*args, source_discovery=True),
        cwd=PROJECT_ROOT,
        capture_output=True,
        text=not null_terminated,
    )
    if result.returncode != 0:
        detail = os.fsdecode(result.stderr).strip() or f"exit {result.returncode}"
        work_tree = os.environ.get("AOBUS_GIT_WORK_TREE")
        if work_tree and absolute_path(work_tree) != absolute_path(PROJECT_ROOT):
            detail += (
                f"; ignored AOBUS_GIT_WORK_TREE={work_tree!r}: it does not match PROJECT_ROOT={str(PROJECT_ROOT)!r}"
            )
        raise die(f"git {' '.join(args)} failed: {detail}")
    if null_terminated:
        # Text-mode newline conversion would corrupt CR/CRLF inside POSIX paths.
        return [field for field in os.fsdecode(result.stdout).split("\0") if field]
    return [line for line in result.stdout.splitlines() if line.strip()]


def _git_ok(*args: str) -> bool:
    return (
        subprocess.run(
            git_command(*args, source_discovery=True),
            cwd=PROJECT_ROOT,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        ).returncode
        == 0
    )


def diff_base(commit: str | None = None) -> str:
    """Default to the topic's merge base with main; on main, use the previous commit."""
    if commit:
        return commit
    branches = _git_lines("branch", "--show-current")
    current = branches[0] if branches else ""
    if current != "main" and _git_ok("rev-parse", "--verify", "--quiet", "main"):
        bases = _git_lines("merge-base", "main", "HEAD")
        if not bases:
            raise die("git merge-base main HEAD returned no revision; select a base explicitly with --commit")
        return bases[0]
    return "HEAD~1"


def _git_worktree_files() -> list[str]:
    # Status compares content, including binary files, without persisting stat
    # refreshes when optional locks are disabled. It covers staged and untracked
    # paths too; porcelain -z preserves their raw names and ignores display config.
    records = iter(_git_lines("status", "--porcelain=v1", "-z", "--untracked-files=all", null_terminated=True))
    names = []
    for record in records:
        if len(record) < 4 or record[2] != " " or any(code not in " MTADRCU?!" for code in record[:2]):
            raise die(f"malformed git status record: {record!r}")
        name = record[3:]
        if "R" in record[:2] or "C" in record[:2]:
            # Porcelain -z puts the destination first, then the source path.
            if next(records, None) is None:
                raise die(f"malformed git status rename/copy record: {record!r}")
        names.append(name)
    return names


def changed_files(commit: str | None = None, *, suffixes: tuple[str, ...] = CPP_SUFFIXES) -> list[str]:
    """Repo-relative sources changed since the diff base, plus working tree, staged, untracked."""
    base = diff_base(commit)
    names: set[str] = set()
    names.update(_git_lines("diff", "--name-only", "-z", f"{base}..HEAD", null_terminated=True))
    names.update(_git_worktree_files())
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
