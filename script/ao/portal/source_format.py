"""Ready-tool source rewriting and independent read-only format verification."""

from __future__ import annotations

import os
import subprocess
import sys
from collections.abc import Callable
from pathlib import Path

from ao.core.paths import PROJECT_ROOT
from ao.policy import windows_llvm_sdk
from ao.portal import build_dir, buildifier, compiler_rc, git_files, prettier, python_check, source_scope
from ao.portal.ready_tool import ReadyTool, ToolMissing
from ao.portal.source_scope import SourceScope

CHUNK = 100


class ClangFormatMissing(ToolMissing, ValueError):
    """The selected LLVM provider has not supplied the formatter yet."""


def clang_format_selection() -> ReadyTool:
    """Select clang-format and the ready-provider inputs needed to recheck that selection."""
    host = build_dir.platform_profile().name
    watches: tuple[Path, ...] = ()
    if host == "windows":
        try:
            manifest = windows_llvm_sdk.read_manifest(windows_llvm_sdk.MANIFEST)
            root, cached = windows_llvm_sdk.ready_root(os.environ, manifest)
            watches = (
                windows_llvm_sdk.MANIFEST,
                root,
                *windows_llvm_sdk.readiness_paths(root, manifest, cached=cached),
            )
            tool = root / "bin/clang-format.exe"
        except windows_llvm_sdk.WindowsLlvmSdkError as error:
            message = f"format requires the ready pinned LLVM SDK: {error}; run ao setup llvm-sdk explicitly"
            if isinstance(error, windows_llvm_sdk.WindowsLlvmSdkSelectedFileMissing) and not os.environ.get(
                "AOBUS_LLVM_SDK_ROOT"
            ):
                raise ClangFormatMissing(message, windows_llvm_sdk.MANIFEST, error.path) from error
            raise ValueError(message) from error
    else:
        key = "AO_NIX_CLANG_FORMAT" if host == "linux" else "AOBUS_LLVM_ROOT"
        selected = Path(os.environ.get(key, ""))
        if not selected.is_absolute():
            raise ValueError(f"format requires {key} from the selected LLVM environment; re-enter ./ao")
        tool = selected if host == "linux" else selected / "bin/clang-format"
        if host == "linux":
            try:
                in_store = tool.is_relative_to("/nix/store") and tool.resolve(strict=True).is_relative_to("/nix/store")
            except (OSError, ValueError, RuntimeError) as error:
                raise ValueError(f"cannot inspect {key} selection {str(tool)!r}: {error}") from error
            if not in_store:
                raise ValueError("format requires clang-format from the selected Nix store")
        else:
            # Homebrew's selected prefix can be a symlink to a versioned keg.
            watches = (selected, PROJECT_ROOT / "script/ao/macos-toolchain.json")
            if not os.path.lexists(tool):
                raise ClangFormatMissing(f"selected clang-format is unavailable: {tool}", tool, *watches)
    if not tool.is_file() or not os.access(tool, os.X_OK):
        raise ValueError(f"selected clang-format is unavailable: {tool}")
    return ReadyTool((str(tool),), (tool, *watches))


def clang_format_executable() -> Path:
    """Select clang-format from the prepared LLVM provider, never from PATH."""
    return Path(clang_format_selection().command[0])


def run_clang_format(files: list[str], *, check: bool, clang_format: str | None = None) -> int:
    if not files:
        return 0
    mode = ["--dry-run", "-Werror"] if check else ["-i"]
    print(f"{'Checking' if check else 'Formatting'} {len(files)} file(s) with clang-format...")
    if clang_format is None:
        clang_format = str(clang_format_executable())
    status = 0
    for start in range(0, len(files), CHUNK):
        result = subprocess.run([clang_format, *mode, *files[start : start + CHUNK]], cwd=PROJECT_ROOT)
        status = max(status, int(result.returncode != 0))
    return status


def run_ruff_format(files: list[str], *, check: bool, python: Path | None = None, tool: Path | None = None) -> int:
    if not files:
        return 0
    mode = ["--check"] if check else []
    print(f"{'Checking' if check else 'Formatting'} {len(files)} file(s) with ruff format...")
    if tool is None:
        command = python_check.ruff_command("format", *mode, *files, python=python)
    else:
        command = (str(tool), "format", "--target-version", python_check.ruff_target_version(), *mode, *files)
    result = subprocess.run(command, cwd=PROJECT_ROOT)
    return int(result.returncode != 0)


def _run(
    scope: SourceScope,
    *,
    check: bool,
    clang_format: str | None,
    python: Path | None,
    command_for: Callable[[str], tuple[str, ...]] | None,
    ruff: Path | None,
) -> int:
    source_scope.require_files(scope)
    files = list(scope.files)
    cpp_files = [name for name in files if name.endswith(git_files.CPP_SUFFIXES)]
    python_files = [name for name in files if name.endswith(git_files.PYTHON_SUFFIXES)]
    bazel_files = [name for name in files if git_files.is_bazel_file(name)]
    prettier_files = [name for name in files if git_files.is_prettier_file(name)]
    msvc_rc = compiler_rc.selects_msvc_rc(files)
    if not (cpp_files or python_files or bazel_files or prettier_files or msvc_rc):
        print("No files to format.")
        return 0

    def format_cpp() -> int:
        tool = command_for("clang-format")[0] if command_for is not None and cpp_files else clang_format
        return run_clang_format(cpp_files, check=check, clang_format=tool)

    def format_bazel() -> int:
        if command_for is not None and bazel_files:
            return buildifier.run(bazel_files, check=check, provision=False, tool=command_for("buildifier")[0])
        return buildifier.run(bazel_files, check=check, provision=False)

    def format_text() -> int:
        if command_for is not None:
            return prettier.run(prettier_files, check=check, provision=False, tool_command=command_for("prettier"))
        return prettier.run(prettier_files, check=check, provision=False)

    def format_python() -> int:
        if ruff is not None:
            return run_ruff_format(python_files, check=check, tool=ruff)
        return run_ruff_format(python_files, check=check, python=python)

    stages: list[Callable[[], int]] = [
        format_cpp,
        format_python,
        format_bazel,
    ]
    if prettier_files:
        stages.append(format_text)
    if msvc_rc:
        stages.append(lambda: compiler_rc.format_msvc_rc(check=check))
    status = 0
    for stage in stages:
        try:
            status = max(status, stage())
        except (buildifier.BuildifierError, prettier.PrettierError, OSError, ValueError) as error:
            print(f"Formatting failed: {error}", file=sys.stderr)
            if not check:
                return 2
            # Read-only checks are independent: a tool error must not hide later diagnostics.
            status = max(status, 2)
    if status == 1:
        print("Formatting issues found." if check else "Formatting failed.", file=sys.stderr)
    elif status == 0:
        print("Done.")
    return status


def rewrite(scope: SourceScope, *, clang_format: str | None = None, python: Path | None = None) -> int:
    """Rewrite selected sources using ready tools; never install missing tools."""
    return _run(scope, check=False, clang_format=clang_format, python=python, command_for=None, ruff=None)


def check(
    scope: SourceScope,
    *,
    clang_format: str | None = None,
    python: Path | None = None,
    command_for: Callable[[str], tuple[str, ...]] | None = None,
    ruff: Path | None = None,
) -> int:
    """Check formatting without rewriting sources or provisioning tools."""
    return _run(scope, check=True, clang_format=clang_format, python=python, command_for=command_for, ruff=ruff)
