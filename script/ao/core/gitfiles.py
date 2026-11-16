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
BAZEL_SUFFIXES = (".bzl", ".bazel")
PRETTIER_SUFFIXES = (".md", ".json", ".yaml", ".yml", ".css")
SOURCE_SUFFIXES = (*CPP_SUFFIXES, *PYTHON_SUFFIXES, *BAZEL_SUFFIXES, *PRETTIER_SUFFIXES)

# Lint checker fixtures contain deliberate violations; batch scans must never pick them up.
LINT_INTEGRATION_DIR = "test/lint"
WORKFLOW_DIR = ".github/workflows"
# Git reports this when the checkout has no repository at all; text discovery then
# falls back to a bounded filesystem scan instead of failing the command.
MISSING_REPOSITORY_DIAGNOSTIC = "not a git repository"


def is_bazel_file(name: str) -> bool:
    """Return whether a path names a Buildifier-supported source file."""
    return name.endswith(BAZEL_SUFFIXES) or Path(name).name in ("BUILD", "WORKSPACE")


def is_prettier_file(name: str) -> bool:
    """Return whether the repository formats this authored text family with Prettier."""
    return name.endswith(PRETTIER_SUFFIXES)


def is_bazel_rc_file(name: str) -> bool:
    """The root rc contains a generated MSVC policy view, not Starlark."""
    return Path(os.path.abspath(PROJECT_ROOT / name)) == PROJECT_ROOT / ".bazelrc"


def is_workflow_file(name: str) -> bool:
    """GitHub Actions loads YAML files directly inside .github/workflows only."""
    path = Path(os.path.abspath(PROJECT_ROOT / name))
    try:
        relative = path.relative_to(PROJECT_ROOT)
    except ValueError:
        return False
    return relative.parent == Path(WORKFLOW_DIR) and relative.suffix in (".yml", ".yaml")


def root_bazel_files() -> list[str]:
    """Bazel sources and the generated root rc view, without recursive scans."""
    return sorted(
        path.name
        for path in PROJECT_ROOT.iterdir()
        if (is_bazel_file(path.name) or is_bazel_rc_file(path.name)) and path.is_file()
    )


def matches_source(name: str, suffixes: tuple[str, ...]) -> bool:
    # Named Bazel files and workflows belong to whole-source hygiene only.
    return name.endswith(suffixes) or (
        suffixes == SOURCE_SUFFIXES and (is_bazel_file(name) or is_bazel_rc_file(name) or is_workflow_file(name))
    )


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
            if _binds_this_checkout(work_tree):
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


def _git_output(stdout: str | bytes, null_terminated: bool) -> list[str]:
    if null_terminated:
        # Text-mode newline conversion would corrupt CR/CRLF inside POSIX paths.
        return [field for field in os.fsdecode(stdout).split("\0") if field]
    return [line for line in os.fsdecode(stdout).splitlines() if line.strip()]


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
    return _git_output(result.stdout, null_terminated)


def _git_lines_if_repository(*args: str, null_terminated: bool = False) -> list[str] | None:
    """Run one discovery command, or None when this checkout has no repository.

    Only two states select the caller's filesystem fallback: no Git executable
    and an unversioned checkout, and only while no effective ``AOBUS_GIT_*``
    binding selects this checkout. Malformed bindings, an effective binding
    that cannot be honored, and every other Git or process failure stay fatal,
    so a broken selection is never mistaken for a plain exported tree.
    """
    try:
        result = subprocess.run(
            git_command(*args, source_discovery=True),
            cwd=PROJECT_ROOT,
            capture_output=True,
            text=not null_terminated,
        )
    except FileNotFoundError as exc:
        # Only a missing executable is an unavailable tool, and even then an
        # effective binding cannot be honored by a filesystem fallback.
        if _explicit_git_binding():
            raise die(f"git {' '.join(args)} failed: {exc}") from exc
        return None
    except OSError as exc:
        raise die(f"git {' '.join(args)} failed: {exc}") from exc
    if result.returncode != 0:
        detail = os.fsdecode(result.stderr).strip() or f"exit {result.returncode}"
        if MISSING_REPOSITORY_DIAGNOSTIC in detail and not _explicit_git_binding():
            return None
        raise die(f"git {' '.join(args)} failed: {detail}")
    return _git_output(result.stdout, null_terminated)


def _binds_this_checkout(work_tree: str) -> bool:
    """A discovery binding applies only to the checkout it names."""
    return absolute_path(work_tree) == absolute_path(PROJECT_ROOT)


