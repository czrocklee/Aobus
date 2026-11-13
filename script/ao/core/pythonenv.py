"""Provision a checkout-isolated native Python tooling environment."""

import argparse
import contextlib
import hashlib
import importlib.util
import json
import os
import shutil
import subprocess
import sys
import time
import uuid
import venv
from collections.abc import Iterator
from pathlib import Path

from . import builddir, buildenv

PACKAGE_ROOT = Path(__file__).parent.parent
TOOLCHAIN_FILE = PACKAGE_ROOT / "toolchain.json"
REQUIREMENTS_FILE = PACKAGE_ROOT / "windows-requirements.txt"
MACOS_REQUIREMENTS_FILE = PACKAGE_ROOT / "macos-requirements.txt"
COMPLETE_MARKER = ".aobus-tooling-complete"
# The digest already covers every governed input (tool versions and the full
# included lock graph), so any contract change moves every historical
# fingerprint without a schema bump; bump this only if the digest recipe itself
# becomes ambiguous about which inputs it covers.
SCHEMA_VERSION = "2"
TOOL_REQUIREMENTS = ("python", "ruff", "mypy", "markdown-it-py", "mdurl")
LOCK_TIMEOUT_SECONDS = 600
STALE_LOCK_SECONDS = 1_800


def tool_versions(config_file: Path = TOOLCHAIN_FILE) -> dict[str, str]:
    values = json.loads(config_file.read_text(encoding="utf-8"))
    if values.get("schemaVersion") != 1:
        raise RuntimeError(f"unsupported toolchain schema in {config_file}: {values.get('schemaVersion')!r}")
    if any(not isinstance(values.get(name), str) or not values[name] for name in TOOL_REQUIREMENTS):
        raise RuntimeError(f"invalid Aobus toolchain configuration: {config_file}")
    return {name: values[name] for name in TOOL_REQUIREMENTS}


def validate_base_python(versions: dict[str, str], *, exact_patch: bool = True) -> None:
    expected = tuple(int(part) for part in versions["python"].split("."))
    actual = sys.version_info[:3]
    matches = actual == expected if exact_patch else actual[:2] == expected[:2]
    if not matches:
        actual_text = ".".join(str(part) for part in sys.version_info[:3])
        requirement = versions["python"] if exact_patch else ".".join(str(part) for part in expected[:2])
        raise RuntimeError(f"Aobus requires Python {requirement} for native tooling; got {actual_text}")
    if importlib.util.find_spec("ensurepip") is None:
        raise RuntimeError("Aobus requires a regular Python installation with ensurepip")


def _included_requirements(requirements_file: Path) -> list[Path]:
    """Return the files referenced by the repository locks' ``-r <path>`` lines.

    Includes resolve relative to the including file, like pip.
    """
    # pip strips a UTF-8 BOM before recognizing controls.
    lines = requirements_file.read_text(encoding="utf-8-sig").splitlines()
    return [requirements_file.parent / tokens[1] for tokens in map(str.split, lines) if tokens[:1] == ["-r"]]


def _lock_input_bytes(requirements_file: Path) -> Iterator[bytes]:
    """Yield the bytes of a lock and every included lock, depth first."""
    yield requirements_file.read_bytes()
    for included in _included_requirements(requirements_file):
        yield from _lock_input_bytes(included)


def environment_fingerprint(
    versions: dict[str, str],
    requirements_file: Path = REQUIREMENTS_FILE,
    *,
    executable: str = sys.executable,
    runtime_version: str | None = None,
) -> str:
    digest = hashlib.sha256()
    digest.update(f"schema={SCHEMA_VERSION}\0".encode())
    digest.update(os.path.normcase(os.path.abspath(executable)).encode())
    digest.update(b"\0")
    if runtime_version is not None:
        digest.update(runtime_version.encode())
        digest.update(b"\0")
    digest.update(json.dumps(versions, sort_keys=True).encode())
    digest.update(b"\0")
    for lock_bytes in _lock_input_bytes(requirements_file):
        digest.update(lock_bytes)
        digest.update(b"\0")
    return digest.hexdigest()[:16]


