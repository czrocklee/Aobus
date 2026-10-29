"""Shared worker defaults and pre-bootstrap CI exports.

Stdlib-only: CI executes this file before the portal environment exists.
Consumers retain explicit overrides and per-stage caps. Defaults allow one
worker when the count is unknown; the CI export fails instead of guessing.
"""

from __future__ import annotations

import argparse
import os
import sys
from collections.abc import Mapping, Sequence
from pathlib import Path

# The pinned CI provider sets this automatically in every workflow step.
CI_ENV = "GITHUB_ACTIONS"
BUILD_PARALLEL_ENV = "CMAKE_BUILD_PARALLEL_LEVEL"
SHARD_ENV = "AOBUS_TEST_SHARDS"
# Shards own fixtures, temporary trees and sometimes X servers, unlike compiler jobs.
# Core-suite measurements improved 34.0s -> 23.7s at 8 -> 16 shards on a 16-vCPU
# Windows guest, and 8.6s -> 5.9s at 8 -> 31 on 32-core Linux: 16 is a process
# budget, not a measured throughput optimum. AOBUS_TEST_SHARDS overrides it.
SHARD_CAP = 16


def available_cores() -> int | None:
    """Return the affinity-aware logical core count, or None when unknown."""
    if hasattr(os, "sched_getaffinity"):
        return len(os.sched_getaffinity(0))
    return os.cpu_count()


def default_workers(*, environ: Mapping[str, str] | None = None) -> int:
    """Return the default worker baseline: every core in CI, one reserved locally."""
    environment = os.environ if environ is None else environ
    cores = available_cores() or 0
    if environment.get(CI_ENV) == "true":
        return max(cores, 1)
    return max(cores - 1, 1)


def build_workers(*, environ: Mapping[str, str] | None = None) -> int:
    """Resolve a positive build override, otherwise use the shared default baseline."""
    environment = os.environ if environ is None else environ
    configured = environment.get(BUILD_PARALLEL_ENV)
    if configured is None:
        return default_workers(environ=environment)
    try:
        count = int(configured)
    except ValueError:
        raise ValueError(f"{BUILD_PARALLEL_ENV} must be a positive integer.") from None
    if count < 1:
        raise ValueError(f"{BUILD_PARALLEL_ENV} must be a positive integer.")
    return count


def export_runner_cores(*, github_env: Path) -> None:
    """Validate the runner's core count, then append the CI worker exports.

    The count is validated before the first byte is appended, so a failed
    detection never leaves a partial contract behind in the step environment.
    """
    cores = available_cores()
    if cores is None or cores < 1:
        raise ValueError(f"the runner core count was reported as {cores!r}; expected a positive integer")
    shards = min(cores, SHARD_CAP)
    with github_env.open("a", encoding="utf-8", newline="\n") as stream:
        stream.write(f"{BUILD_PARALLEL_ENV}={cores}\n{SHARD_ENV}={shards}\n")
    print(f"Detected {cores} runner cores: build and tidy jobs={cores}, test shards={shards}.")


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Aobus worker-baseline policy helper")
    subparsers = parser.add_subparsers(dest="command", required=True)
    export = subparsers.add_parser("export-ci", help="append the runner core exports for the pinned CI provider")
    export.add_argument("--github-env", type=Path, required=True)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        export_runner_cores(github_env=args.github_env)
        return 0
    except (OSError, ValueError) as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
