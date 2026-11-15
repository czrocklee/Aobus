"""Owned Linux display, session-bus and PipeWire services for Bazel gates."""

from __future__ import annotations

import hashlib
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import time
from collections.abc import Generator, Mapping
from concurrent.futures import ThreadPoolExecutor
from contextlib import ExitStack, contextmanager
from pathlib import Path
from typing import Any
from urllib.parse import quote

from . import bazel_native as native

_READY_TIMEOUT = 10.0
_PROBE_TIMEOUT = 2.0
_STOP_TIMEOUT = 5.0
_POLL_INTERVAL = 0.05


def stable_service_root(tree: Path, service: str) -> Path:
    """Name a short private IPC root after its build tree, not after the attempt.

    Service paths enter hashed test environments, so a random name makes every
    attempt miss Bazel's test-result cache. The tree lock serializes the gates
    that use one tree; recreate_private_root() replaces the previous root.
    """
    digest = hashlib.sha256(str(tree.resolve()).encode("utf-8")).hexdigest()[:16]
    return Path("/tmp") / f"aobus-{service}-{digest}"


def recreate_private_root(path: Path) -> None:
    """Replace this user's earlier root; refuse a symlink or another user's entry."""
    if sys.platform == "win32":
        raise native.BazelWorkspaceError("private service roots require a POSIX host")
    try:
        status = path.lstat()
    except FileNotFoundError:
        pass
    else:
        if not stat.S_ISDIR(status.st_mode) or status.st_uid != os.getuid():
            raise native.BazelWorkspaceError(f"private service root is not this user's directory: {path}")
        shutil.rmtree(path)
    # A concurrent creator makes mkdir fail rather than share the root.
    path.mkdir(mode=0o700)
    path.chmod(0o700)


def _start_xvfb() -> subprocess.Popen[str]:
    try:
        return subprocess.Popen(
            ["Xvfb", "-displayfd", "1", "-screen", "0", "1280x1024x24", "-nolisten", "tcp"],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
        )
    except FileNotFoundError as exc:
        raise native.BazelWorkspaceError("GTK tests require Xvfb. Enter the project nix-shell.") from exc


def _start_gtk_bus(config: Path) -> subprocess.Popen[str]:
    try:
        return subprocess.Popen(
            ["dbus-daemon", "--nofork", f"--config-file={config}", "--print-address=1"],
            stdout=subprocess.PIPE,
            # Preserve startup diagnostics without an unread pipe.
            stderr=None,
            text=True,
        )
    except FileNotFoundError as exc:
        raise native.BazelWorkspaceError("GTK tests require dbus-daemon. Enter the project nix-shell.") from exc


def _gtk_daemon_ready(server: subprocess.Popen[str], label: str, *, timeout: float = 5) -> str:
    """Read a single startup handshake without retaining an unread pipe."""
    assert server.stdout is not None
    # Both daemons stay in the foreground and write only their startup address
    # to stdout. Killing a silent daemon closes that pipe before joining its
    # reader; stderr is separate so later diagnostics cannot fill the pipe.
    with ThreadPoolExecutor(max_workers=1) as reader:
        try:
            ready = reader.submit(server.stdout.readline)
            line: str = ready.result(timeout=timeout)
        except BaseException as exc:
            # Retire the writer before close/join can wait for the reader's lock.
            server.kill()
            server.wait()
            if isinstance(exc, TimeoutError):
                raise native.BazelWorkspaceError(
                    f"{label} did not report readiness within {timeout:g} seconds."
                ) from exc
            raise
        finally:
            server.stdout.close()
    return line.strip()


def _gtk_display_environment(display: str) -> dict[str, str]:
    # GTK may select its accessibility and input-method backends before main().
    # Set the complete headless profile rather than relying on fallback defaults.
    return {
        "DISPLAY": display,
        "AOBUS_OWNED_GTK_DISPLAY": "1",
        "GTK_A11Y": "test",
        "GTK_IM_MODULE": "simple",
        "GDK_BACKEND": "x11",
        "GDK_DISABLE": "gl,vulkan",
        "GSK_RENDERER": "cairo",
        "G_DEBUG": f"{os.environ.get('G_DEBUG', '')},fatal-warnings".lstrip(","),
    }


@contextmanager
def _private_bus_directory(ipc_root: Path | None) -> Generator[Path]:
    if ipc_root is None:
        # Unix socket names cannot accommodate an arbitrarily deep TMPDIR.
        with tempfile.TemporaryDirectory(
            prefix="aobus-gtk-bus-", dir="/tmp" if sys.platform == "linux" else None
        ) as directory:
            yield Path(directory)
        return
    recreate_private_root(ipc_root)
    try:
        yield ipc_root
    finally:
        shutil.rmtree(ipc_root, ignore_errors=True)


