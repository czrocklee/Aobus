"""Bound production lint to known inputs of the final checkout snapshot."""

import hashlib
import io
import re
import stat
import subprocess
import tokenize
from collections.abc import Iterator
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

from ao.core.paths import PROJECT_ROOT
from ao.policy.tidy_policy import classify_relative
from ao.portal import bazel_lint, git_files


@dataclass(frozen=True)
class Plan:
    files: tuple[str, ...] | None
    reason: str


_SHA = r"(?:[0-9a-f]{40}|[0-9a-f]{64})"
_MODE = r"(?:000000|100644|100755)"
_RAW = re.compile(rf":({_MODE}) ({_MODE}) ({_SHA}) ({_SHA}) ([AMD])")
_NATIVE_DIRS = ("app/", "include/", "lib/", "test/")
_INPUT_DIRS = (*_NATIVE_DIRS, "asset/", "bazel/", "cmake/", "config/", "script/ao/", "tool/")
_LINT_DIR = f"{git_files.LINT_INTEGRATION_DIR}/"
# Keep code generation and lint inputs outside the path-only shortcuts.
_CONTROLS = ("app/i18n/", "app/include/ao/i18n/", _LINT_DIR, "test/script/", "test/data/")
# Product sources also compiled into //tool/catalog:ao_catalog_compiler.
_CATALOG_TOOL_INPUTS = {
    "include/ao/Contract.h",
    "include/ao/Error.h",
    "include/ao/utility/Path.h",
    "lib/utility/Path.cpp",
}


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


def _matches_base_blob(path: str, oid: str, attrs: dict[str, str]) -> bool:
    """Compare the bytes Bazel reads, without refreshing the index or invoking external filters."""
    if "filter" in attrs:
        return False
    source = PROJECT_ROOT / path
    info = source.lstat()
    if not stat.S_ISREG(info.st_mode):
        return False
    if attrs.get("eol") != "lf" or "working-tree-encoding" in attrs or "ident" in attrs:
        # Let Git project built-in conversions (e.g. .bat CRLF), not clean away real checkout edits.
        return source.read_bytes() == _git("cat-file", "--filters", f"--path={path}", oid)
    # The repository's ordinary eol=lf files need no projection. A touched mirror should
    # not start a Git process for each file: hash the Git blob header and bytes.
    digest = hashlib.new(
        "sha1" if len(oid) == 40 else "sha256", f"blob {info.st_size}\0".encode(), usedforsecurity=False
    )
    with source.open("rb") as stream:
        return hashlib.file_digest(stream, lambda: digest).hexdigest() == oid


def _changes(diff: bytes, untracked: bytes) -> Iterator[tuple[str, str, str, str]]:
    records = iter(_nul_fields(diff))
    uncertain: dict[str, str] = {}
    # Read header/path pairs. With --no-renames a move includes a deletion.
    for header, path in zip(records, records, strict=True):
        change = _RAW.fullmatch(header)
        if change is None or not path:
            raise ValueError(f"unsupported Git change: {path!r}")
        old_mode, new_mode, old, new, status = change.groups()
        # With index refresh disabled, a touch and a real edit can both appear
        # as M with an unknown (all-zero) worktree OID. Keep mode changes.
        if status == "M" and old_mode == new_mode and not new.strip("0") and not _ordinary(path):
            uncertain[path] = old
        else:
            yield old, new, status, path
    if uncertain:
        names = b"".join(path.encode() + b"\0" for path in uncertain)
        fields = iter(_nul_fields(_git("check-attr", "--all", "-z", "--stdin", stdin=names)))
        attributes: dict[str, dict[str, str]] = {}
        for path, attr, value in zip(fields, fields, fields, strict=True):
            attributes.setdefault(path, {})[attr] = value
        for path, old in uncertain.items():
            if not _matches_base_blob(path, old, attributes.get(path, {})):
                yield old, "", "M", path
    for path in _nul_fields(untracked):
        if not stat.S_ISREG((PROJECT_ROOT / path).lstat().st_mode):
            raise ValueError(f"untracked path is not a regular file: {path!r}")
        yield "", "", "A", path


