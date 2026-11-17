"""CLI-only adapters for shared native options and frozen source selections."""

from __future__ import annotations

import argparse
import os
from dataclasses import fields
from pathlib import Path
from typing import Any

from ao.core import jobs
from ao.portal import bazel_workflows, build_dir, build_env, git_files, source_scope


def add_source_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("paths", nargs="*", metavar="path", help="source files or directories")
    parser.add_argument("--all", action="store_true", help="select the governed whole-source inventory")
    parser.add_argument("--commit", metavar="rev", help="select changes since this revision, including local changes")


def add_dependency_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--config", choices=("fastbuild", "debug", "release"), default="fastbuild")
    parser.add_argument("-p", "--path", help="external governed SDK tree (honors BUILD_DIR)")
    parser.add_argument("--vcpkg-prefix", help="existing governed native SDK prefix")
    parser.add_argument("-j", "--jobs", type=bazel_workflows.positive_jobs, default=jobs.default_workers())
    parser.add_argument("--json", type=Path, help="write the complete report to this path")


def add_coverage_arguments(parser: argparse.ArgumentParser, *, measurement: bool) -> None:
    parser.add_argument("-p", "--path", help="external Linux coverage tree (honors BUILD_DIR)")
    parser.add_argument("-j", "--jobs", type=bazel_workflows.positive_jobs, default=jobs.default_workers())
    parser.add_argument("--compiler", choices=("gcc", "clang"), default="gcc")
    parser.add_argument(
        "--suite", choices=("default", "core", "application", "backend", "cli", "tui", "gtk", "all"), default="default"
    )
    if measurement:
        parser.add_argument("--scope", action="append", default=[], help="report prefix; does not narrow execution")
    bazel_workflows.add_remote_cache_argument(parser, "trusted native action cache URL")


def native_values(args: argparse.Namespace) -> dict[str, Any]:
    return {item.name: getattr(args, item.name) for item in fields(bazel_workflows.Request) if hasattr(args, item.name)}


def selection(args: argparse.Namespace) -> source_scope.Selection:
    selected = source_scope.Selection(tuple(args.paths), args.all, args.commit)
    source_scope.validate(selected)
    return selected


def resolved_scope(args: argparse.Namespace, *, analyzer: bool = False) -> source_scope.SourceScope:
    """Reuse a launcher-prepared scope, including individual-file provenance."""
    selected = selection(args)
    docs = bool(getattr(args, "docs", False))
    if docs and (selected.paths or selected.all or selected.commit is not None):
        raise ValueError("--docs cannot be combined with paths, --all or --commit")
    build_env.load_source_scope(args, os.environ.get("AOBUS_PREFLIGHT_SCOPE"))
    if hasattr(args, "_resolved_sources"):
        scope = source_scope.prepared(args._resolved_sources, getattr(args, "_explicit_sources", ()))
        source_scope.require_files(scope)
        return scope
    if docs:
        from ao.portal import documentation_checks

        scope = documentation_checks.markdown_scope()
    elif analyzer:
        from ao.portal import source_analyzer

        scope = source_analyzer.select(selected)
    else:
        scope = source_scope.resolve(selected)
    source_scope.require_files(scope)
    build_env.record_source_scope(args, scope)
    return scope


def has_native_sources(scope: source_scope.SourceScope) -> bool:
    return any(name.endswith(git_files.CPP_SUFFIXES) for name in scope.files)


def needs_formatter_environment(scope: source_scope.SourceScope) -> bool:
    # Windows selects the ready LLVM SDK without initializing Visual Studio.
    return has_native_sources(scope) and build_dir.platform_profile().name == "macos"


def has_python_sources(scope: source_scope.SourceScope) -> bool:
    return any(name.endswith(git_files.PYTHON_SUFFIXES) for name in scope.files)