@contextmanager
def virtual_gtk_displays(count: int = 1, *, ipc_root: Path | None = None) -> Generator[list[dict[str, str]]]:
    """Own Xvfb and private session buses without exposing the caller's desktop.

    The gate owns both daemons beyond the test process, including GTK's retained
    process-global bus clients. Separate buses avoid competing MPRIS names.
    A stable `ipc_root` gives each bus a fixed socket and a guid-free address,
    so Bazel's hashed test environment repeats across attempts.
    """
    servers: list[subprocess.Popen[str]] = []
    with _private_bus_directory(ipc_root) as directory:
        try:
            # Partial startup must still retire every already-started daemon.
            for _ in range(count):
                servers.append(_start_xvfb())

            displays = []
            for server in servers:
                display_number = _gtk_daemon_ready(server, "Xvfb")
                if not display_number.isdecimal():
                    raise native.BazelWorkspaceError(f"Xvfb failed to start. Output: {display_number}")
                displays.append(f":{display_number}")

            def session_config(index: int) -> tuple[Path, str | None]:
                if ipc_root is None:
                    listen, address = f"unix:tmpdir={quote(directory.as_posix(), safe='/')}", None
                else:
                    address = f"unix:path={quote((directory / f'bus-{index}').as_posix(), safe='/')}"
                    listen = address
                config = directory / f"session-{index}.conf"
                # Tests must not activate host desktop services or their mounts.
                config.write_text(
                    f"<busconfig><type>session</type><listen>{listen}</listen>"
                    '<policy context="default"><allow send_destination="*"/>'
                    '<allow receive_sender="*"/><allow own="*"/></policy></busconfig>',
                    encoding="utf-8",
                )
                return config, address

            environments = []
            for index, display in enumerate(displays):
                config, fixed_address = session_config(index)
                bus = _start_gtk_bus(config)
                servers.append(bus)
                address = _gtk_daemon_ready(bus, "GTK private D-Bus")
                if not address.startswith("unix:"):
                    raise native.BazelWorkspaceError(f"GTK private D-Bus failed to start. Output: {address}")
                if fixed_address is not None:
                    if address.split(",guid=", 1)[0] != fixed_address:
                        raise native.BazelWorkspaceError(f"GTK private D-Bus listened elsewhere: {address}")
                    # Clients need no guid; dbus-daemon draws a new one per start.
                    address = fixed_address
                environments.append(
                    _gtk_display_environment(display)
                    | {"DBUS_SESSION_BUS_ADDRESS": address, "AOBUS_OWNED_GTK_BUS": address}
                )

            print(f"GTK display{'s' if count > 1 else ''}: Xvfb {' '.join(displays)} (private session buses)")
            yield environments
        finally:
            for server in servers:
                server.terminate()
            for server in servers:
                try:
                    server.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait()
                if server.stdout is not None:
                    server.stdout.close()


