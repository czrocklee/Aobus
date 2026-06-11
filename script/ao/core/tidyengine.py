"""Shared clang-tidy execution engine for the tidy and analyze commands.

Owns everything the two flows have in common: compile database provisioning, Nix system
include discovery, scope resolution (changed files / folders / --all / explicit files),
and the parallel per-file runner with progress reporting.
"""

import argparse
import concurrent.futures
import os
import subprocess
import sys
import tempfile
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path

from . import gitfiles
from .dedup import DIAGNOSTIC_RE
from .paths import PROJECT_ROOT
from .proc import die


def default_jobs() -> int:
    cpus = os.cpu_count() or 1
    return max(cpus - 1, 1)


def add_scope_arguments(parser: argparse.ArgumentParser, verb: str) -> None:
    """Common scope and runner options shared by tidy and analyze."""
    parser.add_argument("files", nargs="*", metavar="file", help=f"explicit files to {verb}")
    parser.add_argument("--all", action="store_true", help=f"{verb} every source in the project folders")
    parser.add_argument(
        "--folder", action="append", default=[], metavar="<dir>", help="all files under <dir> (repeatable)"
    )
    parser.add_argument("--commit", metavar="<rev>", help="changed files since <rev> + working tree + untracked")
    parser.add_argument("--check", metavar="<name>", help="run only the specified check")
    parser.add_argument("--debug", action="store_true", help="show debug info (config, system includes)")
    parser.add_argument("-o", "--output", metavar="<file>", help="write diagnostics to <file>")
    parser.add_argument("-j", "--jobs", type=int, default=default_jobs(), help="parallel jobs (default: nproc - 1)")
    parser.add_argument("-p", "--path", metavar="<dir>", help="build directory with compile_commands.json")


def resolve_scope(
    args: argparse.Namespace,
    all_folders: list[str],
    label: str,
    *,
    suffixes: tuple[str, ...] = gitfiles.CPP_SUFFIXES,
) -> tuple[list[str], bool]:
    """Return (files, explicit) where files are repo-relative or absolute paths."""
    if args.files:
        return list(args.files), True
    if args.all:
        print(f"{label} all sources in: {' '.join(all_folders)}", file=sys.stderr)
        files = gitfiles.find_sources(all_folders, suffixes=suffixes)
    elif args.folder:
        print(f"{label} folders: {' '.join(args.folder)}", file=sys.stderr)
        files = gitfiles.find_sources(args.folder, suffixes=suffixes)
    else:
        base = gitfiles.diff_base(args.commit)
        print(
            f"No files specified — using git diff {base}..HEAD + working tree + staged + untracked",
            file=sys.stderr,
        )
        files = gitfiles.changed_files(args.commit, suffixes=suffixes)
    return files, False


def ensure_compile_db(build_dir: Path, configure_args: list[str] | None = None) -> None:
    """Configure the tree and build once (generated headers) if the compile DB is missing."""
    if (build_dir / "compile_commands.json").is_file():
        return
    print("compile_commands.json missing, running cmake configure...")
    if (build_dir / "CMakeCache.txt").is_file():
        configure = ["cmake", str(PROJECT_ROOT), "-B", str(build_dir)]
    else:
        configure = ["cmake", "-S", str(PROJECT_ROOT), "--preset", "linux-debug", "-B", str(build_dir)]
    configure += configure_args or []
    _run_tail(configure, "configure")
    print("Configure done.")
    print("Building targets to generate necessary headers (gperf)...")
    _run_tail(["cmake", "--build", str(build_dir), f"-j{os.cpu_count()}"], "header generation build")
    print("Build done.")


def _run_tail(argv: list[str], what: str, tail: int = 5) -> None:
    result = subprocess.run(argv, cwd=PROJECT_ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    lines = result.stdout.splitlines()
    for line in lines[-tail:]:
        print(line)
    if result.returncode != 0:
        print("--- Full Output ---", file=sys.stderr)
        print(result.stdout, file=sys.stderr)
        raise die(f"{what} failed (exit {result.returncode}).")


def system_include_args() -> list[str]:
    """Nix store system include paths, passed explicitly so clang-tidy resolves libstdc++/GTK."""
    try:
        result = subprocess.run(
            ["clang++", "-E", "-x", "c++", "-", "-v"],
            input="",
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            text=True,
        )
    except FileNotFoundError:
        return []
    args = []
    for line in result.stderr.splitlines():
        if line.startswith(" /nix"):
            path = line.strip()
            if Path(path).is_dir():
                args.append(f"--extra-arg-before=-isystem{path}")
    return args


@dataclass
class BatchResult:
    failed: bool = False
    logs: list[Path] = field(default_factory=list)
    failed_logs: list[Path] = field(default_factory=list)


def run_parallel(
    files: list[str],
    jobs: int,
    tmpdir: Path,
    runner: Callable[[str, Path], int],
) -> BatchResult:
    """Run `runner(file, log_path)` for every file with bounded parallelism."""
    result = BatchResult()
    total = len(files)
    done = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(jobs, 1)) as pool:
        futures = {}
        for index, file in enumerate(files):
            log = tmpdir / f"{index:06d}_{file.replace('/', '_')}.log"
            result.logs.append(log)
            futures[pool.submit(runner, file, log)] = (file, log)
        for future in concurrent.futures.as_completed(futures):
            file, log = futures[future]
            try:
                status = future.result()
            except Exception as e:
                status = -1
                print(f"EXCEPTION running {file}: {e}", file=sys.stderr)
            if status != 0:
                result.failed = True
                result.failed_logs.append(log)
                print(f"FAILED: {file}", file=sys.stderr)
            done += 1
            print(f"\r  [{done}/{total}]", end="", file=sys.stderr, flush=True)
    print(file=sys.stderr)
    return result


def logs_with_diagnostics(logs: list[Path]) -> list[Path]:
    matching = []
    for log in logs:
        try:
            with open(log, encoding="utf-8", errors="replace") as fh:
                if any((m := DIAGNOSTIC_RE.match(line)) and m.group(4) in ("warning", "error") for line in fh):
                    matching.append(log)
        except OSError:
            continue
    return matching


def make_tmpdir(prefix: str) -> Path:
    return Path(tempfile.mkdtemp(prefix=prefix, dir="/tmp"))
