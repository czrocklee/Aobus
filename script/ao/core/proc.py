"""Subprocess helpers shared by every command."""

import codecs
import os
import selectors
import subprocess
import sys
import time
from pathlib import Path
from typing import BinaryIO, TextIO

from .paths import PROJECT_ROOT


def die(message: str, code: int = 1) -> SystemExit:
    print(f"Error: {message}", file=sys.stderr)
    return SystemExit(code)


_SUPPRESSED_MARKERS = ("Fontconfig warning:", "Fontconfig error:")


def is_suppressed_output(line: bytes) -> bool:
    """Return whether a child output line is noise the portal hides."""
    text = line.decode("utf-8", errors="ignore")
    return any(marker in text for marker in _SUPPRESSED_MARKERS)


def run(
    argv: list[str],
    *,
    cwd: Path = PROJECT_ROOT,
    env: dict[str, str] | None = None,
    log: Path | BinaryIO | None = None,
    append: bool = False,
    stdout: BinaryIO | None = None,
    console: TextIO | None = None,
) -> int:
    """Run a command with an exact environment when one is supplied.

    Normally tee filtered combined output to stdout and an optional log. With
    ``stdout`` supplied, preserve that stream separately and tee raw stderr
    immediately to the log and ``console`` (default stderr), even without a
    newline. Streams supplied by the caller remain caller-owned and must
    outlive this call; ``append`` applies only to Path logs.

    Drain output until EOF, allowing at most five seconds after the direct
    child exits for descendants that inherited its pipe. The calling thread
    owns the unbuffered, nonblocking reader and closes it before returning;
    no background reader can retain the pipe or write to a closed log. On an
    exception, discard further output and defer propagation until the direct
    child exits, including across repeated interrupts. Cancellation can wait
    for the tool's natural completion; joining its builders remains tool-owned.
    POSIX status checks avoid Popen's interrupt-sensitive wait lock. A reaped
    child whose exit status was lost is an error, never a successful exit.
    """
    separate = stdout is not None
    pipe_name = "stderr" if separate else "stdout"
    owns_sink = isinstance(log, Path)
    sink = open(log, "ab" if append else "wb") if isinstance(log, Path) else log
    selector = selectors.DefaultSelector() if os.name != "nt" else None
    display = sys.stderr if console is None else console
    decoder = codecs.getincrementaldecoder("utf-8")(errors="replace")

    def emit_raw(chunk: bytes, *, final: bool = False) -> None:
        if sink is not None and chunk:
            sink.write(chunk)
            sink.flush()
        text = decoder.decode(chunk, final=final)
        if text:
            display.write(text)
            display.flush()

    def emit(line: bytes) -> None:
        if not is_suppressed_output(line):
            sys.stdout.buffer.write(line)
            sys.stdout.buffer.flush()
            if sink is not None:
                sink.write(line)

    status_unavailable: ChildProcessError | None = None

    def poll(child: subprocess.Popen[bytes]) -> int | None:
        nonlocal status_unavailable
        if status_unavailable is not None:
            child.returncode = 1
            raise status_unavailable
        if sys.platform == "win32":
            return child.poll()
        if child.returncode is None:
            try:
                pid, status = os.waitpid(child.pid, os.WNOHANG)
            except ChildProcessError as error:
                # An interrupt can reap the child before Python records status.
                status_unavailable = ChildProcessError(f"exit status unavailable for owned child {child.pid}")
                # Nonzero destructor-settlement sentinel only: poll always raises
                # for this state, so it is never returned as an observed exit.
                child.returncode = 1
                raise status_unavailable from error
            if pid:
                child.returncode = os.waitstatus_to_exitcode(status)
        return child.returncode

    def wait(child: subprocess.Popen[bytes]) -> int:
        if sys.platform == "win32":
            return child.wait()
        while (status := poll(child)) is None:
            time.sleep(0.1)
        return status

    def drain(child: subprocess.Popen[bytes], *, discard: bool = False) -> None:
        pipe = getattr(child, pipe_name)
        assert pipe is not None
        pending = b""
        drain_deadline = None
        idle_delay = 0.01
        while True:
            chunk = pipe.read(65536)
            if chunk == b"":
                break
            if chunk:
                idle_delay = 0.01
                if not discard:
                    if separate:
                        emit_raw(chunk)
                    else:
                        lines = (pending + chunk).split(b"\n")
                        pending = lines.pop()
                        for line in lines:
                            emit(line + b"\n")
            if poll(child) is not None:
                if drain_deadline is None:
                    drain_deadline = time.monotonic() + 5
                if time.monotonic() >= drain_deadline:
                    break
            if chunk is None:
                if selector is not None and not discard:
                    # Bound process-status checks while the kernel waits for output.
                    selector.select(timeout=0.25)
                else:
                    # Windows pipes and exceptional cleanup use the owned
                    # reader without depending on a possibly failed selector.
                    time.sleep(idle_delay)
                    idle_delay = min(idle_delay * 2, 0.1)
        if separate and not discard:
            emit_raw(b"", final=True)
        elif pending:
            emit(pending)

    def settle(child: subprocess.Popen[bytes]) -> None:
        # Popen.__exit__ only briefly waits after KeyboardInterrupt. Own the
        # returned child before any pipe setup, not through that context.
        draining = True
        while True:
            try:
                if draining:
                    pipe = getattr(child, pipe_name)
                    if pipe is not None:
                        try:
                            # Early setup may have been interrupted before this.
                            os.set_blocking(pipe.fileno(), False)
                            drain(child, discard=True)
                        except OSError, ValueError:
                            # A broken/closed reader cannot drain. Close our
                            # read end, but still own the child until it exits.
                            try:
                                pipe.close()
                            except OSError, ValueError:
                                pass
                            setattr(child, pipe_name, None)
                    draining = False
                wait(child)
                return
            except KeyboardInterrupt:
                continue
            except ChildProcessError:
                if status_unavailable is None:
                    raise
                # ECHILD confirms there is no child left to join. Keep the
                # original pending exception instead of inventing an exit code.
                return

    child = None
    try:
        child = subprocess.Popen(
            argv,
            cwd=cwd,
            env=env,
            stdout=stdout if separate else subprocess.PIPE,
            stderr=subprocess.PIPE if separate else subprocess.STDOUT,
            bufsize=0,
        )
        pipe = getattr(child, pipe_name)
        assert pipe is not None
        os.set_blocking(pipe.fileno(), False)
        if selector is not None:
            selector.register(pipe, selectors.EVENT_READ)
        drain(child)
        return wait(child)
    finally:
        unwinding = sys.exception() is not None
        if child is not None and child.returncode is None:
            settle(child)
        reader = getattr(child, pipe_name) if child is not None else None
        for resource in (reader, selector, sink if owns_sink else None):
            if resource is not None:
                try:
                    resource.close()
                except OSError:
                    # A failed log flush must not replace the pending exception.
                    if not unwinding:
                        raise


def capture(argv: list[str], *, cwd: Path = PROJECT_ROOT, check: bool = True) -> str:
    """Run a command and return its stdout as text."""
    result = subprocess.run(argv, cwd=cwd, stdout=subprocess.PIPE, text=True, check=check)
    return result.stdout
