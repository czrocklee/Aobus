"""Provision the checkout-isolated Windows Python tooling environment."""

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

from . import builddir

PACKAGE_ROOT = Path(__file__).parent.parent
TOOLCHAIN_FILE = PACKAGE_ROOT / "windows-toolchain.json"
REQUIREMENTS_FILE = PACKAGE_ROOT / "windows-requirements.txt"
COMPLETE_MARKER = ".aobus-tooling-complete"
SCHEMA_VERSION = "1"
LOCK_TIMEOUT_SECONDS = 600
STALE_LOCK_SECONDS = 1_800


def tool_versions(config_file: Path = TOOLCHAIN_FILE) -> dict[str, str]:
    values = json.loads(config_file.read_text(encoding="utf-8"))
    required = ("python", "ruff", "mypy")
    if any(not isinstance(values.get(name), str) or not values[name] for name in required):
        raise RuntimeError(f"invalid Windows toolchain configuration: {config_file}")
    return {name: values[name] for name in required}


def validate_base_python(versions: dict[str, str]) -> None:
    expected = tuple(int(part) for part in versions["python"].split("."))
    if sys.version_info[:3] != expected:
        actual = ".".join(str(part) for part in sys.version_info[:3])
        raise RuntimeError(f"Aobus requires Python {versions['python']} for Windows tooling; got {actual}")
    if importlib.util.find_spec("ensurepip") is None:
        raise RuntimeError("Aobus requires a regular Python installation with ensurepip")


def environment_fingerprint(
    versions: dict[str, str], requirements_file: Path = REQUIREMENTS_FILE, *, executable: str = sys.executable
) -> str:
    digest = hashlib.sha256()
    digest.update(f"schema={SCHEMA_VERSION}\0".encode())
    digest.update(os.path.normcase(os.path.abspath(executable)).encode())
    digest.update(b"\0")
    digest.update(json.dumps(versions, sort_keys=True).encode())
    digest.update(b"\0")
    digest.update(requirements_file.read_bytes())
    return digest.hexdigest()[:16]


def environment_path(
    project_root: Path,
    state_root: Path,
    fingerprint: str,
    *,
    environ: dict[str, str] | None = None,
) -> Path:
    checkout = builddir.windows_checkout_key(project_root, environ=environ, create_id=True)
    return state_root / "tools" / "venvs" / checkout / fingerprint


def _python_path(environment: Path) -> Path:
    return environment / "Scripts" / "python.exe"


def _process_environment(state_root: Path) -> dict[str, str]:
    environment = dict(os.environ)
    environment.pop("PYTHONHOME", None)
    environment.pop("PYTHONPATH", None)
    environment["PIP_CACHE_DIR"] = str(state_root / "cache" / "pip")
    environment["PIP_DISABLE_PIP_VERSION_CHECK"] = "1"
    environment["PYTHONIOENCODING"] = "utf-8"
    environment["PYTHONUTF8"] = "1"
    return environment


def _tools_match(environment: Path, versions: dict[str, str], state_root: Path, fingerprint: str) -> bool:
    python = _python_path(environment)
    marker = environment / COMPLETE_MARKER
    if not python.is_file() or not marker.is_file():
        return False
    try:
        if marker.read_text(encoding="utf-8").strip() != fingerprint:
            return False
    except OSError:
        return False

    python_version = tuple(int(part) for part in versions["python"].split("."))
    commands = (
        (
            [
                str(python),
                "-I",
                "-c",
                f"import sys; raise SystemExit(tuple(sys.version_info[:3]) != {python_version!r})",
            ],
            "",
        ),
        ([str(python), "-I", "-m", "ruff", "--version"], f"ruff {versions['ruff']}"),
        ([str(python), "-I", "-m", "mypy", "--version"], f"mypy {versions['mypy']}"),
    )
    for command, expected in commands:
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
        if completed.returncode != 0 or (expected and not completed.stdout.strip().startswith(expected)):
            return False
    return True


def _build_environment(
    destination: Path,
    versions: dict[str, str],
    requirements_file: Path,
    state_root: Path,
    fingerprint: str,
) -> None:
    venv.EnvBuilder(with_pip=True, clear=False, symlinks=False).create(destination)
    python = _python_path(destination)
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
    if not _tools_match(destination, versions, state_root, fingerprint):
        raise RuntimeError("the provisioned Ruff/mypy environment failed its version probe")


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
    requirements_file: Path = REQUIREMENTS_FILE,
    environ: dict[str, str] | None = None,
) -> Path:
    versions = tool_versions(config_file)
    validate_base_python(versions)
    fingerprint = environment_fingerprint(versions, requirements_file)
    destination = environment_path(project_root, state_root, fingerprint, environ=environ)
    if _tools_match(destination, versions, state_root, fingerprint):
        return _python_path(destination)

    destination.parent.mkdir(parents=True, exist_ok=True)
    lock = destination.parent / f".{fingerprint}.lock"
    with _environment_lock(lock):
        if not _tools_match(destination, versions, state_root, fingerprint):
            staging = destination.parent / f".{fingerprint}.tmp-{os.getpid()}-{uuid.uuid4().hex}"
            try:
                _build_environment(staging, versions, requirements_file, state_root, fingerprint)
                if destination.exists():
                    shutil.rmtree(destination)
                staging.rename(destination)
                if not _tools_match(destination, versions, state_root, fingerprint):
                    raise RuntimeError("the published Ruff/mypy environment failed its version probe")
            finally:
                if staging.exists():
                    shutil.rmtree(staging)
        return _python_path(destination)


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser()
    parser.add_argument("--project-root", type=Path, required=True)
    parser.add_argument("--state-root", type=Path, required=True)
    parser.add_argument("--result-file", type=Path, required=True)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        python = ensure_environment(args.project_root, args.state_root)
        args.result_file.parent.mkdir(parents=True, exist_ok=True)
        args.result_file.write_text(f"{python}\n", encoding="utf-8")
    except (OSError, RuntimeError, subprocess.CalledProcessError) as exc:
        print(f"Aobus Python tooling bootstrap failed: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
