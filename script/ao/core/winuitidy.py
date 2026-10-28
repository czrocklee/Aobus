"""Extract native WinUI compile commands from the Visual Studio build tree."""

import json
import os
import re
import subprocess
import xml.etree.ElementTree as ET
from collections import deque
from collections.abc import Mapping
from concurrent.futures import ThreadPoolExecutor
from contextvars import copy_context
from pathlib import Path
from types import MappingProxyType

from . import phases, workspace_cache
from .paths import PROJECT_ROOT, absolute_path
from .proc import die

_WINUI_ROOT = PROJECT_ROOT / "app" / "windows-winui"
_AUXILIARY_SOURCE_PROJECTS = MappingProxyType(
    {
        os.path.normcase(source).replace("\\", "/"): project
        for source, project in {
            "test/helper/WinUiLocalizationProbe.cpp": "app/windows-winui/ao_winui_localization_probe.vcxproj",
            "test/helper/WinUiTrackItemProbe.cpp": "app/windows-winui/ao_winui_track_item_probe.vcxproj",
            "test/winui/probe/WinUiActivityStatusProbe.cpp": "app/windows-winui/ao_winui_activity_status_probe.vcxproj",
            "test/winui/probe/WinUiAutomationProbe.cpp": "app/windows-winui/ao_winui_automation_probe.vcxproj",
            "test/winui/probe/WinUiCreditsProbe.cpp": "app/windows-winui/ao_winui_credits_probe.vcxproj",
            "test/winui/probe/WinUiQualityToolTipProbe.cpp": "app/windows-winui/ao_winui_quality_tooltip_probe.vcxproj",
            "test/winui/probe/WinUiSilentAudio.cpp": "app/windows-winui/ao_winui_automation_probe.vcxproj",
            "test/winui/probe/WinUiSoulProbe.cpp": "app/windows-winui/ao_winui_soul_probe.vcxproj",
            "test/winui/probe/WinUiTrackTableProbe.cpp": "app/windows-winui/ao_winui_track_table_probe.vcxproj",
        }.items()
    }
)
_TRANSLATION_UNIT_SUFFIXES = frozenset((".c", ".cc", ".cpp", ".cxx"))
_INCLUDE_DIRECTIVE_RE = re.compile(r'^\s*#\s*include\s*([<"])([^">]+)[">]', re.MULTILINE)


def _native_key(path: Path | str) -> str:
    """Normalize comparison strings, never compiler replay spellings."""
    return os.path.normcase(str(path)).replace("\\", "/")


def _path_key(path: Path) -> str:
    """Compare owned portal paths with native case rules, without changing replay paths."""
    return _native_key(workspace_cache.canonical_portal_path(path, project_root=PROJECT_ROOT))


def _relative_key(key: str, root_key: str) -> str | None:
    """Compare canonical keys; callers normalize authoritative roots once at entry."""
    prefix = root_key.rstrip("/") + "/"
    return key[len(prefix) :] if key.startswith(prefix) else None


def is_winui_path(path: Path, *, winui_root: Path | None = None) -> bool:
    """Test the frontend-only boundary, including owned views but not auxiliary sources."""
    root_key = _path_key(_WINUI_ROOT if winui_root is None else winui_root)
    return _relative_key(_path_key(path), root_key) is not None


def requires_winui_compile_context(path: Path) -> bool:
    """Return whether a repository file needs the native WinUI compile database."""
    source_key = _path_key(PROJECT_ROOT)
    frontend_key = _path_key(_WINUI_ROOT)
    key = _path_key(path)
    relative = _relative_key(key, source_key)
    if relative is None:
        return False
    return (
        _relative_key(key, frontend_key) is not None
        or relative in _AUXILIARY_SOURCE_PROJECTS
        or (relative[-2:].lower() == ".h" and relative[:-2] + ".cpp" in _AUXILIARY_SOURCE_PROJECTS)
    )


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
        if _relative_key(_path_key(resolved), _native_key(project_root)) is None:
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
        raise die("MSBuild query returned no JSON object.")
    try:
        payload = json.loads(output[start:])
    except json.JSONDecodeError as error:
        raise die(f"MSBuild query returned malformed JSON: {error}") from error
    if not isinstance(payload, dict):
        raise die("MSBuild query returned a non-object JSON payload.")
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


