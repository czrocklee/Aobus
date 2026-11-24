"""Supervise opt-in native AppKit smoke scenarios for the Bazel bundle launcher."""

import argparse
import json
import os
import shlex
import socket
import stat
import subprocess
import sys
import tempfile
import time
import uuid
from pathlib import Path
from typing import NamedTuple

SCENARIOS = (
    "desktop",
    "initial-index",
    "membership",
    "authoring",
    "presentation",
    "media",
    "credits-controls",
    "credits-previews",
    "quality-presentation",
)
PARENT_TIMEOUT_SECONDS = 120.0
SUCCESSOR_TIMEOUT_SECONDS = 60.0
TERMINATE_TIMEOUT_SECONDS = 5.0
RUN_ID_ENV = "AOBUS_APPKIT_TEST_RUN_ID"
SUCCESSOR_LAUNCHER_ENV = "AOBUS_APPKIT_TEST_SUCCESSOR_LAUNCHER"
_MAX_REQUEST_BYTES = 65536
_POLL_SECONDS = 0.05
# Darwin <sys/unistd.h>; Python need not expose this platform-specific selector by name.
_DARWIN_PC_CASE_SENSITIVE = 11


def _local_socket() -> socket.socket:
    if sys.platform == "win32":
        raise RuntimeError("process supervision requires a POSIX host")
    return socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)


def _stop(process: subprocess.Popen[bytes], timeout: float) -> None:
    if process.poll() is not None:
        return
    try:
        process.terminate()
    except ProcessLookupError:
        pass
    try:
        process.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()


def _receive_arguments(connection: socket.socket) -> list[str]:
    connection.settimeout(2.0)
    request = bytearray()
    while chunk := connection.recv(4096):
        request.extend(chunk)
        if len(request) > _MAX_REQUEST_BYTES:
            raise ValueError("successor launch arguments exceed the test protocol limit")
    arguments = json.loads(request)
    if not isinstance(arguments, list) or not arguments or not all(isinstance(arg, str) for arg in arguments):
        raise ValueError("successor launch requires a nonempty string argument list")
    return arguments


def run_desktop(
    command: list[str],
    *,
    cwd: Path,
    environment: dict[str, str],
    parent_timeout: float,
    successor_timeout: float,
    terminate_timeout: float,
) -> int:
    """Collect real exit statuses; bound and reap every GUI child on all exits.

    The test bundle supplies a temporary launcher through DesktopLaunch.executable.
    The production detached launcher runs that bridge with its real successor
    arguments. The bridge hands those arguments to this process, which owns the
    actual successor Popen and therefore observes its complete termination.
    """
    parent: subprocess.Popen[bytes] | None = None
    successor: subprocess.Popen[bytes] | None = None
    with tempfile.TemporaryDirectory(prefix="ao-appkit-") as directory:
        root = Path(directory)
        endpoint = root / "launch.sock"
        launcher = root / "launch"
        launcher.write_text(
            "#!/bin/sh\nexec "
            + shlex.join([sys.executable, str(Path(__file__).resolve()), "--handoff", str(endpoint)])
            + ' "$@"\n',
            encoding="utf-8",
        )
        launcher.chmod(0o700)
        child_environment = {**os.environ, **environment, SUCCESSOR_LAUNCHER_ENV: str(launcher)}
        with _local_socket() as listener:
            listener.bind(str(endpoint))
            listener.listen(1)
            listener.settimeout(_POLL_SECONDS)
            try:
                parent = subprocess.Popen(command, cwd=cwd, env=child_environment)
                parent_deadline = time.monotonic() + parent_timeout
                successor_deadline: float | None = None
                while True:
                    parent_status = parent.poll()
                    successor_status = successor.poll() if successor is not None else None
                    if parent_status is not None and parent_status != 0:
                        return parent_status
                    if successor_status is not None and successor_status != 0:
                        return successor_status
                    if parent_status == 0 and successor_status == 0:
                        return 0

                    now = time.monotonic()
                    if parent_status is None and now >= parent_deadline:
                        raise TimeoutError("desktop parent exceeded the controller timeout")
                    if parent_status == 0 and successor_deadline is None:
                        successor_deadline = now + successor_timeout
                    if successor_status is None and successor_deadline is not None and now >= successor_deadline:
                        if successor is None:
                            raise TimeoutError(
                                "desktop successor launch request was not received within the controller timeout"
                            )
                        raise TimeoutError("desktop successor did not exit within the controller timeout")

                    try:
                        connection, _ = listener.accept()
                    except TimeoutError:
                        continue
                    with connection:
                        if successor is not None:
                            raise ValueError("desktop scenario requested more than one successor")
                        arguments = _receive_arguments(connection)
                        successor = subprocess.Popen([command[0], *arguments], cwd=cwd, env=child_environment)
                        successor_deadline = time.monotonic() + successor_timeout
                        connection.sendall(b"OK")
            except (OSError, ValueError) as exc:
                print(f"Error: AppKit {exc}.", file=sys.stderr)
                return 1
            finally:
                # Also covers exceptions and an interrupted controller. These are
                # owned children, not unrelated processes found by name or PID.
                if successor is not None:
                    _stop(successor, terminate_timeout)
                if parent is not None:
                    _stop(parent, terminate_timeout)


