"""Pinned Buildifier provisioning and configuration-isolated Starlark checks."""

from __future__ import annotations

import hashlib
import json
import os
import platform
import re
import subprocess
import sys
import tempfile
import urllib.request
from pathlib import Path
from typing import Any, TextIO, cast

from . import builddir
from .buildlock import shared_file_lock
from .paths import PROJECT_ROOT

MANIFEST_PATH = PROJECT_ROOT / "script/ao/buildifier.json"


class BuildifierError(RuntimeError):
    """A governed Buildifier setup, execution, or diagnostic-protocol failure."""


def _json_document(text: str) -> dict[str, object]:
    try:
        return cast(dict[str, object], json.loads(text))
    except json.JSONDecodeError as exc:
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


def _sha256(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def _validate_native(tool: Path, sha256: str, version: str) -> None:
    if _sha256(tool) != sha256:
        raise BuildifierError(f"managed Buildifier SHA-256 mismatch: {tool}; remove the damaged cache explicitly")
    _tool_version(tool, version)


def executable() -> Path:
    """Select the pinned Nix tool or lazily install a missing hashed native tool.

    Existing native cache entries are checked on every call, never repaired or
    replaced on a mismatch. This entry point does not discover any other SDK.
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
        builddir.windows_state_root()
        if host == "win32"
        else Path(os.environ.get("AOBUS_STATE_ROOT") or Path.home() / "Library/Caches/Aobus")
    )
    tool = (
        state / "tools/buildifier" / version / key / ("buildifier.exe" if host == "win32" else "buildifier")
    ).absolute()
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
                urllib.request.urlretrieve(url, staged)
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


def _check_output(text: str, requested: dict[Path, str], *, sink: TextIO | None = None) -> int:
    diagnostics: list[str] = []
    for record in cast(list[dict[str, Any]], _json_document(text)["files"]):
        logical = requested[_absolute_file(record["filename"])]
        if not record["valid"]:
            diagnostics.append(f"{logical}: Buildifier syntax error")
        elif not record["formatted"]:
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


def run(files: list[str], *, check: bool, lint: bool = False, sink: TextIO | None = None) -> int:
    """Format only, or check complete structured format/project-lint diagnostics.

    Empty selections never provision a tool. Relative inputs are checkout-relative;
    diagnostics retain their caller-visible spelling. Infrastructure/protocol
    failures raise BuildifierError; ordinary findings return 1. Diagnostics go to
    sink when provided, otherwise stderr; fix-mode stdout remains on stdout.
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
    base = [str(executable()), *options]
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
