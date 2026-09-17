"""Runner for unit tests that validate the ao development tooling."""

import os
import re
import subprocess
import sys
from pathlib import Path

from . import compiler_cache, pythoncheck, workspace_cache
from .paths import PROJECT_ROOT

TEST_COUNT_RE = re.compile(r"Ran (\d+) tests?")


def run(*, log: Path | None = None) -> int:
    # Ruff and mypy are read-only, so they can gate the tooling suite without touching files.
    static_status = pythoncheck.run_paths([], log=log)
    env = dict(os.environ)
    for key in (
        compiler_cache.SHARED_WORKSPACES_EFFECTIVE,
        compiler_cache.SHARED_WORKSPACES_CCACHE,
        compiler_cache.SHARED_WORKSPACES_NAMESPACE_PREFIX,
        compiler_cache.SHARED_WORKSPACES_FALLBACK,
        compiler_cache.SHARED_WORKSPACES_MSBUILD_WRAPPER,
        workspace_cache.WINDOWS_SOURCE_VIEW,
        workspace_cache.WINDOWS_BUILD_VIEW,
        workspace_cache.WINDOWS_BUILD_PHYSICAL_ROOT,
        "CCACHE_NAMESPACE",
        "CCACHE_BASEDIR",
        "CCACHE_HASHDIR",
        "CCACHE_SLOPPINESS",
    ):
        env.pop(key, None)
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

    return result.returncode or static_status
