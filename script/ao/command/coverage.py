"""ao coverage — gcov line coverage for the C++ sources, with uncovered-line context.

Unlike the old shell flow, gcov reports for the same source are merged across all object
files (union of executed lines), so a header exercised by several translation units is
reported once with its true combined coverage.
"""

import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

from ..core import builddir, buildlock, compiler_cache, phases, workspace_cache
from ..core.paths import PROJECT_ROOT, absolute_path
from ..core.proc import capture, die, run
from . import build
from .test import SUITE_TARGETS, run_catch2_suite

HELP = "Build with --coverage, run tests, and report uncovered lines per source file"
NAME = "coverage"
# True when ao.bat must initialize the MSVC/vcpkg build environment first.
REQUIRES_BUILD_ENV = False


def requires_build_environment(_args: argparse.Namespace) -> bool:
    return builddir.platform_profile().name == "linux"


EPILOG = """\
examples:
  ./ao coverage                          # core and backend suites, full report
  ./ao coverage "SmartListEvaluator*"    # coverage for a test subset
  ./ao coverage --core                   # core suite only
  ./ao coverage --backend --scope lib/audio
  ./ao coverage --cli --scope app/cli
  ./ao coverage --tui --scope app/tui
  ./ao coverage --gtk "validUtf8Text*"   # GTK suite with a case-name filter
  ./ao coverage --gtk --scope app/linux-gtk
"""

REPORTED_TOP_DIRS = ("app", "lib", "include")
YELLOW, GREEN, RESET = "\033[33m", "\033[32m", "\033[0m"


