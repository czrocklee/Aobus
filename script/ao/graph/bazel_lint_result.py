"""Consume one completed lint graph gate result from Bazel runfiles.

The lint graph macro declares the gate's result and log files as runfiles of
an ordinary official py_test; this entrypoint resolves both through the
official ``python.runfiles`` library (directory or manifest layouts), replays
the gate log to the test's own output, and turns the gate's status into the
test status. No Bazel or ao command runs here: the graph's replay and gate
actions already completed before the test was scheduled.
"""

from __future__ import annotations

import json
import os
import sys
from collections.abc import Mapping, Sequence
from pathlib import Path
from typing import Any

SCHEMA = 1
_PASS, _FINDINGS, _FAILED = 0, 1, 2
_STATUSES = (_PASS, _FINDINGS, _FAILED)


def _create_runfiles(environment: Mapping[str, str]) -> Any:
    """Initialize the official runfiles library for the gate's artifacts.

    The import stays lazy so host-side unit tests do not need the Bazel
    runfiles repository on their Python path; the py_test supplies it.
    """
    from python.runfiles import Runfiles

    runfiles = Runfiles.Create(environment)
    if runfiles is None:
        raise RuntimeError("lint graph result could not initialize Bazel runfiles")
    return runfiles


def _locate(runfiles: Any, key: str) -> Path:
    path = runfiles.Rlocation(key)
    if not path:
        raise RuntimeError("lint graph gate runfile is missing from runfiles: " + key)
    resolved = Path(path)
    if not resolved.is_file():
        raise RuntimeError("lint graph gate runfile is not a file: " + key)
    return resolved


def run(arguments: Sequence[str], environment: Mapping[str, str]) -> int:
    """Interpret one completed gate result and return the test status."""
    if len(arguments) != 3:
        raise RuntimeError("lint graph result requires result, log and platform arguments")
    result_key, log_key, platform = arguments
    runfiles = _create_runfiles(environment)
    result = json.loads(_locate(runfiles, result_key).read_text(encoding="utf-8"))
    if (
        not isinstance(result, dict)
        or type(result.get("schema")) is not int
        or result["schema"] != SCHEMA
        or type(result.get("status")) is not int
        or result["status"] not in _STATUSES
        or result.get("platform") != platform
    ):
        raise ValueError("malformed completed lint graph gate result")
    sys.stdout.write(_locate(runfiles, log_key).read_text(encoding="utf-8"))
    status: int = result["status"]
    return status


def main(argv: Sequence[str] | None = None) -> int:
    """Entry point of the lint graph result py_test."""
    arguments = list(sys.argv[1:] if argv is None else argv)
    try:
        return run(arguments, dict(os.environ))
    except (OSError, RuntimeError, ValueError) as error:
        print("Cannot read lint graph gate result: " + str(error), file=sys.stderr)
        return _FAILED


if __name__ == "__main__":
    raise SystemExit(main())
