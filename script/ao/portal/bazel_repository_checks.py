"""Bazel checks that read the live checkout, not the test's runfiles copy."""

from __future__ import annotations

import argparse
import atexit
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from collections.abc import Mapping, MutableMapping, Sequence
from functools import partial
from pathlib import Path

TEST_MODULE_DIR = Path("test/script")
_LIVE_FLAG = "--live-checkout"
_TOOL_MANIFEST_FLAG = "--check-tool-manifest"
_ENTRY_PATH = "script/ao/portal/bazel_repository_checks.py"
type _ToolSelection = tuple[str, ...] | str  # Command or a provider's unavailability diagnostic.

# A direct script invocation has no package context; add the checkout's script
# directory so the ao package resolves without a caller-provided PYTHONPATH.
if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).absolute().parents[2]))

# The shared live-checkout companion owns the physical-root primitive, so the
# validation role and this entry read the same checkout boundary.
from ao.graph.bazel_live_checkout import PROJECT_ROOT_ENV, project_root  # noqa: E402


def host_platform(platform: str | None = None) -> str:
    """Map a Python platform name to the guardrails host vocabulary."""
    value = sys.platform if platform is None else platform
    return {"linux": "linux", "win32": "windows", "darwin": "macos"}.get(value, value)


def _test_tmpdir(environ: Mapping[str, str]) -> Path | None:
    """Return the test-private temporary root, creating it when Bazel set it."""
    value = environ.get("TEST_TMPDIR")
    if not value:
        return None
    path = Path(value).resolve()
    path.mkdir(parents=True, exist_ok=True)
    return path


def _short_windows_test_root(environment: MutableMapping[str, str]) -> None:
    """Replace a long Windows TEST_TMPDIR with a short per-test platform temp.

    Windows hosts without LongPathsEnabled still cap paths at MAX_PATH. Bazel's
    output-base TEST_TMPDIR is deep, and a fixture name can push a child path
    over the limit. A private directory under the platform temp keeps the
    fixture tree short without changing the build/state tree or Linux behavior.
    """
    if sys.platform != "win32" or not environment.get("TEST_TMPDIR"):
        return
    platform_temp = Path(environment.get("TEMP") or environment.get("TMP") or tempfile.gettempdir())
    try:
        root = Path(tempfile.mkdtemp(prefix="aobus-test-", dir=platform_temp)).resolve()
    except OSError:
        return
    atexit.register(shutil.rmtree, root, ignore_errors=True)
    environment["TEST_TMPDIR"] = str(root)


def _prepare_runtime(environ: MutableMapping[str, str] | None = None) -> Path | None:
    """Redirect temporary files, bytecode, and tool caches into TEST_TMPDIR.

    Products canonicalize the paths they select, so the test root is resolved
    before it is exported to children. Without a Bazel test root nothing is
    redirected and the caller keeps the ambient platform defaults. Windows
    shortens the root before exporting it so MAX_PATH-limited fixtures stay
    inside the platform temp.
    """
    environment = os.environ if environ is None else environ
    _short_windows_test_root(environment)
    temporary = _test_tmpdir(environment)
    if temporary is None:
        return None
    for name in ("TMPDIR", "TEMP", "TMP"):
        environment[name] = str(temporary)
    bytecode = temporary / "pycache"
    sys.pycache_prefix = str(bytecode)
    environment["PYTHONPYCACHEPREFIX"] = str(bytecode)
    # Ruff and mypy honor these when no explicit cache option is passed.
    environment["RUFF_CACHE_DIR"] = str(temporary / "ruff-cache")
    environment["MYPY_CACHE_DIR"] = str(temporary / "mypy-cache")
    return temporary


def _declared_outputs_dir(environ: Mapping[str, str] | None = None) -> Path:
    """Select the directory that receives the guardrails receipt."""
    environment = os.environ if environ is None else environ
    declared = environment.get("TEST_UNDECLARED_OUTPUTS_DIR")
    if declared:
        return Path(declared)
    temporary = _test_tmpdir(environment)
    if temporary is not None:
        return temporary / "guardrails"
    return Path(tempfile.gettempdir()) / "aobus-guardrails"


def _is_tooling_test(target: str) -> bool:
    """Match the tooling tests that the project mypy configuration excludes."""
    return target == TEST_MODULE_DIR.as_posix() or target.startswith(TEST_MODULE_DIR.as_posix() + "/")


