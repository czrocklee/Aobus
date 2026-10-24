"""ao build — configure and build one flavor, without running tests (see `ao check`)."""

import argparse
import hashlib
import json
import os
import re
import shutil
import sys
from collections.abc import Mapping
from dataclasses import dataclass
from pathlib import Path, PureWindowsPath

from ..core import builddir, buildlock, compiler_cache, msbuild_cache, phases, winui, workspace_cache
from ..core.paths import PROJECT_ROOT
from ..core.proc import die, run

HELP = "Configure and build a native flavor"
NAME = "build"
# True when ao.bat must initialize the MSVC/vcpkg build environment first.
REQUIRES_BUILD_ENV = True


EPILOG = """\
examples:
  ./ao build                     # incremental debug build
  ./ao build release --clean     # clean Release build with IPO/LTO
  ./ao build --target aobus-gtk  # build a single target
  ./ao build --gtk off --system-media on -p /path/to/tree
  ./ao build debug --clang       # clang build in its own build tree
"""

WINDOWS_EPILOG = """\
examples:
  ao.bat build                       # incremental debug build
  ao.bat build release --clean       # clean Release build with IPO/LTCG
  ao.bat build --target aobus-tui    # build only the TUI target
  ao.bat build --target aobus        # build only the CLI target
  ao.bat build --target winui        # build WinUI through the dedicated MSBuild tree
  ao.bat build release --target winui  # build Release WinUI with IPO/LTCG
  ao.bat build --target aobus-winui-lint-context  # generate WinUI headers without compiling the app
"""

WINUI_TARGETS = frozenset({"winui", "aobus-winui"})
WINUI_LINT_CONTEXT_TARGET = "aobus-winui-lint-context"

# Private WinUI MSBuild capture (`--msbuild-capture`): one fixed binary-logger
# tail plus tlog snapshots under an exclusive user-local directory. Raw logs
# may contain environment-derived properties and paths, so all capture evidence
# stays on the user's machine; only fixed phase dimensions ever reach the
# opt-in phase records. Destination privacy (Windows ACLs) stays the user's
# responsibility; the portal only creates the leaf with the strongest mode the
# platform honors.
_CAPTURE_WARNING = "Warning: --msbuild-capture output contains sensitive data; keep the capture directory private."
# Only identity-relevant CMake cache entries are recorded; never the whole cache.
_CAPTURE_CACHE_KEYS = (
    "CMAKE_BUILD_TYPE",
    "CMAKE_CXX_COMPILER",
    "CMAKE_GENERATOR",
    "CMAKE_GENERATOR_INSTANCE",
    "CMAKE_GENERATOR_PLATFORM",
    "CMAKE_VS_PLATFORM_TOOLSET",
    "CMAKE_VS_PLATFORM_TOOLSET_VERSION",
    "CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION",
)
# Selected executable/library identities; object files are not hashed or copied.
# This is not a bound on file count, bytes read, or capture runtime.
_CAPTURE_OUTPUT_SUFFIXES = (".exe", ".dll", ".lib")


def add_build_arguments(parser: argparse.ArgumentParser, *, default_flavor: str = "debug") -> None:
    parser.add_argument(
        "flavor",
        nargs="?",
        default=default_flavor,
        choices=builddir.flavors(),
        help=f"build flavor (default: {default_flavor})",
    )
    parser.add_argument("--clean", action="store_true", help="remove the build directory first")
    parser.add_argument("--clang", action="store_true", help="build with clang in a dedicated build tree")
    sanitizers = parser.add_mutually_exclusive_group()
    sanitizers.add_argument(
        "--asan",
        action="store_true",
        help="enable AddressSanitizer and UndefinedBehaviorSanitizer where available (debug only)",
    )
    sanitizers.add_argument("--tsan", action="store_true", help="enable thread sanitizer (debug only)")
    parser.add_argument("--verbose", action="store_true", help="show full build command lines")
    parser.add_argument("--gtk", choices=("on", "off"), help="configure the Linux GTK frontend")
    parser.add_argument(
        "--system-media", choices=("on", "off"), help="configure Linux native system-media integration (Linux only)"
    )
    parser.add_argument("-p", "--path", metavar="<dir>", help="override the build directory")