def _build_tokens(blob: bytes) -> list[str]:
    """Ignore only trivia in the lexical subset shared by BUILD and Python."""
    text = blob.decode()
    # Preserve literal newlines inside strings; normalize only NEWLINE tokens below.
    if any(char in text for char in "\\\f\v") or "\r" in text.replace("\r\n", ""):
        raise ValueError("unsupported BUILD continuation or whitespace")
    tokens: list[str] = []
    for kind, word, *_ in tokenize.generate_tokens(io.StringIO(text).readline):
        if kind in (tokenize.COMMENT, tokenize.NL, tokenize.ENDMARKER):
            continue
        if kind == tokenize.NEWLINE:
            tokens.append("\n")  # unlike a newline inside a call, this ends the statement
        elif (kind in (tokenize.NAME, tokenize.NUMBER, tokenize.OP) and word.isascii()) or (
            kind == tokenize.STRING and word.startswith(("'", '"'))
        ):
            tokens.append(word)
        else:
            raise ValueError("unsupported BUILD token")
    return tokens


def _ordinary(path: str) -> bool:
    return (
        path in ("README.md", "AGENTS.md", "CONTRIBUTING.md", "LICENSE")
        or path in ("script/ao/command/format.py", "script/ao/command/measure.py")
        or (path.endswith(".md") and path.startswith(("doc/", ".agents/skills/", "app/", "tool/", "asset/")))
        or re.fullmatch(r"test/script/test_[^/]+\.py", path) is not None
        or (path.startswith(f"{_LINT_DIR}fixture/") and path.endswith((*bazel_lint.LINT_SUFFIXES, ".inc")))
    )


def plan(base: str | None) -> Plan:
    """Plan from clean commits (explicit base) or the final working tree (default base).

    Local lint reads the files as they are now, not the index as a separate
    snapshot. A staged edit subsequently undone on disk need not be linted.
    Unverifiable history and unbounded inputs request full lint. Invalid Git
    bindings are configuration errors, not a reason to change the selection.
    """
    try:
        _verify_checkout()
        head = _commit("HEAD")
        if _git("config", "--type=bool", "--default=false", "core.sparseCheckout").strip() != b"false":
            raise ValueError("sparse checkout can omit build inputs")
        # Skip-worktree and assume-unchanged can hide edits even outside a sparse checkout.
        if any(
            record.startswith(b"S ") or record[:1].islower() for record in _git("ls-files", "-v", "-z").split(b"\0")
        ):
            raise ValueError("index flags can hide build input changes")

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
        # Bazel globs do not honor .gitignore. Inspect authored input directories,
        # not repository-wide build/cache trees or ordinary Python bytecode.
        if _git(
            "ls-files",
            "--others",
            "--ignored",
            "--exclude-standard",
            "-z",
            "--",
            *_INPUT_DIRS,
            ":(exclude,glob)**/__pycache__/*.pyc",
        ):
            raise ValueError("ignored files in a build input directory")
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
        if base is not None and not diff:
            return Plan(None, "no changed paths between verified commits")
        files: set[str] = set()
        for old, new, status, path in _changes(diff, untracked):
            if _ordinary(path):
                continue
            if status == "M" and PurePosixPath(path).name in ("BUILD", "BUILD.bazel"):
                before = _git("cat-file", "blob", old)
                after = (PROJECT_ROOT / path).read_bytes() if base is None else _git("cat-file", "blob", new)
                if _build_tokens(before) != _build_tokens(after):
                    raise ValueError(f"semantic BUILD change: {path!r}")
                continue
            if (
                status != "D"
                and path.startswith(_NATIVE_DIRS)
                and not path.startswith(_CONTROLS)
                and path not in _CATALOG_TOOL_INPUTS
                # WinUI's projection action reads the paired headers, not their implementations.
                and not (path.startswith("app/windows-winui/") and path.endswith(".xaml.h"))
                and path.endswith(bazel_lint.LINT_SUFFIXES)
                and classify_relative(path, explicit=False) != "IGNORE"
                and (PROJECT_ROOT / path).is_file()
            ):
                files.add(path)
            else:
                raise ValueError(f"unbounded native lint input: {path!r}")
        reason = "verified native inputs" if files else "no production lint inputs"
        if base is None:
            reason += f" from local base {revisions[0]}"
        return Plan(tuple(sorted(files)), reason)
    except git_files.GitBindingError:
        raise
    except (OSError, ValueError, SyntaxError, tokenize.TokenError) as exc:
        return Plan(None, str(exc))
