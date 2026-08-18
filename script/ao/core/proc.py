"""Subprocess helpers shared by every command."""

import os
import subprocess
import sys
import threading
from pathlib import Path

from .paths import PROJECT_ROOT


def die(message: str, code: int = 1) -> "SystemExit":
    print(f"Error: {message}", file=sys.stderr)
    return SystemExit(code)


def run(
    argv: list[str],
    *,
    cwd: Path = PROJECT_ROOT,
    env: dict[str, str] | None = None,
    log: Path | None = None,
    append: bool = False,
) -> int:
    """Run a command, optionally teeing combined stdout/stderr to a log file.

    On Windows, child processes can spawn grandchildren that inherit the
    stdout pipe write handle.  If a grandchild does not exit promptly the pipe
    never reaches EOF and a plain ``for line in child.stdout`` blocks forever.
    A daemon reader thread drains the pipe while the main thread waits for the
    *direct* child with ``child.wait()``, which does not depend on pipe EOF.
    """
    full_env = {**os.environ, **env} if env else None

    sink = open(log, "ab" if append else "wb") if log is not None else None
    try:
        with subprocess.Popen(
            argv,
            cwd=cwd,
            env=full_env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        ) as child:
            stdout = child.stdout
            assert stdout is not None

            def _drain() -> None:
                try:
                    for line in stdout:
                        line_str = line.decode("utf-8", errors="ignore")
                        if "Fontconfig warning:" in line_str or "Fontconfig error:" in line_str:
                            continue
                        sys.stdout.buffer.write(line)
                        sys.stdout.buffer.flush()
                        if sink is not None:
                            sink.write(line)
                except (OSError, ValueError):
                    pass  # stdout closed while draining after child exit

            reader = threading.Thread(target=_drain, daemon=True)
            reader.start()
            child.wait()
            reader.join(timeout=5)
            return child.returncode
    finally:
        if sink is not None:
            sink.close()


def capture(argv: list[str], *, cwd: Path = PROJECT_ROOT, check: bool = True) -> str:
    """Run a command and return its stdout as text."""
    result = subprocess.run(argv, cwd=cwd, stdout=subprocess.PIPE, text=True, check=check)
    return result.stdout
