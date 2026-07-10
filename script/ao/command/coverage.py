"""ao coverage — gcov line coverage for the C++ sources, with uncovered-line context.

Unlike the old shell flow, gcov reports for the same source are merged across all object
files (union of executed lines), so a header exercised by several translation units is
reported once with its true combined coverage.
"""

import argparse
import os
import subprocess
import sys
from pathlib import Path

from ..core import builddir
from ..core.paths import PROJECT_ROOT, absolute_path
from ..core.proc import capture, die, run
from .test import SUITE_TARGETS, run_suite

HELP = "Build with --coverage, run tests, and report uncovered lines per source file"

EPILOG = """\
examples:
  ./ao coverage                          # core suite, full report
  ./ao coverage "rt::SmartListEvaluator" # coverage for a test subset
  ./ao coverage --tui --scope app/tui
  ./ao coverage --gtk "[layout]"         # GTK suite with a Catch2 filter
  ./ao coverage --gtk --scope app/linux-gtk
"""

REPORTED_TOP_DIRS = ("app", "lib", "include")
YELLOW, GREEN, RESET = "\033[33m", "\033[32m", "\033[0m"


def register(subparsers: "argparse._SubParsersAction[argparse.ArgumentParser]") -> None:
    parser = subparsers.add_parser(
        "coverage", help=HELP, description=HELP, epilog=EPILOG, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("filter", nargs="?", default="", help="Catch2 test filter")
    suite = parser.add_mutually_exclusive_group()
    suite.add_argument(
        "--suite", choices=("core", "tui", "gtk", "all"), default="core", help="test suite (default: core)"
    )
    suite.add_argument("--core", dest="suite", action="store_const", const="core", help="shortcut for --suite core")
    suite.add_argument("--tui", dest="suite", action="store_const", const="tui", help="shortcut for --suite tui")
    suite.add_argument("--gtk", dest="suite", action="store_const", const="gtk", help="shortcut for --suite gtk")
    suite.add_argument("--all", dest="suite", action="store_const", const="all", help="shortcut for --suite all")
    parser.add_argument("-p", "--path", metavar="<dir>", help="build directory (default: /tmp/build/coverage)")
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
    """Run gcov for every .gcda under app/ and lib/ and merge the reports per source."""
    gcda_files = sorted(path for top in ("app", "lib") for path in (build_dir / top).rglob("*.gcda") if path.is_file())
    if not gcda_files:
        raise die("no coverage data generated. Did the tests run successfully?")

    merged: dict[str, dict[int, tuple[int | None, str]]] = {}
    for gcda in gcda_files:
        subprocess.run(["gcov", str(gcda)], cwd=build_dir, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for gcov_file in build_dir.glob("*.gcov"):
            source, lines = parse_gcov_text(gcov_file.read_text(encoding="utf-8", errors="replace"))
            gcov_file.unlink()
            if source is None:
                continue
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
    merged: dict[str, dict[int, tuple[int | None, str]]], scopes: list[str] | None, summary_limit: int
) -> None:
    if not scopes:
        return
    rows = scoped_stats(merged, scopes)
    covered = sum(row[1] for row in rows)
    total = sum(row[2] for row in rows)
    missing = sum(row[3] for row in rows)
    percent = covered / total * 100 if total else 100.0
    scope_text = ", ".join(_normalized_scope(scope) for scope in scopes)
    print(f"Scoped coverage ({scope_text}): {percent:.2f}% ({covered}/{total} lines), {missing} missing")
    if not rows:
        return
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
    print_scoped_summary(merged, scopes, summary_limit)
    for rel in sorted(merged):
        if not _matches_scope(rel, scopes):
            continue
        lines = merged[rel]
        _, total, missing_count, percent = file_stats(lines)
        if not total:
            continue
        missing = sorted(lineno for lineno, (hits, _) in lines.items() if hits == 0)
        if missing:
            print(f"{YELLOW}{rel}: {percent:.2f}% ({total} lines) -> {missing_count} missing lines{RESET}")
            for row in context_blocks(lines, missing)[:context_limit]:
                print(f"    {row}")
            if len(missing) > 5:
                print("    ...")
        else:
            print(f"{GREEN}{rel}: {percent:.2f}% ({total} lines) -> OK{RESET}")


def run_command(args: argparse.Namespace) -> int:
    build_dir = Path(args.path) if args.path else builddir.COVERAGE_DIR

    if not (build_dir / "CMakeCache.txt").is_file():
        print(f"Configuring coverage build in {build_dir}...")
        configure = [
            "cmake",
            "-S",
            str(PROJECT_ROOT),
            "--preset",
            "linux-debug",
            "-B",
            str(build_dir),
            f"-DCMAKE_CXX_FLAGS={coverage_cxx_flags()}",
            "-DCMAKE_EXE_LINKER_FLAGS=--coverage",
            "-DCMAKE_SHARED_LINKER_FLAGS=--coverage",
        ]
        if run(configure) != 0:
            raise die("coverage configure failed.")

    print("Building tests...")
    suites = ("core", "tui", "gtk") if args.suite == "all" else (args.suite,)
    targets = [target for suite in suites for target in SUITE_TARGETS[suite]]
    if run(["cmake", "--build", str(build_dir), f"-j{args.jobs}", "--target", *targets]) != 0:
        raise die("coverage build failed.")

    print("Clearing old .gcda files...")
    for gcda in build_dir.rglob("*.gcda"):
        gcda.unlink()

    print(f"Running tests (suite: {args.suite})...")
    for suite in suites:
        if (status := run_suite(suite, build_dir, test_filter=args.filter)) != 0:
            print(f"Warning: {suite} tests exited with status {status}; coverage may be partial.", file=sys.stderr)

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
    print("Coverage check completed.")
    return 0
