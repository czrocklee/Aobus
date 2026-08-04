"""Extract native WinUI compile commands from the Visual Studio build tree."""

import json
import re
import subprocess
from collections import deque
from pathlib import Path

from .paths import PROJECT_ROOT, absolute_path
from .proc import die

_WINUI_ROOT = PROJECT_ROOT / "app" / "windows-winui"
_TRANSLATION_UNIT_SUFFIXES = frozenset((".c", ".cc", ".cpp", ".cxx"))
_INCLUDE_DIRECTIVE_RE = re.compile(r'^\s*#\s*include\s*([<"])([^">]+)[">]', re.MULTILINE)


def _path_key(path: Path) -> str:
    """Return a platform-neutral key for comparing repository paths."""
    return str(path).replace("\\", "/").casefold()


def _resolve_include(
    including_file: Path,
    delimiter: str,
    spelling: str,
    include_roots: tuple[Path, ...],
    project_root: Path,
) -> Path | None:
    """Resolve one repository include without attempting to model compiler system headers."""
    candidates: list[Path] = []
    if delimiter == '"':
        candidates.append(including_file.parent / spelling)
    candidates.extend(root / spelling for root in include_roots)
    for candidate in candidates:
        resolved = absolute_path(candidate)
        try:
            resolved.relative_to(project_root)
        except ValueError:
            continue
        if resolved.is_file():
            return resolved
    return None


def _repository_include_distances(
    source: Path,
    *,
    include_roots: tuple[Path, ...],
    project_root: Path,
) -> dict[str, int]:
    """Index reachable repository headers and their shortest include distance from a source."""
    distances: dict[str, int] = {}
    visited: set[str] = {_path_key(source)}
    pending: deque[tuple[Path, int]] = deque([(source, 0)])
    while pending:
        current, distance = pending.popleft()
        try:
            contents = current.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for match in _INCLUDE_DIRECTIVE_RE.finditer(contents):
            dependency = _resolve_include(
                current,
                match.group(1),
                match.group(2).strip(),
                include_roots,
                project_root,
            )
            if dependency is None:
                continue
            dependency_key = _path_key(dependency)
            dependency_distance = distance + 1
            previous_distance = distances.get(dependency_key)
            if previous_distance is None or dependency_distance < previous_distance:
                distances[dependency_key] = dependency_distance
            if dependency_key not in visited:
                visited.add(dependency_key)
                pending.append((dependency, dependency_distance))
    return distances


def _cmake_cache_value(build_dir: Path, name: str) -> str | None:
    cache = build_dir / "CMakeCache.txt"
    try:
        lines = cache.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError as error:
        raise die(f"cannot read WinUI CMake cache {cache}: {error}") from error
    prefix = f"{name}:"
    for line in lines:
        if not line.startswith(prefix):
            continue
        _, _, value = line.partition("=")
        return value
    return None


def _parse_msbuild_json(output: str) -> dict[str, object]:
    start = output.find("{")
    if start < 0:
        raise die("MSBuild GetCompileCommands returned no JSON object.")
    try:
        payload = json.loads(output[start:])
    except json.JSONDecodeError as error:
        raise die(f"MSBuild GetCompileCommands returned malformed JSON: {error}") from error
    if not isinstance(payload, dict):
        raise die("MSBuild GetCompileCommands returned a non-object JSON payload.")
    return payload


def _target_items(payload: dict[str, object]) -> list[dict[str, object]]:
    target_results = payload.get("TargetResults")
    if not isinstance(target_results, dict):
        raise die("MSBuild result has no TargetResults object.")
    target = target_results.get("GetCompileCommands")
    if not isinstance(target, dict) or target.get("Result") != "Success":
        raise die("MSBuild GetCompileCommands did not report success.")
    items = target.get("Items")
    if not isinstance(items, list) or not items:
        raise die("MSBuild GetCompileCommands returned no command items.")
    if not all(isinstance(item, dict) for item in items):
        raise die("MSBuild GetCompileCommands returned an invalid item.")
    return items


