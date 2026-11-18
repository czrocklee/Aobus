"""Pinned Prettier provisioning and configuration-isolated text formatting."""

from __future__ import annotations

import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
import zipfile
from collections.abc import Sequence
from pathlib import Path, PurePosixPath
from typing import cast

from ao.core.build_lock import shared_file_lock
from ao.core.paths import PROJECT_ROOT
from ao.portal import build_dir

MANIFEST_PATH = PROJECT_ROOT / "script/ao/prettier.json"
# A bounded per-socket-operation timeout; the transfer itself is not bounded.
_DOWNLOAD_TIMEOUT_SECONDS = 60
_RECEIPT_FILE = ".aobus-prettier-receipt.json"
_SHA256 = re.compile(r"[0-9a-f]{64}")
# The same Windows command-line budget Buildifier batches against.
_MAXIMUM_BATCH_FILES = 100
_MAXIMUM_COMMAND_UNITS = 24000
# Node would honor ambient NODE_OPTIONS/NODE_PATH selectors; the pinned
# runtime, not the caller's shell, defines the child behavior.
_NODE_ENVIRONMENT = ("NODE_OPTIONS", "NODE_PATH")
# Child diagnostics are collapsed into one bounded line in error messages.
_PROBE_DIAGNOSTIC_LIMIT = 400


class PrettierError(RuntimeError):
    """A governed Prettier setup, execution, or diagnostic-protocol failure."""


def _json_document(text: str) -> dict[str, object]:
    try:
        document = json.loads(text)
    except json.JSONDecodeError as exc:
        raise PrettierError(f"invalid Prettier JSON: {exc}") from exc
    if not isinstance(document, dict):
        raise PrettierError("invalid Prettier JSON: expected an object")
    return cast(dict[str, object], document)


def _digest(value: object, label: str) -> str:
    if not isinstance(value, str) or _SHA256.fullmatch(value) is None:
        raise PrettierError(f"invalid Prettier manifest {MANIFEST_PATH}: {label} must be a lowercase SHA-256 digest")
    return value


def _pinned_archive(record: object, label: str) -> tuple[str, str]:
    """Return the url and archive digest of a pinned download."""
    if not isinstance(record, dict):
        raise PrettierError(f"invalid Prettier manifest {MANIFEST_PATH}: {label} must be an object")
    url = record.get("url")
    if not isinstance(url, str) or not url:
        raise PrettierError(f"invalid Prettier manifest {MANIFEST_PATH}: {label} needs a url")
    return url, _digest(record.get("sha256"), f"{label} sha256")


def _manifest() -> tuple[str, str, str, str, str, dict[str, tuple[str, str, str]]]:
    """Return the pinned versions plus archive and decoded-content digests.

    The manifest pins both every downloadable archive and the decoded content
    (Node.js executable and extracted Prettier package tree), so a cache is
    verified against independent expectations, never against its own receipt.
    """
    try:
        document = _json_document(MANIFEST_PATH.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, PrettierError) as exc:
        raise PrettierError(f"cannot read Prettier manifest {MANIFEST_PATH}: {exc}") from exc
    version, package, node = document.get("version"), document.get("package"), document.get("node")
    if not isinstance(version, str) or not version or not isinstance(package, dict) or not isinstance(node, dict):
        raise PrettierError(f"invalid Prettier manifest {MANIFEST_PATH}: version, package, and node are required")
    node_version, artifacts = node.get("version"), node.get("artifacts")
    if not isinstance(node_version, str) or not node_version or not isinstance(artifacts, dict) or not artifacts:
        raise PrettierError(f"invalid Prettier manifest {MANIFEST_PATH}: node needs a version and artifacts")
    pinned: dict[str, tuple[str, str, str]] = {}
    for key, record in artifacts.items():
        if not isinstance(key, str):
            raise PrettierError(f"invalid Prettier manifest {MANIFEST_PATH}: artifact keys must be strings")
        label = f"node artifact {key}"
        url, archive = _pinned_archive(record, label)
        pinned[key] = (url, archive, _digest(record.get("nodeExecutableSha256"), f"{label} nodeExecutableSha256"))
    package_url, package_sha256 = _pinned_archive(package, "package")
    package_tree = _digest(document.get("packageTreeSha256"), "packageTreeSha256")
    return version, package_url, package_sha256, package_tree, node_version, pinned


