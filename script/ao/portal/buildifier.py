"""Pinned Buildifier provisioning and configuration-isolated Starlark checks."""

from __future__ import annotations

import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.request
from pathlib import Path
from typing import Any, TextIO, cast

from ao.core.build_lock import shared_file_lock
from ao.core.paths import PROJECT_ROOT
from ao.portal import build_dir
from ao.portal.ready_tool import ReadyTool, ToolMissing

MANIFEST_PATH = PROJECT_ROOT / "script/ao/buildifier.json"
# A bounded per-socket-operation timeout; urlretrieve has none.
_DOWNLOAD_TIMEOUT_SECONDS = 60


class BuildifierError(RuntimeError):
    """A governed Buildifier setup, execution, or diagnostic-protocol failure."""


class BuildifierMissing(ToolMissing, BuildifierError):
    """The managed Buildifier artifact has not been set up."""


def ready_tool() -> ReadyTool:
    tool = executable(provision=False)
    return ReadyTool((str(tool),), (MANIFEST_PATH, tool))


def _json_document(text: str) -> dict[str, object]:
    def unique_fields(pairs: list[tuple[str, object]]) -> dict[str, object]:
        result: dict[str, object] = {}
        for key, value in pairs:
            if key in result:
                raise ValueError(f"duplicate JSON field: {key}")
            result[key] = value
        return result

    try:
        document = json.loads(text, object_pairs_hook=unique_fields)
        if not isinstance(document, dict):
            raise ValueError("expected a JSON object")
        return cast(dict[str, object], document)
    except ValueError as exc:
        raise BuildifierError(f"invalid Buildifier JSON: {exc}") from exc


def _manifest() -> tuple[str, dict[str, object]]:
    try:
        document = _json_document(MANIFEST_PATH.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, BuildifierError) as exc:
        raise BuildifierError(f"cannot read Buildifier manifest {MANIFEST_PATH}: {exc}") from exc
    return cast(str, document["version"]), cast(dict[str, object], document["artifacts"])


def _artifact_key(host: str) -> str:
    machine = platform.machine().lower()
    architecture = {"amd64": "x86_64", "x86_64": "x86_64", "aarch64": "arm64", "arm64": "arm64"}.get(machine)
    if architecture is None or (host == "win32" and architecture != "x86_64"):
        raise BuildifierError(f"no pinned Buildifier artifact for {host}-{machine}")
    return f"{'windows' if host == 'win32' else 'darwin'}-{architecture}"


def _environment() -> dict[str, str]:
    # -config=off also disables workspace config; remove the ambient selector
    # rather than letting it influence even version/protocol probes.
    return {key: value for key, value in os.environ.items() if key.upper() != "BUILDIFIER_CONFIG"}


def _invoke(argv: list[str]) -> subprocess.CompletedProcess[str]:
    try:
        return subprocess.run(
            argv,
            cwd=PROJECT_ROOT,
            env=_environment(),
            stdin=subprocess.DEVNULL,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="strict",
            check=False,
        )
    except (OSError, UnicodeError) as exc:
        raise BuildifierError(f"cannot execute Buildifier {argv[0]}: {exc}") from exc


def _tool_version(tool: Path, version: str) -> None:
    result = _invoke([str(tool), "-version", "-config=off"])
    pattern = (
        rf"buildifier version: {re.escape(version)}[ \t]*"
        r"(?:\r?\nbuildifier scm revision: [A-Za-z0-9][A-Za-z0-9._+-]*[ \t]*)?\r?\n?"
    )
    if result.returncode or result.stderr or re.fullmatch(pattern, result.stdout) is None:
        raise BuildifierError(f"{tool} must report buildifier version: {version} with a successful -version probe")


def _download(url: str, destination: Path) -> None:
    """Stream a pinned artifact to a staged file with a bounded socket timeout."""
    with (
        urllib.request.urlopen(url, timeout=_DOWNLOAD_TIMEOUT_SECONDS) as source,
        destination.open("wb") as target,
    ):
        shutil.copyfileobj(source, target)


