"""Subprocess helpers shared by every command."""

import os
import subprocess
import sys
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
    """Run a command, optionally teeing combined stdout/stderr to a log file."""
    full_env = {**os.environ, **env} if env else None

    if log is None:
        return subprocess.call(argv, cwd=cwd, env=full_env)

    with open(log, "ab" if append else "wb") as sink:
        with subprocess.Popen(argv, cwd=cwd, env=full_env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT) as child:
            assert child.stdout is not None
            for line in child.stdout:
                sys.stdout.buffer.write(line)
                sys.stdout.buffer.flush()
                sink.write(line)
        return child.returncode


def capture(argv: list[str], *, cwd: Path = PROJECT_ROOT, check: bool = True) -> str:
    """Run a command and return its stdout as text."""
    result = subprocess.run(argv, cwd=cwd, stdout=subprocess.PIPE, text=True, check=check)
    return result.stdout
