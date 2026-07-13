"""Runner for unit tests that validate the ao development tooling."""

import os
import re
import subprocess
import sys
from pathlib import Path

from . import doccheck, pythoncheck
from .paths import PROJECT_ROOT

TEST_COUNT_RE = re.compile(r"Ran (\d+) tests?")


def run(*, log: Path | None = None) -> int:
    # Ruff and mypy are read-only, so they can gate the tooling suite without touching files.
    static_status = pythoncheck.run_paths([], log=log)
    documentation_issues = doccheck.check_tree()
    documentation_status = 1 if documentation_issues else 0

    if documentation_issues:
        lines = [issue.format() for issue in documentation_issues]
        print("Documentation checks failed.")
        print("\n".join(lines))
        if log is not None:
            with log.open("a", encoding="utf-8") as sink:
                sink.write("\n".join(lines) + "\n")

    env = dict(os.environ)
    script_dir = str(PROJECT_ROOT / "script")
    env["PYTHONPATH"] = script_dir + (os.pathsep + env["PYTHONPATH"] if env.get("PYTHONPATH") else "")
    result = subprocess.run(
        [sys.executable, "-m", "unittest", "discover", "-s", "test/script", "-q"],
        cwd=PROJECT_ROOT,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )

    if log is not None:
        with log.open("a", encoding="utf-8") as sink:
            sink.write(result.stdout)

    if result.returncode == 0:
        match = TEST_COUNT_RE.search(result.stdout)
        count = f" ({match.group(1)} tests)" if match else ""
        print(f"Tooling tests passed{count}.")
    else:
        print("Tooling tests failed.")
        print(result.stdout, end="" if result.stdout.endswith("\n") else "\n")

    return result.returncode or static_status or documentation_status
