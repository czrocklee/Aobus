"""Runner for Python hygiene checks used by ao tidy and the tooling test suite."""

import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import IO

from .paths import PROJECT_ROOT
from .proc import die, run

DEFAULT_TARGETS = ("script/ao", "app/cli/generate_test_library.py", "test/script")


@dataclass(frozen=True)
class Check:
    label: str
    argv: tuple[str, ...]


def checks(paths: list[str]) -> tuple[Check, ...]:
    targets = tuple(paths or DEFAULT_TARGETS)
    mypy_targets = tuple(target for target in targets if not _is_test_script_path(target))
    result = [Check("Ruff", ("ruff", "check", *targets))]
    if mypy_targets:
        result.append(Check("mypy", ("mypy", *mypy_targets)))
    return tuple(result)


def _is_test_script_path(target: str) -> bool:
    path = Path(target)
    if path.is_absolute():
        try:
            target = path.resolve().relative_to(PROJECT_ROOT.resolve()).as_posix()
        except ValueError:
            target = path.as_posix()
    else:
        target = path.as_posix()
    return target == "test/script" or target.startswith("test/script/")


def run_paths(paths: list[str], *, sink: IO[str] | None = None, log: Path | None = None) -> int:
    """Run all checks; diagnostics go to `sink` when given, else to the console (teed to `log`)."""
    status = 0
    for check in checks(paths):
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
            raise die(f"{check.argv[0]} not found. Enter the project shell with ./ao or nix-shell.") from exc
        if code != 0:
            status = 1

    return status