def register(subparsers: "argparse._SubParsersAction[argparse.ArgumentParser]") -> None:
    parser = subparsers.add_parser(
        NAME, help=HELP, description=HELP, epilog=EPILOG, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("filter", nargs="?", default="", help="Catch2 test filter")
    suite = parser.add_mutually_exclusive_group()
    suite.add_argument(
        "--suite",
        choices=("core", "backend", "tui", "cli", "gtk", "all"),
        default=None,
        help="test suite (default: core and backend)",
    )
    suite.add_argument("--core", dest="suite", action="store_const", const="core", help="shortcut for --suite core")
    suite.add_argument(
        "--backend", dest="suite", action="store_const", const="backend", help="shortcut for --suite backend"
    )
    suite.add_argument("--tui", dest="suite", action="store_const", const="tui", help="shortcut for --suite tui")
    suite.add_argument("--cli", dest="suite", action="store_const", const="cli", help="shortcut for --suite cli")
    suite.add_argument("--gtk", dest="suite", action="store_const", const="gtk", help="shortcut for --suite gtk")
    suite.add_argument("--all", dest="suite", action="store_const", const="all", help="shortcut for --suite all")
    parser.add_argument("-p", "--path", metavar="<dir>", help=f"build directory (default: {builddir.COVERAGE_DIR})")
    parser.add_argument("-j", "--jobs", type=int, default=os.cpu_count(), help="parallel jobs (default: nproc)")
    parser.add_argument(
        "--scope",
        action="append",
        metavar="<prefix>",
        help="only report source files under this repository-relative prefix; may be repeated",
    )
    parser.add_argument(
        "--summary-limit",
        type=int,
        default=20,
        help="number of lowest-coverage files to include in scoped summaries (default: 20)",
    )
    parser.set_defaults(func=run_command)


def coverage_cxx_flags() -> str:
    """GCC 15 emits spurious mismatched-new-delete warnings under --coverage."""
    flags = "--coverage"
    version = capture(["c++", "-dumpfullversion", "-dumpversion"], check=False).strip()
    banner = capture(["c++", "--version"], check=False).splitlines()
    major = int(version.split(".")[0]) if version.split(".")[0].isdigit() else 0
    if major >= 15 and banner and "GCC" in banner[0]:
        flags += " -Wno-mismatched-new-delete"
    return flags


def _combine_hits(old_hits: int | None, new_hits: int | None) -> int | None:
    """Sum hit counts, preserving None for non-executable lines."""
    if old_hits is None:
        return new_hits
    if new_hits is None:
        return old_hits
    return old_hits + new_hits


def parse_gcov_text(text: str) -> tuple[str | None, dict[int, tuple[int | None, str]]]:
    """Parse one .gcov file into (source path, {line: (hits or None, source text)}).

    hits is None for non-executable lines, 0 for never-executed ('#####'/'=====').
    The same source line may appear multiple times when gcov splits constructors
    into C1/C2 thunks (and the primary pass); accumulate hits across entries and
    keep the first non-empty content.
    """
    source: str | None = None
    lines: dict[int, tuple[int | None, str]] = {}
    for raw in text.splitlines():
        parts = raw.split(":", 2)
        if len(parts) < 3:
            continue
        count_field, lineno_field, content = parts[0].strip(), parts[1].strip(), parts[2]
        if not lineno_field.isdigit():
            continue
        lineno = int(lineno_field)
        if lineno == 0:
            if content.startswith("Source:"):
                source = content[len("Source:") :]
            continue
        if count_field == "-":
            hits: int | None = None
        elif count_field in ("#####", "====="):
            hits = 0
        else:
            digits = count_field.rstrip("*")
            hits = int(digits) if digits.isdigit() else None
        if lineno in lines:
            old_hits, old_content = lines[lineno]
            lines[lineno] = (_combine_hits(old_hits, hits), old_content or content)
        else:
            lines[lineno] = (hits, content)
    return source, lines


def merge_report(target: dict[int, tuple[int | None, str]], update: dict[int, tuple[int | None, str]]) -> None:
    """Union-merge executed counts: a line is covered if any translation unit ran it."""
    for lineno, (hits, content) in update.items():
        old_hits, old_content = target.get(lineno, (None, content))
        target[lineno] = (_combine_hits(old_hits, hits), old_content or content)


def context_blocks(
    lines: dict[int, tuple[int | None, str]], missing: list[int], before: int = 6, after: int = 6
) -> list[str]:
    """gcov-style context windows around missing lines, with '--' between gaps."""
    wanted: set[int] = set()
    for lineno in missing:
        wanted.update(range(lineno - before, lineno + after + 1))
    output: list[str] = []
    previous: int | None = None
    for lineno in sorted(wanted):
        if lineno not in lines:
            continue
        if previous is not None and lineno > previous + 1:
            output.append("--")
        hits, content = lines[lineno]
        marker = "-" if hits is None else ("#####" if hits == 0 else str(hits))
        output.append(f"{marker:>9}:{lineno:>5}:{content}")
        previous = lineno
    return output


def _normalized_scope(scope: str) -> str:
    return scope.strip().strip("/")


def _matches_scope(rel: str, scopes: list[str] | None) -> bool:
    if not scopes:
        return True
    normalized = [_normalized_scope(scope) for scope in scopes]
    return any(rel == scope or rel.startswith(f"{scope}/") for scope in normalized if scope)


def file_stats(lines: dict[int, tuple[int | None, str]]) -> tuple[int, int, int, float]:
    executable = {lineno: hits for lineno, (hits, _) in lines.items() if hits is not None}
    total = len(executable)
    missing = sum(1 for hits in executable.values() if hits == 0)
    covered = total - missing
    percent = covered / total * 100 if total else 100.0
    return covered, total, missing, percent


def scoped_stats(
    merged: dict[str, dict[int, tuple[int | None, str]]], scopes: list[str] | None
) -> list[tuple[str, int, int, int, float]]:
    rows: list[tuple[str, int, int, int, float]] = []
    for rel, lines in sorted(merged.items()):
        if not _matches_scope(rel, scopes):
            continue
        covered, total, missing, percent = file_stats(lines)
        if total:
            rows.append((rel, covered, total, missing, percent))
    return rows


def collect_coverage(build_dir: Path) -> dict[str, dict[int, tuple[int | None, str]]]:
    """Merge production coverage from all instrumented project and test objects."""
    gcda_files = sorted(
        path for top in ("app", "lib", "test") for path in (build_dir / top).rglob("*.gcda") if path.is_file()
    )
    if not gcda_files:
        raise die("no coverage data generated. Did the tests run successfully?")

    merged: dict[str, dict[int, tuple[int | None, str]]] = {}
    with tempfile.TemporaryDirectory(prefix="ao-gcov-") as temporary:
        report_dir = Path(temporary)
        for gcda in gcda_files:
            result = subprocess.run(
                ["gcov", "--preserve-paths", str(absolute_path(gcda))], cwd=report_dir, capture_output=True, text=True
            )
            if result.returncode != 0:
                detail = (result.stdout + result.stderr).strip()
                raise die(f"gcov failed for {gcda} (exit {result.returncode}); coverage is incomplete.\n{detail}")
            reports = list(report_dir.glob("*.gcov"))
            if not reports:
                raise die(f"gcov produced no reports for {gcda}; coverage is incomplete.")
            for gcov_file in reports:
                source, lines = parse_gcov_text(gcov_file.read_text(encoding="utf-8", errors="replace"))
                gcov_file.unlink()
                if source is None:
                    raise die(f"gcov report {gcov_file.name} has no source; coverage is incomplete.")
                source_path = Path(source)
                if not source_path.is_absolute():
                    source_path = absolute_path(build_dir / source_path)
                try:
                    rel = absolute_path(source_path).relative_to(PROJECT_ROOT).as_posix()
                except ValueError:
                    continue
                if not rel.startswith(tuple(f"{top}/" for top in REPORTED_TOP_DIRS)):
                    continue
                merge_report(merged.setdefault(rel, {}), lines)
    return merged


def print_scoped_summary(
    rows: list[tuple[str, int, int, int, float]], scopes: list[str] | None, summary_limit: int
) -> None:
    if not scopes:
        return
    covered = sum(row[1] for row in rows)
    total = sum(row[2] for row in rows)
    missing = sum(row[3] for row in rows)
    percent = covered / total * 100
    scope_text = ", ".join(_normalized_scope(scope) for scope in scopes)
    print(f"Scoped coverage ({scope_text}): {percent:.2f}% ({covered}/{total} lines), {missing} missing")
    print("Lowest coverage files:")
    for rel, file_covered, file_total, file_missing, file_percent in sorted(
        rows, key=lambda row: (row[4], -row[3], row[0])
    )[:summary_limit]:
        print(f"  {file_percent:6.2f}% {file_covered:5}/{file_total:<5} {file_missing:5} missing  {rel}")
    print()


def report(
    merged: dict[str, dict[int, tuple[int | None, str]]],
    context_limit: int,
    scopes: list[str] | None = None,
    summary_limit: int = 20,
) -> None:
    rows = scoped_stats(merged, None)
    if not rows:
        raise die("no executable project coverage data; measurement is unavailable.")
    for scope in scopes or []:
        if not any(_matches_scope(row[0], [scope]) for row in rows):
            raise die(f"no executable coverage data for scope: {scope}")
    rows = [row for row in rows if _matches_scope(row[0], scopes)]
    print_scoped_summary(rows, scopes, summary_limit)
    for rel, _, total, missing_count, percent in rows:
        lines = merged[rel]
        missing = sorted(lineno for lineno, (hits, _) in lines.items() if hits == 0)
        if missing:
            print(f"{YELLOW}{rel}: {percent:.2f}% ({total} lines) -> {missing_count} missing lines{RESET}")
            for row in context_blocks(lines, missing)[:context_limit]:
                print(f"    {row}")
            if len(missing) > 5:
                print("    ...")
        else:
            print(f"{GREEN}{rel}: {percent:.2f}% ({total} lines) -> OK{RESET}")


def coverage_suites(selection: str | None) -> tuple[str, ...]:
    """Map the CLI suite selection to the suites a coverage run executes.

    The no-option default measures core plus backend; an explicit --core stays
    core-only. ``--all`` adds the portable application frontends but excludes
    the conditional system-media suite, performance, tooling, and lint, which
    do not belong to an unattended coverage run.
    """
    if selection == "all":
        return ("core", "backend", "tui", "cli", "gtk")
    if selection is None:
        return ("core", "backend")
    return (selection,)


def run_coverage_tests(suites: tuple[str, ...], build_dir: Path, test_filter: str) -> int:
    """Run every selected suite so the report is useful, while preserving failure."""
    first_failure = 0
    matched_tests = 0
    for suite in suites:
        # Deliberately unsharded (shards=1): parallel processes would interleave
        # writes to the same gcov counter files, so a hostile AOBUS_TEST_SHARDS
        # cannot shard a coverage run. One suite record per invocation keeps every
        # suite's own status, so the run-all/first-failure policy below stays
        # attribution-friendly. With a filter, a member suite may legitimately
        # match nothing, but the filter must match at least one test globally;
        # without a filter the suite still fails natively on an empty run.
        with phases.measure("suite", tree="native", jobs=1, count=1, suite=suite) as phase:
            outcome = run_catch2_suite(
                suite,
                build_dir,
                test_filter=test_filter,
                shards=1,
                report_matches=bool(test_filter),
                allow_no_tests=bool(test_filter),
            )
            phase.status = outcome.status
        if outcome.status != 0:
            if first_failure == 0:
                first_failure = outcome.status
            print(f"Error: {suite} tests exited with status {outcome.status}; coverage is partial.", file=sys.stderr)
        elif test_filter and outcome.matched is not None:
            matched_tests += outcome.matched
    if test_filter and first_failure == 0 and matched_tests == 0:
        raise die(f"no tests in the selected coverage suites matched filter {test_filter!r}.")
    return first_failure


def run_command(args: argparse.Namespace) -> int:
    if builddir.platform_profile().name != "linux":
        raise die("coverage is supported on Linux only because it requires GCC and gcov.")

    build_dir = Path(args.path) if args.path else builddir.COVERAGE_DIR
    with buildlock.build_tree_lock(build_dir):
        return _run_coverage(args, build_dir)


def _run_coverage(args: argparse.Namespace, build_dir: Path) -> int:
    try:
        workspace = workspace_cache.prepare(build_dir, unsupported_reason="coverage builds")
    except workspace_cache.WorkspaceCacheError as exc:
        raise die(str(exc)) from exc
    if not (build_dir / "CMakeCache.txt").is_file():
        print(f"Configuring coverage build in {build_dir}...")
        configure = [
            "cmake",
            "-S",
            str(workspace.source_dir),
            "--preset",
            "linux-debug",
            "-B",
            str(build_dir),
            f"-DCMAKE_CXX_FLAGS={coverage_cxx_flags()}",
            "-DCMAKE_EXE_LINKER_FLAGS=--coverage",
            "-DCMAKE_SHARED_LINKER_FLAGS=--coverage",
            *compiler_cache.cmake_launcher_arguments(build_dir=build_dir),
            *workspace.cmake_arguments,
        ]
        # The cold configure is portal work this command owns; measure it in
        # place. The actual native exit code is assigned before the failure
        # handling so a failed configure keeps its code in its own record even
        # though die normalizes the command exit to 1.
        with phases.measure("configure", tree="native") as phase:
            phase.status = run(configure)
        if phase.status != 0:
            raise die("coverage configure failed.")
    else:
        build.sync_compiler_cache(build_dir, unsupported_reason="coverage builds")

    print("Building tests...")
    suites = coverage_suites(args.suite)
    targets = [target for suite in suites for target in SUITE_TARGETS[suite]]
    # Keep zero/unbounded and invalid CLI values under the native build's
    # existing policy; they do not identify a known positive worker limit.
    jobs = args.jobs if isinstance(args.jobs, int) and not isinstance(args.jobs, bool) and args.jobs > 0 else None
    with phases.measure("build", tree="native", jobs=jobs) as phase:
        phase.status = run(["cmake", "--build", str(build_dir), f"-j{args.jobs}", "--target", *targets])
    if phase.status != 0:
        raise die("coverage build failed.")

    print("Clearing old .gcda files...")
    for gcda in build_dir.rglob("*.gcda"):
        gcda.unlink()

    print(f"Running tests (suites: {', '.join(suites)})...")
    test_status = run_coverage_tests(suites, build_dir, args.filter)

    print()
    print("=== Coverage Summary ===")
    print()
    merged = collect_coverage(build_dir)
    report(
        merged,
        int(os.environ.get("COVERAGE_CONTEXT_LIMIT", "40")),
        scopes=args.scope,
        summary_limit=args.summary_limit,
    )
    print()
    print("Coverage report completed.")
    return test_status