def _configured_winui_projects(build_dir: Path) -> list[Path]:
    """Read current CMake solution membership; retained project files are not authority."""
    build_dir = workspace_cache.canonical_portal_path(build_dir, project_root=PROJECT_ROOT)
    project_root_key = _native_key(build_dir / "app/windows-winui")
    name = _cmake_cache_value(build_dir, "CMAKE_PROJECT_NAME")
    hint = "regenerate the selected WinUI build tree."
    if not name or re.fullmatch(r"[^/\\:]+", name) is None:
        raise die(f"WinUI build tree has no valid CMAKE_PROJECT_NAME: {build_dir}; {hint}")
    generator = _cmake_cache_value(build_dir, "CMAKE_GENERATOR")
    if generator == "Visual Studio 18 2026":
        suffix = ".slnx"
    elif generator in (
        "Visual Studio 14 2015",
        "Visual Studio 15 2017",
        "Visual Studio 16 2019",
        "Visual Studio 17 2022",
    ):
        suffix = ".sln"
    else:
        raise die(f"unsupported or missing WinUI CMAKE_GENERATOR {generator!r} in {build_dir}; {hint}")
    solution = build_dir / f"{name}{suffix}"
    try:
        if solution.suffix == ".slnx":
            tree = ET.parse(solution)
            if tree.getroot().tag != "Solution":
                raise ValueError("expected a Solution element")
            spellings = [item.get("Path", "") for item in tree.iter("Project")]
            if any(not spelling for spelling in spellings):
                raise ValueError("Project element has no Path")
        else:
            text = solution.read_text(encoding="utf-8-sig")
            # Kitware WriteSlnHeader emits a BOM followed by a newline.
            if not text.lstrip("\r\n").startswith("Microsoft Visual Studio Solution File,"):
                raise ValueError("expected a Visual Studio solution header")
            spellings = []
            for line in text.splitlines():
                if not line.startswith("Project("):
                    continue
                match = re.match(r'^Project\("[^"\r\n]+"\)\s*=\s*"[^"\r\n]*",\s*"([^"\r\n]+)"', line)
                if match is None:
                    raise ValueError("malformed solution Project entry")
                spellings.append(match.group(1))
    except (OSError, ET.ParseError, ValueError) as error:
        raise die(f"cannot read current WinUI solution {solution}: {error}; {hint}") from error
    projects = set()
    for spelling in spellings:
        path = absolute_path(solution.parent / spelling.replace("\\", "/"))
        if path.suffix.lower() != ".vcxproj" or _relative_key(_path_key(path), project_root_key) is None:
            continue
        if not path.is_file():
            raise die(f"current WinUI MSBuild project not found: {path}; {hint}")
        projects.add(path)
    if not projects:
        raise die(f"no current WinUI projects found in {solution}; {hint}")
    return sorted(projects, key=_native_key)


def _msbuild_path(build_dir: Path) -> Path:
    generator_instance = _cmake_cache_value(build_dir, "CMAKE_GENERATOR_INSTANCE")
    if not generator_instance:
        raise die(f"WinUI build tree has no CMAKE_GENERATOR_INSTANCE: {build_dir}")
    msbuild = Path(generator_instance) / "MSBuild" / "Current" / "Bin" / "MSBuild.exe"
    if not msbuild.is_file():
        raise die(f"MSBuild.exe not found for the configured WinUI generator: {msbuild}")
    return msbuild