def environment_path(
    project_root: Path,
    state_root: Path,
    fingerprint: str,
    *,
    environ: dict[str, str] | None = None,
    os_name: str = "nt",
) -> Path:
    checkout = builddir.windows_checkout_key(
        project_root,
        environ=environ,
        create_id=True,
        os_name=os_name,
    )
    return state_root / "tools" / "venvs" / checkout / fingerprint


def _python_path(environment: Path, *, platform_name: str = "windows") -> Path:
    return environment / "Scripts" / "python.exe" if platform_name == "windows" else environment / "bin" / "python"


def _process_environment(state_root: Path) -> dict[str, str]:
    environment = dict(os.environ)
    environment.pop("PYTHONHOME", None)
    environment.pop("PYTHONPATH", None)
    environment["PIP_CACHE_DIR"] = str(state_root / "cache" / "pip")
    environment["PIP_DISABLE_PIP_VERSION_CHECK"] = "1"
    environment["PYTHONIOENCODING"] = "utf-8"
    environment["PYTHONUTF8"] = "1"
    return environment


def _tools_match(
    environment: Path,
    versions: dict[str, str],
    state_root: Path,
    fingerprint: str,
    *,
    platform_name: str = "windows",
) -> bool:
    python = _python_path(environment, platform_name=platform_name)
    marker = environment / COMPLETE_MARKER
    if not python.is_file() or not marker.is_file():
        return False
    try:
        if marker.read_text(encoding="utf-8").strip() != fingerprint:
            return False
    except OSError:
        return False

    python_version = tuple(int(part) for part in versions["python"].split("."))
    version_components = 3 if platform_name == "windows" else 2
    # Ruff and mypy print a version banner that may carry build details, so
    # their probe matches the pinned prefix; the documentation parser packages
    # are pinned to the exact distribution version and match it exactly.
    commands = (
        (
            [
                str(python),
                "-I",
                "-c",
                "import sys; raise SystemExit("
                f"tuple(sys.version_info[:{version_components}]) != {python_version[:version_components]!r})",
            ],
            "",
            False,
        ),
        ([str(python), "-I", "-m", "ruff", "--version"], f"ruff {versions['ruff']}", False),
        ([str(python), "-I", "-m", "mypy", "--version"], f"mypy {versions['mypy']}", False),
        (
            [
                str(python),
                "-I",
                "-c",
                "from importlib.metadata import version; print(version('markdown-it-py'))",
            ],
            versions["markdown-it-py"],
            True,
        ),
        (
            [str(python), "-I", "-c", "from importlib.metadata import version; print(version('mdurl'))"],
            versions["mdurl"],
            True,
        ),
    )
    for command, expected, exact in commands:
        try:
            completed = subprocess.run(
                command,
                env=_process_environment(state_root),
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                timeout=30,
            )
        except (OSError, subprocess.TimeoutExpired):
            return False
        if completed.returncode != 0:
            return False
        actual = completed.stdout.strip()
        if exact and actual != expected:
            return False
        if not exact and expected and not actual.startswith(expected):
            return False
    return True


def _build_environment(
    destination: Path,
    versions: dict[str, str],
    requirements_file: Path,
    state_root: Path,
    fingerprint: str,
    *,
    platform_name: str = "windows",
) -> None:
    venv.EnvBuilder(with_pip=True, clear=False, symlinks=False).create(destination)
    python = _python_path(destination, platform_name=platform_name)
    subprocess.run(
        [
            str(python),
            "-I",
            "-m",
            "pip",
            "install",
            "--disable-pip-version-check",
            "--no-input",
            "--only-binary=:all:",
            "--require-hashes",
            "--no-deps",
            "-r",
            str(requirements_file),
        ],
        check=True,
        env=_process_environment(state_root),
    )
    (destination / COMPLETE_MARKER).write_text(f"{fingerprint}\n", encoding="utf-8")
    if not _tools_match(
        destination,
        versions,
        state_root,
        fingerprint,
        platform_name=platform_name,
    ):
        raise RuntimeError("the provisioned Python tooling environment failed its version probe")


@contextlib.contextmanager
def _environment_lock(path: Path) -> Iterator[None]:
    deadline = time.monotonic() + LOCK_TIMEOUT_SECONDS
    while True:
        try:
            path.mkdir()
            break
        except FileExistsError:
            try:
                if time.time() - path.stat().st_mtime > STALE_LOCK_SECONDS:
                    path.rmdir()
                    continue
            except FileNotFoundError:
                continue
            except OSError:
                pass
            if time.monotonic() >= deadline:
                raise RuntimeError(f"timed out waiting for the Python environment lock: {path}") from None
            time.sleep(0.1)

    try:
        yield
    finally:
        try:
            path.rmdir()
        except FileNotFoundError:
            pass