def _platform_key(host: str) -> str:
    if host not in ("linux", "darwin", "win32"):
        raise PrettierError(f"Prettier does not support platform {host!r}")
    machine = platform.machine().lower()
    architecture = {"amd64": "x86_64", "x86_64": "x86_64", "aarch64": "arm64", "arm64": "arm64"}.get(machine)
    if architecture is None or (host == "win32" and architecture != "x86_64"):
        raise PrettierError(f"no pinned Node.js artifact for {host}-{machine}")
    return f"{'windows' if host == 'win32' else host}-{architecture}"


def _environment() -> dict[str, str]:
    return {key: value for key, value in os.environ.items() if key.upper() not in _NODE_ENVIRONMENT}


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
        raise PrettierError(f"cannot execute Prettier {argv[0]}: {exc}") from exc


def _diagnostic(text: str) -> str:
    """Collapse a child diagnostic into one bounded line for error messages."""
    collapsed = " ".join(text.split())
    if len(collapsed) > _PROBE_DIAGNOSTIC_LIMIT:
        collapsed = collapsed[: _PROBE_DIAGNOSTIC_LIMIT - 3] + "..."
    return collapsed


def _probe(command: Sequence[str], version: str) -> None:
    """Require exactly the pinned version banner from a --version probe."""
    result = _invoke([*command, "--version"])
    pattern = rf"{re.escape(version)}[ \t]*\r?\n?"
    if result.returncode or result.stderr or re.fullmatch(pattern, result.stdout) is None:
        details = [f"exit status {result.returncode}"]
        if result.stderr:
            details.append(f"stderr: {_diagnostic(result.stderr)}")
        if result.stdout and re.fullmatch(pattern, result.stdout) is None:
            details.append(f"stdout: {_diagnostic(result.stdout)}")
        raise PrettierError(
            f"{command[0]} must report Prettier version {version} with a successful --version probe"
            f" ({'; '.join(details)})"
        )


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


def _extract_node(archive: Path, destination: Path, *, windows: bool) -> None:
    """Copy only the pinned Node.js executable out of its distribution archive."""
    if windows:
        # The official distribution carries node.exe inside one top-level
        # directory; a flat archive is admitted too, but never an ambiguous,
        # nested, linked, or escaping candidate.
        with zipfile.ZipFile(archive) as bundle:
            candidates: list[zipfile.ZipInfo] = []
            for info in bundle.infolist():
                name = PurePosixPath(info.filename)
                if name.parts[-1:] != ("node.exe",):
                    continue
                if name.is_absolute() or ".." in name.parts or "\\" in info.filename:
                    raise PrettierError(f"unsafe Node.js archive member: {info.filename}")
                if info.is_dir() or (info.external_attr >> 16) & 0o170000 == 0o120000:
                    continue
                if len(name.parts) > 2:
                    continue
                candidates.append(info)
            if len(candidates) != 1:
                raise PrettierError("the Node.js archive must contain exactly one node.exe executable")
            with bundle.open(candidates[0]) as source, destination.open("wb") as target:
                shutil.copyfileobj(source, target)
        return
    with tarfile.open(archive, "r:*") as bundle:
        members = [member for member in bundle.getmembers() if member.name.endswith("/bin/node")]
        if len(members) != 1 or not members[0].isfile():
            raise PrettierError("the Node.js archive must contain exactly one bin/node executable")
        executable = bundle.extractfile(members[0])
        if executable is None:
            raise PrettierError("cannot read the pinned Node.js executable from its archive")
        with executable, destination.open("wb") as target:
            shutil.copyfileobj(executable, target)


def _extract_package(archive: Path, destination: Path) -> None:
    """Safely extract the full Prettier package subtree from its npm archive.

    Only plain files and directories under ``package/`` are accepted; absolute
    paths, ``..`` segments, Windows separators, and link entries are rejected
    so a hostile archive cannot escape the staged cache.
    """
    with tarfile.open(archive, "r:*") as bundle:
        for member in bundle.getmembers():
            name = PurePosixPath(member.name)
            if name.is_absolute() or ".." in name.parts or "\\" in member.name:
                raise PrettierError(f"unsafe Prettier package archive member: {member.name}")
            if member.issym() or member.islnk():
                raise PrettierError(f"unsafe Prettier package archive member: {member.name}")
            if name.parts[:1] != ("package",):
                continue
            if name.parts == ("package",):
                if not member.isdir():
                    raise PrettierError(f"unsupported Prettier package archive member: {member.name}")
                continue
            target = destination.joinpath(*name.parts[1:])
            if member.isdir():
                target.mkdir(parents=True, exist_ok=True)
                continue
            if not member.isfile():
                raise PrettierError(f"unsupported Prettier package archive member: {member.name}")
            target.parent.mkdir(parents=True, exist_ok=True)
            source = bundle.extractfile(member)
            if source is None:
                raise PrettierError(f"cannot read Prettier package archive member: {member.name}")
            with source, target.open("wb") as stream:
                shutil.copyfileobj(source, stream)


