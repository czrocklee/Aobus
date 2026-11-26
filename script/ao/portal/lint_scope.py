"""Select changed native sources; --extend owns whole-graph lint coverage."""

import hashlib
import re
import stat
import subprocess
from collections.abc import Iterator
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

from ao.core.paths import PROJECT_ROOT
from ao.policy.tidy_policy import LINT_SUFFIXES, classify_relative
from ao.portal import git_files


@dataclass(frozen=True)
class Plan:
    files: tuple[str, ...] | None
    reason: str


_SHA = r"(?:[0-9a-f]{40}|[0-9a-f]{64})"
_MODE = r"[0-7]{6}"
_RAW = re.compile(rf":({_MODE}) ({_MODE}) ({_SHA}) ({_SHA}) ([ADMT])")


def _git(*args: str, failure: str | None = None, stdin: bytes | None = None) -> bytes:
    argv = git_files.git_command("--no-replace-objects", *args, source_discovery=True)
    result = subprocess.run(argv, cwd=PROJECT_ROOT, input=stdin, capture_output=True)
    if result.returncode:
        detail = result.stderr.decode(errors="replace").strip().splitlines()
        raise ValueError(failure or (detail[0] if detail else f"git {args[0]} failed (exit {result.returncode})"))
    return result.stdout


def _commit(revision: str) -> str:
    reason = f"cannot verify {revision} in the checkout history"
    sha = _git("rev-parse", "--verify", f"{revision}^{{commit}}", failure=reason).decode().strip()
    if not re.fullmatch(_SHA, sha):
        raise ValueError(reason)
    return sha


def _verify_checkout() -> None:
    # A missing comparison commit is recoverable even in bound metadata. Only
    # reject the binding if Git cannot open the requested checkout itself.
    directory = git_files.discovery_git_directory()
    try:
        checkout = Path(_git("rev-parse", "--show-toplevel").decode().rstrip("\r\n"))
        if checkout.resolve() != PROJECT_ROOT.resolve():
            raise ValueError("not at the Git checkout root")
    except (OSError, ValueError) as exc:
        if directory:
            raise git_files.GitBindingError(f"cannot use AOBUS_GIT_DIR={directory!r}: {exc}") from exc
        raise


def _nul_fields(data: bytes) -> list[str]:
    if not data:
        return []
    *fields, end = data.decode().split("\0")
    if end:
        raise ValueError("truncated NUL-delimited Git output")
    return fields


def _lint_source(path: str) -> bool:
    return PurePosixPath(path).suffix in LINT_SUFFIXES and classify_relative(path, explicit=False) != "IGNORE"


def _matches_base_blob(path: str, oid: str, attrs: dict[str, str]) -> bool:
    """Compare the bytes Bazel reads, without refreshing the index or invoking external filters."""
    if "filter" in attrs:
        return False
    source = PROJECT_ROOT / path
    info = source.lstat()
    if not stat.S_ISREG(info.st_mode):
        return False
    if attrs.get("eol") != "lf" or "working-tree-encoding" in attrs or "ident" in attrs:
        # Compare checkout bytes after built-in conversions such as CRLF.
        return source.read_bytes() == _git("cat-file", "--filters", f"--path={path}", oid)
    # The repository's ordinary eol=lf files need no projection. A touched mirror should
    # not start a Git process for each file: hash the Git blob header and bytes.
    digest = hashlib.new(
        "sha1" if len(oid) == 40 else "sha256", f"blob {info.st_size}\0".encode(), usedforsecurity=False
    )
    with source.open("rb") as stream:
        return hashlib.file_digest(stream, lambda: digest).hexdigest() == oid


def _changed_sources(diff: bytes, untracked: bytes) -> Iterator[str]:
    records = iter(_nul_fields(diff))
    uncertain: dict[str, str] = {}
    # Read header/path pairs. With --no-renames a move includes a deletion.
    for header, path in zip(records, records, strict=True):
        change = _RAW.fullmatch(header)
        if change is None or not path:
            raise ValueError(f"unsupported Git change: {path!r}")
        old_mode, new_mode, old, new, status = change.groups()
        # Only the surviving regular-file side of a change can have a source report.
        if new_mode not in ("100644", "100755") or not _lint_source(path):
            continue
        # With index refresh disabled, a touch and a real edit can both appear
        # as M with an unknown (all-zero) worktree OID. Keep mode changes.
        if status == "M" and old_mode == new_mode and not new.strip("0"):
            uncertain[path] = old
        else:
            yield path
    if uncertain:
        names = b"".join(path.encode() + b"\0" for path in uncertain)
        fields = iter(_nul_fields(_git("check-attr", "--all", "-z", "--stdin", stdin=names)))
        attributes: dict[str, dict[str, str]] = {}
        for path, attr, value in zip(fields, fields, fields, strict=True):
            attributes.setdefault(path, {})[attr] = value
        for path, old in uncertain.items():
            if not _matches_base_blob(path, old, attributes.get(path, {})):
                yield path
    for path in _nul_fields(untracked):
        if _lint_source(path) and stat.S_ISREG((PROJECT_ROOT / path).lstat().st_mode):
            yield path


def plan(base: str | None) -> Plan:
    """Plan from clean commits (explicit base) or the final working tree (default base).

    Local lint reads the files as they are now, not the index as a separate
    snapshot. A staged edit subsequently undone on disk need not be linted.
    An unverifiable comparison requests full lint. Invalid Git bindings are
    configuration errors, not a reason to change the selection.
    """
    try:
        _verify_checkout()
        head = _commit("HEAD")
        if _git("config", "--type=bool", "--default=false", "core.sparseCheckout").strip() != b"false":
            raise ValueError("sparse checkout can omit build inputs")
        # Only ordinary tracked entries (H) establish a complete comparison:
        # skip-worktree, assume-unchanged and unmerged entries do not.
        if any(record and not record.startswith(b"H ") for record in _git("ls-files", "-v", "-z").split(b"\0")):
            raise ValueError("Git index contains hidden or unmerged entries")

        if base is None:
            revision = git_files.diff_base(ignore_replacements=True, require_local_main=True)
            revisions: tuple[str, ...] = (_commit(revision),)
            untracked = _git("ls-files", "--others", "--exclude-standard", "-z")
        else:
            if not re.fullmatch(_SHA, base) or _commit(base) != base:
                raise ValueError("base is not an exact commit SHA")
            _git("merge-base", "--is-ancestor", base, head, failure="cannot verify base as an ancestor of HEAD")
            if _git("status", "--porcelain=v1", "-z", "--untracked-files=all", "--ignore-submodules=none"):
                raise ValueError("checkout differs from HEAD")
            revisions = (base, head)
            untracked = b""
        diff = _git(
            "diff",
            "--raw",
            "--no-abbrev",
            "--no-renames",
            "--ignore-submodules=none",
            "-z",
            *revisions,
            "--",
        )
        files = tuple(sorted(set(_changed_sources(diff, untracked))))
        reason = "changed native sources" if files else "no changed native sources"
        if base is None:
            reason += f" from local base {revisions[0]}"
        return Plan(files, reason)
    except git_files.GitBindingError:
        raise
    except (OSError, ValueError) as exc:
        return Plan(None, str(exc))