def compile_commands(
    build_dir: Path,
    clang_cl: Path,
    *,
    required_translation_units: tuple[Path, ...] = (),
) -> list[dict[str, object]]:
    """Return repository-owned WinUI commands from one configured VS tree."""
    generator_instance = _cmake_cache_value(build_dir, "CMAKE_GENERATOR_INSTANCE")
    if not generator_instance:
        raise die(f"WinUI build tree has no CMAKE_GENERATOR_INSTANCE: {build_dir}")
    msbuild = Path(generator_instance) / "MSBuild" / "Current" / "Bin" / "MSBuild.exe"
    project = build_dir / "app" / "windows-winui" / "aobus-winui-lib.vcxproj"
    if not msbuild.is_file():
        raise die(f"MSBuild.exe not found for the configured WinUI generator: {msbuild}")
    if not project.is_file():
        raise die(f"WinUI MSBuild project not found: {project}")
    if not clang_cl.is_file():
        raise die(f"pinned clang-cl.exe not found: {clang_cl}")

    command = [
        str(msbuild),
        str(project),
        "/nologo",
        "/v:quiet",
        "/p:Configuration=Release",
        "/p:Platform=x64",
        "-getTargetResult:GetCompileCommands",
    ]
    result = subprocess.run(
        command,
        cwd=PROJECT_ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    if result.returncode != 0:
        raise die(f"MSBuild GetCompileCommands failed (exit {result.returncode}):\n{result.stdout}")

    commands: list[dict[str, object]] = []
    seen: set[str] = set()
    root = absolute_path(_WINUI_ROOT)
    for item in _target_items(_parse_msbuild_json(result.stdout)):
        identity = item.get("Identity")
        working_directory = item.get("WorkingDirectory")
        files = item.get("Files")
        if (
            not isinstance(identity, str)
            or not identity
            or not isinstance(working_directory, str)
            or not working_directory
            or not isinstance(files, str)
            or not files
        ):
            raise die("MSBuild compile-command item is missing Identity, WorkingDirectory, or Files.")
        directory = Path(working_directory)
        for spelling in files.split(";"):
            if not spelling:
                continue
            path = Path(spelling)
            if not path.is_absolute():
                path = directory / path
            path = absolute_path(path)
            try:
                path.relative_to(root)
            except ValueError:
                continue
            if path.suffix.lower() not in _TRANSLATION_UNIT_SUFFIXES or not path.is_file():
                continue
            key = str(path).casefold()
            if key in seen:
                raise die(f"MSBuild returned duplicate WinUI compile commands for {path}.")
            seen.add(key)
            commands.append(
                {
                    "directory": str(directory),
                    "command": f'"{clang_cl}" {identity} "{path}"',
                    "file": str(path),
                }
            )

    if not commands:
        raise die("MSBuild returned no repository-owned WinUI translation units.")
    missing = [path for path in required_translation_units if str(absolute_path(path)).casefold() not in seen]
    if missing:
        details = "\n".join(f"  {path}" for path in missing)
        raise die(f"MSBuild returned no WinUI compile command for:\n{details}")
    return commands


def find_header_companions(
    commands: list[dict[str, object]],
    headers: tuple[Path, ...],
    *,
    project_root: Path,
    winui_root: Path,
) -> dict[Path, Path]:
    """Find exact WinUI translation units that textually include selected headers.

    The Visual Studio compile-command query does not expose Ninja's header dependency
    graph.  Build a small repository-only include index from those same native WinUI
    translation units instead of maintaining one header-to-source rule per header.  A
    shortest reachable include wins; ties are deterministic.  Headers that cannot be
    reached are deliberately omitted so the normal coverage check can defer them.
    """
    root = absolute_path(project_root)
    source_root = absolute_path(winui_root)
    include_roots = (
        source_root,
        root / "app" / "include",
        root / "include",
        root,
    )
    requested = {(_path_key(absolute_path(header))): absolute_path(header) for header in headers}
    if not requested:
        return {}

    sources: set[Path] = set()
    for command in commands:
        spelling = command.get("file")
        if not isinstance(spelling, str) or not spelling:
            continue
        source = Path(spelling)
        if not source.is_absolute():
            directory = command.get("directory")
            if isinstance(directory, str) and directory:
                source = Path(directory) / source
        source = absolute_path(source)
        try:
            source.relative_to(source_root)
        except ValueError:
            continue
        if source.suffix.lower() in _TRANSLATION_UNIT_SUFFIXES and source.is_file():
            sources.add(source)

    candidates: dict[str, list[tuple[int, str, Path]]] = {key: [] for key in requested}
    for source in sorted(sources, key=_path_key):
        distances = _repository_include_distances(
            source,
            include_roots=include_roots,
            project_root=root,
        )
        for header_key in requested:
            if distance := distances.get(header_key):
                candidates[header_key].append((distance, _path_key(source), source))

    return {requested[header_key]: min(matches)[2] for header_key, matches in candidates.items() if matches}
