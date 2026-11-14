"""Runner for Python hygiene checks used by ao tidy and the tooling test suite."""

import os
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import IO

from .paths import PROJECT_ROOT, absolute_path
from .proc import die, run

DEFAULT_TARGETS = ("script/ao", "app/cli/generate_test_library.py", "test/script", "bazel/coverage")


@dataclass(frozen=True)
class Check:
    label: str
    argv: tuple[str, ...]


def module_command(
    module: str,
    *arguments: str,
    python: Path | None = None,
    os_name: str | None = None,
    platform_name: str | None = None,
) -> tuple[str, ...]:
    """Run a Python module, isolating managed native tools from ambient packages.

    An explicit managed interpreter runs the module in place of the portal's own.
    """
    resolved_os_name = os.name if os_name is None else os_name
    resolved_platform = sys.platform if platform_name is None else platform_name
    isolated = ("-I",) if resolved_os_name == "nt" or resolved_platform in ("win32", "darwin") else ()
    return (sys.executable if python is None else str(python), *isolated, "-m", module, *arguments)


def mypy_command(*arguments: str, python: Path | None = None, platform_name: str | None = None) -> tuple[str, ...]:
    """Keep native mypy caches in the managed environment, not the source checkout."""
    cache = _local_cache_option(".mypy_cache", python=python, platform_name=platform_name)
    return module_command("mypy", *cache, *arguments, python=python, platform_name=platform_name)


def ruff_command(
    subcommand: str, *arguments: str, python: Path | None = None, platform_name: str | None = None
) -> tuple[str, ...]:
    """Keep native Ruff check and format caches in the managed environment."""
    cache = _local_cache_option(".ruff_cache", python=python, platform_name=platform_name)
    return module_command("ruff", subcommand, *cache, *arguments, python=python, platform_name=platform_name)


def _local_cache_option(name: str, *, python: Path | None = None, platform_name: str | None = None) -> tuple[str, ...]:
    resolved_platform = sys.platform if platform_name is None else platform_name
    if resolved_platform not in ("win32", "darwin"):
        return ()
    # A managed interpreter is <environment>/bin/python or <environment>/Scripts/python.exe.
    environment = Path(sys.prefix) if python is None else python.parent.parent
    return ("--cache-dir", str(environment / name))


def checks(paths: list[str], *, python: Path | None = None) -> tuple[Check, ...]:
    targets = tuple(paths or DEFAULT_TARGETS)
    mypy_targets = tuple(target for target in targets if not _is_test_script_path(target))
    result = [Check("Ruff", ruff_command("check", *targets, python=python))]
    if mypy_targets:
        result.append(Check("mypy", mypy_command(*mypy_targets, python=python)))
    return tuple(result)


def _is_test_script_path(target: str) -> bool:
    path = Path(target)
    if path.is_absolute():
        try:
            target = absolute_path(path).relative_to(absolute_path(PROJECT_ROOT)).as_posix()
        except ValueError:
            target = path.as_posix()
    else:
        target = path.as_posix()
    return target == "test/script" or target.startswith("test/script/")


def run_paths(
    paths: list[str], *, python: Path | None = None, sink: IO[str] | None = None, log: Path | None = None
) -> int:
    """Run all checks; diagnostics go to `sink` when given, else to the console (teed to `log`)."""
    status = 0
    for check in checks(paths, python=python):
        print("=====================================")
        print(f"Running {check.label}")
        print(f"CMD: {' '.join(check.argv)}")
        print("=====================================")
        try:
            if sink is None:
                code = run(list(check.argv), log=log, append=log is not None)
            else:
                completed = subprocess.run(
                    check.argv, cwd=PROJECT_ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True
                )
                sink.write(completed.stdout)
                code = completed.returncode
        except FileNotFoundError as exc:
            raise die(f"Python environment is unavailable: {check.argv[0]}") from exc
        if code != 0:
            status = 1

    return status