def _tree_digest(root: Path) -> str:
    """Fingerprint an extracted package tree so cached corruption cannot pass."""
    digest = hashlib.sha256()
    for path in sorted(root.rglob("*"), key=lambda item: item.relative_to(root).as_posix()):
        relative = path.relative_to(root).as_posix()
        if path.is_symlink():
            raise PrettierError(f"symlink inside the managed Prettier package: {path}")
        if path.is_file():
            digest.update(f"f {relative}\0".encode())
            with path.open("rb") as stream:
                digest.update(hashlib.file_digest(stream, "sha256").digest())
        else:
            digest.update(f"d {relative}\0".encode())
    return digest.hexdigest()


def _receipt(
    *,
    version: str,
    node_version: str,
    key: str,
    node_archive: str,
    node_executable: str,
    package_archive: str,
    package_tree: str,
) -> dict[str, str]:
    return {
        "nodeArchiveSha256": node_archive,
        "nodeExecutableSha256": node_executable,
        "nodeVersion": node_version,
        "packageArchiveSha256": package_archive,
        "packageTreeSha256": package_tree,
        "platform": key,
        "prettierVersion": version,
    }


def _verify_cache(
    cache: Path,
    *,
    version: str,
    node_version: str,
    key: str,
    node_archive: str,
    node_executable: str,
    package_archive: str,
    package_tree: str,
) -> tuple[Path, Path]:
    """Verify cache content and receipt against manifest pins; never repair."""
    node = cache / ("node.exe" if key.startswith("windows-") else "node")
    package = cache / "package"
    damaged = f"the managed Prettier cache at {cache} is damaged; remove it explicitly"
    # An authentic Windows Node executable can load an adjacent DLL before
    # JavaScript starts. Bound its directory as well as its bytes. The raw
    # download archives are legacy entries: they are never part of the
    # execution closure, but older caches that retained them stay admissible.
    allowed = {node.name, "package", _RECEIPT_FILE, "node-download", "package-download"}
    if cache.is_symlink():
        raise PrettierError(f"{damaged} (the cache root is a symlink)")
    try:
        entries = list(cache.iterdir())
    except OSError as exc:
        raise PrettierError(f"{damaged}: {exc}") from exc
    for entry in entries:
        if entry.is_symlink():
            raise PrettierError(f"{damaged} (symlink at the cache root: {entry.name})")
        if entry.name not in allowed:
            raise PrettierError(f"{damaged} (unexpected root entry: {entry.name})")
    if not node.is_file() or not (package / "bin/prettier.cjs").is_file():
        raise PrettierError(damaged)
    try:
        document = _json_document((cache / _RECEIPT_FILE).read_text(encoding="utf-8"))
        executable_sha256 = _sha256(node)
        tree_sha256 = _tree_digest(package)
    except (OSError, UnicodeError, PrettierError) as exc:
        raise PrettierError(f"{damaged}: {exc}") from exc
    # The independent manifest pins, not the receipt, decide cache identity.
    if executable_sha256 != node_executable or tree_sha256 != package_tree:
        raise PrettierError(f"{damaged} (content does not match the pinned decoded digests)")
    expected = _receipt(
        version=version,
        node_version=node_version,
        key=key,
        node_archive=node_archive,
        node_executable=node_executable,
        package_archive=package_archive,
        package_tree=package_tree,
    )
    if document != expected:
        differences = sorted(field for field in expected if document.get(field) != expected[field])
        raise PrettierError(f"{damaged} (receipt mismatch: {', '.join(differences) or 'unexpected fields'})")
    return node, package


