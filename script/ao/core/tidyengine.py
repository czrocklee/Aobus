"""Shared clang-tidy execution engine for the tidy and analyze commands.

Owns everything the two flows have in common: compile database provisioning, Nix system
include discovery, scope resolution (changed files / folders / --all / explicit files),
and the parallel per-file runner with progress reporting.
"""

import argparse
import concurrent.futures
import json
import os
import posixpath
import re
import shlex
import subprocess
import sys
import tempfile
from collections.abc import Callable, Iterator
from dataclasses import dataclass, field
from pathlib import Path
from typing import Literal

from . import builddir, buildlock, gitfiles
from .dedup import DIAGNOSTIC_RE
from .paths import PROJECT_ROOT, absolute_path
from .proc import die

PINNED_LLVM_VERSION = "22.1.8"
PINNED_LLVM_SHA256 = "d96c2cc1736f4eb7fa43cb9bbdf56d93551a9ae0a9aadb9c99c3c3b2b712a234"
LLVM_SDK_COMPLETION_MARKER = ".aobus-llvm-sdk-complete"
LLVM_SDK_REQUIRED_FILES = (
    "bin/clang-apply-replacements.exe",
    "bin/clang-cl.exe",
    "bin/clang-format.exe",
    "bin/clang-tidy.exe",
    "include/clang-tidy/tool/ClangTidyMain.h",
    "lib/clang/22/include/stddef.h",
    "lib/clangTidyMain.lib",
    "lib/cmake/clang/ClangConfig.cmake",
    "lib/cmake/llvm/LLVMConfig.cmake",
)


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


def ensure_compile_db(
    build_dir: Path,
    configure_args: list[str] | None = None,
    *,
    preset: str | None = None,
    reconfigure_preset: bool = False,
) -> None:
    """Provision a compile DB, optionally refreshing a dedicated preset-owned tree."""
    with buildlock.build_tree_lock(build_dir):
        _ensure_compile_db(
            build_dir,
            configure_args,
            preset=preset,
            reconfigure_preset=reconfigure_preset,
        )


def _ensure_compile_db(
    build_dir: Path,
    configure_args: list[str] | None = None,
    *,
    preset: str | None = None,
    reconfigure_preset: bool = False,
) -> None:
    database = build_dir / "compile_commands.json"
    database_existed = database.is_file()
    if database_existed and not reconfigure_preset:
        return

    if reconfigure_preset:
        selected_preset = preset or builddir.preset("debug")
        action = "Refreshing" if database_existed else "Creating"
        print(f"{action} compile_commands.json with the {selected_preset} preset...")
        if selected_preset == builddir.tidy_preset("nt"):
            print(
                "  The first Windows tidy configure downloads and verifies the pinned LLVM SDK; "
                "later runs reuse the local Aobus cache."
            )
        configure = [
            "cmake",
            "-S",
            str(PROJECT_ROOT),
            "--preset",
            selected_preset,
            "-B",
            str(build_dir),
        ]
        if selected_preset == builddir.tidy_preset("nt"):
            # This dedicated tree must follow current platform defaults even
            # after a preset stops overriding an AOBUS_BUILD_* cache option.
            configure += ["-U", "AOBUS_BUILD_*"]
    elif (build_dir / "CMakeCache.txt").is_file():
        print("compile_commands.json missing, running cmake configure...")
        configure = ["cmake", str(PROJECT_ROOT), "-B", str(build_dir)]
    else:
        print("compile_commands.json missing, running cmake configure...")
        configure = [
            "cmake",
            "-S",
            str(PROJECT_ROOT),
            "--preset",
            preset or builddir.preset("debug"),
            "-B",
            str(build_dir),
        ]
    configure += configure_args or []
    _run_tail(configure, "configure")
    print("Configure done.")
    if not database.is_file():
        raise die(f"configure did not generate {database}")

    print("Generating required headers (gperf)...")
    _run_tail(
        [
            "cmake",
            "--build",
            str(build_dir),
            "--target",
            "aobus_generated_headers",
            "--parallel",
            str(os.cpu_count() or 1),
        ],
        "header generation build",
    )
    print("Generated headers are ready.")


def _cmake_cache_value(build_dir: Path, name: str) -> str | None:
    cache = build_dir / "CMakeCache.txt"
    try:
        lines = cache.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return None
    prefix = f"{name}:"
    for line in lines:
        if line.startswith(prefix) and "=" in line:
            return line.split("=", 1)[1].strip()
    return None