def _handoff(endpoint: str, arguments: list[str]) -> None:
    with _local_socket() as connection:
        connection.settimeout(5.0)
        connection.connect(endpoint)
        connection.sendall(json.dumps(arguments).encode("utf-8"))
        connection.shutdown(socket.SHUT_WR)
        reply = bytearray()
        while len(reply) < 2:
            chunk = connection.recv(2 - len(reply))
            if not chunk:
                break
            reply.extend(chunk)
        if reply != b"OK":
            raise RuntimeError("desktop controller rejected the successor launch")


def _require_utf8_fixture(path: Path, option: str) -> None:
    try:
        str(path).encode("utf-8")
    except UnicodeEncodeError as error:
        raise ValueError(f"{option} path is not valid UTF-8: {str(path)!r}") from error


def _check_parent_traversal(path: Path, option: str) -> None:
    prefix = Path(path.anchor)
    for part in path.parts[1:]:
        if part == "..":
            # Ask the filesystem to traverse '..' itself, checking search
            # permission on the directory rather than merely inspecting it.
            try:
                (prefix / "..").stat()
            except OSError as error:
                raise ValueError(
                    f"cannot resolve {option} path {str(path)!r}: "
                    f"'..' follows an unavailable directory: {prefix}: {error}"
                ) from error
        prefix /= part


class _FixtureLocation(NamedTuple):
    anchor: Path
    identity: os.stat_result
    missing: tuple[str, ...]

    @property
    def path(self) -> Path:
        return self.anchor.joinpath(*self.missing)


def _fixture_location(path: Path, option: str) -> _FixtureLocation:
    if sys.platform == "darwin":
        _require_utf8_fixture(path, option)
    path = path.absolute()
    if ".." in path.parts:
        _check_parent_traversal(path, option)
    missing = []
    for ancestor in (path, *path.parents):
        context = f"cannot inspect {option} fixture ancestor {ancestor}"
        try:
            ancestor.lstat()
        except FileNotFoundError:
            missing.append(ancestor.name)
            continue
        except OSError as error:
            raise ValueError(f"{context}: {error}") from error
        # Only append names that are actually absent. Do not let non-strict
        # resolution reinterpret dangling links or unavailable '..' prefixes.
        try:
            identity = ancestor.stat()
            resolved = ancestor.resolve(strict=True)
            resolved_identity = resolved.stat()
        except FileNotFoundError as error:
            raise ValueError(f"{context}: dangling symbolic link or unavailable target: {error}") from error
        except (OSError, RuntimeError) as error:
            raise ValueError(f"{context}: {error}") from error
        if not os.path.samestat(identity, resolved_identity):
            raise ValueError(f"{context}: resolved path does not identify the same fixture ancestor: {resolved}")
        if missing and not stat.S_ISDIR(identity.st_mode):
            raise ValueError(f"{context}: not a directory")
        location = _FixtureLocation(resolved, identity, tuple(reversed(missing)))
        if sys.platform == "darwin":
            _require_utf8_fixture(location.path, option)
        return location
    raise ValueError(f"cannot locate an existing {option} fixture ancestor for {path}")


def _has_existing_ancestor(identity: os.stat_result, anchor: Path, option: str) -> bool:
    for parent in anchor.parents:
        try:
            if os.path.samestat(identity, parent.stat()):
                return True
        except OSError as error:
            raise ValueError(f"cannot inspect {option} fixture ancestor {parent}: {error}") from error
    return False


