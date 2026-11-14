"""ao format - format C++, Python and Bazel sources, the whole tree on demand."""

import argparse
import os
import subprocess
import sys
from pathlib import Path

from ..core import builddir, buildenv, buildifier, gitfiles, pythoncheck, windows_llvm_sdk
from ..core.paths import PROJECT_ROOT
from ..core.proc import die

HELP = "Format C++, Python and Bazel sources (changed files by default)"
NAME = "format"
# Formatting needs a ready LLVM tool, not a Visual Studio compiler environment.
REQUIRES_BUILD_ENV = False
REQUIRES_PYTHON_TOOLS = True


EPILOG = """\
examples:
  ./ao format                   # format changes since the merge base with local main
  ./ao format --check           # dry run: fail if anything needs reformatting
  ./ao format lib/audio/Foo.cpp # format explicit files
  ./ao format --folder script   # format one folder
  ./ao format --all             # format the whole tree
"""

FORMAT_TOP_DIRS = ("app", "asset", "bazel", "include", "lib", "script", "test", "tool")
CHUNK = 100


def register(subparsers: "argparse._SubParsersAction[argparse.ArgumentParser]") -> None:
    parser = subparsers.add_parser(
        NAME, help=HELP, description=HELP, epilog=EPILOG, formatter_class=argparse.RawDescriptionHelpFormatter
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
    for name in args.files:
        if not _file_exists(name):
            raise die(f"explicitly selected path does not exist or is not a file: {name}")
    for folder in getattr(args, "folder", []):
        if not (PROJECT_ROOT / folder).is_dir():
            raise die(f"explicitly selected folder does not exist or is not a directory: {folder}")
    if (prepared := getattr(args, "_resolved_sources", None)) is not None:
        return list(prepared)
    if args.files:
        return list(args.files)
    if args.all:
        files = gitfiles.find_sources(list(FORMAT_TOP_DIRS), suffixes=gitfiles.SOURCE_SUFFIXES)
        files.extend(gitfiles.root_bazel_files())
        return sorted(set(files))
    if args.folder:
        return gitfiles.find_sources(args.folder, suffixes=gitfiles.SOURCE_SUFFIXES)
    changed = gitfiles.changed_files(args.commit, suffixes=gitfiles.SOURCE_SUFFIXES)
    return [
        name
        for name in changed
        if (
            name.startswith(tuple(f"{top}/" for top in FORMAT_TOP_DIRS))
            or (Path(name).parent == Path(".") and gitfiles.is_bazel_file(name))
        )
        and (PROJECT_ROOT / name).is_file()
    ]


def _file_exists(name: str) -> bool:
    path = Path(name)
    if not path.is_absolute():
        path = PROJECT_ROOT / path
    return path.is_file()


def requires_build_environment(args: argparse.Namespace) -> bool:
    cpp_scope = buildenv.requires_source_build_env(args, resolve_files(args))
    # macOS exposes ready LLVM tools through its native environment, not ambient PATH.
    return cpp_scope and builddir.platform_profile().name == "macos"


def run_clang_format(
    files: list[str], *, check: bool, build_dir: Path | None = None, clang_format: str | None = None
) -> int:
    """Run clang-format; an explicit executable replaces the Windows lint-tree SDK selection."""
    if not files:
        return 0

    mode = ["--dry-run", "-Werror"] if check else ["-i"]
    action = "Checking" if check else "Formatting"
    print(f"{action} {len(files)} file(s) with clang-format...")
    if clang_format is None:
        clang_format = "clang-format"
        if builddir.platform_profile().name == "windows":
            try:
                manifest = windows_llvm_sdk.read_manifest(windows_llvm_sdk.MANIFEST)
                root, _cached = windows_llvm_sdk.ready_root(os.environ, manifest)
                clang_format = str(root / "bin/clang-format.exe")
            except windows_llvm_sdk.WindowsLlvmSdkError as error:
                raise die(
                    f"format requires the ready pinned LLVM SDK: {error}; run ao setup llvm-sdk explicitly"
                ) from error

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


def run_ruff_format(files: list[str], *, check: bool, python: Path | None = None) -> int:
    if not files:
        return 0

    mode = ["--check"] if check else []
    action = "Checking" if check else "Formatting"
    print(f"{action} {len(files)} file(s) with ruff format...")
    try:
        result = subprocess.run(pythoncheck.ruff_command("format", *mode, *files, python=python), cwd=PROJECT_ROOT)
    except FileNotFoundError as exc:
        raise die(f"Python environment is unavailable: {python or sys.executable}") from exc
    return 1 if result.returncode != 0 else 0


def run_command(
    args: argparse.Namespace,
    *,
    files: list[str] | None = None,
    build_dir: Path | None = None,
    clang_format: str | None = None,
    python: Path | None = None,
) -> int:
    if files is None:
        files = resolve_files(args)
    cpp_files = [name for name in files if name.endswith(gitfiles.CPP_SUFFIXES) and _file_exists(name)]
    python_files = [name for name in files if name.endswith(gitfiles.PYTHON_SUFFIXES) and _file_exists(name)]
    bazel_files = [name for name in files if gitfiles.is_bazel_file(name) and _file_exists(name)]
    if not cpp_files and not python_files and not bazel_files:
        print("No files to format.")
        return 0

    status = 0
    if run_clang_format(cpp_files, check=args.check, build_dir=build_dir, clang_format=clang_format) != 0:
        status = 1
    if run_ruff_format(python_files, check=args.check, python=python) != 0:
        status = 1
    try:
        if buildifier.run(bazel_files, check=args.check) != 0:
            status = 1
    except buildifier.BuildifierError as exc:
        raise die(str(exc)) from exc

    if status != 0 and args.check:
        print("Formatting issues found.", file=sys.stderr)
        return 1
    if status != 0:
        raise die("formatting failed.")
    print("Done.")
    return 0
