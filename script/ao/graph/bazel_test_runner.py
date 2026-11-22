"""Run one wrapped native test under owned private Linux fixtures.

The runner is the executable of the thin ``native_context_test`` wrapper: it
starts only the fixtures requested by ``AOBUS_TEST_CONTEXT`` using the
wrapper's declared fixture executables, launches the wrapped native test
executable declared in runfiles, and returns the child status. Private service
logs use Bazel's ``TEST_UNDECLARED_OUTPUTS_DIR`` when present so testlogs
retain their diagnostics. No installation, desktop bypass or remote-session
setup happens here.
"""

from __future__ import annotations

import os
import shutil
import signal
import subprocess
import sys
import tempfile
from collections.abc import Callable, Iterator, Mapping, Sequence
from contextlib import ExitStack, contextmanager
from pathlib import Path
from typing import Any

from ao.core import bazel_native as native
from ao.graph import bazel_test_context as test_context
from ao.graph import bazel_test_services as services

_EXECUTABLE_ENV = "AOBUS_TEST_EXECUTABLE"
_IPC_TMPDIR = "/tmp"


class _TestExit(SystemExit):
    """Carry a nonzero test status through fixture cleanup as the primary failure."""


class _Termination:
    def __init__(self) -> None:
        self.requested = False
        self.waiting = False

    def request_stop(self, _signum: int, _frame: Any) -> None:
        if self.requested:
            return  # Repeated SIGTERM must not interrupt teardown.
        self.requested = True
        if self.waiting:
            self.check()

    def check(self) -> None:
        if self.requested:
            raise _TestExit(128 + signal.SIGTERM)

    def wait_for(self, child: subprocess.Popen[bytes]) -> int:
        # Only this blocking wait may be interrupted; the child is already
        # owned by a cleanup scope. Startup and teardown use checkpoints.
        try:
            self.waiting = True
            self.check()
            return child.wait()
        finally:
            self.waiting = False


@contextmanager
def _linux_sigterm() -> Iterator[_Termination]:
    termination = _Termination()
    previous = signal.signal(signal.SIGTERM, termination.request_stop)
    try:
        yield termination
    finally:
        signal.signal(signal.SIGTERM, previous)


def _service_logs(environment: Mapping[str, str]) -> Path:
    """Keep service diagnostics outside the disposable IPC root."""
    directory = environment.get("TEST_UNDECLARED_OUTPUTS_DIR") or environment.get("TEST_TMPDIR")
    if not directory:
        raise RuntimeError("Bazel test runner requires TEST_UNDECLARED_OUTPUTS_DIR or TEST_TMPDIR for service logs")
    path = Path(directory) / "aobus-test-services"
    path.mkdir(parents=True, exist_ok=True)
    return path


def _fixture_executable(environment: Mapping[str, str], variable: str) -> str:
    value = environment.get(variable)
    if not value:
        raise RuntimeError(f"Linux test fixtures require {variable} to declare an absolute executable path")
    if not Path(value).is_absolute():
        raise RuntimeError(f"{variable} must be an absolute executable path: {value!r}")
    if not Path(value).is_file() or not os.access(value, os.X_OK):
        raise RuntimeError(f"{variable} must name an executable file: {value!r}")
    return value


