"""ao tidy — project clang-tidy with the Aobus plugin, strict/relaxed modes, and dedup.

Each file is classified as STRICT (lib/app/include/tool) or RELAXED (test/). Diagnostics
are de-duplicated across translation units before being printed or written to a file.
"""

import argparse
import contextlib
import ctypes
import fnmatch
import hashlib
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

from ..core import (
    builddir,
    buildlock,
    gitfiles,
    phases,
    pythoncheck,
    tidyconfig,
    tidyengine,
    winuitidy,
    workspace_cache,
)
from ..core.dedup import deduplicate
from ..core.paths import PROJECT_ROOT, absolute_path
from ..core.phases import LOG_DIR_ENV
from ..core.proc import die
from ..core.tidyconfig import checks_for, config_for, mode_disabled_checks
from ..core.tidyengine import TidyToolchain as TidyToolchain
from . import build

HELP = "Run C++ clang-tidy and Python Ruff/mypy checks"
NAME = "tidy"
# True when ao.bat must initialize the MSVC/vcpkg build environment first.
REQUIRES_BUILD_ENV = True
REQUIRES_PYTHON_TOOLS = True


EPILOG = """\
scope: with no arguments, checks files changed since the merge base with local main + working tree +
staged + untracked. Explicit files, --folder, --commit, or --all override that.

examples:
  ./ao tidy                                  # changed files
  ./ao tidy --all                            # whole repository
  ./ao tidy --folder lib --folder script     # selected folders
  ./ao tidy --commit HEAD~3 -o report.txt    # changes since a commit, to a file
  ./ao tidy --check readability-implicit-bool-conversion
"""

ALL_FOLDERS = ["lib", "app", "include", "script", "test", "tool"]
WINUI_ROOT = PROJECT_ROOT / "app" / "windows-winui"
# Clang tooling does not consume MSVC code-generation toggles. Strip deterministic
# output and both IPO spellings; MSVC 14.51 workarounds may override /GL with /GL-.
WINDOWS_EXCLUDED_COMPILE_ARGUMENTS = (
    "/Zc:preprocessor",
    "/c",
    "/ZW:nostdlib",
    "/GL",
    "/GL-",
    "/experimental:deterministic",
)
WINDOWS_FORCED_CMAKE_PCH_PATTERN = r'/FI(?:"[^"]*[/\\]cmake_pch\.hxx"|[^\s]*[/\\]cmake_pch\.hxx)'


def expected_aobus_checks(fixture_dir: Path | None = None) -> frozenset[str]:
    """Read the working-tree fixture inventory needed by C++ toolchain preflight.

    Include untracked additions just as the lint suite does. Non-blank supported
    files establish inventory membership, not diagnostic or positive coverage.
    """
    from ..core.linttest import FIXTURE_DIR, discover_fixtures

    root = FIXTURE_DIR if fixture_dir is None else fixture_dir
    try:
        checks = frozenset(
            fixture.check
            for fixture in discover_fixtures(root)
            if fixture.check.startswith("aobus-") and fixture.path.read_text(encoding="utf-8", errors="replace").strip()
        )
    except OSError as error:
        raise die(f"cannot read Aobus checker fixtures under {root}: {error}") from error
    if not checks:
        raise die(f"no non-blank Aobus checker fixtures found under {root}; cannot validate the toolchain registry.")
    return checks


_PATH_SEPARATOR_RE = r"[/\\]"


def _path_regex(path: Path) -> str:
    return re.escape(path.as_posix().rstrip("/")).replace("/", _PATH_SEPARATOR_RE)


def _source_spellings(path: Path, source_roots: tuple[Path, ...]) -> list[Path]:
    canonical = absolute_path(path)
    try:
        relative = canonical.relative_to(absolute_path(PROJECT_ROOT))
    except ValueError:
        return [canonical]

    spellings: list[Path] = []
    for root in source_roots:
        spelling = Path(os.path.abspath(root / relative))
        if spelling not in spellings:
            spellings.append(spelling)
    return spellings


def project_header_filter(folders: tuple[str, ...], *, source_roots: tuple[Path, ...] | None = None) -> str:
    """Return a project-root filter that accepts native and POSIX separators."""
    roots = tidyconfig.source_roots_or_default(source_roots, project_root=absolute_path(PROJECT_ROOT))
    root = "|".join(_path_regex(path) for path in roots)
    return f"({root}){_PATH_SEPARATOR_RE}({'|'.join(folders)}){_PATH_SEPARATOR_RE}.*"


STRICT_HEADER_FOLDERS = ("lib", "app", "include", "tool")
RELAXED_HEADER_FOLDERS = ("test", "include")


def exact_header_filter(headers: list[Path], *, source_roots: tuple[Path, ...] | None = None) -> str:
    """Return an anchored filter for mapped headers with either path separator."""
    roots = tidyconfig.source_roots_or_default(source_roots, project_root=absolute_path(PROJECT_ROOT))
    alternatives = [_path_regex(spelling) for path in headers for spelling in _source_spellings(path, roots)]
    return f"^({'|'.join(alternatives)})$"


