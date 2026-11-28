"""Launch an existing native executable from Bazel runfiles.

The py_test wrapper passes the executable's runfiles-relative path as its first
argument. This module resolves that path with the official rules_python
runfiles library, forwards the remaining arguments and environment, and returns
the child status. UIA tests use the existing WinUI runtime/session admission.
"""

from __future__ import annotations

import os
import subprocess
import sys
from collections.abc import Mapping, Sequence
from pathlib import Path
from typing import Any

from ao.graph import bazel_test_context as test_context

_BATCH_SUFFIXES = frozenset({".bat", ".cmd"})


def _require_uia() -> None:
    if sys.platform != "win32":
        raise RuntimeError("UI Automation requires Windows")
    # The declared py_library owns the admission modules in runfiles; Windows
    # can copy runfiles. These are the real Windows App Runtime and session
    # checks. Do not replace them with SESSIONNAME/USERNAME guesses; the
    # native probe checks again.
    from ao.portal import winui

    winui.require_runtime()
    winui.require_interactive_session()


def _create_runfiles(environment: Mapping[str, str]) -> Any:
    # Import lazily so host-side unit tests do not need the Bazel runfiles
    # repository on their Python path. The py_test dependency supplies it.
    from python.runfiles import Runfiles

    runfiles = Runfiles.Create(environment)
    if runfiles is None:
        raise RuntimeError("native test smoke could not initialize Bazel runfiles")
    return runfiles


def _resolve(runfiles: Any, key: str) -> Path:
    path = runfiles.Rlocation(key)
    if not path:
        raise RuntimeError("native test executable is missing from runfiles: " + key)
    executable = Path(path)
    if not executable.is_file():
        raise RuntimeError("native test executable is not a file: " + str(executable))
    return executable


def _command(executable: Path, arguments: Sequence[str], environment: Mapping[str, str]) -> list[str] | str:
    if sys.platform == "win32" and executable.suffix.lower() in _BATCH_SUFFIXES:
        # cmd.exe owns batch parsing. list2cmdline quotes the resolved runfile
        # path and arguments; the outer pair lets /s strip exactly one layer.
        command = subprocess.list2cmdline([str(executable), *arguments])
        comspec = environment.get("COMSPEC") or "cmd.exe"
        return f'"{comspec}" /d /s /c "{command}"'
    return [str(executable), *arguments]


def run(arguments: Sequence[str], environment: Mapping[str, str] | None = None) -> int:
    """Resolve and launch one native executable from runfiles."""
    if not arguments:
        raise RuntimeError("native test smoke requires a runfiles executable path")
    current_environment = dict(os.environ if environment is None else environment)
    if test_context.UIA_CONTEXT in test_context.contexts(current_environment):
        _require_uia()
    runfiles = _create_runfiles(current_environment)
    executable = _resolve(runfiles, arguments[0])
    child_environment = dict(current_environment)
    child_environment.update(runfiles.EnvVars())
    command = _command(executable, arguments[1:], child_environment)
    try:
        completed = subprocess.run(command, env=child_environment)
    except FileNotFoundError as error:
        print(f"Native test smoke cannot execute {executable}: {error}", file=sys.stderr)
        return 127
    except KeyboardInterrupt:
        return 130
    return completed.returncode


def main(argv: Sequence[str] | None = None) -> int:
    """Entry point for the generated py_test."""
    arguments = list(sys.argv[1:] if argv is None else argv)
    try:
        return run(arguments, dict(os.environ))
    except (OSError, RuntimeError, ValueError) as error:
        print(f"Native test smoke: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