def _run_tool(label: str, argv: Sequence[str], root: Path) -> int:
    """Run one external tool from the checkout root and normalize its status."""
    print(f"Running {label}: {' '.join(argv)}")
    try:
        completed = subprocess.run(argv, cwd=root)
    except FileNotFoundError as error:
        print(f"{label} is unavailable: {error}", file=sys.stderr)
        return 1
    if completed.returncode != 0:
        print(f"{label} failed with status {completed.returncode}.", file=sys.stderr)
        return 1
    return 0


def run_docs(*, root: Path | None = None) -> int:
    """Run the documentation integrity check over the physical checkout.

    The root guard makes a runfiles-tree mismatch fail loudly instead of
    silently checking the wrong tree. Shared services own the semantics.
    """
    from ao.core.paths import PROJECT_ROOT
    from ao.portal import documentation_checks

    expected = project_root() if root is None else Path(root)
    if PROJECT_ROOT != expected:
        print(f"Documentation check root mismatch: {PROJECT_ROOT} != {expected}", file=sys.stderr)
        return 1
    try:
        return documentation_checks.check()
    except ImportError as error:
        print(f"Documentation check requires a declared Python dependency: {error}", file=sys.stderr)
        return 1


def run_guardrails(*, root: Path | None = None, output_dir: Path | None = None, platform: str | None = None) -> int:
    """Run the complete lexical source audit inventory without source edits."""
    from ao.portal import guardrails

    base = project_root() if root is None else Path(root)
    output = _declared_outputs_dir() if output_dir is None else Path(output_dir)
    try:
        result = guardrails.run(base, output, host_platform(platform))
    except (guardrails.AuditInputError, OSError, re.error) as error:
        print(f"Guardrails audit failed: {error}", file=sys.stderr)
        return 1
    for finding in (*result.errors, *result.warnings):
        print(finding)
    print(f"Guardrails report: {result.report_path}")
    return result.exit_code


def run_ruff(targets: Sequence[str] | None = None, *, tool: Path, root: Path | None = None) -> int:
    """Run Ruff check over the repository tooling scope."""
    from ao.portal import python_check

    base = project_root() if root is None else Path(root)
    selected = list(targets) if targets else list(python_check.DEFAULT_TARGETS)
    _prepare_runtime()
    argv = (str(tool), "check", "--target-version", python_check.ruff_target_version(), *selected)
    return _run_tool("Ruff", argv, base)


def run_mypy(targets: Sequence[str] | None = None, *, tool: Path, root: Path | None = None) -> int:
    """Run mypy over the repository tooling scope, excluding tooling tests."""
    from ao.portal import python_check

    base = project_root() if root is None else Path(root)
    selected = list(targets) if targets else list(python_check.DEFAULT_TARGETS)
    selected = [target for target in selected if not _is_tooling_test(target)]
    _prepare_runtime()
    if not selected:
        return 0
    # The Nix wrapper owns its immutable closure. On Windows and macOS the
    # declared interpreter runs only the repository's hash-locked wheels.
    prefix = (str(tool),) if sys.platform == "linux" else (str(tool), "-I", "-B", "-m", "mypy")
    argv = (*prefix, "--python-version", python_check.governed_python_version(), *selected)
    return _run_tool("mypy", argv, base)


def run_format(
    *,
    docs: bool,
    root: Path | None = None,
    commands: Mapping[str, _ToolSelection],
    ruff: Path | None = None,
) -> int:
    """Check the docs inventory or its complement in whole-source formatting."""
    from ao.portal import documentation_checks, source_format, source_scope

    if _physical_root(root, "format") is None:
        return 1
    scope = documentation_checks.markdown_scope()
    if not docs:
        # Markdown fixtures outside the docs inventory still need formatting.
        doc_paths = set(scope.files)
        whole = source_scope.resolve(source_scope.Selection(all=True))
        scope = source_scope.SourceScope(tuple(name for name in whole.files if name not in doc_paths))
    _prepare_runtime()
    return source_format.check(scope, command_for=partial(_command_for, commands), ruff=ruff)


def _physical_root(root: Path | None, label: str) -> Path | None:
    """Return the physical checkout or report a runfiles/physical mismatch."""
    from ao.core.paths import PROJECT_ROOT

    expected = project_root() if root is None else Path(root)
    if PROJECT_ROOT != expected:
        print(f"{label} root mismatch: {PROJECT_ROOT} != {expected}", file=sys.stderr)
        return None
    return expected