def path_line_filter(paths: list[Path], *, source_roots: tuple[Path, ...] | None = None) -> str:
    """Limit diagnostics to the explicitly selected paths."""
    roots = tidyconfig.source_roots_or_default(source_roots, project_root=absolute_path(PROJECT_ROOT))
    # LLVM matches these names literally, unlike the separator-aware header regex.
    names = dict.fromkeys(
        name
        for path in paths
        for spelling in _source_spellings(path, roots)
        for name in (str(spelling), spelling.as_posix())
    )
    entries = [{"name": name, "lines": [[1, 2_147_483_647]]} for name in names]
    return json.dumps(entries, separators=(",", ":"))


def write_selected_file_overlay(destination: Path, selected: Path, *, source_roots: tuple[Path, ...]) -> Path:
    """Keep LLVM's file identity stable without replacing any selected bytes."""
    spellings = _source_spellings(selected, source_roots)
    if not selected.is_file() or not spellings:
        raise ValueError(f"selected file is unavailable: {selected}")
    for spelling in spellings:
        if not spelling.is_absolute() or not spelling.is_file() or not spelling.samefile(selected):
            raise ValueError(f"selected-file alias does not identify {selected}: {spelling}")
    # LLVM's Windows VFS lookup handles mixed separators, including nested
    # includes. External names make FileEntryRef and diagnostic paths use this
    # validated full name, rather than inventing include-spelling permutations.
    payload = {
        "version": 0,
        "case-sensitive": False,
        "use-external-names": True,
        "roots": [
            {"type": "file", "name": spelling.as_posix(), "external-contents": str(spellings[0])}
            for spelling in dict.fromkeys(spellings)
        ],
    }
    destination.write_text(json.dumps(payload, ensure_ascii=False), encoding="utf-8")
    return destination


def _validate_selected_vfs_inputs(database_dir: Path, translation_units: set[Path], tidy_arguments: list[str]) -> None:
    """Fail closed instead of shadowing caller-owned virtual file contents."""

    def key(path: Path) -> str:
        return os.path.normcase(str(workspace_cache.canonical_portal_path(path)))

    def command_arguments(command: str) -> list[str]:
        if os.name != "nt":
            return shlex.split(command, posix=False)
        count = ctypes.c_int()
        shell: ctypes.CDLL = vars(ctypes)["WinDLL"]("shell32", use_last_error=True)
        split = shell.CommandLineToArgvW
        split.argtypes = (ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_int))
        split.restype = ctypes.POINTER(ctypes.c_wchar_p)
        arguments = split(command, ctypes.byref(count))
        if not arguments:
            raise OSError("cannot inspect Windows compile-command arguments")
        try:
            return list(arguments[: count.value])
        finally:
            kernel: ctypes.CDLL = vars(ctypes)["WinDLL"]("kernel32", use_last_error=True)
            free = kernel.LocalFree
            free.argtypes = (ctypes.c_void_p,)
            free.restype = ctypes.c_void_p
            free(arguments)

    required = {key(path) for path in translation_units}
    found: set[str] = set()
    arguments_to_check = list(tidy_arguments)
    for variable in ("CL", "_CL_"):
        arguments_to_check.extend(command_arguments("clang-cl " + os.environ.get(variable, ""))[1:])
    for entry in json.loads((database_dir / "compile_commands.json").read_text(encoding="utf-8")):
        path = Path(entry["file"])
        if not path.is_absolute():
            path = Path(entry["directory"]) / path
        identity = key(path)
        if identity not in required:
            continue
        found.add(identity)
        arguments = entry.get("arguments")
        arguments_to_check.extend(arguments if isinstance(arguments, list) else command_arguments(entry["command"]))
    if found != required:
        raise ValueError("selected fragment compile commands could not be inspected for VFS overrides")
    # Caller VFS overlays, response files, and config files cannot compose
    # with the selected-file overlay: their hidden contents cannot be inspected
    # or merged here. Do not reinterpret or replace those contexts until
    # composition can preserve their virtual contents. The slash overlay
    # spellings are also rejected as defense in depth.
    wrappers = ("--extra-arg-before=", "-extra-arg-before=", "--extra-arg=", "-extra-arg=", "/clang:")
    for argument in arguments_to_check:
        token = argument.strip("\"'").casefold()
        while wrapper := next((prefix for prefix in wrappers if token.startswith(prefix)), None):
            token = token.removeprefix(wrapper).strip("\"'")
        if token.startswith(("-ivfsoverlay", "-vfsoverlay", "--vfsoverlay", "/ivfsoverlay", "/vfsoverlay")):
            raise ValueError(
                "selected Windows fragments cannot compose caller VFS overlays with the selected-file overlay"
            )
        # The governed clang-tidy invocation passes its own -config, and a
        # hidden compiler response or config file may carry overlays.
        if token.startswith(("@", "--config=", "-config=")) or token in ("--config", "-config"):
            raise ValueError(
                "selected Windows fragments cannot compose response/config files: their hidden contents "
                "may shadow the selected-file overlay, and a caller config cannot override the governed "
                "clang-tidy -config"
            )


