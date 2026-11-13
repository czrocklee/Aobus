"""Configured executable admission shared by test, run and performance workflows.

Queries describe DefaultInfo, not a bazel-bin filename convention. The caller
holds the selected tree through launch; --no-build performs analysis only and
never executes a build action. Native provider/API acceptance needs the pinned
Bazel integration probe in addition to the Python boundary tests.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
from collections.abc import Generator, Mapping, Sequence
from contextlib import contextmanager
from dataclasses import dataclass, replace
from functools import cache
from pathlib import Path, PurePosixPath
from typing import TYPE_CHECKING, Any

from . import bazel_native as native
from . import bazel_profiles, bazel_workspace, builddir, jobs
from .buildlock import build_tree_lock
from .paths import PROJECT_ROOT

if TYPE_CHECKING:
    from .bazel_sanitizers import FrozenRuntime

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


def add_arguments(
    parser: argparse.ArgumentParser, *, default_flavor: str = "debug", flavor: bool = True, gtk_option: str = "--gtk"
) -> None:
    """Retained native command options, independent of the retired build module."""
    if flavor:
        parser.add_argument(
            "flavor",
            nargs="?",
            choices=("debug", "release", "profile"),
            default=default_flavor,
            help=f"build flavor (default: {default_flavor})",
        )
    else:
        parser.set_defaults(flavor=default_flavor)
    parser.add_argument("-p", "--path", help="Bazel tree outside the source checkout (honors BUILD_DIR)")
    parser.add_argument("--jobs", type=positive_jobs, default=jobs.default_workers(), help="Bazel build workers")
    parser.add_argument("--vcpkg-prefix", help="existing governed native SDK prefix")
    add_remote_cache_argument(parser, "trusted Bazel remote cache URL")
    parser.add_argument("--clang", action="store_true", help="request the governed Clang toolchain (not a GCC switch)")
    sanitizer = parser.add_mutually_exclusive_group()
    sanitizer.add_argument("--asan", action="store_true", help="use the governed AddressSanitizer configuration")
    sanitizer.add_argument("--tsan", action="store_true", help="use the governed ThreadSanitizer configuration")
    parser.add_argument(gtk_option, dest="gtk", choices=("on", "off"), help="enable or disable the GTK graph feature")
    parser.add_argument("--system-media", choices=("on", "off"), help="enable or disable Linux system-media support")


def configuration(args: argparse.Namespace) -> str:
    """Resolve legacy flavor/sanitizer spelling and the shared config namespace."""
    selected = str(getattr(args, "config", None) or getattr(args, "flavor", "debug"))
    asan, tsan = bool(getattr(args, "asan", False)), bool(getattr(args, "tsan", False))
    if asan and tsan:
        raise native.BazelWorkspaceError("AddressSanitizer and ThreadSanitizer are mutually exclusive")
    if asan or tsan:
        requested = "asan" if asan else "tsan"
        if selected not in ("debug", requested):
            raise native.BazelWorkspaceError("sanitizer switches require the debug flavor")
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


def feature_options(args: argparse.Namespace) -> tuple[str, ...]:
    """Forward every explicit feature request to the graph-owned build setting."""
    return tuple(
        f"--//bazel:{setting}={'true' if _feature(value, option) else 'false'}"
        for option, setting in (("gtk", "gtk_enabled"), ("system_media", "system_media_enabled"))
        if (value := getattr(args, option, None)) is not None
    )


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
    requested_by: tuple[str, ...] = ()
    # The rule's declared env, which bazel test and bazel run would also apply.
    target_environment: tuple[tuple[str, str], ...] = ()
    runtime_environment: tuple[tuple[str, str], ...] = ()
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
            **dict(self.runtime_environment),
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
        self.runtime: FrozenRuntime | None = None
        if config in ("asan", "tsan"):
            from .bazel_sanitizers import freeze_runtime

            self.runtime = freeze_runtime(config, builddir.platform_profile().name, dict(os.environ), self.directory)
            with (self.prepared / "aobus.bazelrc").open("a", encoding="utf-8") as output:
                for name, value in sorted(self.runtime.environment.items()):
                    output.write("build " + json.dumps(f"--test_env={name}={value}") + "\n")

    def invoke(self, action: str, arguments: Sequence[str], *, capture: bool = True) -> str:
        self.sequence += 1
        if self.runtime is not None:
            self.runtime.verify()
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
                # The parent supplies execute's default-compatible isolate_rc
                # parameter before integration. Its governed build log/lifecycle
                # owns execution; this invocation owns only this local receipt.
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
            return output.read_text(encoding="utf-8") if capture and action != "build" else ""
        except BaseException as error:
            receipt.update(status="interrupted" if isinstance(error, KeyboardInterrupt) else "error", error=str(error))
            raise
        finally:
            native.write_json(self.directory / f"{self.sequence:02}-{action}.receipt.json", receipt)

    def query(self, expression: str, starlark: str, *, options: Sequence[str] = ()) -> list[dict[str, Any]]:
        text = self.invoke("cquery", [expression, *options, "--output=starlark", "--starlark:expr=" + starlark])
        try:
            rows = [json.loads(line) for line in text.splitlines() if line.strip()]
        except json.JSONDecodeError as error:
            raise native.BazelWorkspaceError("invalid configured provider JSON; inspect workflow query logs") from error
        if any(not isinstance(row, dict) for row in rows):
            raise native.BazelWorkspaceError("configured provider query must emit JSON objects")
        return rows

    def compiler_identity(self, labels: Sequence[str], execution_root: Path) -> str:
        from .bazel_contract import extract_compile_actions

        expression = 'mnemonic("CppCompile", deps(set(' + " ".join(labels) + ")))"
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


@dataclass(frozen=True)
class LintFixtureAction:
    """The configured fixture producer, not its cacheable result launcher."""

    configuration: str
    action_key: str
    argv: tuple[str, ...]
    environment: tuple[tuple[str, str], ...]
    inputs: tuple[Path, ...]
    execution_root: Path
    directory: Path
    host: str

    def run(self, *, log: Path | None = None) -> int:
        # A distinct, exclusively created directory makes an old fixture report
        # ineligible even when --no-build or --repeat reuses the same tools.
        attempt = Path(tempfile.mkdtemp(prefix="lint-run-", dir=self.directory))
        destinations = {name: attempt / name for name in ("report", "log", "work")}
        argv = list(self.argv)
        for name, path in destinations.items():
            argv[argv.index("--" + name) + 1] = str(path)
        receipt: dict[str, Any] = {
            "schema": 1,
            "target": "//test/lint:lint_test",
            "configuration": self.configuration,
            "action_key": self.action_key,
            "argv": argv,
            "environment": dict(self.environment),
            "cwd": str(self.execution_root),
            "helper_exit": None,
            "status": "incomplete",
            "report": str(destinations["report"]),
            "fixture_status": None,
        }
        stdout, stderr = attempt / "stdout.log", attempt / "stderr.log"
        receipt.update(stdout=str(stdout), stderr=str(stderr))
        try:
            for path in self.inputs:
                if not path.exists() or not (path.is_file() or path.is_dir()):
                    raise native.BazelWorkspaceError(
                        f"missing declared lint fixture input: {path}; build verification first"
                    )
            with stdout.open("xb") as out, stderr.open("xb") as err:
                result = subprocess.run(
                    argv,
                    cwd=self.execution_root,
                    env=native.child_environment(dict(self.environment), isolate_rc=True),
                    stdout=out,
                    stderr=err,
                    check=False,
                )
            receipt["helper_exit"] = result.returncode
            for path, stream in ((stdout, sys.stdout), (stderr, sys.stderr)):
                stream.write(path.read_text(encoding="utf-8", errors="replace"))
                stream.flush()
            if log is not None:
                with log.open("ab") as sink:
                    sink.write(f"=== Fresh lint fixtures: {attempt} ===\n".encode())
                    for path in (stdout, stderr):
                        sink.write(path.read_bytes())
            if result.returncode:
                receipt["status"] = "helper failed"
                return result.returncode
            report = destinations["report"]
            if report.is_symlink() or not report.is_file():
                raise native.BazelWorkspaceError(f"lint helper did not produce its new regular report: {report}")
            completed = json.loads(report.read_text(encoding="utf-8"), object_pairs_hook=_lint_unique_pairs)
            if (
                not isinstance(completed, dict)
                or set(completed) != {"schema", "status", "platform"}
                or type(completed["schema"]) is not int
                or completed["schema"] != 1
                or type(completed["status"]) is not int
                or completed["status"] not in (0, 1)
                or completed["platform"] != self.host
            ):
                raise native.BazelWorkspaceError(f"malformed fresh lint fixture report: {report}")
            fixture_log = destinations["log"]
            if fixture_log.is_symlink() or not fixture_log.is_file() or not destinations["work"].is_dir():
                raise native.BazelWorkspaceError(f"lint helper omitted fresh log/work outputs: {attempt}")
            if log is not None:
                with log.open("ab") as sink:
                    sink.write(fixture_log.read_bytes())
            receipt.update(status="completed", fixture_status=completed["status"])
            # The cacheable producer deliberately exits zero on a completed
            # assertion failure. Only this newly written report owns that status.
            return int(completed["status"])
        except (OSError, ValueError, KeyError, TypeError) as error:
            receipt.update(status="error", error=str(error))
            raise native.BazelWorkspaceError(f"fresh lint fixtures failed; evidence: {attempt}: {error}") from error
        except BaseException as error:
            receipt.update(status="interrupted" if isinstance(error, KeyboardInterrupt) else "error", error=str(error))
            raise
        finally:
            native.write_json(attempt / "receipt.json", receipt)


def _lint_unique_pairs(pairs: Sequence[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate lint action evidence key: {key}")
        result[key] = value
    return result


def _lint_action(
    document: Any,
    *,
    checksum: str,
    execution_root: Path,
    directory: Path,
    tree: Path,
    host: str,
    verification_outputs: set[str],
) -> LintFixtureAction:
    """Join only the actual AobusLintFixtures action and expand declared params."""
    if not isinstance(document, dict) or not re.fullmatch(r"[0-9a-f]{64}", checksum):
        raise ValueError("lint action requires configured aquery evidence and a full checksum")
    targets = _lint_unique_pairs([(str(row["id"]), row["label"]) for row in document.get("targets", [])])
    configurations = _lint_unique_pairs([(str(row["id"]), row) for row in document.get("configuration", [])])
    fragments = _lint_unique_pairs([(str(row["id"]), row) for row in document.get("pathFragments", [])])
    artifacts = _lint_unique_pairs([(str(row["id"]), row) for row in document.get("artifacts", [])])
    depsets = _lint_unique_pairs([(str(row["id"]), row) for row in document.get("depSetOfFiles", [])])
    active_fragments: set[str] = set()
    active_depsets: set[str] = set()

    @cache
    def fragment(identifier: str) -> str:
        if identifier in active_fragments:
            raise ValueError("recursive lint action path fragment")
        active_fragments.add(identifier)
        try:
            row = fragments[identifier]
            name = str(_relative(row["label"]))
            parent = row.get("parentId")
            return (fragment(str(parent)) + "/" if parent else "") + name
        finally:
            active_fragments.remove(identifier)

    def artifact(identifier: object) -> str:
        return fragment(str(artifacts[str(identifier)]["pathFragmentId"]))

    @cache
    def inputs(identifier: str) -> frozenset[str]:
        if identifier in active_depsets:
            raise ValueError("recursive lint action input depset")
        active_depsets.add(identifier)
        try:
            row = depsets[identifier]
            direct = frozenset(artifact(key) for key in row.get("directArtifactIds", []))
            return direct.union(*(inputs(str(key)) for key in row.get("transitiveDepSetIds", [])))
        finally:
            active_depsets.remove(identifier)

    actions = document.get("actions")
    if not isinstance(actions, list) or len(actions) != 1:
        raise ValueError("expected exactly one configured AobusLintFixtures action")
    action = actions[0]
    configuration = configurations[str(action["configurationId"])]
    if (
        action.get("mnemonic") != "AobusLintFixtures"
        or _label(targets[str(action["targetId"])]) != "//test/lint:lint_test"
        or configuration.get("checksum") != checksum
        or configuration.get("isTool", False) is not False
        or not isinstance(action.get("actionKey"), str)
        or not action["actionKey"]
        or any(ord(char) < 32 or ord(char) == 127 for char in action["actionKey"])
    ):
        raise ValueError("lint action target/configuration/key does not match the selected fixture producer")
    params = _lint_unique_pairs([(row["execPath"], row["arguments"]) for row in action.get("paramFiles", [])])

    def expand(arguments: object, active: tuple[str, ...] = ()) -> list[str]:
        if not isinstance(arguments, list) or any(
            not isinstance(arg, str) or not arg or any(ord(char) < 32 or ord(char) == 127 for char in arg)
            for arg in arguments
        ):
            raise ValueError("lint action arguments must be nonempty strings without control characters")
        result = []
        for arg in arguments:
            if arg.startswith("@"):
                path = arg[1:]
                _relative(path)
                if path not in params or path in active:
                    raise ValueError("opaque or recursive lint action response file: " + arg)
                result.extend(expand(params[path], (*active, path)))
            else:
                result.append(arg)
        return result

    argv = expand(action.get("arguments"))
    helper = "script/ao/core/bazel_lint_fixture.py"
    if (
        len(argv) < 7
        or Path(argv[0]) != Path(sys.executable).absolute()
        or argv[1:7] != ["-I", "-S", "-B", "-X", "utf8", helper]
        or not Path(argv[0]).is_file()
    ):
        raise ValueError("lint action must use the selected Python and exact isolated fixture helper prefix")
    options = argv[7:]
    names = ["policy", "sdk", "verification", "tidy"]
    if host != "windows":
        names.append("plugin")
    names.extend(["report", "log", "work", "jobs"])
    if len(options) != len(names) * 2 or options[::2] != ["--" + name for name in names] or options[-1] != "2":
        raise ValueError("lint action differs from the exact fixture producer options/two-worker contract")
    roles = dict(zip(names, options[1::2], strict=True))
    declared_inputs: frozenset[str] = frozenset().union(*(inputs(str(key)) for key in action.get("inputDepSetIds", [])))
    outputs = [artifact(key) for key in action.get("outputIds", [])]
    expected_outputs = [str(_relative(roles[name])) for name in ("report", "log", "work")]
    if len(set(expected_outputs)) != 3 or len(outputs) != 3 or set(outputs) != set(expected_outputs):
        raise ValueError("lint action outputs do not bind exactly to report/log/work roles")
    if set(outputs) & declared_inputs:
        raise ValueError("lint action outputs alias declared inputs")
    for path in expected_outputs:
        _output_path(path, execution_root, tree / "state/output")
    if verification_outputs != {roles["verification"]}:
        raise ValueError("fixture verification input is not the configured //tool/lint:verification output")
    required = {
        helper,
        *(str(_relative(roles[name])) for name in names if name not in {"report", "log", "work", "jobs"}),
    }
    if not required <= declared_inputs:
        raise ValueError("lint action helper/tool/policy/SDK/verification roles are not declared inputs")
    environment = _lint_unique_pairs(
        [(row["key"], row.get("value", "")) for row in action.get("environmentVariables", [])]
    )
    if (
        not environment
        or any(
            not isinstance(key, str)
            or not key
            or "=" in key
            or "\x00" in key
            or not isinstance(value, str)
            or "\x00" in value
            for key, value in environment.items()
        )
        or (host == "windows" and len({key.casefold() for key in environment}) != len(environment))
    ):
        raise ValueError("malformed or ambiguous lint action environment")
    paths = tuple(execution_root.joinpath(*_relative(path).parts) for path in sorted(declared_inputs))
    for input_path in paths:
        if not input_path.exists() or not (input_path.is_file() or input_path.is_dir()):
            raise ValueError(f"missing declared lint fixture input: {input_path}; build verification first")
    return LintFixtureAction(
        checksum, action["actionKey"], tuple(argv), tuple(environment.items()), paths, execution_root, directory, host
    )


@contextmanager
def prepare_lint_fixtures(
    args: argparse.Namespace,
    *,
    no_build: bool = False,
) -> Generator[LintFixtureAction, None, None]:
    """Build only verification prerequisites; every manual fixture run is fresh."""
    config, host = validate_request(args)
    tree = bazel_workspace.workspace_path(getattr(args, "path", None), config)
    with build_tree_lock(tree):
        native.require_supported_source()
        invocation = _Invocation(args, tree, config)
        try:
            from .bazel_test_ownership import extract_configured_targets

            identities = extract_configured_targets(
                json.loads(
                    invocation.invoke(
                        "cquery",
                        ["//test/lint:lint_test", "--output=jsonproto"],
                    ),
                    object_pairs_hook=_lint_unique_pairs,
                )
            )
            if len(identities) != 1 or next(iter(identities)).label != "//test/lint:lint_test":
                raise ValueError("fixture producer requires one exact configured target")
            checksum = next(iter(identities)).configuration
            document = json.loads(
                invocation.invoke(
                    "aquery",
                    [
                        'mnemonic("AobusLintFixtures", //test/lint:lint_test)',
                        "--output=jsonproto",
                        "--include_artifacts=true",
                        "--include_commandline=true",
                        "--include_param_files=true",
                    ],
                ),
                object_pairs_hook=_lint_unique_pairs,
            )
            verification = invocation.query(
                "//tool/lint:verification",
                "json.encode({'label': str(target.label), "
                "'files': [f.path for f in providers(target)['DefaultInfo'].files.to_list()]})",
            )
            if len(verification) != 1 or _label(verification[0].get("label")) != "//tool/lint:verification":
                raise ValueError("missing/ambiguous configured lint verification producer")
            verification_outputs = {str(_relative(path)) for path in verification[0]["files"]}
            if not no_build:
                invocation.invoke("build", ["//tool/lint:verification"], capture=False)
            execution_root = Path(invocation.invoke("info", ["execution_root"]).strip())
            if not execution_root.is_absolute() or not execution_root.resolve().is_relative_to(
                (tree / "state/output").resolve()
            ):
                raise ValueError("lint execution_root is not inside the selected output base")
            selected = _lint_action(
                document,
                checksum=checksum,
                execution_root=execution_root,
                directory=invocation.directory,
                tree=tree,
                host=host,
                verification_outputs=verification_outputs,
            )
            native.write_json(
                invocation.directory / "lint-action.json",
                {
                    "schema": 1,
                    "target": "//test/lint:lint_test",
                    "configuration": checksum,
                    "action_key": selected.action_key,
                    "argv": selected.argv,
                    "environment": dict(selected.environment),
                    "execution_root": str(execution_root),
                    "inputs": [str(path) for path in selected.inputs],
                    "verification_built": not no_build,
                    "assertions": "not yet run",
                },
            )
            if invocation.runtime is not None:
                invocation.runtime.verify()
            yield selected
            if invocation.runtime is not None:
                invocation.runtime.verify()
        except (OSError, ValueError, KeyError, TypeError, RecursionError) as error:
            raise native.BazelWorkspaceError(
                f"cannot admit configured lint fixture action; logs: {invocation.directory}: {error}"
            ) from error


@contextmanager
def prepare_artifacts(
    args: argparse.Namespace,
    labels: Sequence[str],
    *,
    no_build: bool = False,
    expand_tests: bool = False,
    compiler_identity: bool = False,
) -> Generator[tuple[ExecutableArtifact, ...], None, None]:
    """Build/resolve exact configured outputs, retaining the tree lock through use."""
    config, _host = validate_request(args)
    requested = tuple(_label(label) for label in labels)
    if not requested or len(set(requested)) != len(requested):
        raise native.BazelWorkspaceError("executable preparation requires unique explicit labels")
    tree = bazel_workspace.workspace_path(getattr(args, "path", None), config)
    with build_tree_lock(tree):
        native.require_supported_source()
        invocation = _Invocation(args, tree, config)
        expression = "set(" + " ".join(requested) + ")"
        memberships: dict[str, list[str]] = {}
        compatibility: list[dict[str, Any]]
        if expand_tests:
            compatibility = []
            for root in requested:
                group_rows = invocation.query(f"tests({root})", _COMPATIBILITY_EXPRESSION)
                group_labels = set()
                for row in group_rows:
                    label = _label(row.get("label"))
                    if label in group_labels:
                        raise native.BazelWorkspaceError(f"ambiguous configured test identity in {root}: {label}")
                    group_labels.add(label)
                    memberships.setdefault(label, []).append(root)
                    if label not in {_label(existing.get("label")) for existing in compatibility}:
                        compatibility.append(row)
                    elif next(existing for existing in compatibility if _label(existing.get("label")) == label) != row:
                        raise native.BazelWorkspaceError(f"conflicting configured test compatibility: {label}")
        else:
            compatibility = invocation.query(expression, _COMPATIBILITY_EXPRESSION)
        selected = []
        observed = set()
        for row in compatibility:
            label = _label(row.get("label"))
            if label in observed or type(row.get("incompatible")) is not bool:
                raise native.BazelWorkspaceError("ambiguous configured executable/compatibility selection")
            observed.add(label)
            if row["incompatible"]:
                if not expand_tests:
                    raise native.BazelWorkspaceError(f"{label} is incompatible with the selected native configuration")
            else:
                selected.append(label)
        if not expand_tests and observed != set(requested):
            raise native.BazelWorkspaceError("configured query did not return the exact requested executable labels")
        if not selected:
            raise native.BazelWorkspaceError("no compatible executable targets in selected native scope")
        selected.sort()
        scope = "set(" + " ".join(selected) + ")"
        from .bazel_test_ownership import extract_configured_targets

        identities = extract_configured_targets(json.loads(invocation.invoke("cquery", [scope, "--output=jsonproto"])))
        checksums = {}
        for identity in identities:
            if identity.label in checksums:
                raise native.BazelWorkspaceError(f"multiple configurations selected for executable {identity.label}")
            checksums[identity.label] = identity.configuration
        if set(checksums) != set(selected):
            raise native.BazelWorkspaceError("configured executable identity query disagrees with selection")
        rows = invocation.query(scope, _PROVIDER_EXPRESSION)
        providers = {}
        for row in rows:
            label = _label(row.get("label"))
            if label in providers:
                raise native.BazelWorkspaceError(f"duplicate configured executable provider: {label}")
            providers[label] = row
        if set(providers) != set(selected):
            raise native.BazelWorkspaceError("configured executable providers disagree with selected identities")
        if not no_build:
            invocation.invoke("build", selected, capture=False)
        root_text = invocation.invoke("info", ["execution_root"]).strip()
        execution_root = Path(root_text)
        if not execution_root.is_absolute() or not execution_root.resolve().is_relative_to(
            (tree / "state/output").resolve()
        ):
            raise native.BazelWorkspaceError("Bazel execution_root is not inside the selected output base")
        compiler = invocation.compiler_identity(selected, execution_root) if compiler_identity else "unverified"
        if compiler_identity and no_build:
            compiler = "unverified existing artifact; currently configured compiler: " + compiler
        artifacts = tuple(
            parse_artifact(
                providers[label],
                target=label,
                checksum=checksums[label],
                config=config,
                tree=tree,
                execution_root=execution_root,
                windows=_host == "windows",
            )
            for label in selected
        )
        artifacts = tuple(
            replace(
                artifact,
                compiler_identity=compiler,
                requested_by=tuple(memberships.get(artifact.target, (artifact.target,))),
                runtime_environment=tuple(invocation.runtime.environment.items())
                if invocation.runtime is not None
                else (),
            )
            for artifact in artifacts
        )
        native.write_json(
            invocation.directory / "artifacts.json",
            {
                "schema": 1,
                "source_revision": "unverified",
                "artifact_origin": "existing; build skipped"
                if no_build
                else "built by this invocation (not a source snapshot)",
                "artifacts": [artifact.provenance() for artifact in artifacts],
            },
        )
        if invocation.runtime is not None:
            invocation.runtime.verify()
        yield artifacts
        if invocation.runtime is not None:
            invocation.runtime.verify()