def run_test_audit(*, root: Path | None = None) -> int:
    """Run the Catch2 naming and tag audit over the physical checkout."""
    from ao.portal import source_lint

    if _physical_root(root, "test-audit") is None:
        return 1
    return source_lint.audit_tests()


def run_name_audit(*, root: Path | None = None) -> int:
    """Run the class and file naming audit over the physical checkout."""
    from ao.portal import source_lint

    if _physical_root(root, "name-audit") is None:
        return 1
    return source_lint.audit_names()


def _workflow_files(root: Path) -> list[str]:
    """List the canonical direct GitHub workflow YAML files under the checkout."""
    from ao.portal import git_files

    directory = root / ".github" / "workflows"
    if not directory.is_dir():
        return []
    return sorted(
        path.relative_to(root).as_posix()
        for path in directory.rglob("*")
        if path.is_file() and git_files.is_workflow_file(path.relative_to(root).as_posix())
    )


def run_workflow_lint(*, tool: str, root: Path | None = None) -> int:
    """Lint the repository GitHub workflows with the pinned actionlint."""
    from ao.portal import source_lint

    expected = _physical_root(root, "workflow-lint")
    if expected is None:
        return 1
    return source_lint.check_workflows(_workflow_files(expected), tool=tool)


def run_bazel_lint(*, tool: str, root: Path | None = None) -> int:
    """Lint the full Bazel/Starlark scope with the pinned Buildifier.

    The formatter pass and the Buildifier lint verdict are independent: the
    complete graph can format cleanly while a naming or API convention finding
    remains. The full formatter inventory supplies the authoritative Bazel file
    set, and the ready-only selection never installs a missing tool.
    """
    from ao.portal import buildifier, git_files, source_scope

    expected = _physical_root(root, "bazel-lint")
    if expected is None:
        return 1
    scope = source_scope.resolve(source_scope.Selection(all=True))
    bazel_files = [name for name in scope.files if git_files.is_bazel_file(name) and source_scope.file_exists(name)]
    try:
        return buildifier.lint_paths(bazel_files, provision=False, tool=tool)
    except buildifier.BuildifierError as error:
        print(f"Bazel lint failed: {error}", file=sys.stderr)
        return 1


def _declared_commands(paths: Sequence[str], *names: str) -> dict[str, _ToolSelection]:
    commands: dict[str, _ToolSelection] = {}
    for path in paths:
        with Path(path).open(encoding="utf-8") as stream:
            fact = json.load(stream)
        if not isinstance(fact, dict):
            raise ValueError(f"invalid declared check tool: {path}")
        name = fact.get("name")
        if name not in names or name in commands:
            raise ValueError(f"invalid declared check tool: {path}")
        command, reason = fact.get("command"), fact.get("unavailable")
        if set(fact) == {"name", "unavailable"} and isinstance(reason, str) and reason.strip():
            commands[name] = reason
        elif (
            set(fact) == {"name", "command"}
            and isinstance(command, list)
            and len(command) in ({1, 2} if name == "prettier" else {1})
            and all(isinstance(item, str) and "\0" not in item and Path(item).is_absolute() for item in command)
        ):
            commands[name] = tuple(command)
        else:
            raise ValueError(f"invalid declared check tool: {path}")
    if missing := set(names) - commands.keys():
        raise ValueError(f"missing declared check tools: {', '.join(sorted(missing))}")
    return commands


def _command_for(commands: Mapping[str, _ToolSelection], name: str) -> tuple[str, ...]:
    selected = commands[name]
    if isinstance(selected, str):
        raise ValueError(selected)
    return selected


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(_TOOL_MANIFEST_FLAG, action="append", default=[])
    parser.add_argument("--ruff", type=Path)
    parser.add_argument("--mypy", type=Path)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("docs", help="check documentation integrity")
    guardrails = commands.add_parser("guardrails", help="run the lexical source audits")
    guardrails.add_argument("--platform")
    ruff = commands.add_parser("ruff", help="run Ruff over the repository tooling scope")
    ruff.add_argument("targets", nargs="*")
    mypy = commands.add_parser("mypy", help="run mypy over the repository tooling scope")
    mypy.add_argument("targets", nargs="*")
    commands.add_parser("format-sources", help="check source formatting outside the docs inventory")
    commands.add_parser("format-docs", help="check the documentation Markdown inventory")
    commands.add_parser("test-audit", help="run the Catch2 naming and tag audit")
    commands.add_parser("name-audit", help="run the class/file naming audit")
    commands.add_parser("workflow-lint", help="lint the GitHub workflow YAML")
    commands.add_parser("bazel-lint", help="lint the full Bazel/Starlark scope")
    return parser


