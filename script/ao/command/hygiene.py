"""ao hygiene - the check-only commit gate: format, naming audits, and tidy checks.

Deliberately never modifies files: rewriting sources mid-session disturbs in-flight work,
and most clang-tidy findings have no safe auto-fix. The gate reports; fixes are applied
explicitly (./ao format for formatting, manual edits for lint findings).

Stop on formatting failure before collecting diagnostics whose locations would
be invalidated by formatting. Every stage uses the same resolved source scope.
"""

import argparse
import sys
from collections.abc import Callable
from pathlib import Path

from ..core import bazel_native, bazel_workflows, buildenv, gitfiles, testaudit, tidyengine
from ..core.proc import die
from . import format as format_command
from . import name_audit, test_audit

HELP = "Run the commit gate: format --check, naming audits, then tidy (check-only, never edits files)"
NAME = "hygiene"
# True when ao.bat must initialize the MSVC/vcpkg build environment first.
REQUIRES_BUILD_ENV = True
REQUIRES_PYTHON_TOOLS = True


EPILOG = """\
With no paths, checks files changed since the merge base with local main, plus
working tree, staged, and untracked files. Empty stage subsets are skipped.

examples:
  ./ao hygiene
  ./ao hygiene --all
  ./ao hygiene script/ao/command/hygiene.py
"""

Register = Callable[["argparse._SubParsersAction[argparse.ArgumentParser]"], None]
# Receives the resolved scope and whether its files were explicit; returns a status.
LintStage = Callable[[list[str], bool], int]


