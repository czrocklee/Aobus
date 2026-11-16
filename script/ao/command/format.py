"""ao format - format native, Python, Bazel, and authored text sources."""

import argparse
import os
import subprocess
import sys
from pathlib import Path

from ..core import builddir, buildenv, buildifier, compiler_policy, gitfiles, pythoncheck, windows_llvm_sdk
from ..core.paths import PROJECT_ROOT
from ..core.proc import die

HELP = "Format native, Python, Bazel, Markdown, JSON, YAML, and CSS sources"
NAME = "format"
# Formatting needs a ready LLVM tool, not a Visual Studio compiler environment.
REQUIRES_BUILD_ENV = False


EPILOG = """\
examples:
  ./ao format                   # format changes since the merge base with local main
  ./ao format --check           # dry run: fail if anything needs reformatting
  ./ao format lib/audio/Foo.cpp # format explicit files
  ./ao format --folder script   # format one folder
  ./ao format --all             # format the whole tree
"""

FORMAT_TOP_DIRS = (
    "app",
    "asset",
    "bazel",
    "cmake",
    "config",
    "doc",
    "include",
    "lib",
    "script",
    "test",
    "tool",
    ".agents/skills",
    ".github",
)
# One control edit can change the result for every file it governs, so those diff
# scopes widen to the whole Prettier family; unrelated scope stays unchanged.
PRETTIER_CONTROL_FILES = (".prettierrc.json", ".prettierignore", ".gitignore", "script/ao/prettier.json")
CHUNK = 100


def register(subparsers: argparse._SubParsersAction[argparse.ArgumentParser]) -> None:
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
        return _whole_tree_sources()
    if args.folder:
        return _folder_sources(args.folder)
    return _changed_sources(args.commit)


def _whole_tree_sources() -> list[str]:
    """Every source under FORMAT_TOP_DIRS plus the root Bazel and authored text files."""
    files = _non_text_sources(gitfiles.find_sources(list(FORMAT_TOP_DIRS), suffixes=gitfiles.SOURCE_SUFFIXES))
    files.extend(gitfiles.root_bazel_files())
    files.extend(gitfiles.prettier_sources(list(FORMAT_TOP_DIRS), include_root=True))
    return sorted(set(files))


def _folder_sources(folders: list[str]) -> list[str]:
    files = _non_text_sources(gitfiles.find_sources(folders, suffixes=gitfiles.SOURCE_SUFFIXES))
    files.extend(gitfiles.prettier_sources(folders))
    return sorted(set(files))


def _non_text_sources(files: list[str]) -> list[str]:
    """Keep the existing filesystem scan for native, Python, and Bazel sources.

    Prettier's family is discovered separately so ignored artifacts never enter
    text formatting; the shared scan still owns the other languages.
    """
    return [name for name in files if not gitfiles.is_prettier_file(name)]


def _changed_sources(commit: str | None) -> list[str]:
    changed = gitfiles.changed_paths(commit)
    files = [
        name
        for name in changed
        if gitfiles.matches_source(name, gitfiles.SOURCE_SUFFIXES)
        and (
            name.startswith(tuple(f"{top}/" for top in FORMAT_TOP_DIRS))
            or (
                Path(name).parent == Path(".")
                and (gitfiles.is_bazel_file(name) or gitfiles.is_bazel_rc_file(name) or gitfiles.is_prettier_file(name))
            )
            or gitfiles.is_workflow_file(name)
        )
        and (PROJECT_ROOT / name).is_file()
    ]
    if any(name in PRETTIER_CONTROL_FILES for name in changed):
        # A control edit can change the result for every governed file, so the
        # diff scope widens to the whole owned text family, never to native code.
        files.extend(gitfiles.prettier_sources(list(FORMAT_TOP_DIRS), include_root=True))
    return sorted(set(files))


def _file_exists(name: str) -> bool:
    path = Path(name)
    if not path.is_absolute():
        path = PROJECT_ROOT / path
    return path.is_file()


def requires_build_environment(args: argparse.Namespace) -> bool:
    cpp_scope = buildenv.requires_source_build_env(args, resolve_files(args))
    # macOS exposes ready LLVM tools through its native environment, not ambient PATH.
    return cpp_scope and builddir.platform_profile().name == "macos"


def requires_python_tools(args: argparse.Namespace) -> bool:
    """Only a Python source scope needs the managed Ruff/mypy environment."""
    buildenv.load_source_scope(args, os.environ.get("AOBUS_PREFLIGHT_SCOPE"))
    files = resolve_files(args)
    args._resolved_sources = files
    return any(name.endswith(gitfiles.PYTHON_SUFFIXES) for name in files)


REQUIRES_PYTHON_TOOLS = requires_python_tools


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
    prettier_files = [name for name in files if gitfiles.is_prettier_file(name) and _file_exists(name)]
    msvc_rc = compiler_policy.selects_msvc_rc(files)
    if not cpp_files and not python_files and not bazel_files and not prettier_files and not msvc_rc:
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
    if prettier_files:
        from ..core import prettier

        try:
            status = max(status, prettier.run(prettier_files, check=args.check))
        except prettier.PrettierError as exc:
            raise die(str(exc)) from exc
    if msvc_rc:
        try:
            status = max(status, compiler_policy.format_msvc_rc(check=args.check))
        except (OSError, ValueError) as exc:
            raise die(str(exc)) from exc

    if status != 0 and args.check:
        print("Formatting issues found.", file=sys.stderr)
        return 1
    if status != 0:
        raise die("formatting failed.")
    print("Done.")
    return 0
