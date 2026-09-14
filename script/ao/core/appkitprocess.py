"""Own both native desktop test processes while exercising the production launcher."""

import json
import os
import shlex
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

SUCCESSOR_LAUNCHER_ENV = "AOBUS_APPKIT_TEST_SUCCESSOR_LAUNCHER"
_MAX_REQUEST_BYTES = 65536
_POLL_SECONDS = 0.05


def _local_socket() -> socket.socket:
    if sys.platform == "win32":
        raise RuntimeError("AppKit process supervision requires a POSIX host")
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
            "#!/bin/sh\nexec " + shlex.join([sys.executable, str(Path(__file__).resolve()), str(endpoint)]) + ' "$@"\n',
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


if __name__ == "__main__":
    _handoff(sys.argv[1], sys.argv[2:])