@dataclass(frozen=True)
class TidyInvocation:
    """One selected file and the native command supplying its compiler flags."""

    selected: Path
    compile_command_source: Path

    @property
    def is_header(self) -> bool:
        return self.selected.suffix.lower() in {".h", ".hh", ".hpp", ".hxx"}

    @property
    def is_include_fragment(self) -> bool:
        return self.selected.suffix.lower() == ".def"


def build_invocations(
    plan: tidyengine.CompileCommandPlan,
    buckets: dict[str, list[Path]],
) -> dict[str, list[TidyInvocation]]:
    """Create one native invocation for every covered selected file."""
    mode_by_path = {absolute_path(path): mode for mode, paths in buckets.items() for path in paths}
    invocations: dict[str, list[TidyInvocation]] = {"STRICT": [], "RELAXED": []}
    for target in plan.targets:
        mode = mode_by_path[absolute_path(target.selected)]
        invocations[mode].append(
            TidyInvocation(
                selected=absolute_path(target.selected),
                compile_command_source=absolute_path(target.translation_unit),
            )
        )
    return invocations


_NOLINT_OPEN_RE = re.compile(r"\bNOLINT(?:NEXTLINE|BEGIN)?\(([^)]*)\)")


@dataclass(frozen=True)
class StaleNolintSuppression:
    path: Path
    line: int
    check: str


def find_stale_nolint_suppressions(buckets: dict[str, list[Path]]) -> list[StaleNolintSuppression]:
    """Find NOLINT entries naming checks that the file's tidy mode never enables."""
    issues: list[StaleNolintSuppression] = []
    for mode, paths in buckets.items():
        disabled_patterns = [token.removeprefix("-") for token in mode_disabled_checks(mode)]
        for path in paths:
            try:
                lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
            except OSError:
                continue
            for line_number, line in enumerate(lines, start=1):
                for match in _NOLINT_OPEN_RE.finditer(line):
                    checks = (check.strip() for check in match.group(1).split(","))
                    for check in checks:
                        if check and any(fnmatch.fnmatchcase(check, pattern) for pattern in disabled_patterns):
                            issues.append(StaleNolintSuppression(path=path, line=line_number, check=check))
    return issues


class _RemovedFixFlag(argparse.Action):
    """Reject the removed tidy --fix flag during parse. Not advertised in help."""

    def __call__(
        self,
        parser: argparse.ArgumentParser,
        namespace: argparse.Namespace,
        values: object,
        option_string: str | None = None,
    ) -> None:
        parser.error("--fix was removed; ao tidy is diagnostic-only and does not apply replacements")


