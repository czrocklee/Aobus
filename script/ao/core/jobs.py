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


def export_runner_cores(*, github_env: Path) -> None:
    """Validate the runner's core count, then append the CI worker exports.

    The count is validated before the first byte is appended, so a failed
    detection never leaves a partial contract behind in the step environment.
    """
    cores = available_cores()
    if cores is None or cores < 1:
        raise ValueError(f"the runner core count was reported as {cores!r}; expected a positive integer")
    with github_env.open("a", encoding="utf-8", newline="\n") as stream:
        stream.write(f"{BUILD_PARALLEL_ENV}={cores}\n")
    print(f"Detected {cores} runner cores: Bazel and tidy jobs={cores}.")


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