def register(subparsers: "argparse._SubParsersAction[argparse.ArgumentParser]") -> None:
    parser = subparsers.add_parser(
        NAME,
        help=HELP,
        description=HELP,
        epilog=WINDOWS_EPILOG if builddir.platform_profile().name == "windows" else EPILOG,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    add_build_arguments(parser)
    parser.add_argument(
        "--target", action="append", default=[], metavar="<target>", help="build specific target(s) only"
    )
    parser.add_argument(
        "--msbuild-capture",
        metavar="<dir>",
        help="capture a private MSBuild binary log and tlog snapshots for one WinUI build into <dir>",
    )
    parser.set_defaults(func=run_command)


@dataclass
class BuildResult:
    build_dir: Path
    log: Path
    compiler: str
    preset: str = ""


def _windows_extended_path(path: str) -> str:
    if path.startswith("\\\\?\\"):
        return path
    if path.startswith("\\\\"):
        return "\\\\?\\UNC\\" + path[2:]
    return "\\\\?\\" + path


def _remove_build_directory(path: Path, profile: builddir.PlatformProfile) -> None:
    if profile.name == "windows":
        shutil.rmtree(_windows_extended_path(str(path.resolve())))
        return
    shutil.rmtree(path)


def validate_build_options(args: argparse.Namespace) -> builddir.PlatformProfile:
    """Reject build modes that the native platform cannot provide."""
    profile = builddir.platform_profile()
    if getattr(args, "flavor", "debug") != "debug" and (args.asan or args.tsan):
        raise die("Sanitizers are available only for debug builds.")
    if args.clang and profile.name == "windows":
        raise die(
            "Clang application builds are unavailable on Windows; the managed LLVM SDK is reserved for format and tidy."
        )
    if args.tsan and not profile.tsan_suites:
        raise die("ThreadSanitizer is unavailable on the Windows MSVC toolchain.")
    if profile.name != "linux" and getattr(args, "gtk", None) == "on":
        raise die("--gtk on is supported only on Linux.")
    if profile.name != "linux" and getattr(args, "system_media", None) is not None:
        raise die("--system-media is currently configurable only on Linux.")
    return profile


def parallel_build_jobs() -> int:
    """Return the shared build concurrency limit."""
    configured = os.environ.get("CMAKE_BUILD_PARALLEL_LEVEL")
    if configured is not None:
        try:
            jobs = int(configured)
        except ValueError:
            raise die("CMAKE_BUILD_PARALLEL_LEVEL must be a positive integer.") from None
        if jobs < 1:
            raise die("CMAKE_BUILD_PARALLEL_LEVEL must be a positive integer.")
    else:
        jobs = max(1, (os.cpu_count() or 1) - 1)
    return jobs


def parallel_build_arguments() -> list[str]:
    """Return the CMake arguments for the shared build concurrency limit."""
    jobs = parallel_build_jobs()
    return ["--parallel", str(jobs)]


def _winui_build_environment(jobs: int) -> dict[str, str]:
    """Coordinate C++ compilation concurrency across MSBuild projects."""
    return {
        "UseMultiToolTask": "true",
        "EnforceProcessCountAcrossBuilds": "true",
        "MultiProcMaxCount": str(jobs),
        "CL_MPCount": str(jobs),
    }


def _capture_syntax_error(capture: str) -> str | None:
    """Return why the capture destination cannot be used, or None when valid.

    Quoted and whitespace-bearing paths are rejected because MSBuild logger
    parameter transport for them is unproven; semicolons would terminate the
    logger parameter early.
    """
    if not capture:
        return "--msbuild-capture requires an explicit destination directory."
    for character in capture:
        if character in "\"';" or character.isspace() or ord(character) < 0x20 or ord(character) == 0x7F:
            return (
                "--msbuild-capture destination contains a quote, whitespace, or a MSBuild delimiter "
                "character; choose a plain fixed-drive directory name."
            )
    destination = PureWindowsPath(capture)
    if not destination.is_absolute() or re.fullmatch(r"[A-Za-z]:", destination.drive) is None:
        return (
            "--msbuild-capture destination must be an absolute path on a fixed local drive, "
            "for example C:\\aobus-capture\\run1."
        )
    if len(destination.parts) < 3:
        return "--msbuild-capture destination needs an existing private parent directory below the drive root."
    return None


def _capture_drive_is_fixed(destination: Path) -> bool:
    """Whether the destination drive is verified fixed-local storage.

    Only a native Windows host can classify volumes; UNC and unverifiable
    storage (including every non-Windows host) is rejected, never assumed local.
    """
    if os.name != "nt" or re.fullmatch(r"[A-Za-z]:", destination.drive) is None:
        return False
    try:
        import ctypes

        # vars() keeps strict type checking away from the Windows-only attribute.
        return bool(vars(ctypes)["windll"].kernel32.GetDriveTypeW(f"{destination.drive}\\") == 3)  # DRIVE_FIXED
    except (AttributeError, OSError):
        return False


def _paths_overlap(first: Path, second: Path) -> bool:
    """Whether two resolved paths are equal or one contains the other."""
    return first == second or first.is_relative_to(second) or second.is_relative_to(first)


def _prepare_capture(destination: str, *, build_dir: Path, source_dir: Path) -> Path:
    """Create the exclusive private capture leaf before any build mutation."""
    path = Path(destination)
    resolved = path.resolve()
    # Check both spellings: a junction parent can resolve to remote storage
    # outside the source and build trees while looking lexically local.
    if not _capture_drive_is_fixed(path) or not _capture_drive_is_fixed(resolved):
        raise die(
            "--msbuild-capture destination is not a verified fixed local drive; "
            "UNC and remote storage are not supported."
        )
    if not path.parent.is_dir():
        raise die("--msbuild-capture parent directory does not exist; create it under a private local directory first.")
    if path.exists() or path.is_symlink():
        raise die("--msbuild-capture destination already exists; choose a new exclusive directory for this capture.")
    for protected in (build_dir, source_dir):
        if _paths_overlap(resolved, protected.resolve()):
            raise die("--msbuild-capture destination must stay disjoint from the source and build trees.")
    try:
        # Effective where the platform honors POSIX modes; Windows DACLs remain the user's responsibility.
        path.mkdir(0o700)
    except FileExistsError:
        raise die(
            "--msbuild-capture destination already exists; choose a new exclusive directory for this capture."
        ) from None
    except OSError as exc:
        raise die(f"cannot create the --msbuild-capture destination: {exc}.") from exc
    return path


def _capture_digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def _capture_identity(path: Path, root: Path) -> dict[str, object]:
    stat = path.stat()
    return {
        "path": path.relative_to(root).as_posix(),
        "size": stat.st_size,
        "mtime_ns": stat.st_mtime_ns,
        "sha256": _capture_digest(path),
    }


def _capture_cache_identity(build_dir: Path) -> dict[str, object] | None:
    """Digest the CMake cache and record only allowlisted identity entries."""
    cache = build_dir / "CMakeCache.txt"
    if not cache.is_file():
        return None
    entries: dict[str, str] = {}
    for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
        key, separator, value = line.partition("=")
        if separator and not key.startswith(("//", "#")) and key.split(":", 1)[0] in _CAPTURE_CACHE_KEYS:
            entries[key.split(":", 1)[0]] = value
    return {"sha256": _capture_digest(cache), "entries": entries}


def _capture_tool_identity(path_text: str | None) -> dict[str, object] | None:
    """Record a selected tool's path and digest without its output or arguments."""
    if not path_text:
        return None
    path = Path(path_text)
    if not path.is_file():
        return {"path": path_text, "sha256": None}
    return {"path": path_text, "sha256": _capture_digest(path)}


def _capture_build_metadata(build_env: Mapping[str, str], cache: Mapping[str, object] | None) -> dict[str, object]:
    """Private identity of the tools this build actually selected."""
    entries: Mapping[str, str] = {}
    if cache is not None:
        recorded = cache.get("entries")
        if isinstance(recorded, dict):
            entries = recorded
    return {
        "tracking_mode": build_env.get(msbuild_cache.TRACKING_ENV),
        # proc.run merges the computed environment over os.environ, so the
        # selected wrapper is often inherited instead of passed explicitly;
        # only this one allowlisted variable is consulted, never a dump.
        "wrapper": _capture_tool_identity(
            build_env.get("AOBUS_MSBUILD_CL_TOOL_EXE") or os.environ.get("AOBUS_MSBUILD_CL_TOOL_EXE")
        ),
        # The cache entry may be absent for Visual Studio trees; the missing
        # boundary is recorded instead of inventing a compiler identity.
        "compiler": _capture_tool_identity(entries.get("CMAKE_CXX_COMPILER")),
    }


def _capture_walk_error(error: OSError) -> None:
    """Propagate unreadable-tree errors instead of silently skipping evidence."""
    raise error


def _write_capture_snapshot(
    capture_dir: Path,
    stage: str,
    build_tree: Path,
    *,
    metadata: dict[str, object] | None = None,
    build_status: str | None = None,
) -> None:
    """Copy selected tracking/state files and write one private snapshot manifest."""
    stage_dir = capture_dir / stage
    stage_dir.mkdir(parents=True, exist_ok=True)
    tlogs: list[dict[str, object]] = []
    project_state: list[dict[str, object]] = []
    outputs: list[dict[str, object]] = []
    if build_tree.is_dir():
        for root, directories, files in os.walk(build_tree, onerror=_capture_walk_error):
            base = Path(root)
            if base == build_tree:
                # These root dependency trees are not WinUI project evidence.
                # Prune their descendants, but not identically named directories
                # inside first-party output or tool trees.
                directories[:] = [
                    name for name in directories if name.lower() not in {"vcpkg_installed", "third-party_deps"}
                ]
            directories.sort()
            for name in sorted(files):
                path = base / name
                lowered = name.lower()
                if not path.is_file():
                    continue
                if lowered.endswith(".tlog"):
                    # Copy the raw bytes with the tree-relative layout preserved.
                    copy = stage_dir / "tlogs" / path.relative_to(build_tree)
                    copy.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copyfile(path, copy)
                    tlogs.append(_capture_identity(path, build_tree))
                elif lowered == "unsuccessfulbuild" or lowered.endswith((".lastbuildstate", ".unsuccessfulbuild")):
                    # VC uses the bare unsuccessfulbuild name; also retain the
                    # suffix-shaped variant for project-specific marker names.
                    copy = stage_dir / "project-state" / path.relative_to(build_tree)
                    copy.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copyfile(path, copy)
                    project_state.append(_capture_identity(path, build_tree))
                elif lowered.endswith(_CAPTURE_OUTPUT_SUFFIXES):
                    # Identity only; executable and library payloads are never copied.
                    outputs.append(_capture_identity(path, build_tree))
    manifest: dict[str, object] = {
        "stage": stage,
        "complete": True,
        "build_status": build_status,
        "tlogs": tlogs,
        "project_state": project_state,
        "outputs": outputs,
        "cmake_cache": _capture_cache_identity(build_tree),
    }
    if metadata is not None:
        manifest["build"] = metadata
    (stage_dir / "manifest.json").write_text(json.dumps(manifest, indent=1, sort_keys=True) + "\n", encoding="utf-8")


def _write_capture_lifecycle(capture_dir: Path, *, stage: str, outcome: str, build_status: str) -> None:
    """Private fixed-category receipt, separate from snapshot completeness.

    Replace atomically so a failed final write retains the initial aborted
    classification. No exception text, arguments or environment enter it.
    """
    receipt = {"stage": stage, "outcome": outcome, "build_status": build_status}
    temporary = capture_dir / "capture.json.tmp"
    temporary.write_text(json.dumps(receipt, sort_keys=True) + "\n", encoding="utf-8")
    temporary.replace(capture_dir / "capture.json")


def _snapshot_or_die(
    capture_dir: Path, stage: str, build_tree: Path, *, metadata: dict[str, object] | None = None
) -> None:
    """Write one pre-build snapshot; a failure is an explicit capture error."""
    try:
        _write_capture_snapshot(capture_dir, stage, build_tree, metadata=metadata)
    except OSError as exc:
        raise die(f"cannot write the {stage} capture snapshot: {exc}.") from exc


def _workspace_cache(build_dir: Path, *, unsupported_reason: str | None = None) -> workspace_cache.WorkspaceCache:
    try:
        return workspace_cache.prepare(build_dir, project_root=PROJECT_ROOT, unsupported_reason=unsupported_reason)
    except workspace_cache.WorkspaceCacheError as exc:
        raise die(str(exc)) from exc


def cmake_option_enabled(cache: Mapping[str, str], option: str, build_dir: Path) -> bool:
    """Read a configured CMake BOOL; unknown or missing values cannot authorize a suite."""
    value = cache.get(option)
    if value is None:
        raise die(f"Cannot determine {option} in {build_dir / 'CMakeCache.txt'}.")
    normalized = value.upper()
    if normalized in {"ON", "YES", "TRUE", "Y", "1"}:
        return True
    if normalized in {"OFF", "NO", "FALSE", "N", "0", "IGNORE", "NOTFOUND", ""} or normalized.endswith("-NOTFOUND"):
        return False
    raise die(f"Invalid {option}={value!r} in {build_dir / 'CMakeCache.txt'}.")


def sync_compiler_cache(build_dir: Path, *, unsupported_reason: str | None = None) -> None:
    """Reconfigure an existing tree only when its managed cache settings changed."""
    workspace = _workspace_cache(build_dir, unsupported_reason=unsupported_reason)
    arguments = [
        *compiler_cache.cmake_launcher_arguments(build_dir=build_dir),
        *workspace.cmake_arguments,
    ]
    if not arguments:
        return
    print(f"Updating compiler-cache settings in {build_dir}...")
    cmake_build_dir = workspace.compiler_build_dir or build_dir
    # Only native Ninja trees reach this reconfigure today, so the tree label is fixed.
    with phases.measure("configure", tree="native") as phase:
        phase.status = run(
            ["cmake", "-S", str(workspace.source_dir), "-B", str(cmake_build_dir), *arguments],
            cwd=workspace.compiler_build_dir or PROJECT_ROOT,
        )
    if phase.status != 0:
        raise die("compiler-cache configure failed.")


def validate_build_tree(
    args: argparse.Namespace, build_dir: Path, *, compiler_only: bool = False, expected_build_type: str | None = None
) -> str:
    """Check CMake's configured identity before reusing or reporting a native tree."""
    workspace_cache.validate_consumer(build_dir, project_root=PROJECT_ROOT)
    profile = builddir.platform_profile()
    compiler = "clang" if args.clang else profile.compiler
    portal = "ao.bat" if profile.name == "windows" else "./ao"
    expected_id = {"gcc": "GNU", "clang": "Clang", "msvc": "MSVC"}[compiler]
    remedy = (
        f"Select a compatible build directory or run {portal} build --clean -p {build_dir} with the intended options."
    )
    try:
        cache = {}
        for line in (build_dir / "CMakeCache.txt").read_text(encoding="utf-8").splitlines():
            key, separator, value = line.partition("=")
            if separator and ":" in key and not key.startswith(("//", "#")):
                cache[key.split(":", 1)[0]] = value
        version_parts = [cache.get(f"CMAKE_CACHE_{part}_VERSION", "") for part in ("MAJOR", "MINOR", "PATCH")]
        if not all(part.isdecimal() for part in version_parts):
            raise die(f"Cannot determine the CMake version recorded in {build_dir}. {remedy}")
        version = ".".join(version_parts)
        compiler_dirs = [
            path
            for path in (build_dir / "CMakeFiles").iterdir()
            if path.is_dir() and (path.name == version or path.name.startswith(f"{version}-"))
        ]
        if len(compiler_dirs) != 1:
            raise die(f"Cannot identify one CMake {version} compiler metadata directory in {build_dir}. {remedy}")
        compiler_dir = compiler_dirs[0]
        for language in ("C", "CXX"):
            metadata = (compiler_dir / f"CMake{language}Compiler.cmake").read_text(encoding="utf-8")
            identity = re.search(rf'^set\(CMAKE_{language}_COMPILER_ID "([^"\n]+)"\)$', metadata, re.MULTILINE)
            if identity is None:
                raise die(f"Cannot determine the configured {language} compiler in {build_dir}. {remedy}")
            if identity[1] != expected_id:
                raise die(
                    f"Compiler mismatch in {build_dir}: requested {expected_id}, "
                    f"configured {language} compiler is {identity[1]}. {remedy}"
                )
    except OSError as exc:
        raise die(f"Cannot read build configuration in {build_dir}: {exc}. {remedy}") from exc

    if not compiler_only:
        for option, enabled in (("ASAN", args.asan), ("TSAN", args.tsan)):
            key = f"AOBUS_ENABLE_{option}"
            expected = "ON" if enabled else "OFF"
            actual = cache.get(key, "<missing>")
            if cmake_option_enabled(cache, key, build_dir) != bool(enabled):
                raise die(
                    f"Sanitizer mismatch in {build_dir}: requested {key}={expected}, "
                    f"configured {actual}. Run {portal} build -p {build_dir} with the intended options first."
                )
        for argument, option in (("gtk", "AOBUS_BUILD_GTK"), ("system_media", "AOBUS_BUILD_SYSTEM_MEDIA")):
            requested = getattr(args, argument, None)
            if requested is None:
                continue
            expected = requested.upper()
            actual = cache.get(option, "<missing>")
            if cmake_option_enabled(cache, option, build_dir) != (expected == "ON"):
                raise die(
                    f"Feature mismatch in {build_dir}: requested {option}={expected}, configured {actual}. "
                    f"Run {portal} build -p {build_dir} with the intended options first."
                )
    if expected_build_type is not None and cache.get("CMAKE_BUILD_TYPE") != expected_build_type:
        raise die(
            f"Build mode mismatch in {build_dir}: requested {expected_build_type}, "
            f"configured CMAKE_BUILD_TYPE={cache.get('CMAKE_BUILD_TYPE', '<missing>')}. "
            f"Select the matching flavor or run {portal} build {args.flavor} -p {build_dir} first."
        )
    return compiler


def do_build(args: argparse.Namespace, targets: list[str]) -> BuildResult:
    """Shared by `ao build` and `ao check`. Raises SystemExit on failure."""
    profile = validate_build_options(args)

    lint_context = WINUI_LINT_CONTEXT_TARGET in targets
    if lint_context and targets != [WINUI_LINT_CONTEXT_TARGET]:
        raise die("The WinUI lint-context target must be built alone.")
    requested_winui = lint_context or any(target in WINUI_TARGETS for target in targets)
    if requested_winui and not lint_context and any(target not in WINUI_TARGETS for target in targets):
        raise die("WinUI cannot be combined with Ninja targets in one build invocation.")
    if requested_winui:
        if profile.name != "windows":
            raise die("The WinUI frontend is available only on native Windows.")
        if args.clang or args.asan or args.tsan:
            raise die("The initial WinUI MSBuild profile does not support compiler or sanitizer overrides.")
        try:
            winui.require_build_host()
        except RuntimeError as exc:
            raise die(str(exc)) from exc
        preset = builddir.WINDOWS_WINUI_PRESET
    else:
        preset = builddir.preset(args.flavor)

    capture_request = getattr(args, "msbuild_capture", None)
    if capture_request is not None:
        if profile.name != "windows" or not requested_winui or lint_context:
            raise die("--msbuild-capture is available only for full native Windows WinUI builds.")
        if (reason := _capture_syntax_error(capture_request)) is not None:
            raise die(reason)
    phase_tree = "winui" if requested_winui else "native"
    build_dir = (
        Path(args.path)
        if getattr(args, "path", None)
        else builddir.winui_build_dir()
        if requested_winui
        else builddir.build_dir(
            args.flavor,
            clang=args.clang,
            asan=args.asan,
            tsan=args.tsan,
        )
    )

    with buildlock.build_tree_lock(build_dir):
        capture_dir = None
        # Fallbacks must precede every fallible operation after leaf creation.
        cmake_build_dir = build_dir
        build_env = None
        native_started = False
        native_succeeded = False
        primary_error_type: type[BaseException] | None = None
        capture_stage = "setup"
        if capture_request is not None:
            capture_dir = _prepare_capture(capture_request, build_dir=build_dir, source_dir=PROJECT_ROOT)
        try:
            if capture_dir is not None:
                _write_capture_lifecycle(capture_dir, stage=capture_stage, outcome="aborted", build_status="not-run")
                print(_CAPTURE_WARNING)
            if args.clean and build_dir.exists():
                print(f"Cleaning build directory ({build_dir})...")
                _remove_build_directory(build_dir, profile)

            build_dir.mkdir(parents=True, exist_ok=True)
            log = build_dir / "build.log"

            if (build_dir / "CMakeCache.txt").is_file():
                validate_build_tree(args, build_dir, compiler_only=True)

            env = {"CC": "clang", "CXX": "clang++"} if args.clang else None
            build_env = env
            if args.clang:
                print("clang enabled for this build.")

            sanitizer = "sanitizer builds" if args.asan or args.tsan else None
            workspace = _workspace_cache(build_dir, unsupported_reason=sanitizer)
            cmake_build_dir = workspace.compiler_build_dir or build_dir
            configure = ["cmake", "-S", str(workspace.source_dir), "--preset", preset, "-B", str(cmake_build_dir)]
            configure.append(f"-DCMAKE_VERBOSE_MAKEFILE={'ON' if args.verbose else 'OFF'}")
            configure.append(f"-DAOBUS_ENABLE_ASAN={'ON' if args.asan else 'OFF'}")
            configure.append(f"-DAOBUS_ENABLE_TSAN={'ON' if args.tsan else 'OFF'}")
            if (gtk := getattr(args, "gtk", None)) is not None:
                configure.append(f"-DAOBUS_BUILD_GTK={gtk.upper()}")
            if (system_media := getattr(args, "system_media", None)) is not None:
                configure.append(f"-DAOBUS_BUILD_SYSTEM_MEDIA={system_media.upper()}")
            configure.extend(compiler_cache.cmake_launcher_arguments(build_dir=build_dir))
            configure.extend(workspace.cmake_arguments)
            if args.asan:
                sanitizer_name = "ASan" if profile.name == "windows" else "ASan/UBSan"
                print(f"{sanitizer_name} enabled for this build.")
            if args.tsan:
                print("TSan enabled for this build.")

            if capture_dir is not None:
                capture_stage = "before-configure"
                _snapshot_or_die(capture_dir, "before-configure", cmake_build_dir)

            capture_stage = "configure"
            print(f"Configuring Aobus with preset '{preset}' in '{build_dir}'...")
            with phases.measure("configure", tree=phase_tree) as configure_phase:
                configure_phase.status = run(
                    configure, cwd=workspace.compiler_build_dir or PROJECT_ROOT, env=env, log=log
                )
            if configure_phase.status != 0:
                raise die("configure failed.")
            compiler = validate_build_tree(args, build_dir)

            build = ["cmake", "--build", str(cmake_build_dir)]
            if requested_winui:
                build += ["--config", "Debug" if args.flavor == "debug" else "Release"]
            parallel_jobs = parallel_build_jobs()
            build += ["--parallel", str(parallel_jobs)]
            canonical_targets = ["aobus-winui"] if requested_winui and not lint_context else targets
            for target in canonical_targets:
                build += ["--target", target]
            if args.verbose:
                build.append("--verbose")
            if capture_dir is not None:
                # One fixed native logger tail; console verbosity is never changed.
                build += ["--", f"/bl:{capture_dir / 'build.binlog'};ProjectImports=None"]

            print("Generating WinUI lint context..." if lint_context else "Building Aobus...")
            if requested_winui:
                build_env = {
                    **(env or {}),
                    **_winui_build_environment(parallel_jobs),
                    **compiler_cache.msbuild_environment(
                        build_dir, compiler_source_dir=workspace.source_dir, compiler_build_dir=cmake_build_dir
                    ),
                }
            if capture_dir is not None:
                capture_stage = "before-build"
                _snapshot_or_die(
                    capture_dir,
                    "before-build",
                    cmake_build_dir,
                    metadata=_capture_build_metadata(build_env or {}, _capture_cache_identity(cmake_build_dir)),
                )
            build_phase_name = "generated-headers" if lint_context else "build"
            capture_stage = "build"
            with phases.measure(build_phase_name, tree=phase_tree, jobs=parallel_jobs) as build_phase:
                native_started = True
                build_phase.status = run(
                    build, cwd=workspace.compiler_build_dir or PROJECT_ROOT, env=build_env, log=log, append=True
                )
            if build_phase.status != 0:
                raise die("build failed.")
            native_succeeded = True
            return BuildResult(build_dir=build_dir, log=log, compiler=compiler, preset=preset)
        except BaseException as exc:
            primary_error_type = type(exc)
            raise
        finally:
            if capture_dir is not None:
                capture_error: OSError | None = None
                native_status = "ok" if native_succeeded else "failed" if native_started else "not-run"
                if native_succeeded:
                    try:
                        binlog = capture_dir / "build.binlog"
                        if not binlog.is_file() or binlog.stat().st_size == 0:
                            raise OSError("the MSBuild binary logger produced no build.binlog")
                    except OSError as exc:
                        capture_error = exc
                if capture_stage != "setup":
                    try:
                        _write_capture_snapshot(
                            capture_dir,
                            "after-build",
                            cmake_build_dir,
                            metadata=_capture_build_metadata(build_env or {}, _capture_cache_identity(cmake_build_dir)),
                            build_status="ok" if native_succeeded and capture_error is None else "failed",
                        )
                    except OSError as exc:
                        capture_error = capture_error or exc
                outcome = "failed" if primary_error_type or capture_error else "ok"
                if primary_error_type is not None and issubclass(
                    primary_error_type, (KeyboardInterrupt, GeneratorExit)
                ):
                    outcome = "aborted"
                try:
                    _write_capture_lifecycle(
                        capture_dir,
                        stage=capture_stage if primary_error_type else "after-build",
                        outcome=outcome,
                        build_status=native_status,
                    )
                except OSError as exc:
                    capture_error = capture_error or exc
                if capture_error is not None:
                    if primary_error_type is None:
                        # Requested evidence is part of success, not an optional side effect.
                        raise die(
                            f"the build succeeded, but the capture is incomplete: {capture_error}."
                        ) from capture_error
                    print(
                        f"Warning: the after-build capture failed; evidence is incomplete: {capture_error}",
                        file=sys.stderr,
                    )


def print_summary(args: argparse.Namespace, result: BuildResult, tests: str) -> None:
    print()
    print("All done!")
    print(f"  Preset: {result.preset or builddir.preset(args.flavor)}")
    print(f"  Build dir: {result.build_dir}")
    print(f"  Log file: {result.log}")
    print(f"  compiler: {result.compiler}")
    print(f"  asan: {args.asan}")
    print(f"  tsan: {args.tsan}")
    print(f"  tests: {tests}")


def run_command(args: argparse.Namespace) -> int:
    result = do_build(args, args.target)
    print_summary(args, result, tests="skipped (use ./ao check or ./ao test)")
    return 0