def _query_project(msbuild: Path, project: Path, *, commands: bool) -> subprocess.CompletedProcess[str]:
    command = [
        str(msbuild),
        str(project),
        "/nologo",
        "/v:quiet",
        "/nr:false",
        "/p:Configuration=Debug",
        "/p:Platform=x64",
        "-getItem:ClCompile",
    ]
    if commands:
        command.append("-getTargetResult:GetCompileCommands")
    return subprocess.run(command, cwd=PROJECT_ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)


def _evaluated_compile_items(payload: dict[str, object]) -> list[dict[str, object]]:
    items = payload.get("Items")
    entries = items.get("ClCompile") if isinstance(items, dict) else None
    if not isinstance(entries, list) or not all(isinstance(item, dict) for item in entries):
        raise die("MSBuild query has no valid evaluated ClCompile item list.")
    return entries


def verify_auxiliary_source_projects(
    build_dir: Path,
    *,
    evaluated_projects: Mapping[Path, dict[str, object]],
) -> None:
    """Reconcile Debug|x64 ownership from one command-scoped current solution snapshot.

    The caller holds the build-tree lock across configure, evaluation and replay
    extraction. This validator does not read the solution or query MSBuild.
    """
    build_key = _path_key(build_dir)
    source_key = _path_key(PROJECT_ROOT)
    owners: dict[str, set[str]] = {}
    for project, payload in evaluated_projects.items():
        project_key = _relative_key(_path_key(project), build_key)
        for item in _evaluated_compile_items(payload):
            spelling = item.get("Identity")
            excluded = item.get("ExcludedFromBuild", "false")
            if not isinstance(spelling, str) or not spelling:
                raise die(f"MSBuild ClCompile item has no Identity in {project}.")
            if not isinstance(excluded, str) or excluded.strip().lower() not in ("", "true", "false"):
                raise die(f"MSBuild ClCompile item has invalid ExcludedFromBuild in {project}: {excluded!r}")
            if excluded.strip().lower() == "true":
                continue
            source = Path(spelling)
            if not source.is_absolute():
                source = project.parent / source
            relative = _relative_key(_path_key(source), source_key)
            if (
                relative is not None
                and relative.startswith("test/")
                and source.suffix.lower() in _TRANSLATION_UNIT_SUFFIXES
            ):
                if project_key is not None:
                    owners.setdefault(relative, set()).add(project_key)
    issues = []
    for relative_source in sorted(owners.keys() | _AUXILIARY_SOURCE_PROJECTS.keys()):
        selected_project = _AUXILIARY_SOURCE_PROJECTS.get(relative_source)
        selected_project = _native_key(selected_project) if selected_project is not None else None
        configured = owners.get(relative_source, set())
        # Shared support can have several owners, but replay selects one real Debug owner.
        if selected_project is None or selected_project not in configured:
            issues.append(f"  {relative_source}: selected={selected_project!r}, configured={sorted(configured)}")
    if issues:
        raise die(
            "WinUI auxiliary source/project mapping differs from configured Debug|x64 ownership:\n" + "\n".join(issues)
        )