def ensure_environment(
    project_root: Path,
    state_root: Path,
    *,
    config_file: Path = TOOLCHAIN_FILE,
    requirements_file: Path | None = None,
    environ: dict[str, str] | None = None,
    platform_name: str = "windows",
) -> Path:
    if platform_name not in {"macos", "windows"}:
        raise RuntimeError(f"unsupported native Python tooling platform {platform_name!r}")
    if requirements_file is None:
        requirements_file = MACOS_REQUIREMENTS_FILE if platform_name == "macos" else REQUIREMENTS_FILE
    versions = tool_versions(config_file)
    validate_base_python(versions, exact_patch=platform_name == "windows")
    actual_version = ".".join(str(part) for part in sys.version_info[:3])
    fingerprint = environment_fingerprint(versions, requirements_file, runtime_version=actual_version)
    destination = environment_path(
        project_root,
        state_root,
        fingerprint,
        environ=environ,
        os_name="nt" if platform_name == "windows" else "posix",
    )
    if _tools_match(destination, versions, state_root, fingerprint, platform_name=platform_name):
        return _python_path(destination, platform_name=platform_name)

    destination.parent.mkdir(parents=True, exist_ok=True)
    lock = destination.parent / f".{fingerprint}.lock"
    with _environment_lock(lock):
        if not _tools_match(destination, versions, state_root, fingerprint, platform_name=platform_name):
            staging = destination.parent / f".{fingerprint}.tmp-{os.getpid()}-{uuid.uuid4().hex}"
            try:
                _build_environment(
                    staging,
                    versions,
                    requirements_file,
                    state_root,
                    fingerprint,
                    platform_name=platform_name,
                )
                if destination.exists():
                    shutil.rmtree(destination)
                staging.rename(destination)
                if not _tools_match(
                    destination,
                    versions,
                    state_root,
                    fingerprint,
                    platform_name=platform_name,
                ):
                    raise RuntimeError("the published Python tooling environment failed its version probe")
            finally:
                if staging.exists():
                    shutil.rmtree(staging)
        return _python_path(destination, platform_name=platform_name)


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser()
    parser.add_argument("--project-root", type=Path, required=True)
    parser.add_argument("--state-root", type=Path, required=True)
    output = parser.add_mutually_exclusive_group(required=True)
    output.add_argument("--result-file", type=Path)
    output.add_argument("--launch-windows-portal", action="store_true")
    parser.add_argument("--platform", choices=("macos", "windows"))
    return parser


def _native_platform_name(platform_name: str | None = None) -> str:
    return {"darwin": "macos", "win32": "windows"}[sys.platform if platform_name is None else platform_name]


def main(argv: list[str] | None = None) -> int:
    arguments = sys.argv[1:] if argv is None else argv
    command = None
    # The portal owns this tail, including its own '--' and help flags.
    if "--" in arguments:
        separator = arguments.index("--")
        command = arguments[separator + 1 :]
        arguments = arguments[:separator]
    args = _parser().parse_args(arguments)
    try:
        platform_name = args.platform or _native_platform_name()
        if (
            platform_name == "windows"
            and command is not None
            and not buildenv.windows_bootstrap_requires_python_tools(command)
        ):
            # The governed base installation is already the portal runtime.
            # Do not provision or inspect unrelated lint/documentation wheels.
            validate_base_python(tool_versions())
            python = Path(sys.executable)
        else:
            python = ensure_environment(args.project_root, args.state_root, platform_name=platform_name)
        if args.result_file is not None:
            args.result_file.parent.mkdir(parents=True, exist_ok=True)
            args.result_file.write_text(f"{python}\n", encoding="utf-8")
            return 0
        return subprocess.run([str(python), "-m", "ao.core.windows_portal", *(command or [])], check=False).returncode
    except (OSError, RuntimeError, subprocess.CalledProcessError) as exc:
        print(f"Aobus native Python tooling bootstrap failed: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
