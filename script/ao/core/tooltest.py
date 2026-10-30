"""Runner for unit tests that validate the ao development tooling."""

import os
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from . import compiler_cache, phases, pythoncheck, workspace_cache
from .jobs import default_workers
from .paths import PROJECT_ROOT

TEST_DIR = PROJECT_ROOT / "test" / "script"
TEST_COUNT_RE = re.compile(r"Ran (\d+) tests?")
SKIPPED_RE = re.compile(r"skipped=(\d+)")
# unittest exits with 5 when a module collects no tests; discovery accepted such modules.
NO_TESTS_RAN = 5


def test_modules() -> list[str]:
    return sorted(path.stem for path in TEST_DIR.glob("test_*.py"))


def _run_modules(modules: list[str], env: dict[str, str]) -> list[subprocess.CompletedProcess[str]]:
    # Each module gets its own interpreter: tests patch process-wide state, and the
    # few modules that drive real compilers and CMake dominate a serial run.
    def run_module(module: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, "-m", "unittest", "-q", module],
            cwd=PROJECT_ROOT,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )

    workers = max(1, min(default_workers(), len(modules)))
    with ThreadPoolExecutor(max_workers=workers) as pool:
        return list(pool.map(run_module, modules))


def _count(pattern: re.Pattern[str], output: str) -> int:
    match = pattern.search(output)
    return int(match.group(1)) if match else 0


def run(*, log: Path | None = None) -> int:
    # Static checks do not rewrite sources, but the tools may update their caches.
    static_status = pythoncheck.run_paths([], log=log)
    env = dict(os.environ)
    # Unit modules invoke mocked portal operations. Keep their synthetic phases
    # out of the real command trace; profiling tests opt into private temp logs.
    for key in (
        phases.LOG_DIR_ENV,
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
    search_path = [str(PROJECT_ROOT / "script"), str(TEST_DIR)]
    if env.get("PYTHONPATH"):
        search_path.append(env["PYTHONPATH"])
    env["PYTHONPATH"] = os.pathsep.join(search_path)

    modules = test_modules()
    results = _run_modules(modules, env)

    if log is not None:
        with log.open("a", encoding="utf-8") as sink:
            for module, result in zip(modules, results, strict=True):
                sink.write(f"== {module}\n{result.stdout}")

    failed = [
        (module, result)
        for module, result in zip(modules, results, strict=True)
        if result.returncode not in (0, NO_TESTS_RAN)
    ]
    if not failed:
        tests = sum(_count(TEST_COUNT_RE, result.stdout) for result in results)
        skipped = sum(_count(SKIPPED_RE, result.stdout) for result in results)
        detail = f"{tests} tests, {skipped} skipped" if skipped else f"{tests} tests"
        print(f"Tooling tests passed ({detail}).")
        return static_status

    print("Tooling tests failed.")
    for module, result in failed:
        print(f"== {module}")
        print(result.stdout, end="" if result.stdout.endswith("\n") else "\n")
    return failed[0][1].returncode
