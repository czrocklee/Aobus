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
from ..core.paths import PROJECT_ROOT
from ..core.proc import capture, die, run
from .test import SUITE_TARGETS, run_suite

HELP = "Build with --coverage, run tests, and report uncovered lines per source file"

EPILOG = """\
examples:
  ./ao coverage                          # core suite, full report
  ./ao coverage "rt::SmartListEvaluator" # coverage for a test subset
  ./ao coverage --gtk "[layout]"         # GTK suite with a Catch2 filter
"""

REPORTED_TOP_DIRS = ("app", "lib", "include")
YELLOW, GREEN, RESET = "\033[33m", "\033[32m", "\033[0m"


def register(subparsers: "argparse._SubParsersAction[argparse.ArgumentParser]") -> None:
    parser = subparsers.add_parser(
        "coverage", help=HELP, description=HELP, epilog=EPILOG, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("filter", nargs="?", default="", help="Catch2 test filter")
    suite = parser.add_mutually_exclusive_group()
    suite.add_argument("--suite", choices=("core", "gtk", "all"), default="core", help="test suite (default: core)")
    suite.add_argument("--core", dest="suite", action="store_const", const="core", help="shortcut for --suite core")
    suite.add_argument("--gtk", dest="suite", action="store_const", const="gtk", help="shortcut for --suite gtk")
    suite.add_argument("--all", dest="suite", action="store_const", const="all", help="shortcut for --suite all")
    parser.add_argument("-p", "--path", metavar="<dir>", help="build directory (default: /tmp/build/coverage)")
    parser.add_argument("-j", "--jobs", type=int, default=os.cpu_count(), help="parallel jobs (default: nproc)")
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


def parse_gcov_text(text: str) -> tuple[str | None, dict[int, tuple[int | None, str]]]:
    """Parse one .gcov file into (source path, {line: (hits or None, source text)}).

    hits is None for non-executable lines, 0 for never-executed ('#####'/'=====').
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
        lines[lineno] = (hits, content)
    return source, lines


def merge_report(target: dict[int, tuple[int | None, str]], update: dict[int, tuple[int | None, str]]) -> None:
    """Union-merge executed counts: a line is covered if any translation unit ran it."""
    for lineno, (hits, content) in update.items():
        old_hits, old_content = target.get(lineno, (None, content))
        if old_hits is None:
            merged = hits
        elif hits is None:
            merged = old_hits
        else:
            merged = old_hits + hits
        target[lineno] = (merged, old_content or content)


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
                source_path = (build_dir / source_path).resolve()
            try:
                rel = source_path.resolve().relative_to(PROJECT_ROOT).as_posix()
            except ValueError:
                continue
            if not rel.startswith(tuple(f"{top}/" for top in REPORTED_TOP_DIRS)):
                continue
            merge_report(merged.setdefault(rel, {}), lines)
    return merged


def report(merged: dict[str, dict[int, tuple[int | None, str]]], context_limit: int) -> None:
    for rel in sorted(merged):
        lines = merged[rel]
        executable = {lineno: hits for lineno, (hits, _) in lines.items() if hits is not None}
        if not executable:
            continue
        missing = sorted(lineno for lineno, hits in executable.items() if hits == 0)
        percent = (len(executable) - len(missing)) / len(executable) * 100
        if missing:
            print(f"{YELLOW}{rel}: {percent:.2f}% ({len(executable)} lines) -> {len(missing)} missing lines{RESET}")
            for row in context_blocks(lines, missing)[:context_limit]:
                print(f"    {row}")
            if len(missing) > 5:
                print("    ...")
        else:
            print(f"{GREEN}{rel}: {percent:.2f}% ({len(executable)} lines) -> OK{RESET}")


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
    suites = ("core", "gtk") if args.suite == "all" else (args.suite,)
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
    report(merged, int(os.environ.get("COVERAGE_CONTEXT_LIMIT", "40")))
    print()
    print("Coverage check completed.")
    return 0