def _provision(
    cache: Path,
    *,
    version: str,
    node_version: str,
    key: str,
    node_url: str,
    node_archive: str,
    node_executable: str,
    package_url: str,
    package_archive: str,
    package_tree: str,
) -> tuple[Path, Path]:
    """Stage the pinned runtime and package, then publish them atomically."""
    windows = key.startswith("windows-")
    stage = Path(tempfile.mkdtemp(prefix=".prettier-stage-", dir=cache.parent))
    try:
        node_archive_file = stage / "node-download"
        _download(node_url, node_archive_file)
        if _sha256(node_archive_file) != node_archive:
            raise PrettierError("downloaded Node.js SHA-256 does not match prettier.json")
        node = stage / ("node.exe" if windows else "node")
        _extract_node(node_archive_file, node, windows=windows)
        node.chmod(0o755)
        package_archive_file = stage / "package-download"
        _download(package_url, package_archive_file)
        if _sha256(package_archive_file) != package_archive:
            raise PrettierError("downloaded Prettier package SHA-256 does not match prettier.json")
        package = stage / "package"
        package.mkdir()
        _extract_package(package_archive_file, package)
        # Decoded content is checked against the manifest pins before any
        # staged runtime is probed or published.
        if _sha256(node) != node_executable:
            raise PrettierError("extracted Node.js executable SHA-256 does not match prettier.json")
        if _tree_digest(package) != package_tree:
            raise PrettierError("extracted Prettier package tree does not match prettier.json")
        receipt = _receipt(
            version=version,
            node_version=node_version,
            key=key,
            node_archive=node_archive,
            node_executable=node_executable,
            package_archive=package_archive,
            package_tree=package_tree,
        )
        (stage / _RECEIPT_FILE).write_text(json.dumps(receipt, sort_keys=True, indent=2) + "\n", encoding="utf-8")
        _probe([str(node), str(package / "bin/prettier.cjs")], version)
        # Publish decoded tool state only; archive verification is complete.
        node_archive_file.unlink()
        package_archive_file.unlink()
        os.replace(stage, cache)
        # The staged paths died with the rename; hand back the published pair.
        return cache / node.name, cache / "package"
    finally:
        shutil.rmtree(stage, ignore_errors=True)


def _managed_command(
    version: str,
    package_url: str,
    package_archive: str,
    package_tree: str,
    node_version: str,
    artifacts: dict[str, tuple[str, str, str]],
    *,
    provision: bool = True,
) -> tuple[str, str]:
    """Install or verify the governed Node.js runtime and Prettier package.

    With ``provision=False`` a missing cache is an actionable error and no
    download, directory, or lock is created; an existing cache is still
    verified and probed.
    """
    key = _platform_key(sys.platform)
    pinned = artifacts.get(key)
    if pinned is None:
        raise PrettierError(f"no pinned Node.js artifact for {key}")
    node_url, node_archive, node_executable = pinned
    cache = (build_dir.state_root() / "tools/prettier" / version / node_version / key).absolute()
    if not provision:
        if not os.path.lexists(cache):
            raise PrettierError(f"Prettier is not ready at {cache}; run ao setup prettier explicitly")
        node, package = _verify_cache(
            cache,
            version=version,
            node_version=node_version,
            key=key,
            node_archive=node_archive,
            node_executable=node_executable,
            package_archive=package_archive,
            package_tree=package_tree,
        )
        _probe([str(node), str(package / "bin/prettier.cjs")], version)
        return str(node), str(package / "bin/prettier.cjs")
    try:
        cache.parent.mkdir(parents=True, exist_ok=True)
        with shared_file_lock(cache.parent / ".setup.lock"):
            if os.path.lexists(cache):
                node, package = _verify_cache(
                    cache,
                    version=version,
                    node_version=node_version,
                    key=key,
                    node_archive=node_archive,
                    node_executable=node_executable,
                    package_archive=package_archive,
                    package_tree=package_tree,
                )
            else:
                node, package = _provision(
                    cache,
                    version=version,
                    node_version=node_version,
                    key=key,
                    node_url=node_url,
                    node_archive=node_archive,
                    node_executable=node_executable,
                    package_url=package_url,
                    package_archive=package_archive,
                    package_tree=package_tree,
                )
            _probe([str(node), str(package / "bin/prettier.cjs")], version)
    except OSError as exc:
        raise PrettierError(f"cannot prepare managed Prettier {cache}: {exc}") from exc
    return str(node), str(package / "bin/prettier.cjs")


def _nix_command(selected: str, version: str) -> str:
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
        raise PrettierError(f"cannot inspect AO_NIX_PRETTIER selection {selected!r}: {exc}") from exc
    if not available:
        raise PrettierError("Prettier is unavailable in AO_NIX_PRETTIER from the pinned Nix shell; re-enter ./ao")
    _probe([str(tool)], version)
    return str(tool)


