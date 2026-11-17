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
from ao.portal.source_scope import SourceScope

CHUNK = 100


def run_clang_format(files: list[str], *, check: bool, clang_format: str | None = None) -> int:
    if not files:
        return 0
    mode = ["--dry-run", "-Werror"] if check else ["-i"]
    print(f"{'Checking' if check else 'Formatting'} {len(files)} file(s) with clang-format...")
    if clang_format is None:
        clang_format = "clang-format"
        if build_dir.platform_profile().name == "windows":
            try:
                manifest = windows_llvm_sdk.read_manifest(windows_llvm_sdk.MANIFEST)
                root, _cached = windows_llvm_sdk.ready_root(os.environ, manifest)
                clang_format = str(root / "bin/clang-format.exe")
            except windows_llvm_sdk.WindowsLlvmSdkError as error:
                raise ValueError(
                    f"format requires the ready pinned LLVM SDK: {error}; run ao setup llvm-sdk explicitly"
                ) from error
    status = 0
    for start in range(0, len(files), CHUNK):
        result = subprocess.run([clang_format, *mode, *files[start : start + CHUNK]], cwd=PROJECT_ROOT)
        status = max(status, int(result.returncode != 0))
    return status


def run_ruff_format(files: list[str], *, check: bool, python: Path | None = None) -> int:
    if not files:
        return 0
    mode = ["--check"] if check else []
    print(f"{'Checking' if check else 'Formatting'} {len(files)} file(s) with ruff format...")
    result = subprocess.run(python_check.ruff_command("format", *mode, *files, python=python), cwd=PROJECT_ROOT)
    return int(result.returncode != 0)


def _run(scope: SourceScope, *, check: bool, clang_format: str | None, python: Path | None) -> int:
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
    stages: list[Callable[[], int]] = [
        lambda: run_clang_format(cpp_files, check=check, clang_format=clang_format),
        lambda: run_ruff_format(python_files, check=check, python=python),
        lambda: buildifier.run(bazel_files, check=check, provision=False),
    ]
    if prettier_files:
        stages.append(lambda: prettier.run(prettier_files, check=check, provision=False))
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
    return _run(scope, check=False, clang_format=clang_format, python=python)


def check(scope: SourceScope, *, clang_format: str | None = None, python: Path | None = None) -> int:
    """Check formatting without rewriting sources or provisioning tools."""
    return _run(scope, check=True, clang_format=clang_format, python=python)
