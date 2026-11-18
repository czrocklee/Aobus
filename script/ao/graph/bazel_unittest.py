"""Minimal standard-library unittest entry for one Bazel test target.

The entry runs exactly one test module in the current interpreter and fails
when that module collects no tests, so a broken import or a renamed test can
never let a target pass silently. It imports nothing from the ``ao`` package
at module level: ordinary unit targets keep the narrow declared closure of
their own sources and library dependencies, and Bazel owns isolation and
parallelism.

Host-backed targets pass the declared ``VERSION.txt`` anchor as
``--live-checkout``. The entry always re-executes itself from the physical
checkout then, because Windows copies runfiles while those modules read the
live tree, and even on POSIX the already loaded runfiles ``ao`` packages would
shadow the physical checkout. The child receives the plain arguments without
the flag, which is the loop guard; no loaded package is patched. Cacheable
targets never pass the flag and run straight from their declared runfiles.

The only runtime adjustment is a small, bounded temporary-root setup. On
Windows, Bazel's ``TEST_TMPDIR`` lies deep in the output base while hosts
without LongPathsEnabled still cap paths at MAX_PATH, so a private short
directory under the platform temp keeps fixture trees usable; bytecode goes
to the same test root instead of the shared sources. POSIX keeps Bazel's own
``TEST_TMPDIR`` untouched.
"""

from __future__ import annotations

import atexit
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

USAGE = "usage: bazel_unittest [--live-checkout ANCHOR] MODULE"

_LIVE_FLAG = "--live-checkout"
# The physical child is this entry, fully specified from the checkout root.
_ENTRY_PATH = "script/ao/graph/bazel_unittest.py"


def _short_windows_test_root() -> None:
    """Point this process at a short private temp root on Windows hosts.

    The replacement stays bounded to the running test: one directory under
    the platform temp, removed at exit, changing no build or system state.
    """
    if sys.platform != "win32":
        return
    value = os.environ.get("TEST_TMPDIR")
    if not value:
        return
    platform_temp = os.environ.get("TEMP") or os.environ.get("TMP") or tempfile.gettempdir()
    try:
        root = Path(tempfile.mkdtemp(prefix="aobus-test-", dir=platform_temp)).resolve()
    except OSError:
        return
    atexit.register(shutil.rmtree, root, ignore_errors=True)
    for name in ("TEST_TMPDIR", "TMP", "TEMP"):
        os.environ[name] = str(root)


def _prepare_test_root() -> None:
    """Apply the bounded test-root adjustments for the current platform."""
    _short_windows_test_root()
    temporary = os.environ.get("TEST_TMPDIR")
    if temporary:
        # Keep bytecode out of the shared source tree, in or out of runfiles.
        sys.pycache_prefix = os.path.join(temporary, "pycache")


def _live_reexec(arguments: list[str]) -> int | None:
    """Re-exec the physical entry for a live target; ``None`` runs in place.

    The flag is rare, so the live companion module is imported only here:
    cacheable targets never pay for it. A missing flag returns ``None`` and
    the caller keeps its arguments; an unresolvable anchor fails closed.

    The re-execution is unconditional when the flag is present: the child
    receives the plain arguments without the flag, so the loop closes without
    any loaded-package patching. A path-equality guard would be wrong on
    POSIX, where the runfiles entry file is a symlink to the physical file
    while the already loaded ``ao`` packages still point into the runfiles
    tree; only the child's absent flag proves the physical checkout is in
    use.
    """
    if _LIVE_FLAG not in arguments:
        return None
    from ao.graph import bazel_live_checkout

    anchor, remaining = bazel_live_checkout.take_anchor(arguments)
    root = bazel_live_checkout.physical_root(anchor, os.environ) if anchor else None
    if root is None:
        print(f"the live checkout anchor could not be resolved: {anchor}", file=sys.stderr)
        return 1
    return bazel_live_checkout.reexec(root, _ENTRY_PATH, remaining)


def _export_search_path() -> None:
    """Let child interpreters import the ao package and support modules.

    Several unit modules spawn Python children that import ``ao``; the child
    environment needs the same search paths the entry itself resolved from
    its own runfiles location, without resolving symlinks into the physical
    checkout. The inherited value stays appended so an official tool
    environment's site dependencies keep working.
    """
    script = Path(__file__).absolute().parents[2]
    search = [str(script), str(script.parent / "test" / "script")]
    inherited = os.environ.get("PYTHONPATH")
    if inherited:
        search.append(inherited)
    os.environ["PYTHONPATH"] = os.pathsep.join(search)


def main(argv: list[str] | None = None) -> int:
    """Run one test module and return a process status."""
    arguments = sys.argv[1:] if argv is None else list(argv)
    live_status = _live_reexec(arguments)
    if live_status is not None:
        return live_status
    if len(arguments) != 1:
        print(USAGE, file=sys.stderr)
        return 2
    module = arguments[0]
    _prepare_test_root()
    _export_search_path()
    try:
        program = unittest.main(module=module, argv=["unittest", "-q"], exit=False)
    except Exception as error:  # a broken import must fail the target, not abort it
        print(f"{module} could not be imported: {error}", file=sys.stderr)
        return 1
    result = program.result
    if result.testsRun == 0:
        print(f"{module} ran no tests; a test target must execute at least one test", file=sys.stderr)
        return 1
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
