"""Shared clang-tidy execution engine for the tidy and analyze commands.

Owns everything the two flows have in common: compile database provisioning, Nix system
include discovery, scope resolution (changed files / folders / --all / explicit files),
and the parallel per-file runner with progress reporting.
"""

import argparse
import concurrent.futures
import json
import os
import re
import subprocess
import sys
import tempfile
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path

from . import builddir, gitfiles
from .dedup import DIAGNOSTIC_RE
from .paths import PROJECT_ROOT, absolute_path
from .proc import die

PINNED_LLVM_VERSION = "22.1.8"
PINNED_LLVM_SHA256 = "d96c2cc1736f4eb7fa43cb9bbdf56d93551a9ae0a9aadb9c99c3c3b2b712a234"
LLVM_SDK_COMPLETION_MARKER = ".aobus-llvm-sdk-complete"
LLVM_SDK_REQUIRED_FILES = (
    "bin/clang-apply-replacements.exe",
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
class CompileCommandTarget:
    """A selected file and the exact native translation unit used to check it."""

    selected: Path
    translation_unit: Path

    @property
    def is_header(self) -> bool:
        return self.selected.suffix.lower() in _HEADER_SUFFIXES


@dataclass(frozen=True)
class CompileCommandPlan:
    """Native clang-tidy targets plus files deferred to another platform.

    Header targets require a same-component implementation and are checked as main
    files using a synthetic compile command copied from that implementation. Textual
    include reachability is intentionally insufficient because conditional includes
    can name a header that the native preprocessor never enters.
    """

    targets: tuple[CompileCommandTarget, ...]
    deferred: tuple[Path, ...]


def compile_command_plan(
    build_dir: Path,
    files: list[Path],
    *,
    project_root: Path = PROJECT_ROOT,
) -> CompileCommandPlan:
    """Map every covered selection to an exact native translation unit."""
    compiled = _compile_database_entries(build_dir)
    compiled_by_key = {
        _path_key(entry.path): entry.path
        for entry in compiled
        if entry.path.suffix.lower() in _TRANSLATION_UNIT_SUFFIXES
    }

    targets: list[CompileCommandTarget] = []
    deferred: list[Path] = []
    seen: set[str] = set()
    for path in files:
        path = absolute_path(path)
        if (key := _path_key(path)) in seen:
            continue
        seen.add(key)
        suffix = path.suffix.lower()
        if suffix in _TRANSLATION_UNIT_SUFFIXES:
            translation_unit = compiled_by_key.get(key)
        elif suffix in _HEADER_SUFFIXES:
            translation_unit = _header_translation_unit(path, compiled, project_root)
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

            # Do not use fallback for files that are platform-specific and incompatible with this platform
            is_incompatible = False
            path_lower = path.as_posix().lower()
            if builddir.platform_profile().name == "linux":
                if "wasapi" in path_lower or "win32" in path_lower:
                    is_incompatible = True
            elif builddir.platform_profile().name == "windows":
                if any(x in path_lower for x in ("alsa", "pipewire", "linux", "gtk")):
                    is_incompatible = True

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
        else:
            targets.append(CompileCommandTarget(path, absolute_path(translation_unit)))

    return CompileCommandPlan(tuple(targets), tuple(deferred))


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
) -> Path:
    """Write exact synthetic commands that make selected headers the main files."""
    entries = {_path_key(entry.path): entry for entry in _compile_database_entries(build_dir)}
    synthetic: list[dict[str, object]] = []
    for target in targets:
        if not target.is_header:
            continue
        entry = entries.get(_path_key(target.translation_unit))
        if entry is None:
            raise die(f"compile command disappeared for mapped translation unit {target.translation_unit}.")
        synthetic.append(_replace_compile_input(entry, target.selected))

    destination.mkdir(parents=True, exist_ok=True)
    database = destination / "compile_commands.json"
    database.write_text(json.dumps(synthetic, indent=2) + "\n", encoding="utf-8")
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
