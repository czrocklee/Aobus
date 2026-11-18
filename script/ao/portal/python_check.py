"""Runner for Python semantic checks used by source lint and tooling tests."""

import os
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import IO

from ao.core.paths import PROJECT_ROOT, absolute_path
from ao.core.proc import run
from ao.portal.python_env import tool_versions

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


def _governed_python() -> str:
    """The governed toolchain Python as ``major.minor``, for tool target options."""
    major, minor, _ = tool_versions()["python"].split(".")
    return f"{major}.{minor}"


def mypy_command(*arguments: str, python: Path | None = None, platform_name: str | None = None) -> tuple[str, ...]:
    """Keep native mypy caches in the managed environment, not the source checkout."""
    cache = _local_cache_option(".mypy_cache", python=python, platform_name=platform_name)
    target = ("--python-version", _governed_python())
    return module_command("mypy", *cache, *target, *arguments, python=python, platform_name=platform_name)


def ruff_command(
    subcommand: str, *arguments: str, python: Path | None = None, platform_name: str | None = None
) -> tuple[str, ...]:
    """Keep native Ruff check and format caches in the managed environment."""
    cache = _local_cache_option(".ruff_cache", python=python, platform_name=platform_name)
    target = ("--target-version", "py" + _governed_python().replace(".", ""))
    return module_command("ruff", subcommand, *cache, *target, *arguments, python=python, platform_name=platform_name)


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


def _require_tool(module: str, *, python: Path | None = None) -> None:
    """Execute module startup under the selected interpreter before admitting exit 1.

    Python itself uses exit 1 for missing modules and broken imports. A real
    version invocation, not package discovery, establishes ready-tool admission.
    Pins remain owned by python_env; this probe never installs or repairs tools.
    """
    if module not in ("ruff", "mypy"):
        raise RuntimeError(f"unsupported Python checker: {module}")
    expected = tool_versions()[module]
    completed = subprocess.run(
        module_command(module, "--version", python=python),
        cwd=PROJECT_ROOT,
        stdin=subprocess.DEVNULL,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="strict",
        timeout=30,
        check=False,
    )
    banner = rf"{module} {re.escape(expected)}(?:[ \t]+[^\r\n]+)?\r?\n?"
    if completed.returncode or completed.stderr or re.fullmatch(banner, completed.stdout) is None:
        detail = (completed.stderr or completed.stdout).strip()
        raise RuntimeError(
            f"{module} is not ready under {module_command(module, python=python)[0]} "
            f"(expected {module} {expected}, probe exit {completed.returncode}): {detail}"
        )


def run_paths(
    paths: list[str], *, python: Path | None = None, sink: IO[str] | None = None, log: Path | None = None
) -> int:
    """Return 0 for clean checks, 1 for findings, 2 for invocation/tool failure.

    Ruff and mypy document exit 1 for findings only after ready-tool admission
    under the selected interpreter. Failed startup/version probes and other
    nonzero exits (including signals) must remain failures in report-only mode.
    Diagnostics go to `sink`, or the console (teed to `log`).
    """
    try:
        selected = checks(paths, python=python)
    except (OSError, ValueError, RuntimeError, KeyError) as exc:
        print(f"Cannot select Python checks: {exc}", file=sys.stderr if sink is None else sink)
        return 2
    status = 0
    for check in selected:
        print("=====================================")
        print(f"Running {check.label}")
        print(f"CMD: {' '.join(check.argv)}")
        print("=====================================")
        try:
            _require_tool(check.label.lower(), python=python)
            if sink is None:
                code = run(list(check.argv), log=log, append=log is not None)
            else:
                completed = subprocess.run(
                    check.argv, cwd=PROJECT_ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True
                )
                sink.write(completed.stdout)
                code = completed.returncode
        except (OSError, ValueError, RuntimeError, KeyError, subprocess.SubprocessError) as exc:
            print(f"Cannot run {check.label}: {exc}", file=sys.stderr if sink is None else sink)
            code = 2
        status = max(status, 0 if code == 0 else 1 if code == 1 else 2)

    return status
