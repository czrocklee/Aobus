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

    sink = open(log, "ab" if append else "wb") if log is not None else None
    try:
        with subprocess.Popen(
            argv,
            cwd=cwd,
            env=full_env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        ) as child:
            assert child.stdout is not None
            for line in child.stdout:
                line_str = line.decode("utf-8", errors="ignore")
                if "Fontconfig warning:" in line_str or "Fontconfig error:" in line_str:
                    continue
                sys.stdout.buffer.write(line)
                sys.stdout.buffer.flush()
                if sink is not None:
                    sink.write(line)
        return child.returncode
    finally:
        if sink is not None:
            sink.close()


def capture(argv: list[str], *, cwd: Path = PROJECT_ROOT, check: bool = True) -> str:
    """Run a command and return its stdout as text."""
    result = subprocess.run(argv, cwd=cwd, stdout=subprocess.PIPE, text=True, check=check)
    return result.stdout