def _sha256(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def _validate_native(tool: Path, sha256: str, version: str) -> None:
    if _sha256(tool) != sha256:
        raise BuildifierError(f"managed Buildifier SHA-256 mismatch: {tool}; remove the damaged cache explicitly")
    _tool_version(tool, version)


def executable(*, provision: bool = True) -> Path:
    """Select the pinned Nix tool or install a missing hashed native tool.

    Existing native cache entries are checked on every call, never repaired or
    replaced on a mismatch. With ``provision=False`` a missing native cache is
    an actionable error and no download, directory, or lock is created; an
    existing cache is still verified. This entry point does not discover any
    other SDK.
    """
    version, artifacts = _manifest()
    host = sys.platform
    if host == "linux":
        selected = os.environ.get("AO_NIX_BUILDIFIER", "")
        tool = Path(selected)
        try:
            available = (
                bool(selected)
                and tool.is_absolute()
                and tool.is_relative_to("/nix/store")
                and tool.resolve().is_relative_to("/nix/store")
                and tool.is_file()
                and os.access(tool, os.X_OK)
            )
        except (OSError, ValueError, RuntimeError) as exc:
            raise BuildifierError(f"cannot inspect AO_NIX_BUILDIFIER selection {selected!r}: {exc}") from exc
        if not available:
            raise BuildifierError(
                "Buildifier is unavailable in AO_NIX_BUILDIFIER from the pinned Nix shell; re-enter ./ao"
            )
        _tool_version(tool, version)
        return tool
    if host not in ("darwin", "win32"):
        raise BuildifierError(f"Buildifier does not support platform {host!r}")
    key = _artifact_key(host)
    artifact = artifacts.get(key)
    if not isinstance(artifact, dict):
        raise BuildifierError(f"no pinned Buildifier artifact for {key}")
    url, sha256 = cast(str, artifact["url"]), cast(str, artifact["sha256"])
    state = (
        build_dir.windows_state_root()
        if host == "win32"
        else Path(os.environ.get("AOBUS_STATE_ROOT") or Path.home() / "Library/Caches/Aobus")
    )
    tool = (
        state / "tools/buildifier" / version / key / ("buildifier.exe" if host == "win32" else "buildifier")
    ).absolute()
    if not provision:
        if not os.path.lexists(tool):
            raise BuildifierMissing(
                f"Buildifier is not ready at {tool}; run ao setup buildifier explicitly", MANIFEST_PATH, tool
            )
        _validate_native(tool, sha256, version)
        return tool
    try:
        with shared_file_lock(tool.parent / ".setup.lock"):
            if os.path.lexists(tool):
                _validate_native(tool, sha256, version)
                return tool
            descriptor, name = tempfile.mkstemp(
                prefix=".buildifier-stage-", suffix=".exe" if host == "win32" else "", dir=tool.parent
            )
            os.close(descriptor)
            staged = Path(name)
            try:
                _download(url, staged)
                if _sha256(staged) != sha256:
                    raise BuildifierError("downloaded Buildifier SHA-256 does not match buildifier.json")
                staged.chmod(0o755)
                _validate_native(staged, sha256, version)
                os.replace(staged, tool)
            finally:
                staged.unlink(missing_ok=True)
    except OSError as exc:
        raise BuildifierError(f"cannot prepare managed Buildifier {tool}: {exc}") from exc
    return tool


def _absolute_file(filename: str) -> Path:
    path = Path(filename)
    # Do not resolve symlinks: Buildifier must enforce its own workspace write
    # safety. Path uses native separators; a POSIX backslash is not a separator.
    return (path if path.is_absolute() else PROJECT_ROOT / path).absolute()


def _check_output(
    text: str, requested: dict[Path, str], *, sink: TextIO | None = None, verify_format: bool = True
) -> int:
    diagnostics: list[str] = []
    for record in cast(list[dict[str, Any]], _json_document(text)["files"]):
        logical = requested[_absolute_file(record["filename"])]
        if not record["valid"]:
            diagnostics.append(f"{logical}: Buildifier syntax error")
        elif verify_format and not record["formatted"]:
            diagnostics.append(f"{logical}: needs Buildifier formatting")
        for warning in record["warnings"]:
            start = warning["start"]
            diagnostics.append(
                f"{logical}:{start['line']}:{start['column']}: "
                f"{warning['category']}: {warning['message']} ({warning['url']})"
            )
    for diagnostic in diagnostics:
        print(diagnostic, file=sys.stderr if sink is None else sink)
    return int(bool(diagnostics))


def _lint_output(result: subprocess.CompletedProcess[str], requested: dict[Path, str], *, sink: TextIO | None) -> int:
    """Admit a complete official JSON response before reporting any findings."""
    try:
        document = _json_document(result.stdout)
        if not isinstance(document, dict) or type(document.get("success")) is not bool:
            raise ValueError("missing boolean success")
        records = document.get("files")
        if not isinstance(records, list):
            raise ValueError("missing files array")
        seen: set[Path] = set()
        valid = True
        formatted = True
        warnings_found = False
        for record in records:
            if not isinstance(record, dict) or not isinstance(record.get("filename"), str):
                raise ValueError("invalid file record")
            filename = record["filename"]
            if not filename or "\0" in filename:
                raise ValueError("invalid filename")
            path = _absolute_file(filename)
            if path not in requested or path in seen:
                raise ValueError("foreign or duplicate file record")
            seen.add(path)
            if type(record.get("valid")) is not bool or type(record.get("formatted")) is not bool:
                raise ValueError("missing boolean file status")
            valid = valid and record["valid"]
            formatted = formatted and record["formatted"]
            warnings = record.get("warnings")
            if not isinstance(warnings, list):
                raise ValueError("missing warnings array")
            warnings_found = warnings_found or bool(warnings)
            for warning in warnings:
                if not isinstance(warning, dict):
                    raise ValueError("invalid warning record")
                for key in ("category", "message", "url"):
                    if not isinstance(warning.get(key), str) or not warning[key]:
                        raise ValueError(f"invalid warning {key}")
                for key in ("start", "end"):
                    position = warning.get(key)
                    if not isinstance(position, dict) or any(
                        type(position.get(axis)) is not int or position[axis] < 1 for axis in ("line", "column")
                    ):
                        raise ValueError("invalid warning position")
        if seen != set(requested):
            raise ValueError("incomplete file records")
        if document["success"] != (valid and formatted and not warnings_found):
            raise ValueError("inconsistent success status")
        # JSON mode in the pinned tool reports findings with exit 0; admit the
        # documented syntax/check exits only when their matching evidence exists.
        if result.returncode not in (0, 1, 4):
            raise ValueError(f"tool exited with status {result.returncode}")
        if result.returncode == 1 and valid:
            raise ValueError("syntax exit without syntax diagnostics")
        if result.returncode == 4 and valid and formatted and not warnings_found:
            raise ValueError("check exit without findings")
        if result.stderr and valid:
            raise ValueError(f"unexpected stderr: {result.stderr.strip()}")
    except (KeyError, TypeError, ValueError) as exc:
        raise BuildifierError(f"invalid Buildifier lint response: {exc}") from exc
    if result.stderr:
        output = sys.stderr if sink is None else sink
        print(result.stderr, end="" if result.stderr.endswith("\n") else "\n", file=output)
    return _check_output(result.stdout, requested, sink=sink, verify_format=False)


def lint_paths(
    files: list[str], *, sink: TextIO | None = None, provision: bool = False, tool: str | None = None
) -> int:
    """Return semantic findings only (0/1); tool/protocol failures raise.

    The official read-only JSON check mode also calculates formatting metadata;
    neither that metadata nor its format-only exit is a semantic finding.
    """
    return _run(files, check=True, lint=True, sink=sink, provision=provision, semantic_only=True, tool=tool)


def run(
    files: list[str],
    *,
    check: bool,
    lint: bool = False,
    sink: TextIO | None = None,
    provision: bool = True,
    tool: str | None = None,
) -> int:
    """Run the existing format/check operation, including formatting diagnostics."""
    return _run(files, check=check, lint=lint, sink=sink, provision=provision, tool=tool)


def _run(
    files: list[str],
    *,
    check: bool,
    lint: bool = False,
    sink: TextIO | None = None,
    provision: bool = True,
    semantic_only: bool = False,
    tool: str | None = None,
) -> int:
    """Format only, or check complete structured format/project-lint diagnostics.

    Empty selections never provision a tool. Relative inputs are checkout-relative;
    diagnostics retain their caller-visible spelling. Infrastructure/protocol
    failures raise BuildifierError; ordinary findings return 1. Diagnostics go to
    sink when provided, otherwise stderr; fix-mode stdout remains on stdout.
    With ``provision=False`` a missing pinned tool fails before any download or
    state creation.
    """
    if not files:
        return 0
    if lint and not check:
        raise BuildifierError("Buildifier lint requires check mode; automatic lint fixes are not supported")
    requested: dict[Path, str] = {}
    for filename in files:
        if not filename or "\0" in filename:
            raise BuildifierError("Buildifier requires non-empty file paths without NUL characters")
        path = _absolute_file(filename)
        if path in requested:
            raise BuildifierError(f"duplicate requested Buildifier file: {filename}")
        requested[path] = filename
    options = (
        ["-mode=check", f"-lint={'warn' if lint else 'off'}", "-format=json", "-config=off"]
        if check
        else ["-mode=fix", "-lint=off", "-config=off"]
    )
    if lint:
        # Repository graph factories own fixed labels, not per-instance names.
        # Keep all other upstream default warnings; never filter returned records.
        options.append("-warnings=-unnamed-macro")
    base = [tool if tool is not None else str(executable(provision=provision)), *options]
    batches: list[dict[Path, str]] = []
    batch: dict[Path, str] = {}
    for path, logical in requested.items():
        argv = [*base, *(str(item) for item in batch), str(path)]
        # Bound both argument count and Windows' UTF-16 command-line length.
        length = len(subprocess.list2cmdline(argv).encode("utf-16-le")) // 2
        if batch and (len(batch) >= 100 or length > 24000):
            batches.append(batch)
            batch = {}
        if len(subprocess.list2cmdline([*base, str(path)]).encode("utf-16-le")) // 2 > 24000:
            raise BuildifierError(f"Buildifier file path exceeds the command-line limit: {logical}")
        batch[path] = logical
    batches.append(batch)
    status = 0
    output = sys.stderr if sink is None else sink
    for batch in batches:
        result = _invoke([*base, *(str(path) for path in batch)])
        if semantic_only:
            status = max(status, _lint_output(result, batch, sink=sink))
            continue
        if result.stderr:
            print(result.stderr, end="" if result.stderr.endswith("\n") else "\n", file=output)
            status = 1
        if check:
            status |= _check_output(result.stdout, batch, sink=sink)
        elif result.stdout:
            print(result.stdout, end="" if result.stdout.endswith("\n") else "\n")
        if result.returncode:
            print(f"Buildifier exited with status {result.returncode}", file=output)
            status = 1
    return status
