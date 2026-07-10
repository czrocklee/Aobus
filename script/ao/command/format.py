"""ao format - format C++ and Python sources by default, the whole tree on demand."""

import argparse
import subprocess
import sys
from pathlib import Path

from ..core import builddir, gitfiles, tidyengine
from ..core.paths import PROJECT_ROOT
from ..core.proc import die

HELP = "Format C++ and Python sources (changed files by default)"

EPILOG = """\
examples:
  ./ao format                   # format files changed against local main
  ./ao format --check           # dry run: fail if anything needs reformatting
  ./ao format lib/audio/Foo.cpp # format explicit files
  ./ao format --folder script   # format one folder
  ./ao format --all             # format the whole tree
"""

FORMAT_TOP_DIRS = ("app", "include", "lib", "script", "test", "tool")
CHUNK = 100


def register(subparsers: "argparse._SubParsersAction[argparse.ArgumentParser]") -> None:
    parser = subparsers.add_parser(
        "format", help=HELP, description=HELP, epilog=EPILOG, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("files", nargs="*", metavar="file", help="explicit files to format")
    parser.add_argument("--all", action="store_true", help="format every source under " + " ".join(FORMAT_TOP_DIRS))
    parser.add_argument(
        "--folder", action="append", default=[], metavar="<dir>", help="all files under <dir> (repeatable)"
    )
    parser.add_argument("--commit", metavar="<rev>", help="format files changed since <rev>")
    parser.add_argument("--check", action="store_true", help="dry run, non-zero exit if reformatting is needed")
    parser.set_defaults(func=run_command)


def resolve_files(args: argparse.Namespace) -> list[str]:
    if args.files:
        return list(args.files)
    if args.all:
        return gitfiles.find_sources(list(FORMAT_TOP_DIRS), suffixes=gitfiles.SOURCE_SUFFIXES)
    if args.folder:
        return gitfiles.find_sources(args.folder, suffixes=gitfiles.SOURCE_SUFFIXES)
    changed = gitfiles.changed_files(args.commit, suffixes=gitfiles.SOURCE_SUFFIXES)
    return [
        name
        for name in changed
        if name.startswith(tuple(f"{top}/" for top in FORMAT_TOP_DIRS)) and (PROJECT_ROOT / name).is_file()
    ]


def _file_exists(name: str) -> bool:
    path = Path(name)
    if not path.is_absolute():
        path = PROJECT_ROOT / path
    return path.is_file()


def run_clang_format(files: list[str], *, check: bool) -> int:
    if not files:
        return 0

    mode = ["--dry-run", "-Werror"] if check else ["-i"]
    action = "Checking" if check else "Formatting"
    print(f"{action} {len(files)} file(s) with clang-format...")
    clang_format = "clang-format"
    if builddir.platform_profile().name == "windows":
        tidyengine.ensure_windows_llvm_sdk(builddir.TIDY_DIR)
        clang_format = tidyengine.clang_tool(builddir.TIDY_DIR, "clang-format")

    status = 0
    for start in range(0, len(files), CHUNK):
        chunk = files[start : start + CHUNK]
        try:
            result = subprocess.run([clang_format, *mode, *chunk], cwd=PROJECT_ROOT)
        except FileNotFoundError as exc:
            raise die(f"clang-format not found: {clang_format}") from exc
        if result.returncode != 0:
            status = result.returncode

    if status != 0:
        return 1
    return 0


def run_ruff_format(files: list[str], *, check: bool) -> int:
    if not files:
        return 0

    mode = ["--check"] if check else []
    action = "Checking" if check else "Formatting"
    print(f"{action} {len(files)} file(s) with ruff format...")
    try:
        result = subprocess.run(["ruff", "format", *mode, *files], cwd=PROJECT_ROOT)
    except FileNotFoundError as exc:
        raise die("ruff not found. Enter the project shell with ./ao or nix-shell.") from exc
    return 1 if result.returncode != 0 else 0


def run_command(args: argparse.Namespace) -> int:
    files = resolve_files(args)
    cpp_files = [name for name in files if name.endswith(gitfiles.CPP_SUFFIXES) and _file_exists(name)]
    python_files = [name for name in files if name.endswith(gitfiles.PYTHON_SUFFIXES) and _file_exists(name)]
    if not cpp_files and not python_files:
        print("No files to format.")
        return 0

    status = 0
    if run_clang_format(cpp_files, check=args.check) != 0:
        status = 1
    if run_ruff_format(python_files, check=args.check) != 0:
        status = 1

    if status != 0 and args.check:
        print("Formatting issues found.", file=sys.stderr)
        return 1
    if status != 0:
        raise die("formatting failed.")
    print("Done.")
    return 0