def register(subparsers: "argparse._SubParsersAction[argparse.ArgumentParser]") -> None:
    parser = subparsers.add_parser(
        NAME, help=HELP, description=HELP, epilog=EPILOG, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    tidyengine.add_scope_arguments(parser, verb="check")
    parser.add_argument("-n", "--no-build", action="store_true", help="use an existing plugin and compile database")
    # One removed portal flag. nargs=0 so it cannot consume a file or a later option.
    parser.add_argument("--fix", nargs=0, action=_RemovedFixFlag, dest=argparse.SUPPRESS, help=argparse.SUPPRESS)
    parser.add_argument(
        "--tidy-arg",
        action="append",
        default=[],
        type=tidyconfig.diagnostic_argument,
        metavar="<arg>",
        help="extra diagnostic-only argument passed to clang-tidy (repeatable)",
    )
    parser.add_argument("--header-filter", metavar="<regex>", help="override the per-mode header filter")
    parser.set_defaults(func=run_command)


def classify(path: Path, explicit: bool) -> str:
    """Classify authoritative checkout paths, including active owned compiler views."""
    path = workspace_cache.canonical_portal_path(path, project_root=PROJECT_ROOT)
    try:
        rel = path.relative_to(absolute_path(PROJECT_ROOT)).as_posix()
    except ValueError:
        return "STRICT"

    if os.name == "nt":
        rel = rel.lower()
    if rel.startswith("test/lint/fixture/"):
        # Fixture files: check when explicitly requested, skip in batch scans.
        return "STRICT" if explicit else "IGNORE"
    if rel.startswith("test/lint/"):
        return "IGNORE"
    if rel.startswith("test/"):
        return "RELAXED"
    return "STRICT"


def expected_lint_artifact_path(build_dir: Path, *, os_name: str | None = None) -> Path:
    """Return the native artifact that carries the Aobus checks."""
    filename = "AobusClangTidy.exe" if builddir.platform_profile(os_name).name == "windows" else "libAobusLintPlugin.so"
    return build_dir / "tool" / "lint" / filename


def find_lint_artifact(build_dir: Path, *, os_name: str | None = None) -> Path | None:
    """Find the native lint artifact, including multi-config output folders."""
    profile = builddir.platform_profile(os_name)
    expected = expected_lint_artifact_path(build_dir, os_name=os_name)
    if expected.is_file():
        return expected
    pattern = "**/AobusClangTidy.exe" if profile.name == "windows" else "**/libAobusLintPlugin.so"
    matches = sorted((build_dir / "tool" / "lint").glob(pattern))
    return matches[0] if len(matches) == 1 else None


def verify_tidy_toolchain(toolchain: TidyToolchain) -> None:
    """Fail closed unless clang-tidy has the expected ABI and complete Aobus registry."""
    from ..core.linttest import FIXTURE_DIR

    expected_checks = expected_aobus_checks(FIXTURE_DIR)
    if toolchain.expected_llvm_version is not None:
        version_command = [toolchain.clang_tidy, "--version"]
        try:
            version_result = subprocess.run(
                version_command,
                cwd=PROJECT_ROOT,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
        except FileNotFoundError as error:
            raise die(f"clang-tidy tool not found: {toolchain.clang_tidy}") from error
        version_pattern = rf"(?<![0-9.]){re.escape(toolchain.expected_llvm_version)}(?![0-9.])"
        if version_result.returncode != 0 or re.search(version_pattern, version_result.stdout) is None:
            raise die(
                f"the selected clang-tidy is not LLVM {toolchain.expected_llvm_version}; "
                f"command exited {version_result.returncode}: {' '.join(version_command)}"
            )

    command = [toolchain.clang_tidy]
    if toolchain.plugin is not None:
        command.append(f"-load={toolchain.plugin}")
    command.extend(["-checks=-*,aobus-*", "-list-checks"])
    try:
        result = subprocess.run(
            command,
            cwd=PROJECT_ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
    except FileNotFoundError as error:
        raise die(f"clang-tidy tool not found: {toolchain.clang_tidy}") from error
    if result.returncode != 0:
        raise die(f"cannot list the selected clang-tidy check registry (exit {result.returncode}): {' '.join(command)}")
    registered_checks = {line.strip() for line in result.stdout.splitlines() if line.strip().startswith("aobus-")}
    if registered_checks != expected_checks:
        missing = sorted(expected_checks - registered_checks)
        unexpected = sorted(registered_checks - expected_checks)
        details = []
        if missing:
            details.append(f"fixture checks absent from the selected toolchain: {', '.join(missing)}")
        if unexpected:
            details.append(f"registered checks without non-blank supported fixtures: {', '.join(unexpected)}")
        raise die(
            f"Aobus checker registry does not match the working-tree fixtures at {FIXTURE_DIR}:\n  "
            + "\n  ".join(details)
            + "\nCheck fixture additions/removals (including untracked files); "
            "rebuild the toolchain if its checks changed." + f"\nRegistry command: {' '.join(command)}"
        )


def prepare_toolchain(
    build_dir: Path,
    *,
    no_build: bool,
    reconfigure_preset: bool = False,
) -> TidyToolchain:
    profile = builddir.platform_profile()
    expected = expected_lint_artifact_path(build_dir)

    if no_build:
        if not (build_dir / "compile_commands.json").is_file():
            raise die(f"compile_commands.json not found in {build_dir}")
    else:
        with buildlock.build_tree_lock(build_dir):
            tidyengine.ensure_compile_db(
                build_dir,
                ["-DAOBUS_BUILD_LINT_PLUGIN=ON"],
                preset=builddir.tidy_preset(),
                reconfigure_preset=reconfigure_preset,
            )
            target = "AobusClangTidy" if profile.name == "windows" else "AobusLintPlugin"
            print(f"Building {target} (incremental)...")
            # The --no-build route never executes this work, so it emits no
            # checker-build record at all.
            with phases.measure("checker-build", tree="tidy") as phase:
                tool_build = subprocess.run(
                    [
                        "cmake",
                        "--build",
                        str(workspace_cache.compiler_build_dir(build_dir)),
                        "--target",
                        target,
                        *build.parallel_build_arguments(),
                    ],
                    cwd=PROJECT_ROOT,
                )
                phase.status = tool_build.returncode
        if tool_build.returncode != 0:
            raise die(f"failed to build {target}.")

    artifact = find_lint_artifact(build_dir)
    if artifact is None:
        raise die(f"native lint artifact was not found at {expected}")
    toolchain = TidyToolchain(
        clang_tidy=tidyengine.clang_tool(build_dir, "clang-tidy"),
        plugin=None if profile.name == "windows" else artifact,
        resource_dir=tidyengine.clang_resource_dir(build_dir) if profile.name == "windows" else None,
        expected_llvm_version=(tidyengine.llvm_sdk_version(build_dir) if profile.name == "windows" else None),
        sdk_root=tidyengine.llvm_sdk_root(build_dir) if profile.name == "windows" else None,
    )
    # A verify failure ends the phase through the same die() the caller sees.
    with phases.measure("checker-verify", tree="tidy"):
        verify_tidy_toolchain(toolchain)
    return toolchain


def classify_existing(files: list[str], explicit: bool) -> dict[str, list[Path]]:
    """Resolve, dedupe, and bucket the candidate files."""
    buckets: dict[str, list[Path]] = {"STRICT": [], "RELAXED": []}
    seen: set[Path] = set()
    for name in files:
        path = Path(name)
        if not path.is_absolute():
            path = PROJECT_ROOT / path
        if not path.is_file():
            continue
        path = workspace_cache.canonical_portal_path(path, project_root=PROJECT_ROOT)
        if path in seen:
            continue
        seen.add(path)
        mode = classify(path, explicit)
        if mode != "IGNORE":
            buckets[mode].append(path)
    return buckets


def split_existing(files: list[str]) -> tuple[list[str], list[str]]:
    cpp_files: list[str] = []
    python_files: list[str] = []
    seen: set[Path] = set()
    for name in files:
        path = Path(name)
        if not path.is_absolute():
            path = PROJECT_ROOT / path
        if not path.is_file():
            continue
        resolved = workspace_cache.canonical_portal_path(path, project_root=PROJECT_ROOT)
        if resolved in seen:
            continue
        seen.add(resolved)
        try:
            rel = resolved.relative_to(absolute_path(PROJECT_ROOT)).as_posix()
        except ValueError:
            rel = resolved.as_posix()
        if resolved.suffix in gitfiles.CPP_SUFFIXES:
            cpp_files.append(rel)
        elif resolved.suffix in gitfiles.PYTHON_SUFFIXES:
            python_files.append(rel)
    return cpp_files, python_files


def missing_explicit_files(files: list[str]) -> list[str]:
    """Return explicit paths that do not name regular files."""
    missing: list[str] = []
    for name in files:
        path = Path(name)
        if not path.is_absolute():
            path = PROJECT_ROOT / path
        if not path.is_file():
            missing.append(name)
    return missing


def is_winui_path(path: Path) -> bool:
    """Return whether a selected file belongs to the WinUI frontend."""
    return winuitidy.is_winui_path(path, winui_root=WINUI_ROOT)


def requires_winui_compile_context(path: Path) -> bool:
    """Return whether a file is compiled only by the native WinUI project graph."""
    return winuitidy.requires_winui_compile_context(path)


def limits_diagnostics_to_selected_path(invocation: TidyInvocation) -> bool:
    """Keep header and forced-PCH WinUI diagnostics attached to their selected file.

    Native WinUI compile commands force a shared PCH that includes hand-authored
    C++/WinRT projection headers.  Reporting those headers from every selected
    source duplicates unrelated diagnostics; changed headers already receive an
    exact header-as-main invocation of their own.
    """
    return (
        invocation.is_header
        or invocation.is_include_fragment
        or requires_winui_compile_context(invocation.compile_command_source)
    )


def winui_build_directory(tidy_build_dir: Path, *, path_was_explicit: bool) -> Path:
    """Keep VS-generated WinUI state separate from the Ninja tidy tree."""
    return builddir.winui_companion_build_dir(
        tidy_build_dir,
        primary_path_was_explicit=path_was_explicit,
    )


def additional_dependency_build_directories(
    tidy_build_dir: Path,
    *,
    path_was_explicit: bool,
) -> tuple[Path, ...]:
    """Return existing normal build trees whose Ninja dependencies may prove header coverage."""
    if path_was_explicit or os.environ.get("BUILD_DIR"):
        return ()

    normal_debug_dir = builddir.build_dir("debug")
    if absolute_path(normal_debug_dir) == absolute_path(tidy_build_dir):
        return ()
    if not (normal_debug_dir / "build.ninja").is_file():
        return ()
    if not (normal_debug_dir / "compile_commands.json").is_file():
        return ()
    return (normal_debug_dir,)


def prepare_winui_compile_commands(
    args: argparse.Namespace,
    tidy_build_dir: Path,
    toolchain: TidyToolchain,
    selected: list[Path],
) -> list[dict[str, object]]:
    """Generate/query native WinUI compile context only when the selected scope needs it."""
    if toolchain.resource_dir is None or not any(requires_winui_compile_context(path) for path in selected):
        return []

    winui_dir = winui_build_directory(tidy_build_dir, path_was_explicit=args.path is not None)
    required = tuple(
        path
        for path in selected
        if requires_winui_compile_context(path) and path.suffix.lower() in {".c", ".cc", ".cpp", ".cxx"}
    )
    clang_cl = Path(tidyengine.clang_tool(tidy_build_dir, "clang-cl"))
    with buildlock.build_tree_lock(winui_dir):
        if not args.no_build:
            build.do_build(
                argparse.Namespace(
                    flavor="debug",
                    clean=False,
                    clang=False,
                    asan=False,
                    tsan=False,
                    verbose=False,
                    path=str(winui_dir),
                ),
                [build.WINUI_LINT_CONTEXT_TARGET],
            )
        return winuitidy.compile_commands(winui_dir, clang_cl, required_translation_units=required, jobs=args.jobs)


def write_windows_header_preamble(destination: Path) -> Path:
    """Create the minimal Windows macro cleanup normally supplied by the product PCH."""
    destination.mkdir(parents=True, exist_ok=True)
    preamble = destination / "aobus-tidy-windows-header-preamble.h"
    preamble.write_text(
        "#pragma once\n\n#include <windows.h>\n\n#undef GetCurrentTime\n",
        encoding="utf-8",
    )
    return preamble


def requires_build_environment(args: argparse.Namespace) -> bool:
    from ..core import buildenv

    if args.no_build:
        return False
    files, _ = tidyengine.resolve_scope(args, ALL_FOLDERS, "Checking", suffixes=gitfiles.SOURCE_SUFFIXES)
    return buildenv.requires_source_build_env(args, files)


def _analysis_source_id(path: Path) -> str | None:
    """Return the stable phase identifier for one analyzed source.

    The digest is over the repository-relative resolved POSIX path, so it stays
    stable across CI checkout locations while no raw host path is published.
    Sources outside the repository root deliberately keep ``None``: their
    timing still lands in the record, but no identifier is claimed for fixture
    data whose relative spelling is not meaningful.
    """
    try:
        relative = absolute_path(path).relative_to(absolute_path(PROJECT_ROOT)).as_posix()
    except ValueError:
        return None
    return hashlib.sha256(relative.encode("utf-8")).hexdigest()


def run_command(args: argparse.Namespace, *, resolved_scope: tuple[list[str], bool] | None = None) -> int:
    # Argparse rejects these during parsing. This remains the guard for a
    # programmatic Namespace that did not pass through the option type.
    try:
        tidyconfig.validate_diagnostic_arguments(args.tidy_arg)
    except ValueError as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1
    build_dir = Path(args.path) if args.path else builddir.tidy_dir()
    files, explicit = (
        resolved_scope
        if resolved_scope is not None
        else tidyengine.resolve_scope(args, ALL_FOLDERS, "Checking", suffixes=gitfiles.SOURCE_SUFFIXES)
    )
    if explicit:
        missing_files = missing_explicit_files(files)
        if missing_files:
            print("ERROR: explicitly selected paths do not exist or are not files:", file=sys.stderr)
            for missing_name in missing_files:
                print(f"  {missing_name}", file=sys.stderr)
            return 1
    cpp_files, python_files = split_existing(files)

    out = open(args.output, "w", encoding="utf-8") if args.output else sys.stdout
    with contextlib.ExitStack() as stack:
        if args.output:
            stack.callback(out.close)

        overall_failed = False

        if python_files:
            if args.check:
                print("Skipping Python checks because --check selects a clang-tidy check.", file=sys.stderr)
            else:
                print(f"=== Python hygiene — {len(python_files)} file(s) ===")
                if pythoncheck.run_paths(python_files, sink=out if args.output else None) != 0:
                    overall_failed = True

        if not cpp_files:
            if not python_files:
                print("No C++, Objective-C++, or Python sources found.", file=sys.stderr)
            return 1 if overall_failed else 0

        toolchain = prepare_toolchain(
            build_dir,
            no_build=args.no_build,
            reconfigure_preset=args.path is None,
        )
        try:
            source_roots = workspace_cache.validated_source_roots(build_dir)
        except workspace_cache.WorkspaceCacheError as exc:
            raise die(str(exc)) from exc

        buckets = classify_existing(cpp_files, explicit)
        if not buckets["STRICT"] and not buckets["RELAXED"]:
            return 1 if overall_failed else 0

        stale_suppressions = find_stale_nolint_suppressions(buckets)
        if stale_suppressions:
            print("ERROR: stale NOLINT suppressions name checks disabled for their file mode:", file=sys.stderr)
            for issue in stale_suppressions:
                try:
                    display_path = issue.path.relative_to(absolute_path(PROJECT_ROOT)).as_posix()
                except ValueError:
                    display_path = issue.path.as_posix()
                print(f"  {display_path}:{issue.line}: {issue.check}", file=sys.stderr)
            print("Remove these suppressions; the configured check cannot emit there.", file=sys.stderr)
            return 1

        selected = [*buckets["STRICT"], *buckets["RELAXED"]]
        winui_commands = prepare_winui_compile_commands(args, build_dir, toolchain, selected)
        automatic_winui_companions = (
            winuitidy.find_header_companions(
                winui_commands,
                tuple(
                    path
                    for path in selected
                    if is_winui_path(path) and path.suffix.lower() in {".h", ".hh", ".hpp", ".hxx"}
                ),
                project_root=PROJECT_ROOT,
                winui_root=WINUI_ROOT,
            )
            if toolchain.resource_dir is not None
            else {}
        )
        native_database_dir = build_dir
        if toolchain.resource_dir is not None:
            native_database_dir = tidyengine.make_tmpdir("tidy-windows-db-")
            stack.callback(shutil.rmtree, native_database_dir, ignore_errors=True)
            tidyengine.write_merged_compile_database(
                build_dir,
                native_database_dir,
                WINDOWS_EXCLUDED_COMPILE_ARGUMENTS,
                winui_commands,
            )

        coverage_plan = tidyengine.compile_command_plan(
            native_database_dir,
            selected,
            explicit_header_companions=(automatic_winui_companions if toolchain.resource_dir is not None else None),
            additional_dependency_build_dirs=additional_dependency_build_directories(
                build_dir,
                path_was_explicit=args.path is not None,
            ),
        )
        if coverage_plan.deferred:
            deferral_details = {absolute_path(detail.selected): detail for detail in coverage_plan.deferral_details}
            platform_deferred = [
                path
                for path in coverage_plan.deferred
                if (detail := deferral_details.get(absolute_path(path))) is not None and detail.is_platform_incompatible
            ]
            native_deferred = [path for path in coverage_plan.deferred if path not in platform_deferred]

            if explicit:
                print("ERROR: explicitly selected files without a compile command on this platform:", file=sys.stderr)
                reported = list(coverage_plan.deferred)
            elif native_deferred:
                print(
                    "ERROR: native clang-tidy coverage is incomplete; files lack compile context:",
                    file=sys.stderr,
                )
                reported = native_deferred
            else:
                print("Deferred files that are incompatible with this platform:", file=sys.stderr)
                reported = platform_deferred

            for path in reported:
                detail = deferral_details.get(absolute_path(path))
                reason = detail.reason if detail is not None else None
                suffix = f" ({reason})" if reason else ""
                print(f"  {path}{suffix}", file=sys.stderr)

            if explicit and not native_deferred:
                print(
                    "Run those files on the platform that builds them, or use a build directory "
                    "whose compile_commands.json contains them.",
                    file=sys.stderr,
                )
                return 1
            if native_deferred:
                portal = "ao.bat" if builddir.platform_profile().name == "windows" else "./ao"
                if args.path is not None or os.environ.get("BUILD_DIR"):
                    print(
                        "Refresh the explicitly selected build tree and its Ninja dependency data, "
                        f"or rerun without the exact build override after `{portal} build` or "
                        f"`{portal} check`.",
                        file=sys.stderr,
                    )
                else:
                    print(
                        f"Run `{portal} build` or `{portal} check` to refresh the normal debug "
                        "build and its Ninja dependency data, then rerun tidy.",
                        file=sys.stderr,
                    )
                print(
                    "The tidy portal does not build the full product graph automatically.",
                    file=sys.stderr,
                )
                return 1
            print(
                "  These files were not checked here; the platform that builds them must cover them.",
                file=sys.stderr,
            )
            if not coverage_plan.targets:
                return 1 if overall_failed else 0

        invocations = build_invocations(coverage_plan, buckets)
        if not invocations["STRICT"] and not invocations["RELAXED"]:
            print(
                "ERROR: clang-tidy coverage is incomplete: no selected C++ file has a native compile command.",
                file=sys.stderr,
            )
            return 1

        selected_overlays: dict[Path, Path] = {}
        fragments = [
            invocation for batch in invocations.values() for invocation in batch if invocation.is_include_fragment
        ]
        if os.name == "nt" and fragments:
            try:
                _validate_selected_vfs_inputs(
                    native_database_dir,
                    {invocation.compile_command_source for invocation in fragments},
                    args.tidy_arg,
                )
                overlay_dir = tidyengine.make_tmpdir("tidy-selected-vfs-")
                stack.callback(shutil.rmtree, overlay_dir, ignore_errors=True)
                for index, invocation in enumerate(fragments):
                    selected_overlays[invocation.selected] = write_selected_file_overlay(
                        overlay_dir / f"{index}.json", invocation.selected, source_roots=source_roots
                    )
            except (OSError, ValueError) as error:
                print(f"ERROR: cannot preserve selected fragment identity: {error}", file=sys.stderr)
                return 1

        mapped_headers = [target for target in coverage_plan.targets if target.is_header]
        header_database_dir: Path | None = None
        if mapped_headers:
            translation_units = {absolute_path(target.translation_unit) for target in mapped_headers}
            print(
                f"Header coverage: {len(mapped_headers)} header(s) mapped to "
                f"{len(translation_units)} exact native translation unit(s).",
                file=sys.stderr,
            )
            print(
                "  Each header is checked as the main file with compiler flags copied from its native implementation.",
                file=sys.stderr,
            )
            header_database_dir = tidyengine.make_tmpdir("tidy-header-db-")
            stack.callback(shutil.rmtree, header_database_dir, ignore_errors=True)
            windows_preamble = (
                write_windows_header_preamble(header_database_dir) if toolchain.resource_dir is not None else None
            )
            tidyengine.write_header_compile_database(
                native_database_dir,
                mapped_headers,
                header_database_dir,
                excluded_arguments=("/TP",) if toolchain.resource_dir is not None else (),
                excluded_argument_patterns=(
                    (WINDOWS_FORCED_CMAKE_PCH_PATTERN,) if toolchain.resource_dir is not None else ()
                ),
                additional_arguments=(f"/FI{windows_preamble}",) if windows_preamble is not None else (),
                command_line_style="windows" if toolchain.resource_dir is not None else "posix",
            )

        clang_tidy = toolchain.clang_tidy
        isystem = tidyengine.system_include_args()
        if args.debug:
            print(f"DEBUG ISYSTEM_ARGS: {isystem}", file=sys.stderr)

        def run_one(mode: str, invocation: TidyInvocation, log: Path) -> int:
            checks = checks_for(mode, args.check)
            header_filter = args.header_filter or (
                exact_header_filter([invocation.selected], source_roots=source_roots)
                if invocation.is_header or invocation.is_include_fragment
                else project_header_filter(
                    RELAXED_HEADER_FOLDERS if mode == "RELAXED" else STRICT_HEADER_FOLDERS,
                    source_roots=source_roots,
                )
            )
            extra: list[str] = list(isystem)
            if toolchain.resource_dir is not None:
                extra.extend(
                    f"--extra-arg-before={argument}"
                    for argument in tidyengine.windows_analysis_arguments(toolchain.resource_dir)
                )
                if requires_winui_compile_context(invocation.compile_command_source):
                    # Generated C++/WinRT headers preserve schema spelling while
                    # Windows resolves include paths case-insensitively.
                    extra.append("--extra-arg-before=-Wno-nonportable-include-path")
            if limits_diagnostics_to_selected_path(invocation):
                extra.append(f"-line-filter={path_line_filter([invocation.selected], source_roots=source_roots)}")
            if overlay := selected_overlays.get(invocation.selected):
                extra.append(f"--vfsoverlay={overlay}")
            if invocation.is_header and invocation.compile_command_source.suffix.lower() != ".mm":
                extra.append("--extra-arg-before=-x")
                extra.append("--extra-arg-before=c++-header")
            if "linux-gtk/" in invocation.compile_command_source.as_posix():
                # GTK framework patterns: gtkmm widget trees own children, printf-style APIs.
                checks += ",-cppcoreguidelines-owning-memory"
                extra.append("--extra-arg=-Wno-format-nonliteral")
            if toolchain.plugin is not None:
                extra.append(f"-load={toolchain.plugin}")
            config = config_for(checks, source_roots=source_roots)
            compile_database = header_database_dir if invocation.is_header else native_database_dir
            if compile_database is None:
                raise die(f"synthetic compile database was not created for {invocation.selected}")
            tidy_file = workspace_cache.compiler_source_path(
                invocation.compile_command_source if invocation.is_include_fragment else invocation.selected
            )
            command = [
                clang_tidy,
                "--quiet",
                "-p",
                str(compile_database),
                *args.tidy_arg,
                f"-config={config}",
                f"-header-filter={header_filter}",
                *extra,
                str(tidy_file),
            ]
            # Per-native-invocation record: the caller computes the identifier
            # once per invocation, and only while phase logging is enabled, so
            # a disabled run skips the digest entirely; the status is the
            # actual native exit status (including the normalized
            # database-failure marker below). The log-variable name is bound
            # at import time: tests replace the ``phases`` attribute with a
            # call recorder, and the gate must not depend on that seam.
            source_id = _analysis_source_id(invocation.selected) if os.environ.get(LOG_DIR_ENV) else None
            with phases.measure("analysis-item", tree="tidy", count=1, mode=mode, source_id=source_id) as item:
                with open(log, "w", encoding="utf-8") as sink:
                    if args.debug:
                        sink.write(f"DEBUG CONFIG: {config}\n")
                    status = subprocess.call(command, cwd=PROJECT_ROOT, stdout=sink, stderr=subprocess.STDOUT)
                if tidyengine.log_has_compile_command_error(log):
                    status = status or 1
                item.status = status
            return status

        for mode in ("STRICT", "RELAXED"):
            batch = invocations[mode]
            if not batch:
                continue
            print(f"=== clang-tidy [{mode}] — {len(batch)} selected file(s), {len(batch)} native invocation(s) ===")
            tmpdir = tidyengine.make_tmpdir("tidy-")

            def run_file(invocation: TidyInvocation, log: Path, current_mode: str = mode) -> int:
                return run_one(current_mode, invocation, log)

            try:
                # One record per strict/relaxed batch: count is the number of
                # native invocations and status the actual failed-batch bool;
                # findings are reported separately below, and deferred files
                # plus header input scope stay outside this record. The
                # per-invocation analysis-item children carry the heavy-tail
                # detail.
                with phases.measure(
                    "analysis", tree="tidy", mode=mode, jobs=max(args.jobs, 1), count=len(batch)
                ) as phase:
                    result = tidyengine.run_parallel(batch, args.jobs, tmpdir, run_file)
                    phase.status = 1 if result.failed else 0
                if result.failed:
                    overall_failed = True
                    print("=== Failed logs ===", file=sys.stderr)
                    for log in result.failed_logs:
                        print(f"--- Log: {log} ---", file=sys.stderr)
                        sys.stderr.write(log.read_text(encoding="utf-8", errors="replace"))

                noisy = tidyengine.logs_with_diagnostics(result.logs)
                if noisy:
                    deduplicate(
                        noisy,
                        out,
                        PROJECT_ROOT,
                        path_mapper=lambda path: workspace_cache.canonical_portal_path(path, project_root=PROJECT_ROOT),
                    )
                    out.flush()
                    overall_failed = True
                    if args.output:
                        print(f"  Output appended to {args.output}")
                else:
                    print("  No warnings found.")
            finally:
                shutil.rmtree(tmpdir, ignore_errors=True)

        return 1 if overall_failed else 0