def _fixtures_overlap(library: _FixtureLocation, state_root: _FixtureLocation) -> bool:
    library_anchor, library_identity, library_suffix = library
    state_anchor, state_identity, state_suffix = state_root
    if os.path.samestat(library_identity, state_identity):
        depth = min(len(library_suffix), len(state_suffix))
        library_prefix, state_prefix = library_suffix[:depth], state_suffix[:depth]
        if library_prefix == state_prefix:
            return True
        if sys.platform != "darwin":
            return False
        # Unicode normalization and folding cannot establish how the volume
        # will compare missing names. Only ASCII comparisons can prove disjointness.
        needs_case_check = False
        ambiguous = False
        for left, right in zip(library_prefix, state_prefix, strict=True):
            if left == right:
                continue
            if not (left.isascii() and right.isascii()):
                ambiguous = True
            elif left.lower() != right.lower():
                return False
            else:
                needs_case_check = True
        if needs_case_check:
            context = f"cannot determine fixture volume case sensitivity at {library_anchor}"
            selector = os.pathconf_names.get("PC_CASE_SENSITIVE", _DARWIN_PC_CASE_SENSITIVE)
            try:
                case_sensitive = os.pathconf(library_anchor, selector)
            except (OSError, ValueError) as error:
                raise ValueError(f"{context}: pathconf failed: {error}") from error
            if case_sensitive not in (0, 1):
                raise ValueError(f"{context}: indeterminate pathconf result ({case_sensitive})")
            if case_sensitive == 1:
                return False
        if ambiguous:
            raise ValueError(
                "cannot determine whether --library and --state-root overlap for missing non-ASCII names; "
                "precreate distinct empty fixture directories"
            )
        return True
    # A missing path cannot contain an existing anchor. For an existing
    # fixture, check whether it appears in the other anchor's ancestry.
    return (not library_suffix and _has_existing_ancestor(library_identity, state_anchor, "--state-root")) or (
        not state_suffix and _has_existing_ancestor(state_identity, library_anchor, "--library")
    )


def _resolve_fixtures(library: Path, state_root: Path) -> tuple[Path, Path]:
    library_location = _fixture_location(library, "--library")
    state_location = _fixture_location(state_root, "--state-root")
    library, state_root = library_location.path, state_location.path
    if (
        library == state_root
        or library in state_root.parents
        or state_root in library.parents
        or _fixtures_overlap(library_location, state_location)
    ):
        raise ValueError("--library and --state-root must not equal or contain one another")
    for option, location in (("--library", library_location), ("--state-root", state_location)):
        if not location.missing:
            if not stat.S_ISDIR(location.identity.st_mode):
                raise ValueError(f"{option} is not a directory: {location.path}")
            if next(location.path.iterdir(), None) is not None:
                raise ValueError(f"{option} must be absent or empty: {location.path}")
    return library, state_root


def _require_marker(marker: Path, run_id: str) -> None:
    if marker.read_text(encoding="utf-8") != f"{run_id}\n":
        raise RuntimeError(f"marker does not belong to this invocation: {marker}")


def _run_parent(command: list[str], environment: dict[str, str]) -> int:
    parent = subprocess.Popen(command, env={**os.environ, **environment})
    try:
        try:
            return parent.wait(timeout=PARENT_TIMEOUT_SECONDS)
        except subprocess.TimeoutExpired:
            print(
                f"Error: AppKit smoke parent exceeded the {PARENT_TIMEOUT_SECONDS:g}-second timeout.", file=sys.stderr
            )
            _stop(parent, TERMINATE_TIMEOUT_SECONDS)
            return 1
    except BaseException:
        try:
            parent.kill()
        except ProcessLookupError:
            pass
        while True:
            try:
                parent.wait()
                break
            except KeyboardInterrupt:
                continue
        raise


def run_smoke(executable: Path, *, scenario: str, library: Path, state_root: Path) -> int:
    """Run one configured native bundle without artifact discovery or build policy.

    The caller supplies the bundle executable and its runtime environment.
    Disposable fixtures, bounded child lifetimes and per-invocation desktop
    markers remain owned here, not by the retired portal test command.
    """
    library, state_root = _resolve_fixtures(library, state_root)
    command = [
        str(executable.resolve()),
        "--scenario",
        scenario,
        "--library",
        str(library),
        "--state-root",
        str(state_root),
    ]
    run_id = uuid.uuid4().hex
    environment = {RUN_ID_ENV: run_id}
    if scenario != "desktop":
        return _run_parent(command, environment)
    status = run_desktop(
        command,
        cwd=Path.cwd(),
        environment=environment,
        parent_timeout=PARENT_TIMEOUT_SECONDS,
        successor_timeout=SUCCESSOR_TIMEOUT_SECONDS,
        terminate_timeout=TERMINATE_TIMEOUT_SECONDS,
    )
    if status == 0:
        _require_marker(state_root / "desktop-pass.txt", run_id)
        _require_marker(state_root / "successor-pass.txt", run_id)
    return status


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path, help="configured smoke bundle executable")
    parser.add_argument("--scenario", choices=SCENARIOS, default="desktop")
    parser.add_argument("--library", type=Path, required=True, help="absent or empty disposable media directory")
    parser.add_argument("--state-root", type=Path, required=True, help="absent or empty isolated state directory")
    args = parser.parse_args(argv)
    try:
        return run_smoke(args.executable, scenario=args.scenario, library=args.library, state_root=args.state_root)
    except (OSError, RuntimeError, ValueError) as error:
        print(f"Error: AppKit {error}.", file=sys.stderr)
        return 1


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--handoff":
        _handoff(sys.argv[2], sys.argv[3:])
    else:
        sys.exit(main())