def _live_reexec(arguments: Sequence[str]) -> tuple[int | None, list[str]]:
    """Re-exec the physical entry before any service import.

    Live targets pass the declared ``VERSION.txt`` anchor as
    ``--live-checkout``. The checks always re-execute this very entry from
    the physical checkout and pin the child's project root there before any
    service module is imported: Windows copies runfiles, and even on POSIX
    the already loaded runfiles ``ao`` packages would shadow the physical
    checkout. The child gets the plain command arguments without the flag,
    which is the loop guard. No loaded package is patched.

    Returns the process status together with the arguments without the
    anchor pair; ``None`` means the caller dispatches in place.
    """
    if _LIVE_FLAG not in arguments:
        return None, list(arguments)
    from ao.graph import bazel_live_checkout

    anchor, remaining = bazel_live_checkout.take_anchor(list(arguments))
    root = bazel_live_checkout.physical_root(anchor, os.environ) if anchor else None
    if root is None:
        print(f"the live checkout anchor could not be resolved: {anchor}", file=sys.stderr)
        return 1, remaining
    # The re-execution is unconditional: the child gets the plain command
    # arguments without the flag, so the loop closes without any loaded-
    # package patching. A path-equality guard would be wrong on POSIX, where
    # the runfiles entry file is a physical symlink while the already loaded
    # ``ao`` packages still point into the runfiles tree; the child's absent
    # flag is the only guard.
    environment = dict(os.environ)
    environment[PROJECT_ROOT_ENV] = str(root)
    for index, value in enumerate(remaining[:-1]):
        if value in (_TOOL_MANIFEST_FLAG, "--ruff", "--mypy"):
            location = remaining[index + 1]
            path = bazel_live_checkout.declared_file(location, environment)
            if path is None:
                print(f"declared {value} runfile could not be resolved: {location}", file=sys.stderr)
                return 1, remaining
            remaining[index + 1] = str(path)
    return bazel_live_checkout.reexec(root, _ENTRY_PATH, remaining, environment=environment), remaining


def main(argv: Sequence[str] | None = None) -> int:
    """Dispatch one repository check from the target's declared arguments."""
    raw = sys.argv[1:] if argv is None else list(argv)
    live_status, remaining = _live_reexec(raw)
    if live_status is not None:
        return live_status
    arguments = _parser().parse_args(remaining)
    if arguments.command == "docs":
        return run_docs()
    if arguments.command == "guardrails":
        return run_guardrails(platform=arguments.platform)
    if arguments.command in ("ruff", "mypy"):
        tool = arguments.ruff if arguments.command == "ruff" else arguments.mypy
        if tool is None or not tool.is_absolute():
            print(f"A declared {arguments.command} executable is required", file=sys.stderr)
            return 1
        run = run_ruff if arguments.command == "ruff" else run_mypy
        return run(arguments.targets, tool=tool)
    if arguments.command == "test-audit":
        return run_test_audit()
    if arguments.command == "name-audit":
        return run_name_audit()
    manifests = arguments.check_tool_manifest
    docs = arguments.command == "format-docs"
    try:
        if arguments.command == "workflow-lint":
            return run_workflow_lint(tool=_command_for(_declared_commands(manifests, "actionlint"), "actionlint")[0])
        if arguments.command == "bazel-lint":
            return run_bazel_lint(tool=_command_for(_declared_commands(manifests, "buildifier"), "buildifier")[0])
        tools = ("prettier",) if docs else ("clang-format", "buildifier", "prettier")
        commands = _declared_commands(manifests, *tools)
    except (OSError, ValueError, KeyError, TypeError, AttributeError) as error:
        print(f"Repository check tool selection failed: {error}", file=sys.stderr)
        return 1
    if not docs and (arguments.ruff is None or not arguments.ruff.is_absolute()):
        print("An absolute declared Ruff executable is required for source formatting", file=sys.stderr)
        return 1
    return run_format(docs=docs, commands=commands, ruff=arguments.ruff)


if __name__ == "__main__":
    raise SystemExit(main())