def command(*, provision: bool = True) -> tuple[str, ...]:
    """Select and verify the pinned Prettier command prefix.

    Linux prefers the pinned Nix shell selection in AO_NIX_PRETTIER; every
    supported host without that selection, including lightweight Linux docs CI,
    uses governed managed artifacts. Existing caches are verified on every
    call, never repaired or replaced on a mismatch. With ``provision=False`` a
    missing managed cache fails before any download or state creation.
    """
    version, package_url, package_archive, package_tree, node_version, artifacts = _manifest()
    host = sys.platform
    if host == "linux":
        selected = os.environ.get("AO_NIX_PRETTIER", "")
        if selected:
            return (_nix_command(selected, version),)
    if host not in ("linux", "darwin", "win32"):
        raise PrettierError(f"Prettier does not support platform {host!r}")
    return _managed_command(
        version, package_url, package_archive, package_tree, node_version, artifacts, provision=provision
    )


def _absolute_file(filename: str) -> Path:
    path = Path(filename)
    # Do not resolve symlinks: the checkout containment check in run() rejects
    # links that escape, and Prettier receives the caller-visible spelling.
    return (path if path.is_absolute() else PROJECT_ROOT / path).absolute()


def run(files: list[str], *, check: bool, provision: bool = True) -> int:
    """Check only, or write, Prettier formatting for an explicit selection.

    Empty selections never provision a tool. Relative inputs are
    checkout-relative; diagnostics keep the caller-visible spelling.
    Infrastructure failures raise PrettierError; formatting findings and tool
    errors both return 1. With ``provision=False`` a missing pinned tool fails
    before any download or state creation.
    """
    if not files:
        return 0
    requested: dict[Path, str] = {}
    checkout = Path(os.path.realpath(PROJECT_ROOT))
    for filename in files:
        if not filename or "\0" in filename:
            raise PrettierError("Prettier requires non-empty file paths without NUL characters")
        path = _absolute_file(filename)
        if path in requested:
            raise PrettierError(f"duplicate requested Prettier file: {filename}")
        if not Path(os.path.realpath(path)).is_relative_to(checkout):
            raise PrettierError(f"Prettier can only format files inside the checkout: {filename}")
        requested[path] = filename
    config = PROJECT_ROOT / ".prettierrc.json"
    ignores = (PROJECT_ROOT / ".gitignore", PROJECT_ROOT / ".prettierignore")
    for required in (*ignores, config):
        if not required.is_file():
            raise PrettierError(f"Prettier requires {required.name} at the repository root; it is missing")
    # Prettier replaces its default ignore files when --ignore-path is given,
    # so name both repository ignore files explicitly.
    options = ["--config", str(config), "--no-editorconfig"]
    for ignore in ignores:
        options.extend(("--ignore-path", str(ignore)))
    # Keep file-level formatting findings and operational diagnostics while
    # suppressing Prettier's routine progress chatter.
    options.extend(("--log-level", "warn"))
    if check:
        options.append("--check")
    else:
        # Without an explicit mode Prettier prints formatted output to stdout
        # instead of rewriting files, so write mode must name --write.
        options.append("--write")
    base = [*command(provision=provision), *options]
    batches: list[dict[Path, str]] = []
    batch: dict[Path, str] = {}
    for path, logical in requested.items():
        argv = [*base, *(str(item) for item in batch), str(path)]
        # Bound both argument count and Windows' UTF-16 command-line length.
        length = len(subprocess.list2cmdline(argv).encode("utf-16-le")) // 2
        if batch and (len(batch) >= _MAXIMUM_BATCH_FILES or length > _MAXIMUM_COMMAND_UNITS):
            batches.append(batch)
            batch = {}
        if len(subprocess.list2cmdline([*base, str(path)]).encode("utf-16-le")) // 2 > _MAXIMUM_COMMAND_UNITS:
            raise PrettierError(f"Prettier file path exceeds the command-line limit: {logical}")
        batch[path] = logical
    batches.append(batch)
    status = 0
    for batch in batches:
        result = _invoke([*base, *(str(path) for path in batch)])
        if result.stdout:
            print(result.stdout, end="" if result.stdout.endswith("\n") else "\n")
        if result.stderr:
            print(result.stderr, end="" if result.stderr.endswith("\n") else "\n", file=sys.stderr)
            status = 1
        if result.returncode:
            print(f"Prettier exited with status {result.returncode}", file=sys.stderr)
            status = 1
    return status