def register(subparsers: "argparse._SubParsersAction[argparse.ArgumentParser]") -> None:
    parser = subparsers.add_parser(
        NAME, help=HELP, description=HELP, epilog=EPILOG, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("files", nargs="*", metavar="file", help="explicit files to check")
    parser.add_argument("--all", action="store_true", help="check every source in the project folders")
    parser.add_argument(
        "--folder", action="append", default=[], metavar="<dir>", help="all files under <dir> (repeatable)"
    )
    parser.add_argument("--commit", metavar="<rev>", help="changed files since <rev> + working tree + untracked")
    parser.add_argument("-p", "--path", metavar="<dir>", help="external Bazel analysis tree (honors BUILD_DIR)")
    parser.add_argument("--config", choices=("debug", "release"), default="debug")
    parser.add_argument("--vcpkg-prefix", help="existing governed native SDK prefix")
    parser.add_argument("-j", "--jobs", type=int, default=tidyengine.default_jobs(), help="parallel jobs")
    bazel_workflows.add_remote_cache_argument(parser, "opt in to a trusted native action cache")
    parser.set_defaults(func=run_command)


def _subcommand_defaults(register_command: Register, name: str, **overrides: object) -> argparse.Namespace:
    """Parse the subcommand's real defaults so hygiene can never drift from its parser."""
    parser = argparse.ArgumentParser()
    register_command(parser.add_subparsers())
    namespace = parser.parse_args([name])
    for key, value in overrides.items():
        setattr(namespace, key, value)
    return namespace


def _format_args() -> argparse.Namespace:
    return _subcommand_defaults(
        format_command.register,
        "format",
        check=True,
    )


def _test_audit_args(paths: list[str]) -> argparse.Namespace:
    return _subcommand_defaults(test_audit.register, "test-audit", paths=paths, fail_on_issue=True)


def _name_audit_args(paths: list[str]) -> argparse.Namespace:
    return _subcommand_defaults(name_audit.register, "name-audit", paths=paths, fail_on_issue=True)


def requires_build_environment(args: argparse.Namespace) -> bool:
    return buildenv.requires_source_build_env(args, format_command.resolve_files(args))


def _run_native(args: argparse.Namespace) -> int:
    """Select ready native tools and share one source scope across every stage."""
    import os

    from ..core import builddir, pythonenv, windows_llvm_sdk
    from ..core.paths import PROJECT_ROOT
    from . import tidy

    files = format_command.resolve_files(args)
    args._resolved_sources = files
    cpp_files, python_files = tidy.split_existing(files)
    host = bazel_native.native_platform()
    clang_format = None
    if cpp_files and host == "windows":
        manifest = windows_llvm_sdk.read_manifest(windows_llvm_sdk.MANIFEST)
        clang_format = str(windows_llvm_sdk.ready_root(os.environ, manifest)[0] / "bin/clang-format.exe")
    python = None
    if python_files and host != "linux":
        state = builddir.state_root(system="Windows" if host == "windows" else "Darwin")
        try:
            python = pythonenv.ensure_environment(PROJECT_ROOT, state, platform_name=host)
        except RuntimeError as exc:
            raise bazel_native.BazelWorkspaceError(f"Aobus native Python tooling bootstrap failed: {exc}") from exc

    def lint(files: list[str], explicit: bool) -> int:
        return tidy.run_lint_stages(args, files, explicit, python=python)

    return run_command(args, clang_format=clang_format, python=python, lint=("bazel lint", lint))


def run_command(
    args: argparse.Namespace,
    *,
    clang_format: str | None = None,
    python: Path | None = None,
    lint: tuple[str, LintStage] | None = None,
) -> int:
    """Run the gate; native setup supplies the verified tools and lint callback."""
    if lint is None:
        try:
            return _run_native(args)
        except (bazel_native.BazelWorkspaceError, OSError, ValueError) as error:
            raise die(str(error)) from error
    files = format_command.resolve_files(args)
    if not files:
        print("No files to check.")
        return 0

    print("=== format --check ===")
    build_dir = Path(args.path) if args.path else None
    formatted = format_command.run_command(
        _format_args(), files=files, build_dir=build_dir, clang_format=clang_format, python=python
    )
    if formatted != 0:
        print("Hygiene stopped after formatting failure; this gate is check-only.", file=sys.stderr)
        print("Run ./ao format on the same scope, review the diff, then rerun ./ao hygiene.", file=sys.stderr)
        return 1

    cpp_files = [name for name in files if name.endswith(gitfiles.CPP_SUFFIXES) and format_command._file_exists(name)]
    # Content discovery, not the registration filename convention: every
    # candidate test-tree file reaches the audit, including headers, include
    # fragments, and benchmark sources such as TrackTableBenchmark.cpp.
    test_files = [name for name in cpp_files if testaudit.is_audit_candidate(name)]

    print()
    print("=== test-audit ===")
    test_audit_failed = bool(test_files) and test_audit.run_command(_test_audit_args(test_files)) != 0
    if not test_files:
        print("No applicable test files in scope.")

    print()
    print("=== name-audit ===")
    name_audit_failed = bool(cpp_files) and name_audit.run_command(_name_audit_args(cpp_files)) != 0
    if not cpp_files:
        print("No C++ files in scope.")

    print()
    print(f"=== {lint[0]} ===")
    tidy_failed = lint[1](files, bool(args.files)) != 0

    if not (test_audit_failed or name_audit_failed or tidy_failed):
        return 0

    print("Hygiene issues found. This gate is check-only; fix and re-run.", file=sys.stderr)
    if tidy_failed:
        print("  - Lint findings: formatting is already clean, so line numbers are stable -", file=sys.stderr)
        print("    fix the findings manually, re-run scoped validation, then ./ao hygiene.", file=sys.stderr)

    if test_audit_failed:
        print(
            "  - Test names/tags: fix the reported TEST_CASE names or tags, then rerun "
            "./ao test-audit --fail-on-issue.",
            file=sys.stderr,
        )
    if name_audit_failed:
        print(
            "  - Class/file names: fix the reported role or catch-all names, then rerun "
            "./ao name-audit --fail-on-issue.",
            file=sys.stderr,
        )
    return 1