@contextmanager
def _fixture_environment(
    selected: frozenset[str], environment: Mapping[str, str], check_cancelled: Callable[[], None]
) -> Iterator[dict[str, str]]:
    """Start only the owned fixtures requested by the Linux test context."""
    if not selected & test_context.LINUX_CONTEXTS:
        yield {}
        return
    # Validate every required declaration before starting any fixture.
    dbus_daemon = _fixture_executable(environment, "AOBUS_TEST_DBUS_DAEMON")
    xvfb = _fixture_executable(environment, "AOBUS_TEST_XVFB") if "gtk" in selected else None
    audio = None
    if selected & {"audio", "gtk"}:
        tools = services.PipeWireTools(
            pipewire=_fixture_executable(environment, "AOBUS_TEST_PIPEWIRE"),
            wireplumber=_fixture_executable(environment, "AOBUS_TEST_WIREPLUMBER"),
            pw_cli=_fixture_executable(environment, "AOBUS_TEST_PW_CLI"),
        )
        audio = tools, _service_logs(environment)

    # Bazel's per-test temp path can exceed the Unix socket limit. Keep only
    # fixture IPC here, and retire services before removing this invocation's root.
    root = Path(tempfile.mkdtemp(prefix="aobus-test-", dir=_IPC_TMPDIR))
    runtime = root / "pipewire"

    def remove_ipc_root() -> None:
        # PipeWire retains its runtime on uncertain cleanup; its receipt must
        # remain valid. The other services remove their own IPC directories.
        if not runtime.exists():
            shutil.rmtree(root)

    with services.cleanup_after(remove_ipc_root), ExitStack() as fixtures:
        root.chmod(0o700)
        if xvfb is not None:
            session = fixtures.enter_context(
                services.virtual_gtk_displays(
                    ipc_root=root / "gtk-bus", dbus_daemon=dbus_daemon, xvfb=xvfb, check_cancelled=check_cancelled
                )
            )[0]
        else:
            session = fixtures.enter_context(
                services.private_session_bus(
                    ipc_root=root / "session-bus", dbus_daemon=dbus_daemon, check_cancelled=check_cancelled
                )
            )
        if audio is not None:
            tools, logs = audio
            audio_environment = fixtures.enter_context(
                services.private_pipewire(
                    logs, session, tools=tools, runtime_root=runtime, check_cancelled=check_cancelled
                )
            )
            session = session | audio_environment
        yield session


def _run_linux(arguments: Sequence[str], environment: Mapping[str, str], selected: frozenset[str]) -> int:
    with _linux_sigterm() as termination:
        termination.check()
        with _fixture_environment(selected, environment, termination.check) as fixtures:
            with services.owned_processes() as children:
                termination.check()
                try:
                    child = subprocess.Popen(arguments, env=dict(environment) | fixtures)
                except FileNotFoundError as error:
                    print(f"Bazel test runner cannot execute {arguments[0]}: {error}", file=sys.stderr)
                    raise _TestExit(127) from error
                children.append(child)
                status = termination.wait_for(child)
                if status == -signal.SIGTERM:
                    termination.check()
                if status:
                    raise _TestExit(status)
        # SIGTERM can also arrive while a fixture is shutting down normally.
        termination.check()
    return 0


def run(arguments: Sequence[str], environment: Mapping[str, str]) -> int:
    """Run one test command and return its status."""
    if not arguments:
        raise RuntimeError("Bazel test runner requires a test executable")
    selected = test_context.contexts(environment)
    unknown = selected - test_context.ALLOWED_CONTEXTS
    if unknown:
        raise RuntimeError("unknown AOBUS_TEST_CONTEXT marker: " + ", ".join(sorted(unknown)))
    try:
        if sys.platform == "linux":
            return _run_linux(arguments, environment, selected)
        return subprocess.run(arguments, env=dict(environment)).returncode
    except _TestExit as error:
        assert isinstance(error.code, int)
        return error.code
    except FileNotFoundError as error:
        if sys.platform == "linux":
            raise  # A fixture or its files, not the wrapped executable.
        print(f"Bazel test runner cannot execute {arguments[0]}: {error}", file=sys.stderr)
        return 127
    except KeyboardInterrupt:
        return 130


def _create_runfiles(environment: Mapping[str, str]) -> Any:
    """Initialize the official runfiles library for the wrapped executable.

    The import stays lazy so host-side unit tests do not need the Bazel
    runfiles repository on their Python path; the wrapper supplies it.
    """
    from python.runfiles import Runfiles

    runfiles = Runfiles.Create(environment)
    if runfiles is None:
        raise RuntimeError("Bazel test runner could not initialize Bazel runfiles")
    return runfiles


def _resolve(runfiles: Any, key: str) -> Path:
    path = runfiles.Rlocation(key)
    if not path or not Path(path).is_file():
        raise RuntimeError("wrapped native test executable is missing from runfiles: " + key)
    return Path(path)


def main(argv: Sequence[str] | None = None) -> int:
    """Entry point of the native context test wrapper."""
    arguments = list(sys.argv[1:] if argv is None else argv)
    try:
        environment = dict(os.environ)
        key = environment.get(_EXECUTABLE_ENV)
        if not key:
            raise RuntimeError("Bazel test runner requires the wrapped test executable runfiles key")
        executable = _resolve(_create_runfiles(environment), key)
        return run([str(executable), *arguments], environment)
    except (OSError, RuntimeError, native.BazelWorkspaceError) as error:
        print(f"Bazel test runner: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
