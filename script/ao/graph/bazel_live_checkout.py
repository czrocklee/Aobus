"""Physical-checkout ownership for live Bazel test targets.

Windows copies runfiles into the test tree, so a target that must read the
live checkout (host-backed unit modules, repository checks, and native
policy validation) cannot trust ``paths.PROJECT_ROOT`` inside runfiles. This
module owns the shared root primitive: the declared ``VERSION.txt`` anchor is
resolved to the physical checkout with the official rules_python runfiles
library, an explicit override wins for diagnostics, and the module location
is the last-resort fallback.

Live targets pass the anchor as a ``--live-checkout`` argument. The entry
resolves it and re-executes the very same entry script from that checkout;
the child sees a physical ``__file__`` and runs in place, so the loop closes
without any loaded-package patching.

The helper stays standard-library only: importing it never widens a target's
closure beyond the official runfiles library, and a target that never passes
the flag never imports the re-execution half at all.
"""

from __future__ import annotations

import os
import subprocess
import sys
from collections.abc import Mapping, Sequence
from pathlib import Path

ANCHOR_FLAG = "--live-checkout"
VERSION_ANCHOR = "VERSION.txt"
PROJECT_ROOT_ENV = "AOBUS_BAZEL_TEST_PROJECT_ROOT"
# bzlmod names the main repository _main; other layouts use the workspace name.
_ANCHOR_LOCATIONS = ("_main/" + VERSION_ANCHOR, "aobus/" + VERSION_ANCHOR, VERSION_ANCHOR)


def _official_path(location: str, environ: Mapping[str, str]) -> Path | None:
    """Resolve one runfiles location to a physical file path.

    The official library owns both layouts: the POSIX symlink tree and the
    Windows manifest, whose entries keep pointing at the physical checkout
    even when the tree itself is a copied snapshot. The location is resolved
    for the main repository explicitly; never from the caller's stack frames.
    """
    try:
        from python.runfiles import runfiles as runfiles_api
    except ImportError:
        return None
    try:
        instance = runfiles_api.Runfiles.Create(dict(environ))
    except OSError, ValueError:
        return None
    if instance is None:
        return None
    resolved = instance.Rlocation(location, source_repo="")
    if resolved and Path(resolved).is_file():
        return Path(resolved).resolve()
    return None


def physical_root(anchor: str, environ: Mapping[str, str]) -> Path | None:
    """Resolve one anchor argument to the physical checkout root.

    ``$(rlocationpath //:VERSION.txt)`` passes the runfiles-relative location
    of the declared ``VERSION.txt``; a caller that already holds a physical
    path passes it through unchanged. An anchor that cannot be resolved
    returns ``None`` so the entry fails closed instead of silently checking
    the wrong tree.
    """
    candidate = Path(anchor)
    if candidate.is_file():
        return candidate.resolve().parent
    physical = _official_path(anchor, environ)
    if physical is not None:
        return physical.parent
    return None


def project_root(environ: Mapping[str, str] | None = None) -> Path:
    """Return the physical checkout root that live readers use.

    An explicit override wins for diagnostics, then the official runfiles
    library resolves the declared ``VERSION.txt``. Without a resolvable
    runfiles anchor, use the module location, not a marker in the caller's
    working directory. Live entries with an explicit anchor use
    :func:`physical_root` and fail if that anchor cannot be resolved.
    """
    environment = os.environ if environ is None else environ
    override = environment.get(PROJECT_ROOT_ENV)
    if override:
        return Path(override).resolve()
    for location in _ANCHOR_LOCATIONS:
        anchor = _official_path(location, environment)
        if anchor is not None:
            return anchor.parent
    return Path(__file__).resolve().parents[3]


def take_anchor(arguments: Sequence[str]) -> tuple[str | None, list[str]]:
    """Split one ``--live-checkout ANCHOR`` pair from the entry arguments."""
    remaining = list(arguments)
    for index, value in enumerate(remaining):
        if value == ANCHOR_FLAG and index + 1 < len(remaining):
            anchor = remaining[index + 1]
            del remaining[index : index + 2]
            return anchor, remaining
    return None, remaining


def reexec(
    root: Path,
    entry: str,
    arguments: Sequence[str],
    *,
    environment: Mapping[str, str] | None = None,
) -> int:
    """Run the physical entry script once and return the child status.

    ``entry`` is the checkout-relative path of the entry script
    (``script/ao/graph/bazel_unittest.py``), fully specified by its caller:
    the live entries sit in different packages, so there is no shared entry
    directory and no basename mapping. An absolute path or one that escapes
    the checkout fails closed instead of running a different script.

    The child runs from the physical checkout with the checkout's ``script``
    and ``test/script`` directories first on ``PYTHONPATH``; the inherited
    value stays appended so an official tool environment's site dependencies
    keep working. Everything else (``TEST_TMPDIR``, tool variables) is
    inherited unchanged: the child entry owns the bounded temp setup.
    """
    child_environment = dict(os.environ if environment is None else environment)
    relative = Path(entry)
    if relative.is_absolute() or ".." in relative.parts:
        print(f"live checkout entry must be a checkout-relative script path: {entry}", file=sys.stderr)
        return 1
    script = root / relative
    if not script.is_file():
        print(f"live checkout entry script not found: {script}", file=sys.stderr)
        return 1
    search = [str(root / "script"), str(root / "test" / "script")]
    inherited = child_environment.get("PYTHONPATH")
    if inherited:
        search.append(inherited)
    child_environment["PYTHONPATH"] = os.pathsep.join(search)
    try:
        completed = subprocess.run([sys.executable, str(script), *arguments], cwd=root, env=child_environment)
    except OSError as error:
        print(f"live checkout re-execution failed: {error}", file=sys.stderr)
        return 1
    return completed.returncode