def _explicit_git_binding() -> bool:
    """An effective binding selects this checkout and must fail loudly, never fall back.

    A well-formed binding for another checkout is ignored by ``git_command``, so
    it cannot make Git unavailable here and keeps the filesystem fallback.
    """
    git_directory = os.environ.get("AOBUS_GIT_DIR")
    work_tree = os.environ.get("AOBUS_GIT_WORK_TREE")
    return bool(git_directory and work_tree and _binds_this_checkout(work_tree))


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
        if "R" in record[:2] or "C" in record[:2]:
            # Porcelain -z puts the destination first, then the source path.
            next(records)
        names.append(record[3:])
    return names


def changed_paths(commit: str | None = None) -> list[str]:
    """Repo-relative names changed since the diff base, plus working tree, staged, untracked.

    Unfiltered: callers apply ``matches_source`` or their own selection, so a
    formatting-control edit stays visible before any suffix filter drops it.
    """
    base = diff_base(commit)
    names: set[str] = set()
    names.update(_git_lines("diff", "--name-only", "-z", f"{base}..HEAD", null_terminated=True))
    names.update(_git_worktree_files())
    return sorted(names)


def changed_files(commit: str | None = None, *, suffixes: tuple[str, ...] = CPP_SUFFIXES) -> list[str]:
    """Repo-relative sources changed since the diff base, plus working tree, staged, untracked."""
    return [name for name in changed_paths(commit) if matches_source(name, suffixes)]


def find_sources(folders: list[str], *, suffixes: tuple[str, ...] = CPP_SUFFIXES) -> list[str]:
    """Find scoped sources, retaining Bazel graphs but excluding native/Python lint fixtures."""
    found: set[str] = set()
    for folder in folders:
        base = PROJECT_ROOT / folder
        if not base.exists():
            continue
        for path in base.rglob("*"):
            if not path.is_file():
                continue
            rel = path.relative_to(PROJECT_ROOT).as_posix()
            if not matches_source(rel, suffixes):
                continue
            if rel.startswith(f"{LINT_INTEGRATION_DIR}/") and not is_bazel_file(rel):
                continue
            found.add(rel)
    return sorted(found)


def prettier_sources(folders: list[str], *, include_root: bool = False) -> list[str]:
    """Authored Prettier-family sources, enumerated through Git when possible.

    Tracked files plus non-ignored untracked files are the authored scope, so
    ignored local artifacts (a root ``compile_commands.json`` link, ``doc/plan``)
    never enter text formatting. Tracked or unignored links that escape the
    checkout still reach Prettier's own containment error. A checkout Git cannot
    enumerate falls back to a bounded filesystem scan that drops the escaping
    links it discovers, keeping ``--all`` and ``--folder`` usable without a
    repository.
    """
    names = _git_lines_if_repository(
        "ls-files", "-z", "--cached", "--others", "--exclude-standard", null_terminated=True
    )
    if names is None:
        return _prettier_sources_from_filesystem(folders, include_root=include_root)
    return sorted(
        name for name in names if _prettier_in_scope(name, folders, include_root) and (PROJECT_ROOT / name).is_file()
    )


def _prettier_in_scope(name: str, folders: list[str], include_root: bool) -> bool:
    if not is_prettier_file(name) or name.startswith(f"{LINT_INTEGRATION_DIR}/"):
        return False
    if include_root and Path(name).parent == Path("."):
        return True
    path = Path(name)
    for folder in folders:
        # Folder arguments may be absolute or carry "."/".." segments. Compare
        # the logical checkout-relative selection through Path so containment
        # keeps its platform case semantics instead of matching raw text.
        try:
            selected = Path(os.path.normpath(PROJECT_ROOT / folder)).relative_to(PROJECT_ROOT)
        except ValueError:
            continue
        if path.is_relative_to(selected):
            return True
    return False


def _prettier_sources_from_filesystem(folders: list[str], *, include_root: bool) -> list[str]:
    """Bounded fallback scan; ignored artifacts stay only where Prettier skips them."""
    found = find_sources(folders, suffixes=PRETTIER_SUFFIXES)
    if include_root:
        found.extend(path.name for path in PROJECT_ROOT.iterdir() if path.is_file() and is_prettier_file(path.name))
    return sorted({name for name in found if not _escapes_checkout(name)})


def _escapes_checkout(name: str) -> bool:
    """Mirror Prettier's containment rule for links a filesystem scan discovers."""
    checkout = Path(os.path.realpath(PROJECT_ROOT))
    return not Path(os.path.realpath(PROJECT_ROOT / name)).is_relative_to(checkout)