def llvm_sdk_root(build_dir: Path, *, os_name: str | None = None) -> Path:
    """Return the exact configured Windows SDK root without consulting PATH."""
    if builddir.platform_profile(os_name).name != "windows":
        raise die("the downloaded LLVM SDK root is only defined for native Windows tidy builds.")
    configured = _cmake_cache_value(build_dir, "AOBUS_LLVM_SDK_RESOLVED_ROOT")
    if not configured:
        raise die(
            f"pinned LLVM SDK root is missing from {build_dir / 'CMakeCache.txt'}; "
            f"reconfigure the {builddir.tidy_preset(os_name)} preset."
        )
    root = Path(configured)
    return root if root.is_absolute() else absolute_path(build_dir / root)


def llvm_sdk_version(build_dir: Path, *, os_name: str | None = None) -> str:
    """Return the exact LLVM SDK version recorded by the Windows tidy configure."""
    if builddir.platform_profile(os_name).name != "windows":
        raise die("the downloaded LLVM SDK version is only defined for native Windows tidy builds.")
    configured = _cmake_cache_value(build_dir, "AOBUS_LLVM_SDK_RESOLVED_VERSION")
    if not configured:
        raise die(
            f"pinned LLVM SDK version is missing from {build_dir / 'CMakeCache.txt'}; "
            f"reconfigure the {builddir.tidy_preset(os_name)} preset."
        )
    if configured != PINNED_LLVM_VERSION:
        raise die(
            f"configured LLVM SDK version {configured} does not match the portal pin "
            f"{PINNED_LLVM_VERSION}; reconfigure the {builddir.tidy_preset(os_name)} preset."
        )
    return configured


def ensure_windows_llvm_sdk(build_dir: Path) -> None:
    """Configure the pinned SDK tree when a native Windows tool is not ready."""
    if builddir.platform_profile().name != "windows":
        return
    root_value = _cmake_cache_value(build_dir, "AOBUS_LLVM_SDK_RESOLVED_ROOT")
    version_value = _cmake_cache_value(build_dir, "AOBUS_LLVM_SDK_RESOLVED_VERSION")
    preprovisioned_root = _cmake_cache_value(build_dir, "AOBUS_LLVM_SDK_ROOT")
    root = Path(root_value) if root_value else None
    if root is not None and not root.is_absolute():
        root = absolute_path(build_dir / root)
    if version_value == PINNED_LLVM_VERSION and root is not None:
        complete = all((root / relative).is_file() for relative in LLVM_SDK_REQUIRED_FILES)
        if complete and preprovisioned_root:
            return
        if complete:
            marker = root / LLVM_SDK_COMPLETION_MARKER
            expected_marker = f"version={PINNED_LLVM_VERSION}\nsha256={PINNED_LLVM_SHA256}\n"
            try:
                if marker.read_text(encoding="utf-8") == expected_marker:
                    return
            except OSError:
                pass
    ensure_compile_db(
        build_dir,
        ["-DAOBUS_BUILD_LINT_PLUGIN=ON"],
        preset=builddir.tidy_preset(),
        reconfigure_preset=True,
    )


def clang_resource_dir(build_dir: Path, *, os_name: str | None = None) -> Path | None:
    """Return the SDK Clang resource headers required by the custom Windows tool."""
    if builddir.platform_profile(os_name).name != "windows":
        return None
    major_version = llvm_sdk_version(build_dir, os_name=os_name).split(".", maxsplit=1)[0]
    resource_dir = llvm_sdk_root(build_dir, os_name=os_name) / "lib" / "clang" / major_version
    if not resource_dir.is_dir():
        raise die(f"pinned Clang resource directory not found: {resource_dir}")
    return resource_dir


def clang_tool(build_dir: Path, name: str, *, os_name: str | None = None) -> str:
    """Resolve the pinned LLVM tool used by the native lint implementation."""
    if builddir.platform_profile(os_name).name != "windows":
        return name

    executable = f"{name}.exe"
    if name == "clang-tidy":
        expected = build_dir / "tool" / "lint" / "AobusClangTidy.exe"
        if expected.is_file():
            return str(expected)
        matches = sorted((build_dir / "tool" / "lint").glob("**/AobusClangTidy.exe"))
        if len(matches) == 1:
            return str(matches[0])
        if len(matches) > 1:
            choices = ", ".join(str(path) for path in matches)
            raise die(f"multiple AobusClangTidy.exe tools found: {choices}")
        raise die(f"AobusClangTidy.exe not found at {expected}; build the Windows tidy target first.")

    root = llvm_sdk_root(build_dir, os_name=os_name)
    tool = root / "bin" / executable
    if not tool.is_file():
        raise die(f"pinned LLVM SDK tool not found: {tool}")
    return str(tool)