def compile_commands(
    build_dir: Path,
    clang_cl: Path,
    *,
    required_translation_units: tuple[Path, ...] = (),
    jobs: int = 1,
) -> list[dict[str, object]]:
    """Return repository-owned WinUI commands from one configured VS tree."""
    build_dir = workspace_cache.canonical_portal_path(build_dir, project_root=PROJECT_ROOT)
    source_key = _path_key(PROJECT_ROOT)
    frontend_key = _path_key(_WINUI_ROOT)
    msbuild = _msbuild_path(build_dir)
    current_projects = _configured_winui_projects(build_dir)
    if not clang_cl.is_file():
        raise die(f"pinned clang-cl.exe not found: {clang_cl}")

    commands: list[dict[str, object]] = []
    seen: set[str] = set()
    required_by_project: dict[Path, set[str]] = {}
    required_keys = {required: _path_key(required) for required in required_translation_units}
    for key in required_keys.values():
        relative = _relative_key(key, source_key)
        project_relative = _AUXILIARY_SOURCE_PROJECTS.get(relative) if relative is not None else None
        if project_relative is not None:
            required_by_project.setdefault(build_dir / project_relative, set()).add(key)

    projects: list[tuple[Path, set[str] | None]] = [
        (build_dir / "app" / "windows-winui" / "aobus-winui-lib.vcxproj", None)
    ]
    projects.extend(sorted(required_by_project.items(), key=lambda item: _native_key(item[0])))
    current_keys = {_native_key(project) for project in current_projects}
    for project, _ in projects:
        if _native_key(project) not in current_keys:
            raise die(
                f"WinUI replay project is absent from the current solution: {project}; "
                "regenerate the selected WinUI build tree."
            )
    selected_keys = {_native_key(project) for project, _ in projects}
    query_projects = [project for project, _ in projects]
    query_projects.extend(project for project in current_projects if _native_key(project) not in selected_keys)
    inventory = tuple((project, _native_key(project) in selected_keys) for project in query_projects)
    # Metadata evaluation never exceeds the caller's explicit worker width.
    width = min(max(jobs, 1), len(inventory))
    with phases.measure("compile-commands", tree="winui", jobs=width, count=len(inventory)) as phase:
        with ThreadPoolExecutor(max_workers=width) as pool:
            futures = tuple(
                pool.submit(copy_context().run, _query_project, msbuild, project, commands=selected)
                for project, selected in inventory
            )
        # Join every worker before observing failures or leaving the caller's lock
        # and workspace-view context. Publish results/errors in inventory order.
        outputs: dict[Path, str] = {}
        failures: list[str] = []
        for (project, _), future in zip(inventory, futures, strict=True):
            try:
                result = future.result()
            except Exception as error:
                if not failures:
                    phase.status = 1
                failures.append(f"MSBuild WinUI query failed for {project}: {error}")
                continue
            if result.returncode != 0:
                if not failures:
                    phase.status = result.returncode
                failures.append(
                    f"MSBuild WinUI query failed for {project} (exit {result.returncode}):\n{result.stdout}"
                )
            else:
                outputs[project] = result.stdout
        if failures:
            raise die("\n".join(failures))
        evaluated_projects = {project: _parse_msbuild_json(output) for project, output in outputs.items()}
        phase.status = 0
        verify_auxiliary_source_projects(build_dir, evaluated_projects=evaluated_projects)
        for project, accepted_keys in projects:
            for item in _target_items(evaluated_projects[project]):
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
                    compiler_path = absolute_path(path)
                    path = workspace_cache.canonical_portal_path(compiler_path, project_root=PROJECT_ROOT)
                    key = _native_key(path)
                    if accepted_keys is None:
                        if _relative_key(key, frontend_key) is None:
                            continue
                    elif key not in accepted_keys:
                        continue
                    if path.suffix.lower() not in _TRANSLATION_UNIT_SUFFIXES or not path.is_file():
                        continue
                    if key in seen:
                        raise die(f"MSBuild returned duplicate WinUI compile commands for {path}.")
                    seen.add(key)
                    commands.append(
                        {
                            "directory": str(directory),
                            "command": f'"{clang_cl}" {identity} "{compiler_path}"',
                            "file": str(compiler_path),
                        }
                    )

        if not commands:
            raise die("MSBuild returned no repository-owned WinUI translation units.")
        missing = [path for path, key in required_keys.items() if key not in seen]
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
    root = workspace_cache.canonical_portal_path(project_root, project_root=PROJECT_ROOT)
    source_root = workspace_cache.canonical_portal_path(winui_root, project_root=PROJECT_ROOT)
    source_key = _native_key(source_root)
    include_roots = (
        source_root,
        source_root / "include",
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
        source = workspace_cache.canonical_portal_path(source, project_root=root)
        if _relative_key(_path_key(source), source_key) is None and not requires_winui_compile_context(source):
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