@contextmanager
def private_pipewire(
    log_dir: Path, session_environment: Mapping[str, str], *, runtime_root: Path | None = None
) -> Generator[dict[str, str]]:
    """Own foreground audio test services on the caller's already-owned D-Bus.

    No personal runtime/configuration is adopted, and the policy-only profile
    never starts hardware monitors. An unjoined child keeps its runtime files
    until the next attempt recreates a stable `runtime_root`; cleanup
    uncertainty fails completion without replacing a pending failure.
    """
    session = dict(session_environment)
    log_dir.mkdir(parents=True, exist_ok=True)
    # Keep Unix socket paths short even when the build tree or TMPDIR is deep.
    if runtime_root is None:
        runtime = Path(tempfile.mkdtemp(prefix="aobus-pipewire-", dir="/tmp"))
    else:
        recreate_private_root(runtime_root)
        runtime = runtime_root
    lifecycle_path = log_dir / "pipewire-lifecycle.json"
    lifecycle: dict[str, Any] = {
        "schema": 1,
        "runtime": str(runtime),
        "status": "starting",
        "cleanup": "pending",
        "processes": [],
    }
    children: list[tuple[subprocess.Popen[bytes], dict[str, Any]]] = []
    services: list[tuple[subprocess.Popen[bytes], dict[str, Any]]] = []
    cleanup_errors: list[str] = []
    cleanup_interruption: KeyboardInterrupt | SystemExit | None = None
    failure: BaseException | None = None

    def save() -> None:
        native.write_json(lifecycle_path, lifecycle)

    def observe(child: subprocess.Popen[bytes], record: dict[str, Any]) -> int | None:
        status = child.poll()
        if status is not None:
            record.update(status="exited", exit=status)
        return status

    def stop(child: subprocess.Popen[bytes], record: dict[str, Any]) -> None:
        nonlocal cleanup_interruption
        if record.get("cleanup_uncertain"):
            return
        try:
            if observe(child, record) is None:
                try:
                    child.terminate()
                except ProcessLookupError:
                    # The owned process may have exited between poll and signal.
                    pass
                record.update(status="exited", exit=child.wait(timeout=_STOP_TIMEOUT))
        except BaseException as error:
            if isinstance(error, (KeyboardInterrupt, SystemExit)) and cleanup_interruption is None:
                cleanup_interruption = error
            record.update(cleanup_uncertain=True, cleanup_error=str(error) or type(error).__name__)
            cleanup_errors.append(f"{record['name']} (pid {child.pid}): {record['cleanup_error']}")

    def require_services() -> None:
        for child, record in services:
            status = observe(child, record)
            if status is not None:
                raise native.BazelWorkspaceError(
                    f"Private {record['name']} exited {status}; diagnostics: {lifecycle_path}"
                )

    try:
        with ExitStack() as logs:
            try:
                runtime.chmod(0o700)
                directories = {}
                for name in ("home", "config", "state", "cache", "data", "tmp"):
                    path = runtime / name
                    path.mkdir(mode=0o700)
                    directories[name] = str(path)
                environment = session | {
                    "TMPDIR": directories["tmp"],
                    "XDG_RUNTIME_DIR": str(runtime),
                    "PIPEWIRE_RUNTIME_DIR": str(runtime),
                    "PIPEWIRE_REMOTE": "pipewire-0",
                    "AOBUS_REQUIRE_PIPEWIRE": "1",
                }
                # A closed child environment excludes personal PipeWire/SPA and
                # WirePlumber selectors, HOME, and system configuration overlays.
                child_environment = {
                    "PATH": os.environ.get("PATH", os.defpath),
                    "LC_ALL": "C",
                    "HOME": directories["home"],
                    "XDG_CONFIG_HOME": directories["config"],
                    "XDG_CONFIG_DIRS": directories["config"],
                    "XDG_STATE_HOME": directories["state"],
                    "XDG_CACHE_HOME": directories["cache"],
                    "XDG_DATA_HOME": directories["data"],
                    "DBUS_SESSION_BUS_ADDRESS": session["DBUS_SESSION_BUS_ADDRESS"],
                    **{
                        key: environment[key]
                        for key in ("TMPDIR", "XDG_RUNTIME_DIR", "PIPEWIRE_RUNTIME_DIR", "PIPEWIRE_REMOTE")
                    },
                }
                tools = {name: native.executable(name, nix=True) for name in ("pipewire", "wireplumber", "pw-cli")}

                def start(name: str, argv: list[str]) -> tuple[subprocess.Popen[bytes], dict[str, Any]]:
                    stdout_path = log_dir / f"{name}.stdout.log"
                    stderr_path = log_dir / f"{name}.stderr.log"
                    record: dict[str, Any] = {
                        "name": name,
                        "argv": argv,
                        "stdout": str(stdout_path),
                        "stderr": str(stderr_path),
                        "status": "starting",
                        "admission": "not-started",
                        "exit": None,
                    }
                    lifecycle["processes"].append(record)
                    save()
                    out = logs.enter_context(stdout_path.open("xb"))
                    err = logs.enter_context(stderr_path.open("xb"))
                    # Popen can spawn before interruption prevents it returning
                    # a handle. Keep this pending through handle registration,
                    # including the return-to-append interruption window.
                    record["admission"] = "pending"
                    save()
                    try:
                        child = subprocess.Popen(
                            argv, env=dict(child_environment), stdin=subprocess.DEVNULL, stdout=out, stderr=err
                        )
                    except BaseException as error:
                        record.update(status="error", error=str(error) or type(error).__name__)
                        raise
                    children.append((child, record))
                    record.update(pid=child.pid, status="running", admission="admitted")
                    save()
                    return child, record

                deadline = time.monotonic() + _READY_TIMEOUT

                def wait_ready(prefix: str, arguments: list[str], *, metadata: bool = False) -> int:
                    attempt = 0
                    while True:
                        require_services()
                        if time.monotonic() >= deadline:
                            phase = "policy" if metadata else "connection"
                            raise native.BazelWorkspaceError(
                                f"Private PipeWire {phase} was not ready within {_READY_TIMEOUT:g}s; "
                                f"diagnostics: {lifecycle_path}"
                            )
                        attempt += 1
                        probe, record = start(f"{prefix}-{attempt}", [tools["pw-cli"], *arguments])
                        probe_deadline = min(deadline, time.monotonic() + _PROBE_TIMEOUT)
                        while observe(probe, record) is None:
                            require_services()
                            remaining = probe_deadline - time.monotonic()
                            if remaining <= 0:
                                record["timed_out"] = True
                                stop(probe, record)
                                if cleanup_interruption is not None:
                                    raise cleanup_interruption
                                if cleanup_errors:
                                    raise native.BazelWorkspaceError("Private PipeWire readiness probe could not exit")
                                break
                            time.sleep(min(_POLL_INTERVAL, remaining))
                        require_services()
                        ready = record["exit"] == 0
                        if metadata:
                            output = Path(record["stdout"]).read_text(encoding="utf-8", errors="replace")
                            ready = ready and re.search(r'metadata\.name\s*=\s*"default"', output) is not None
                        save()
                        if ready and time.monotonic() < deadline:
                            return attempt
                        remaining = deadline - time.monotonic()
                        if remaining > 0:
                            time.sleep(min(_POLL_INTERVAL, remaining))

                save()
                services.append(start("pipewire", [tools["pipewire"]]))
                # WirePlumber does not retry an initial refused connection. A
                # live daemon process is not yet a listening PipeWire server.
                wait_ready("pipewire-connect-probe", ["info", "0"])
                services.append(start("wireplumber", [tools["wireplumber"], "--profile=policy"]))
                attempt = wait_ready("pipewire-probe", ["ls", "Metadata"], metadata=True)
                lifecycle.update(status="ready", readiness_attempts=attempt)
                save()
                # Callers can compose their frozen Bazel rc without sharing the
                # mapping used by already-started children or the original caller.
                yield dict(environment)
                require_services()
                lifecycle["status"] = "passed"
            finally:
                # Probes first, then policy, then daemon. Never signal by name,
                # process group, or a PID not returned by our own Popen calls.
                for child, record in reversed(children):
                    stop(child, record)
    except BaseException as error:
        failure = error
        lifecycle.update(status="failed", error=str(error) or type(error).__name__)
        if isinstance(error, SystemExit):
            lifecycle["exit"] = error.code if isinstance(error.code, int) else 1
        elif isinstance(error, KeyboardInterrupt):
            lifecycle["exit"] = 130
        raise
    finally:
        # Do not infer that a failed Popen spawned nothing, or discover/signal
        # an unknown PID. A pending admission is uncertainty even when its
        # exception bypassed the Popen handler after the call returned.
        for record in lifecycle["processes"]:
            if record.get("admission") == "pending":
                record.update(
                    admission="uncertain",
                    status="admission-uncertain",
                    cleanup_uncertain=True,
                    cleanup_error="spawn outcome unknown: no completed handle admission",
                )
                cleanup_errors.append(f"{record['name']}: {record['cleanup_error']}")
        if not cleanup_errors:
            try:
                shutil.rmtree(runtime)
            except BaseException as error:
                if isinstance(error, (KeyboardInterrupt, SystemExit)) and cleanup_interruption is None:
                    cleanup_interruption = error
                cleanup_errors.append(f"runtime removal: {error}")
        if cleanup_errors and failure is None:
            lifecycle.update(status="failed", error="Private PipeWire cleanup uncertain")
        lifecycle.update(
            cleanup="uncertain" if cleanup_errors else "complete",
            runtime_preserved=runtime.exists(),
            cleanup_errors=cleanup_errors,
        )
        try:
            save()
        except BaseException as error:
            if isinstance(error, (KeyboardInterrupt, SystemExit)) and cleanup_interruption is None:
                cleanup_interruption = error
            cleanup_errors.append(f"lifecycle receipt: {error}")
        if cleanup_errors:
            runtime_note = f"retained runtime {runtime}" if runtime.exists() else f"runtime {runtime} removed"
            diagnostic = f"Private PipeWire cleanup uncertain; {runtime_note}: {'; '.join(cleanup_errors)}"
            print(diagnostic, file=sys.stderr)
            if failure is not None:
                failure.add_note(diagnostic)
            elif cleanup_interruption is not None:
                cleanup_interruption.add_note(diagnostic)
                raise cleanup_interruption
            else:
                raise native.BazelWorkspaceError(diagnostic)