_TRANSLATION_UNIT_SUFFIXES = frozenset((".c", ".cc", ".cpp", ".cxx"))
_HEADER_SUFFIXES = frozenset((".h", ".hh", ".hpp", ".hxx"))
_PLATFORM_IMPLEMENTATION_SUFFIXES = ("", "Linux", "Posix", "Windows")


def _path_key(path: Path) -> str:
    return os.path.normcase(str(absolute_path(path)))


@dataclass(frozen=True)
class _CompileDatabaseEntry:
    path: Path
    data: dict[str, object]


def _compile_database_entries(build_dir: Path) -> list[_CompileDatabaseEntry]:
    database = build_dir / "compile_commands.json"
    try:
        entries = json.loads(database.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise die(f"cannot read {database}: {error}") from error
    if not isinstance(entries, list):
        raise die(f"invalid compilation database in {database}: expected a JSON array")

    compiled: list[_CompileDatabaseEntry] = []
    for entry in entries:
        if not isinstance(entry, dict) or not isinstance(entry.get("file"), str):
            continue
        path = Path(entry["file"])
        if not path.is_absolute():
            directory = entry.get("directory")
            base = Path(directory) if isinstance(directory, str) else PROJECT_ROOT
            path = base / path
        compiled.append(_CompileDatabaseEntry(absolute_path(path), entry))
    return compiled


def _common_parent_suffix_length(left: Path, right: Path, root: Path) -> int:
    try:
        left_parts = absolute_path(left).relative_to(absolute_path(root)).parent.parts
        right_parts = absolute_path(right).relative_to(absolute_path(root)).parent.parts
    except ValueError:
        return 0
    count = 0
    for left_part, right_part in zip(reversed(left_parts), reversed(right_parts), strict=False):
        if os.path.normcase(left_part) != os.path.normcase(right_part):
            break
        count += 1
    return count


def _common_parent_prefix_length(left: Path, right: Path, root: Path) -> int:
    try:
        left_parts = absolute_path(left).relative_to(absolute_path(root)).parent.parts
        right_parts = absolute_path(right).relative_to(absolute_path(root)).parent.parts
    except ValueError:
        return 0
    count = 0
    for left_part, right_part in zip(left_parts, right_parts, strict=False):
        if os.path.normcase(left_part) != os.path.normcase(right_part):
            break
        count += 1
    return count


def _translation_unit_tier(path: Path, root: Path) -> int:
    try:
        top = absolute_path(path).relative_to(absolute_path(root)).parts[0]
    except (IndexError, ValueError):
        return 3
    return {"lib": 0, "app": 0, "tool": 1, "test": 2}.get(top, 3)


def _is_platform_incompatible(path: Path, root: Path) -> bool:
    """Identify project files that the current native product graph cannot compile."""
    try:
        parts = tuple(part.casefold() for part in absolute_path(path).relative_to(absolute_path(root)).parts)
    except ValueError:
        return False
    joined = "/".join(parts)
    stem = path.stem.casefold()
    profile = builddir.platform_profile().name

    if profile == "linux":
        return (
            parts[:2] in {("app", "windows"), ("app", "windows-winui")}
            or parts[:3] == ("test", "unit", "windows")
            or "wasapi" in joined
            or "win32" in joined
            or stem.endswith("windows")
            or joined == "tool/lint/aobusclangtidymain.cpp"
        )
    if profile == "windows":
        return (
            parts[:2] == ("app", "linux-gtk")
            or parts[:2] in {("tool", "council"), ("test", "council")}
            or parts[:3] == ("test", "unit", "linux-gtk")
            or parts[:3] in {("test", "unit", "council"), ("test", "integration", "council")}
            or "alsa" in joined
            or "pipewire" in joined
            or stem.endswith("linux")
            or stem.endswith("posix")
        )
    return False


def _component_directory(path: Path, root: Path) -> tuple[str, ...] | None:
    """Return a normalized component directory for safe header/TU pairing.

    Public headers normalize to the implementation tree they belong to. Private
    headers retain their top-level component, preventing identical trailing
    directories in unrelated platform or layer trees from being paired.
    """
    try:
        parent = absolute_path(path).relative_to(absolute_path(root)).parent.parts
    except ValueError:
        return None
    if len(parent) >= 2 and parent[:2] == ("include", "ao"):
        return ("lib", *parent[2:])
    if len(parent) >= 3 and parent[:3] == ("app", "include", "ao"):
        return ("app", *parent[3:])
    if parent and parent[0] in {"app", "lib", "test", "tool"}:
        return parent
    return None


def _header_translation_unit(
    header: Path,
    compiled: list[_CompileDatabaseEntry],
    root: Path,
) -> Path | None:
    """Choose a same-component implementation whose flags can compile ``header`` as the main file."""
    expected_stems = {f"{header.stem}{suffix}".casefold() for suffix in _PLATFORM_IMPLEMENTATION_SUFFIXES}
    header_component = _component_directory(header, root)
    if header_component is None:
        return None
    candidates = [
        entry.path
        for entry in compiled
        if entry.path.suffix.lower() in _TRANSLATION_UNIT_SUFFIXES
        and entry.path.stem.casefold() in expected_stems
        and _component_directory(entry.path, root) == header_component
    ]

    if not candidates:
        return None

    def rank(path: Path) -> tuple[int, int, int, str]:
        return (
            0 if path.stem.casefold() == header.stem.casefold() else 1,
            -_common_parent_suffix_length(header, path, root),
            _translation_unit_tier(path, root),
            _path_key(path),
        )

    return min(candidates, key=rank)


@dataclass(frozen=True)
class _NinjaDependencyIndex:
    consumers: dict[str, Path]
    indexed_translation_units: frozenset[str]
    compiled_translation_units: frozenset[str]

    def missing_consumer_reason(self) -> str:
        if not self.indexed_translation_units:
            return "Ninja dependency data is unavailable for the compiled translation units"
        if self.indexed_translation_units != self.compiled_translation_units:
            return "Ninja dependency data is incomplete and contains no recorded consumer"
        return "no compiled translation unit consumes the header"


def _ninja_build_directories(
    database_dir: Path,
    compiled: list[_CompileDatabaseEntry],
    additional_build_dirs: tuple[Path, ...],
) -> list[Path]:
    """Return every requested real Ninja tree without building or configuring it."""
    candidates = {absolute_path(database_dir), *(absolute_path(path) for path in additional_build_dirs)}
    for entry in compiled:
        directory = entry.data.get("directory")
        if not isinstance(directory, str):
            continue
        path = Path(directory)
        if not path.is_absolute():
            path = database_dir / path
        candidates.add(absolute_path(path))
    return sorted((path for path in candidates if (path / "build.ninja").is_file()), key=_path_key)


_WINDOWS_ABSOLUTE_PATH_RE = re.compile(r"^[A-Za-z]:[/\\]")


def _ninja_record_path(value: str, base: Path) -> Path:
    """Resolve Ninja/compile-DB paths while accepting Windows separators on any test host."""
    normalized = value.replace("\\", "/")
    if _WINDOWS_ABSOLUTE_PATH_RE.match(normalized):
        return Path(posixpath.normpath(normalized))
    path = Path(normalized)
    if path.is_absolute():
        return absolute_path(path)

    normalized_base = str(base).replace("\\", "/")
    if _WINDOWS_ABSOLUTE_PATH_RE.match(normalized_base):
        return Path(posixpath.normpath(f"{normalized_base}/{normalized}"))
    return absolute_path(base / path)


def _ninja_path_key(path: Path) -> str:
    """Return a slash- and case-insensitive key for cross-platform Ninja metadata."""
    return posixpath.normpath(str(path).replace("\\", "/")).casefold()


def _compile_database_output(entry: _CompileDatabaseEntry, database_dir: Path) -> Path | None:
    output = entry.data.get("output")
    if not isinstance(output, str):
        return None
    directory = entry.data.get("directory")
    base = _ninja_record_path(directory, database_dir) if isinstance(directory, str) else database_dir
    return _ninja_record_path(output, base)


def _parse_ninja_dependency_records(
    output: str,
    build_dir: Path,
) -> Iterator[tuple[Path, tuple[Path, ...]]]:
    """Parse dependency paths from ``ninja -t deps`` records."""
    dependencies: list[Path] = []
    record_output: Path | None = None

    for line in output.splitlines():
        if not line:
            if record_output is not None:
                yield record_output, tuple(dependencies)
            dependencies = []
            record_output = None
        elif line[0].isspace():
            if record_output is not None and (dependency := line.strip()):
                dependencies.append(_ninja_record_path(dependency, build_dir))
        else:
            if record_output is not None:
                yield record_output, tuple(dependencies)
            dependencies = []
            target, marker, _ = line.partition(": #deps ")
            if marker and line.rstrip().endswith("(VALID)"):
                record_output = _ninja_record_path(target, build_dir)
            else:
                record_output = None
    if record_output is not None:
        yield record_output, tuple(dependencies)


def _ninja_dependency_index(
    database_dir: Path,
    compiled: list[_CompileDatabaseEntry],
    selected_header_keys: frozenset[str],
    additional_build_dirs: tuple[Path, ...],
) -> _NinjaDependencyIndex:
    """Index headers by real consumers while retaining primary-DB compile commands."""
    compiled_by_key = {
        _path_key(entry.path): entry.path
        for entry in compiled
        if entry.path.suffix.lower() in _TRANSLATION_UNIT_SUFFIXES
    }
    primary_by_dependency_key = {_ninja_path_key(path): path for path in compiled_by_key.values()}
    consumers_by_dependency: dict[str, set[Path]] = {}
    indexed_translation_units: set[str] = set()

    for ninja_dir in _ninja_build_directories(database_dir, compiled, additional_build_dirs):
        if _path_key(ninja_dir) == _path_key(database_dir):
            dependency_compiled = compiled
        elif (ninja_dir / "compile_commands.json").is_file():
            dependency_compiled = _compile_database_entries(ninja_dir)
        else:
            dependency_compiled = []

        compiled_by_output: dict[str, set[Path]] = {}
        compiled_by_dependency_key: dict[str, set[Path]] = {}
        for entry in dependency_compiled:
            if entry.path.suffix.lower() not in _TRANSLATION_UNIT_SUFFIXES:
                continue
            primary_consumer = compiled_by_key.get(_path_key(entry.path))
            if primary_consumer is None:
                continue
            compiled_by_dependency_key.setdefault(_ninja_path_key(entry.path), set()).add(primary_consumer)
            if output := _compile_database_output(entry, ninja_dir):
                compiled_by_output.setdefault(_ninja_path_key(output), set()).add(primary_consumer)

        try:
            result = subprocess.run(
                ["ninja", "-t", "deps"],
                cwd=ninja_dir,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                encoding="utf-8",
                errors="replace",
            )
        except FileNotFoundError as error:
            raise die("ninja is required to resolve header compile-command consumers.") from error
        if result.returncode != 0:
            raise die(
                f"cannot read Ninja dependency data from {ninja_dir}; "
                f"`ninja -t deps` exited {result.returncode}:\n{result.stdout}"
            )

        for output, dependencies in _parse_ninja_dependency_records(result.stdout, ninja_dir):
            block_consumers = set(compiled_by_output.get(_ninja_path_key(output), ()))
            if not block_consumers:
                for dependency in dependencies:
                    dependency_key = _ninja_path_key(dependency)
                    block_consumers.update(compiled_by_dependency_key.get(dependency_key, ()))
                    if primary_consumer := primary_by_dependency_key.get(dependency_key):
                        block_consumers.add(primary_consumer)
            if not block_consumers:
                continue
            indexed_translation_units.update(_path_key(consumer) for consumer in block_consumers)
            for dependency in dependencies:
                dependency_key = _ninja_path_key(dependency)
                if dependency_key in selected_header_keys:
                    consumers_by_dependency.setdefault(dependency_key, set()).update(block_consumers)

    consumers = {
        dependency: min(candidates, key=_path_key) for dependency, candidates in consumers_by_dependency.items()
    }
    return _NinjaDependencyIndex(
        consumers,
        frozenset(indexed_translation_units),
        frozenset(compiled_by_key),
    )


@dataclass(frozen=True)
class CompileCommandTarget:
    """A selected file and the exact native translation unit used to check it."""

    selected: Path
    translation_unit: Path

    @property
    def is_header(self) -> bool:
        return self.selected.suffix.lower() in _HEADER_SUFFIXES


@dataclass(frozen=True)
class CompileCommandDeferral:
    """Why a selected file cannot be checked with this native compilation database."""

    selected: Path
    reason: str
    kind: Literal["native-coverage", "platform-incompatible"] = "native-coverage"

    @property
    def is_platform_incompatible(self) -> bool:
        return self.kind == "platform-incompatible"


@dataclass(frozen=True)
class CompileCommandPlan:
    """Native clang-tidy targets plus files deferred to another platform.

    Header targets prefer a same-component implementation, then a real consumer
    recorded by Ninja, and are checked as main files using that exact compile command.
    """

    targets: tuple[CompileCommandTarget, ...]
    deferred: tuple[Path, ...]
    deferral_details: tuple[CompileCommandDeferral, ...] = ()


def compile_command_plan(
    build_dir: Path,
    files: list[Path],
    *,
    project_root: Path = PROJECT_ROOT,
    explicit_header_companions: dict[Path, Path] | None = None,
    additional_dependency_build_dirs: tuple[Path, ...] = (),
) -> CompileCommandPlan:
    """Map every covered selection to an exact translation unit from the primary database."""
    compiled = _compile_database_entries(build_dir)
    compiled_by_key = {
        _path_key(entry.path): entry.path
        for entry in compiled
        if entry.path.suffix.lower() in _TRANSLATION_UNIT_SUFFIXES
    }

    targets: list[CompileCommandTarget] = []
    deferred: list[Path] = []
    deferral_details: list[CompileCommandDeferral] = []
    seen: set[str] = set()
    dependency_index: _NinjaDependencyIndex | None = None
    selected_header_keys = frozenset(
        _ninja_path_key(absolute_path(path)) for path in files if path.suffix.lower() in _HEADER_SUFFIXES
    )
    companions = {
        _path_key(header): absolute_path(translation_unit)
        for header, translation_unit in (explicit_header_companions or {}).items()
    }
    for path in files:
        path = absolute_path(path)
        if (key := _path_key(path)) in seen:
            continue
        seen.add(key)
        suffix = path.suffix.lower()
        if suffix in _TRANSLATION_UNIT_SUFFIXES:
            translation_unit = compiled_by_key.get(key)
        elif suffix in _HEADER_SUFFIXES:
            if explicit_companion := companions.get(key):
                translation_unit = compiled_by_key.get(_path_key(explicit_companion))
                if translation_unit is None:
                    raise die(
                        f"explicit header companion {explicit_companion} has no compile command "
                        f"for selected header {path}."
                    )
            else:
                translation_unit = _header_translation_unit(path, compiled, project_root)
                if translation_unit is None:
                    if dependency_index is None:
                        dependency_index = _ninja_dependency_index(
                            build_dir,
                            compiled,
                            selected_header_keys,
                            additional_dependency_build_dirs,
                        )
                    translation_unit = dependency_index.consumers.get(_ninja_path_key(path))
        else:
            translation_unit = None
        if translation_unit is None:
            # Explicit lint fixtures and temporary files are intentionally absent
            # from the native compile database. Ordinary project files must never
            # borrow unrelated flags: without a safe companion they are deferred.
            try:
                resolved_path = absolute_path(path)
                relative_path = resolved_path.relative_to(absolute_path(project_root))
                is_under_root = True
                is_lint_fixture = relative_path.parts[:4] == ("test", "integration", "lint", "fixture")
            except ValueError:
                is_under_root = False
                is_lint_fixture = False

            is_incompatible = _is_platform_incompatible(path, project_root)

            if (not is_under_root or is_lint_fixture) and not is_incompatible and compiled:
                # Find the best matching translation unit based on common parent directory prefix
                best_match = None
                best_length = -1
                for entry in compiled:
                    if entry.path.suffix.lower() in _TRANSLATION_UNIT_SUFFIXES:
                        length = _common_parent_prefix_length(path, entry.path, project_root)
                        if length > best_length:
                            best_length = length
                            best_match = entry.path
                translation_unit = best_match if best_match is not None else compiled[0].path
            else:
                translation_unit = None

        if translation_unit is None:
            deferred.append(path)
            if is_incompatible:
                reason = "the file is incompatible with the current platform"
            elif suffix in _HEADER_SUFFIXES and dependency_index is not None:
                reason = dependency_index.missing_consumer_reason()
            elif suffix in _TRANSLATION_UNIT_SUFFIXES:
                reason = "the source file has no exact compile command"
            else:
                reason = "the file has no usable compile command"
            kind: Literal["native-coverage", "platform-incompatible"] = (
                "platform-incompatible" if is_incompatible else "native-coverage"
            )
            deferral_details.append(CompileCommandDeferral(path, reason, kind))
        else:
            targets.append(CompileCommandTarget(path, absolute_path(translation_unit)))

    return CompileCommandPlan(tuple(targets), tuple(deferred), tuple(deferral_details))


def _replace_compile_input(entry: _CompileDatabaseEntry, selected: Path) -> dict[str, object]:
    """Clone one command while replacing its exact source token with ``selected``."""
    data = dict(entry.data)
    selected_text = str(absolute_path(selected))
    arguments = data.get("arguments")
    if isinstance(arguments, list) and all(isinstance(argument, str) for argument in arguments):
        directory = data.get("directory")
        base = Path(directory) if isinstance(directory, str) else PROJECT_ROOT
        replaced = 0
        rewritten_arguments: list[str] = []
        for argument in arguments:
            argument_path = Path(argument)
            if not argument_path.is_absolute():
                argument_path = base / argument_path
            if _path_key(argument_path) == _path_key(entry.path):
                rewritten_arguments.append(selected_text)
                replaced += 1
            else:
                rewritten_arguments.append(argument)
        if replaced != 1:
            raise die(
                f"cannot derive a header compile command from {entry.path}: "
                f"expected one input argument, replaced {replaced}."
            )
        data["arguments"] = rewritten_arguments
    elif isinstance(command := data.get("command"), str):
        spellings = {entry.path.as_posix(), str(entry.path)}
        raw_file = data.get("file")
        if isinstance(raw_file, str):
            spellings.add(raw_file)
        directory = data.get("directory")
        base = Path(directory) if isinstance(directory, str) else PROJECT_ROOT
        try:
            spellings.add(os.path.relpath(entry.path, base))
        except ValueError:
            # Windows cannot express a relative path between the mapped source
            # drive and the local build drive. Absolute and raw-file spellings
            # remain available and still fail closed when neither is present.
            pass

        rewritten_command = command
        replaced = 0

        def replace_source(match: re.Match[str]) -> str:
            return f"{match.group(1)}{selected_text}"

        for spelling in sorted(spellings, key=len, reverse=True):
            normalized = spelling.replace("\\", "/")
            path_pattern = re.escape(normalized).replace("/", r"[/\\]")
            pattern = rf"(^|[\s\"']){path_pattern}(?=$|[\s\"'])"
            rewritten_command, count = re.subn(
                pattern,
                replace_source,
                rewritten_command,
                count=1,
                flags=re.IGNORECASE if os.name == "nt" else 0,
            )
            replaced += count
            if count:
                break
        if replaced != 1:
            raise die(f"cannot derive a header compile command: source token not found for {entry.path}.")
        data["command"] = rewritten_command
    else:
        raise die(f"compile command for {entry.path} has neither string command nor argument list.")

    data["file"] = absolute_path(selected).as_posix()
    data.pop("output", None)
    return data


def write_header_compile_database(
    build_dir: Path,
    targets: list[CompileCommandTarget],
    destination: Path,
    excluded_arguments: tuple[str, ...] = (),
    excluded_argument_patterns: tuple[str, ...] = (),
    additional_arguments: tuple[str, ...] = (),
    command_line_style: Literal["posix", "windows"] = "posix",
) -> Path:
    """Write exact synthetic commands that make selected headers the main files."""
    if command_line_style not in {"posix", "windows"}:
        raise die(f"unsupported compile command line style: {command_line_style}.")

    entries = {_path_key(entry.path): entry for entry in _compile_database_entries(build_dir)}
    synthetic: list[dict[str, object]] = []
    for target in targets:
        if not target.is_header:
            continue
        entry = entries.get(_path_key(target.translation_unit))
        if entry is None:
            raise die(f"compile command disappeared for mapped translation unit {target.translation_unit}.")
        data = _replace_compile_input(entry, target.selected)
        data = _without_compile_arguments(data, excluded_arguments, excluded_argument_patterns)
        arguments = data.get("arguments")
        if isinstance(arguments, list) and all(isinstance(argument, str) for argument in arguments):
            data["arguments"] = [*arguments, *additional_arguments]
        elif isinstance(command := data.get("command"), str):
            serialized_arguments = (
                subprocess.list2cmdline(additional_arguments)
                if command_line_style == "windows"
                else shlex.join(additional_arguments)
            )
            data["command"] = f"{command} {serialized_arguments}" if serialized_arguments else command
        else:
            raise die("compile command has neither string command nor argument list.")
        synthetic.append(data)

    destination.mkdir(parents=True, exist_ok=True)
    database = destination / "compile_commands.json"
    database.write_text(json.dumps(synthetic, indent=2) + "\n", encoding="utf-8")
    return destination


def _without_compile_arguments(
    data: dict[str, object],
    excluded_arguments: tuple[str, ...],
    excluded_argument_patterns: tuple[str, ...] = (),
) -> dict[str, object]:
    """Return one compile command with exact driver arguments removed."""
    filtered = dict(data)
    excluded = {argument.casefold() for argument in excluded_arguments}
    patterns = tuple(re.compile(pattern, re.IGNORECASE) for pattern in excluded_argument_patterns)
    arguments = filtered.get("arguments")
    if isinstance(arguments, list) and all(isinstance(argument, str) for argument in arguments):
        filtered["arguments"] = [
            argument
            for argument in arguments
            if argument.casefold() not in excluded and not any(pattern.fullmatch(argument) for pattern in patterns)
        ]
    elif isinstance(command := filtered.get("command"), str):
        for argument in excluded_arguments:
            pattern = rf"(?<!\S){re.escape(argument)}(?=\s|$)"
            command = re.sub(pattern, "", command, flags=re.IGNORECASE)
        for pattern in excluded_argument_patterns:
            command = re.sub(rf"(?<!\S)(?:{pattern})(?=\s|$)", "", command, flags=re.IGNORECASE)
        filtered["command"] = command
    else:
        raise die("compile command has neither string command nor argument list.")
    return filtered


def write_filtered_compile_database(
    build_dir: Path,
    destination: Path,
    excluded_arguments: tuple[str, ...],
) -> Path:
    """Copy a compile database while removing exact driver-only arguments."""
    filtered: list[dict[str, object]] = []

    for entry in _compile_database_entries(build_dir):
        filtered.append(_without_compile_arguments(entry.data, excluded_arguments))

    destination.mkdir(parents=True, exist_ok=True)
    database = destination / "compile_commands.json"
    database.write_text(json.dumps(filtered, indent=2) + "\n", encoding="utf-8")
    return destination


def write_merged_compile_database(
    build_dir: Path,
    destination: Path,
    excluded_arguments: tuple[str, ...],
    extra_entries: list[dict[str, object]],
) -> Path:
    """Merge native compile commands with validated platform-provider entries."""
    merged: list[dict[str, object]] = []
    by_path: dict[str, dict[str, object]] = {}

    for entry in [*(item.data for item in _compile_database_entries(build_dir)), *extra_entries]:
        filtered = _without_compile_arguments(entry, excluded_arguments)
        raw_file = filtered.get("file")
        if not isinstance(raw_file, str):
            raise die("compile command merge entry has no string file path.")
        path = Path(raw_file)
        if not path.is_absolute():
            directory = filtered.get("directory")
            base = Path(directory) if isinstance(directory, str) else PROJECT_ROOT
            path = base / path
        key = _path_key(path)
        if existing := by_path.get(key):
            if existing != filtered:
                raise die(f"conflicting compile commands for {absolute_path(path)}.")
            continue
        by_path[key] = filtered
        merged.append(filtered)

    destination.mkdir(parents=True, exist_ok=True)
    database = destination / "compile_commands.json"
    database.write_text(json.dumps(merged, indent=2) + "\n", encoding="utf-8")
    return destination


_COMPILE_COMMAND_ERROR_RE = re.compile(
    r"(compile command not found|error while trying to load a compilation database|"
    r"could not auto-detect compilation database)",
    re.IGNORECASE,
)


def log_has_compile_command_error(log: Path) -> bool:
    """Detect clang-tidy database failures even when the tool exits successfully."""
    try:
        return bool(_COMPILE_COMMAND_ERROR_RE.search(log.read_text(encoding="utf-8", errors="replace")))
    except OSError:
        return False


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
    if builddir.platform_profile().name == "windows":
        return []
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


def run_parallel[WorkItem](
    files: list[WorkItem],
    jobs: int,
    tmpdir: Path,
    runner: Callable[[WorkItem, Path], int],
) -> BatchResult:
    """Run `runner(file, log_path)` for every file with bounded parallelism."""
    result = BatchResult()
    total = len(files)
    done = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(jobs, 1)) as pool:
        futures = {}
        for index, file in enumerate(files):
            log = tmpdir / f"{index:06d}.log"
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
    return Path(tempfile.mkdtemp(prefix=prefix))
