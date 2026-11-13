"""Shared native request options and configured performance artifact admission.

Queries describe DefaultInfo, not a bazel-bin filename convention. Performance
review holds the selected tree through launch; --no-build performs analysis only
and never executes a build action. Native provider/API acceptance needs the
pinned Bazel integration probe in addition to the Python boundary tests.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import tempfile
from collections.abc import Generator, Mapping, Sequence
from contextlib import contextmanager
from dataclasses import dataclass, replace
from pathlib import Path, PurePosixPath
from typing import Any

from . import bazel_native as native
from . import bazel_profiles, bazel_workspace, builddir, jobs
from .buildlock import build_tree_lock
from .paths import PROJECT_ROOT

CONFIGURATIONS = bazel_profiles.CONFIGURATIONS

# Encode only portable scalar values. An absent FilesToRunProvider API is an
# explicit query failure, not permission to guess an output or ignore runfiles.
_PROVIDER_EXPRESSION = """json.encode({
    'label': str(target.label),
    'incompatible': 'IncompatiblePlatformProvider' in providers(target),
    'executable': (providers(target)['DefaultInfo'].files_to_run.executable.path
        if providers(target)['DefaultInfo'].files_to_run.executable != None else None),
    'manifest': (providers(target)['DefaultInfo'].files_to_run.runfiles_manifest.path
        if providers(target)['DefaultInfo'].files_to_run.runfiles_manifest != None else None),
    'files': [{'path': f.path, 'directory': f.is_directory} for f in providers(target)['DefaultInfo'].files.to_list()],
    'runfiles': [{'path': f.path, 'short_path': f.short_path, 'directory': f.is_directory}
        for f in providers(target)['DefaultInfo'].default_runfiles.files.to_list()],
    'environment': (providers(target)['RunEnvironmentInfo'].environment
        if 'RunEnvironmentInfo' in providers(target) else {}),
})"""
_COMPATIBILITY_EXPRESSION = (
    "json.encode({'label': str(target.label), 'incompatible': 'IncompatiblePlatformProvider' in providers(target)})"
)


def positive_jobs(value: str) -> int:
    parsed = int(value)
    if parsed < 1:
        raise argparse.ArgumentTypeError("must be a positive integer")
    return parsed


def _remote_cache(value: str) -> str:
    try:
        return bazel_workspace.validate_remote_cache(value)
    except native.BazelWorkspaceError as error:
        raise argparse.ArgumentTypeError(str(error)) from error


def add_remote_cache_argument(parser: argparse.ArgumentParser, help_text: str) -> None:
    parser.add_argument(
        "--remote-cache",
        type=_remote_cache,
        default=os.environ.get("AOBUS_BAZEL_REMOTE_CACHE") or None,
        help=f"{help_text} (default: $AOBUS_BAZEL_REMOTE_CACHE)",
    )


def add_arguments(parser: argparse.ArgumentParser, *, default_flavor: str = "fastbuild") -> None:
    """Native options shared by completion checks and performance review."""
    parser.add_argument(
        "flavor",
        nargs="?",
        choices=("fastbuild", "debug", "release", "profile"),
        default=default_flavor,
        help=f"build flavor (default: {default_flavor})",
    )
    parser.add_argument("-p", "--path", help="Bazel tree outside the source checkout (honors BUILD_DIR)")
    parser.add_argument(
        "--jobs",
        type=positive_jobs,
        default=jobs.default_workers(),
        help="parallel workers (Bazel and check's tooling suite)",
    )
    parser.add_argument("--vcpkg-prefix", help="existing governed native SDK prefix")
    add_remote_cache_argument(parser, "trusted Bazel remote cache URL")
    compiler = parser.add_mutually_exclusive_group()
    compiler.add_argument("--compiler", choices=("gcc", "clang"), help="Linux target compiler (GCC by default)")
    compiler.add_argument("--clang", action="store_true", help="select Clang on Linux (macOS already uses Clang)")
    sanitizer = parser.add_mutually_exclusive_group()
    sanitizer.add_argument("--asan", action="store_true", help="use the governed AddressSanitizer configuration")
    sanitizer.add_argument("--tsan", action="store_true", help="use the governed ThreadSanitizer configuration")
    parser.add_argument("--gtk", choices=("on", "off"), help="enable or disable the GTK graph feature")
    parser.add_argument("--system-media", choices=("on", "off"), help="enable or disable Linux system-media support")


def configuration(args: argparse.Namespace) -> str:
    """Resolve flavor/sanitizer options and the shared config namespace."""
    selected = str(getattr(args, "config", None) or getattr(args, "flavor", "fastbuild"))
    asan, tsan = bool(getattr(args, "asan", False)), bool(getattr(args, "tsan", False))
    if asan and tsan:
        raise native.BazelWorkspaceError("AddressSanitizer and ThreadSanitizer are mutually exclusive")
    if asan or tsan:
        requested = "asan" if asan else "tsan"
        # The instrumented subset stays dbg-based; the normal completion keeps
        # the ordinary fastbuild default or an explicitly requested Debug flavor.
        if selected not in ("fastbuild", "debug", requested):
            raise native.BazelWorkspaceError("sanitizer switches are incompatible with the release and profile flavors")
        selected = requested
    if selected not in CONFIGURATIONS:
        raise native.BazelWorkspaceError(f"unknown Bazel configuration: {selected}")
    return selected


def _feature(value: object, name: str) -> bool:
    if type(value) is bool:
        return value
    if value in ("on", "true"):
        return True
    if value in ("off", "false"):
        return False
    raise native.BazelWorkspaceError(f"{name} must be on/off or a boolean")


def selected_compiler(args: argparse.Namespace) -> str:
    """Select a supported target compiler without changing native execution tools."""
    host = builddir.platform_profile().name
    requested = getattr(args, "compiler", None)
    clang = getattr(args, "clang", False)
    if requested is not None and (host != "linux" or requested not in {"gcc", "clang"}):
        raise native.BazelWorkspaceError("--compiler selects gcc or clang on Linux only")
    if clang and host == "windows":
        raise native.BazelWorkspaceError("--clang cannot replace the governed Windows MSVC toolchain")
    if requested is not None and clang:
        raise native.BazelWorkspaceError("--compiler and --clang cannot be combined")
    return requested or ("clang" if clang or host == "macos" else "msvc" if host == "windows" else "gcc")


def feature_options(args: argparse.Namespace) -> tuple[str, ...]:
    """Forward explicit graph features and target-compiler selection to Bazel."""
    compiler = selected_compiler(args)
    options = tuple(
        f"--//bazel:{setting}={'true' if _feature(value, option) else 'false'}"
        for option, setting in (("gtk", "gtk_enabled"), ("system_media", "system_media_enabled"))
        if (value := getattr(args, option, None)) is not None
    )
    if builddir.platform_profile().name == "linux" and (
        getattr(args, "compiler", None) is not None or getattr(args, "clang", False)
    ):
        options += ("--compiler=" + compiler,)
    return options


def validate_request(args: argparse.Namespace) -> tuple[str, str]:
    """Pure admission before SDK discovery, path selection, locking or queries."""
    config, host = configuration(args), builddir.platform_profile().name
    bazel_profiles.validate_request(config, host)
    if getattr(args, "clang", False) and host == "windows":
        raise native.BazelWorkspaceError("--clang cannot replace the governed Windows MSVC toolchain")
    feature_options(args)
    if remote := getattr(args, "remote_cache", None):
        bazel_workspace.validate_remote_cache(remote)
    if getattr(args, "system_media", None) is not None and host != "linux":
        raise native.BazelWorkspaceError("--system-media is a Linux-only graph feature")
    if getattr(args, "gtk", None) is not None and host != "linux":
        raise native.BazelWorkspaceError("--gtk is a Linux-only graph feature")
    if type(getattr(args, "jobs", 1)) is not int or getattr(args, "jobs", 1) < 1:
        raise native.BazelWorkspaceError("Bazel jobs must be positive")
    return config, host


def _label(value: object) -> str:
    if not isinstance(value, str):
        raise native.BazelWorkspaceError("configured executable label must be a string")
    label = value.removeprefix("@@").removeprefix("@") if value.startswith(("@@//", "@//")) else value
    if not re.fullmatch(r"//[A-Za-z0-9._+~/-]*:[A-Za-z0-9._+~/-]+", label):
        raise native.BazelWorkspaceError(f"invalid first-party executable label: {value!r}")
    package, target = label[2:].split(":")
    if any(part in {"", ".", ".."} for text in (package, target) if text for part in text.split("/")):
        raise native.BazelWorkspaceError(f"unsafe executable label: {label}")
    return label


def _relative(value: object) -> PurePosixPath:
    if (
        not isinstance(value, str)
        or not value
        or value.startswith("/")
        or any(character in value for character in "\\:")
        or any(ord(character) < 32 or ord(character) == 127 for character in value)
        or any(part in {"", ".", ".."} for part in value.split("/"))
    ):
        raise native.BazelWorkspaceError(f"invalid exec-relative artifact path: {value!r}")
    return PurePosixPath(value)


def _output_path(value: object, execution_root: Path, output_base: Path) -> Path:
    relative = _relative(value)
    path = execution_root.joinpath(*relative.parts)
    if not path.resolve().is_relative_to(output_base.resolve()):
        raise native.BazelWorkspaceError(f"configured output escapes the selected output base: {path}")
    return path


@dataclass(frozen=True)
class ExecutableArtifact:
    """One actual configured target and its admitted launch context."""

    target: str
    configuration: str
    config: str
    tree: Path
    execution_root: Path
    executable: Path
    declared_outputs: tuple[Path, ...]
    runfiles: tuple[str, ...]
    runfiles_directory: Path | None
    runfiles_manifest: Path | None
    compiler_identity: str = "unverified"
    # The rule's declared env, which bazel test and bazel run would also apply.
    target_environment: tuple[tuple[str, str], ...] = ()
    executable_sha256: str = "unverified"

    def environment(self) -> dict[str, str]:
        # Empty values deliberately override stale inherited Bazel-test context.
        # Bzlmod's main-repository runfiles name is _main, independently of the
        # MODULE's apparent repository name. Native acceptance must probe this.
        directory = str(self.runfiles_directory) if self.runfiles_directory is not None else ""
        return {
            **dict(self.target_environment),
            "RUNFILES_DIR": directory,
            "JAVA_RUNFILES": directory,
            "TEST_SRCDIR": directory,
            "RUNFILES_MANIFEST_FILE": str(self.runfiles_manifest) if self.runfiles_manifest is not None else "",
            "RUNFILES_MANIFEST_ONLY": "1" if self.runfiles_directory is None and self.runfiles_manifest else "0",
            "TEST_WORKSPACE": "_main",
        }

    def provenance(self) -> dict[str, object]:
        return {
            "target": self.target,
            "configuration": self.configuration,
            "config": self.config,
            "tree": str(self.tree),
            "execution_root": str(self.execution_root),
            "executable": str(self.executable),
            "declared_outputs": [str(path) for path in self.declared_outputs],
            "runfiles_directory": str(self.runfiles_directory) if self.runfiles_directory else None,
            "runfiles_manifest": str(self.runfiles_manifest) if self.runfiles_manifest else None,
            "compiler": self.compiler_identity,
            "executable_sha256": self.executable_sha256,
        }


def _runfile_key(value: object) -> str:
    if not isinstance(value, str):
        raise native.BazelWorkspaceError("runfile short_path must be a string")
    # Bazel's external short_path has exactly one leading ../ repository
    # component. This translates a provider identity, not a filesystem search.
    if value.startswith("../"):
        return str(_relative(value[3:]))
    return "_main/" + str(_relative(value))


def _manifest_entries(path: Path) -> dict[str, Path]:
    def decode(value: str) -> str:
        return re.sub(r"\\([bsn])", lambda match: {"b": "\\", "s": " ", "n": "\n"}[match[1]], value)

    entries = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        escaped = line.startswith(" ")
        fields = (line[1:] if escaped else line).split(" ", 1)
        if len(fields) != 2:
            raise native.BazelWorkspaceError(f"malformed runfiles manifest: {path}")
        key, physical = (decode(field) for field in fields) if escaped else fields
        if key in entries:
            raise native.BazelWorkspaceError(f"duplicate runfiles manifest entry: {key}")
        entries[key] = Path(physical)
    return entries


def _verify_runfiles(
    declarations: list[Any],
    directory: Path | None,
    manifest: Path | None,
    execution_root: Path,
    *,
    windows_output_base: Path | None = None,
) -> tuple[str, ...]:
    entries = _manifest_entries(manifest) if manifest is not None else {}

    def manifest_input(key: str) -> Path | None:
        if key in entries:
            return entries[key]
        # Official manifests may map a directory containing this file.
        for ancestor in PurePosixPath(key).parents:
            if str(ancestor) in entries:
                return entries[str(ancestor)] / PurePosixPath(key).relative_to(ancestor)
        return None

    def windows_path_key(path: Path) -> str:
        # Only ASCII case aliases are admitted; distinct Unicode names stay distinct.
        return str(path).translate(str.maketrans("ABCDEFGHIJKLMNOPQRSTUVWXYZ", "abcdefghijklmnopqrstuvwxyz"))

    paths = []
    for declaration in declarations:
        if not isinstance(declaration, dict) or type(declaration.get("directory")) is not bool:
            raise native.BazelWorkspaceError("malformed declared runfile")
        relative = _relative(declaration.get("path"))
        key = _runfile_key(declaration.get("short_path"))
        expected = execution_root.joinpath(*relative.parts)
        supplied = directory / key if directory is not None else None
        if supplied is None or not supplied.exists():
            supplied = manifest_input(key)
        if supplied is None or not supplied.exists() or not expected.exists():
            raise native.BazelWorkspaceError(f"missing declared runfile: {key}")
        matches = declaration["directory"] == supplied.is_dir() and supplied.samefile(expected)
        if (
            not matches
            and windows_output_base is not None
            and not declaration["directory"]
            and supplied.is_file()
            and not supplied.is_symlink()
            and expected.is_file()
        ):
            # Windows Bazel may copy files. The configured manifest must bind
            # the copy to a declaration-derived route before comparing bytes.
            canonical = expected.resolve()
            routes = {windows_path_key(expected), windows_path_key(canonical)}
            if relative.parts[0] == "external":
                repository_file = windows_output_base.joinpath(*relative.parts)
                if windows_path_key(repository_file.resolve()) == windows_path_key(canonical):
                    routes.add(windows_path_key(repository_file))
            origin = manifest_input(key)
            matches = (
                origin is not None
                and windows_path_key(origin) in routes
                and native.sha256(supplied) == native.sha256(expected)
            )
        if not matches:
            raise native.BazelWorkspaceError(f"runfile does not resolve to its configured declared input: {key}")
        paths.append(str(relative))
    return tuple(paths)


def parse_artifact(
    row: Mapping[str, Any],
    *,
    target: str,
    checksum: str,
    config: str,
    tree: Path,
    execution_root: Path,
    windows: bool = False,
) -> ExecutableArtifact:
    """Reject undeclared or missing executables instead of selecting a first file."""
    if not re.fullmatch(r"[0-9a-f]{64}", checksum):
        raise native.BazelWorkspaceError("configured executable requires a full configuration checksum")
    if _label(row.get("label")) != target or row.get("incompatible") is not False:
        raise native.BazelWorkspaceError(f"configured provider row does not admit {target}")
    output_base = tree / "state/output"
    binary_relative = _relative(row.get("executable"))
    declared = row.get("files")
    if not isinstance(declared, list) or not declared:
        raise native.BazelWorkspaceError(f"{target} has no declared DefaultInfo outputs")
    outputs = []
    admitted = False
    seen = set()
    for declaration in declared:
        if not isinstance(declaration, dict) or type(declaration.get("directory")) is not bool:
            raise native.BazelWorkspaceError(f"{target} has malformed DefaultInfo outputs")
        relative = _relative(declaration.get("path"))
        if relative in seen:
            raise native.BazelWorkspaceError(f"{target} has duplicate DefaultInfo outputs")
        seen.add(relative)
        outputs.append(_output_path(str(relative), execution_root, output_base))
        admitted |= relative == binary_relative or (
            declaration["directory"] and binary_relative.is_relative_to(relative)
        )
    if not admitted:
        raise native.BazelWorkspaceError(f"{target} executable is not a declared output or declared tree member")
    binary = _output_path(str(binary_relative), execution_root, output_base)
    if not binary.is_file() or (os.name != "nt" and not os.access(binary, os.X_OK)):
        raise native.BazelWorkspaceError(f"configured executable is missing or not executable: {binary}; build first")
    runfiles = row.get("runfiles")
    if not isinstance(runfiles, list):
        raise native.BazelWorkspaceError(f"{target} has malformed default runfiles")
    manifest = None
    if row.get("manifest") is not None:
        manifest = _output_path(row["manifest"], execution_root, output_base)
        if not manifest.is_file():
            manifest = None
    directory = Path(str(binary) + ".runfiles")
    if directory.exists() and not directory.resolve().is_relative_to(output_base.resolve()):
        raise native.BazelWorkspaceError(f"{target} runfiles directory escapes the selected output base")
    runfiles_directory = directory if directory.is_dir() else None
    if runfiles and runfiles_directory is None and manifest is None:
        raise native.BazelWorkspaceError(
            f"{target} has declared runfiles but no existing runfiles directory/provider manifest"
        )
    verified_runfiles = _verify_runfiles(
        runfiles, runfiles_directory, manifest, execution_root, windows_output_base=output_base if windows else None
    )
    environment = row.get("environment")
    if not isinstance(environment, dict) or any(
        not isinstance(name, str) or not name or "=" in name or not isinstance(value, str)
        for name, value in environment.items()
    ):
        raise native.BazelWorkspaceError(f"{target} has a malformed declared run environment")
    return ExecutableArtifact(
        target,
        checksum,
        config,
        tree,
        execution_root,
        binary,
        tuple(outputs),
        verified_runfiles,
        runfiles_directory,
        manifest,
        target_environment=tuple(sorted(environment.items())),
        executable_sha256=native.sha256(binary),
    )


class _Invocation:
    """One caller-locked preparation; diagnostics survive failures in its tree."""

    def __init__(self, args: argparse.Namespace, tree: Path, config: str):
        self.args, self.tree, self.config = args, tree, config
        tree.mkdir(parents=True, exist_ok=True)
        self.directory = Path(tempfile.mkdtemp(prefix="workflow-", dir=tree))
        self.executable = bazel_workspace.bazel_executable()
        base = bazel_workspace.prepare_workspace(tree, config, prefix=getattr(args, "vcpkg_prefix", None))
        self.prepared = self.directory / "configuration"
        self.prepared.mkdir()
        content = (base / "aobus.bazelrc").read_text(encoding="utf-8")
        lines = [content.rstrip(), "common --lockfile_mode=error", "build --enable_runfiles=true"]
        lines.extend("build " + option for option in feature_options(args))
        (self.prepared / "aobus.bazelrc").write_text("\n".join(lines) + "\n", encoding="utf-8")
        self.sequence = 0

    def invoke(self, action: str, arguments: Sequence[str]) -> str:
        self.sequence += 1
        argv = [*bazel_workspace.invocation(self.executable, self.prepared, isolate_rc=True), action]
        if action not in {"query", "info"}:
            argv.append(f"--jobs={getattr(self.args, 'jobs', 1)}")
        if action in {"build", "cquery", "aquery"} and getattr(self.args, "remote_cache", None):
            argv.append(f"--remote_cache={self.args.remote_cache}")
        argv.extend(arguments)
        output = self.directory / f"{self.sequence:02}-{action}.stdout.log"
        errors = self.directory / f"{self.sequence:02}-{action}.stderr.log"
        receipt: dict[str, Any] = {"schema": 1, "action": action, "argv": argv, "status": "incomplete", "exit": None}
        if action == "build":
            receipt["build_log"] = str(self.tree / "build.log")
        else:
            receipt.update(stdout=str(output), stderr=str(errors))
        try:
            if action == "build":
                # The workspace owns the governed build log and lifecycle;
                # this invocation records only its local receipt.
                status = bazel_workspace.execute(
                    self.executable,
                    self.tree,
                    self.prepared,
                    action,
                    self.config,
                    list(arguments),
                    jobs=getattr(self.args, "jobs", 1),
                    remote_cache=getattr(self.args, "remote_cache", None),
                    isolate_rc=True,
                )
            else:
                with output.open("xb") as out, errors.open("xb") as err:
                    result = subprocess.run(
                        argv,
                        cwd=PROJECT_ROOT,
                        env=native.child_environment(isolate_rc=True),
                        stdout=out,
                        stderr=err,
                        check=False,
                    )
                status = result.returncode
            receipt.update(status="exited", exit=status)
            if status:
                diagnostics = receipt.get("build_log", f"stdout: {output}; stderr: {errors}")
                raise native.BazelWorkspaceError(f"Bazel {action} exited {status}; {diagnostics}")
            return output.read_text(encoding="utf-8") if action != "build" else ""
        except BaseException as error:
            receipt.update(status="interrupted" if isinstance(error, KeyboardInterrupt) else "error", error=str(error))
            raise
        finally:
            native.write_json(self.directory / f"{self.sequence:02}-{action}.receipt.json", receipt)

    def query(self, expression: str, starlark: str) -> list[dict[str, Any]]:
        text = self.invoke("cquery", [expression, "--output=starlark", "--starlark:expr=" + starlark])
        try:
            rows = [json.loads(line) for line in text.splitlines() if line.strip()]
        except json.JSONDecodeError as error:
            raise native.BazelWorkspaceError("invalid configured provider JSON; inspect workflow query logs") from error
        if any(not isinstance(row, dict) for row in rows):
            raise native.BazelWorkspaceError("configured provider query must emit JSON objects")
        return rows

    def compiler_identity(self, target: str, execution_root: Path) -> str:
        from .bazel_contract import extract_compile_actions

        expression = f'mnemonic("CppCompile", deps({target}))'
        document = json.loads(
            self.invoke(
                "aquery",
                [
                    expression,
                    "--output=jsonproto",
                    "--include_artifacts=false",
                    "--include_commandline=true",
                    "--include_param_files=true",
                ],
            )
        )
        actions = extract_compile_actions(document)
        compilers = sorted({action.argv[0] for action in actions if not action.is_tool and action.argv})
        if not compilers:
            raise native.BazelWorkspaceError("no configured C++ compiler actions for performance provenance")
        identities = []
        for compiler in compilers:
            path = Path(compiler)
            path = path if path.is_absolute() else execution_root / path
            if not path.is_file():
                raise native.BazelWorkspaceError(f"configured compiler is not available for provenance: {path}")
            identities.append(f"{path.resolve()} sha256={native.sha256(path)}")
        return "; ".join(identities)


def validate_performance_request(args: argparse.Namespace) -> tuple[str, str]:
    """Reject unsupported instrumentation before SDK discovery or tree selection."""
    config, host = validate_request(args)
    if config in bazel_profiles.SANITIZER_CONFIGS:
        raise native.BazelWorkspaceError(
            "ao perf does not support sanitizer configurations; use debug, release or profile"
        )
    return config, host


@contextmanager
def prepare_artifact(
    args: argparse.Namespace,
    target: str,
    *,
    no_build: bool = False,
) -> Generator[ExecutableArtifact]:
    """Resolve the performance executable and compiler, holding its tree through use."""
    config, host = validate_performance_request(args)
    target = _label(target)
    tree = bazel_workspace.workspace_path(getattr(args, "path", None), config)
    with build_tree_lock(tree):
        native.require_supported_source()
        invocation = _Invocation(args, tree, config)
        compatibility = invocation.query(target, _COMPATIBILITY_EXPRESSION)
        if (
            len(compatibility) != 1
            or _label(compatibility[0].get("label")) != target
            or type(compatibility[0].get("incompatible")) is not bool
        ):
            raise native.BazelWorkspaceError("configured query did not return one exact requested executable")
        if compatibility[0]["incompatible"]:
            raise native.BazelWorkspaceError(f"{target} is incompatible with the selected native configuration")
        from .bazel_test_ownership import extract_configured_targets

        identities = extract_configured_targets(json.loads(invocation.invoke("cquery", [target, "--output=jsonproto"])))
        if len(identities) != 1:
            raise native.BazelWorkspaceError(f"multiple configurations or missing identity for executable {target}")
        identity = next(iter(identities))
        if identity.label != target:
            raise native.BazelWorkspaceError("configured executable identity query disagrees with selection")
        providers = invocation.query(target, _PROVIDER_EXPRESSION)
        if len(providers) != 1 or _label(providers[0].get("label")) != target:
            raise native.BazelWorkspaceError("configured executable providers disagree with selected identity")
        if not no_build:
            invocation.invoke("build", [target])
        execution_root = Path(invocation.invoke("info", ["execution_root"]).strip())
        if not execution_root.is_absolute() or not execution_root.resolve().is_relative_to(
            (tree / "state/output").resolve()
        ):
            raise native.BazelWorkspaceError("Bazel execution_root is not inside the selected output base")
        compiler = invocation.compiler_identity(target, execution_root)
        if no_build:
            compiler = "unverified existing artifact; currently configured compiler: " + compiler
        artifact = replace(
            parse_artifact(
                providers[0],
                target=target,
                checksum=identity.configuration,
                config=config,
                tree=tree,
                execution_root=execution_root,
                windows=host == "windows",
            ),
            compiler_identity=compiler,
        )
        native.write_json(
            invocation.directory / "artifacts.json",
            {
                "schema": 1,
                "source_revision": "unverified",
                "artifact_origin": "existing; build skipped"
                if no_build
                else "built by this invocation (not a source snapshot)",
                "artifacts": [artifact.provenance()],
            },
        )
        yield artifact
