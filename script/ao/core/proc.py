"""Subprocess helpers shared by every command."""

import os
import selectors
import subprocess
import sys
import time
from pathlib import Path

from .paths import PROJECT_ROOT


def die(message: str, code: int = 1) -> "SystemExit":
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
    log: Path | None = None,
    append: bool = False,
) -> int:
    """Run a command, optionally teeing combined stdout/stderr to a log file.

    Drain output until EOF, allowing at most five seconds after the direct
    child exits for descendants that inherited its pipe. The calling thread
    owns the unbuffered, nonblocking reader and closes it before returning;
    no background reader can retain the pipe or write to a closed log.
    """
    full_env = {**os.environ, **env} if env else None

    sink = open(log, "ab" if append else "wb") if log is not None else None
    selector = selectors.DefaultSelector() if os.name != "nt" else None
    try:
        with subprocess.Popen(
            argv,
            cwd=cwd,
            env=full_env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            bufsize=0,
        ) as child:
            stdout = child.stdout
            assert stdout is not None

            os.set_blocking(stdout.fileno(), False)
            if selector is not None:
                selector.register(stdout, selectors.EVENT_READ)

            def emit(line: bytes) -> None:
                if not is_suppressed_output(line):
                    sys.stdout.buffer.write(line)
                    sys.stdout.buffer.flush()
                    if sink is not None:
                        sink.write(line)

            pending = b""
            drain_deadline = None
            idle_delay = 0.01
            while True:
                chunk = stdout.read(65536)
                if chunk == b"":
                    break
                if chunk:
                    idle_delay = 0.01
                    lines = (pending + chunk).split(b"\n")
                    pending = lines.pop()
                    for line in lines:
                        emit(line + b"\n")
                if child.poll() is not None:
                    if drain_deadline is None:
                        drain_deadline = time.monotonic() + 5
                    if time.monotonic() >= drain_deadline:
                        break
                if chunk is None:
                    if selector is not None:
                        # Bound process-status checks while the kernel waits for output.
                        selector.select(timeout=0.25)
                    else:
                        # Windows selectors cannot wait on anonymous pipes. Back off
                        # while quiet and reset immediately when output resumes.
                        time.sleep(idle_delay)
                        idle_delay = min(idle_delay * 2, 0.1)
            if pending:
                emit(pending)
            return child.wait()
    finally:
        if selector is not None:
            selector.close()
        if sink is not None:
            sink.close()


def capture(argv: list[str], *, cwd: Path = PROJECT_ROOT, check: bool = True) -> str:
    """Run a command and return its stdout as text."""
    result = subprocess.run(argv, cwd=cwd, stdout=subprocess.PIPE, text=True, check=check)
    return result.stdout
