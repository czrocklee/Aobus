"""Governed Bazel executable setup and portal launch configuration.

Repository probes own installed SDK discovery; Starlark owns their build graph.
This launcher selects output/cache roots and preserves argv, terminal and logs.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
import urllib.request
from pathlib import Path
from typing import Any
from urllib.parse import urlparse

from . import bazel_environment, builddir
from . import bazel_native as native
from . import bazel_providers as providers

TOOLCHAIN = native.PROJECT_ROOT / "script/ao/bazel-toolchain.json"
# The root BUILD owns the default suite aggregate; subsystem suites stay explicit conveniences.
DEFAULT_TEST_TARGETS = ("//:tests",)
# Client values the generated launch rc pins as --repo_env. This is the portal's
# role; bazel/environment.bzl owns the selectors a bare client's repository rule
# reads and bazel_environment.ENVIRONMENT_KEYS what its helper publishes.
# test_bazel_environment_rules pins how the three lists differ.
PINNED_REPO_ENV = (
    "PATH",
    "PATHEXT",
    "SYSTEMROOT",
    "SYSTEMDRIVE",
    "COMSPEC",
    "PROCESSOR_ARCHITECTURE",
    "PROCESSOR_ARCHITEW6432",
    "USERPROFILE",
    "HOMEDRIVE",
    "HOMEPATH",
    "HOME",
    "LOCALAPPDATA",
    "APPDATA",
    "AOBUS_STATE_ROOT",
    "XDG_CACHE_HOME",
    "WINDIR",
    "AOBUS_NIX_DEPENDENCY_REPORT",
    "AOBUS_NUGET_PACKAGES_DIR",
    "PKG_CONFIG_PATH",
    "PKG_CONFIG_LIBDIR",
    "AOBUS_LLVM_ROOT",
    "AOBUS_LLVM_SDK_ROOT",
    "AOBUS_LLVM_SDK_CACHE_ROOT",
    "AOBUS_LINT_LLVM_CONFIG",
    "AOBUS_LINT_CLANG_TIDY",
    "AOBUS_LINT_CLANG",
    "AOBUS_LINT_CLANGXX",
    "AOBUS_LINT_XCRUN",
    "AOBUS_LINT_CLANG_ROOTS",
    "VSINSTALLDIR",
    "AOBUS_LIBCXX_EXPECTED_SHIM",
    "DEVELOPER_DIR",
    "SDKROOT",
    "VCToolsVersion",
    "VCToolsInstallDir",
    "WindowsSdkDir",
    "WindowsSDKVersion",
    "UCRTVersion",
    "BAZEL_VC",
    "BAZEL_VC_FULL_VERSION",
    "BAZEL_SH",
)


def validate_remote_cache(value: str) -> str:
    """Admit an explicitly selected trusted Bazel action-cache transport."""
    if not isinstance(value, str) or urlparse(value).scheme not in {"http", "https", "grpc", "grpcs"}:
        raise native.BazelWorkspaceError("remote cache requires an http(s) or grpc(s) URL")
    return value


def workspace_path(path: str | None, config: str) -> Path:
    selected = Path(path or os.environ.get("BUILD_DIR") or builddir.platform_profile().build_root / f"bazel-{config}")
    selected = selected.resolve()
    source_root = native.PROJECT_ROOT.resolve()
    if selected.is_relative_to(source_root) or source_root.is_relative_to(selected):
        raise native.BazelWorkspaceError("Bazel --path must be outside and must not contain the source checkout")
    if (selected / "CMakeCache.txt").exists():
        raise native.BazelWorkspaceError(f"{selected} is a CMake tree; select a separate Bazel --path")
    return selected


def _tool_version(executable: Path, expected: str) -> None:
    result = subprocess.run(
        [str(executable), "--version"], env=native.child_environment(), capture_output=True, text=True, check=False
    )
    accepted = {f"bazel {expected}"}
    if executable.resolve().is_relative_to("/nix/store"):
        # Nixpkgs' source build has no release Git metadata, but retains the exact upstream version.
        accepted.add(f"bazel {expected}- (@non-git)")
    if result.returncode or result.stdout.strip() not in accepted:
        raise native.BazelWorkspaceError(f"{executable} must report bazel {expected}; run ao setup bazel")


def _artifact_key() -> str:
    system = {"linux": "linux", "windows": "windows", "macos": "darwin"}[native.native_platform()]
    machine = native.host_machine().lower()
    machine = {"amd64": "x86_64", "aarch64": "arm64"}.get(machine, machine)
    return f"{system}-{machine}"


def _managed_tool(lock: dict[str, Any]) -> Path:
    if override := os.environ.get("AOBUS_STATE_ROOT"):
        state = Path(override)
    elif os.name == "nt":
        state = builddir.windows_state_root()
    else:
        state = Path.home() / "Library/Caches/Aobus"
    suffix = ".exe" if os.name == "nt" else ""
    return state / "tools/bazel" / str(lock["version"]) / f"bazel{suffix}"


def _publish_tool(download: Path, executable: Path, expected_sha256: str) -> None:
    descriptor, name = tempfile.mkstemp(prefix=".bazel-stage-", dir=executable.parent)
    os.close(descriptor)
    staged = Path(name)
    try:
        shutil.copyfile(download, staged)
        if native.sha256(staged) != expected_sha256:
            raise native.BazelWorkspaceError("staged Bazel SHA-256 does not match bazel-toolchain.json")
        staged.chmod(0o755)
        if executable.is_file():
            if native.sha256(executable) != expected_sha256:
                raise native.BazelWorkspaceError(f"managed Bazel SHA-256 mismatch: {executable}")
            return
        try:
            os.replace(staged, executable)
        except OSError:
            # Another setup may have published and started the same executable;
            # Windows cannot replace a running binary. Reuse only verified bytes.
            if not executable.is_file() or native.sha256(executable) != expected_sha256:
                raise
    finally:
        staged.unlink(missing_ok=True)


def bazel_executable(*, setup: bool = False) -> Path:
    lock = native.read_json(TOOLCHAIN)
    if lock.get("schemaVersion") != 1 or (native.PROJECT_ROOT / ".bazelversion").read_text().strip() != lock["version"]:
        raise native.BazelWorkspaceError("Bazel pin files disagree")
    if override := os.environ.get("AOBUS_BAZEL"):
        executable = Path(override).absolute()
    elif native.native_platform() == "linux":
        found = shutil.which("bazel")
        if not found or not Path(found).resolve().is_relative_to("/nix/store"):
            raise native.BazelWorkspaceError("Bazel is missing from the pinned Nix shell; re-enter through ./ao")
        executable = Path(found).absolute()
    else:
        executable = _managed_tool(lock)
        artifact = lock["artifacts"].get(_artifact_key())
        if artifact is None:
            raise native.BazelWorkspaceError(f"no pinned Bazel artifact for {_artifact_key()}")
        if not executable.is_file():
            if not setup:
                raise native.BazelWorkspaceError("managed Bazel is missing; run ao setup bazel")
            executable.parent.mkdir(parents=True, exist_ok=True)
            with tempfile.TemporaryDirectory(prefix="aobus-bazel-") as temporary:
                download = Path(temporary) / "bazel"
                urllib.request.urlretrieve(artifact["url"], download)
                if native.sha256(download) != artifact["sha256"]:
                    raise native.BazelWorkspaceError("downloaded Bazel SHA-256 does not match bazel-toolchain.json")
                _publish_tool(download, executable, artifact["sha256"])
        if native.sha256(executable) != artifact["sha256"]:
            raise native.BazelWorkspaceError(f"managed Bazel SHA-256 mismatch: {executable}")
    _tool_version(executable, lock["version"])
    return executable


def _git_bash() -> Path | None:
    """Return the MSYS bash that Git for Windows installs beside its git executable."""
    if git := shutil.which("git"):
        for root in Path(git).resolve().parents[:3]:
            if (bash := root / "usr/bin/bash.exe").is_file():
                return bash
    return None


def prepare_workspace(
    tree: Path,
    config: str,
    *,
    prefix: str | None = None,
    lint_configuration: Path | None = None,
    graph_options: tuple[str, ...] = (),
) -> Path:
    """Write launch configuration, not a dependency snapshot or freshness stamp."""
    allowed_graph_options = {
        f"--//bazel:{name}={value}" for name in ("gtk_enabled", "system_media_enabled") for value in ("true", "false")
    }
    if any(value not in allowed_graph_options for value in graph_options):
        raise native.BazelWorkspaceError("unsupported product graph setting")
    if len({value.partition("=")[0] for value in graph_options}) != len(graph_options):
        raise native.BazelWorkspaceError("duplicate product graph setting")
    host = native.native_platform()
    prepared = tree / "configuration"
    prepared.mkdir(parents=True, exist_ok=True)
    request = {
        "schema": 1,
        "host": host,
        "prefix": None if host == "linux" else str(providers.vcpkg_prefix(prefix, tree)),
        # The WinUI quality probe needs the original checkout, not Bazel's execution root or a compiler view.
        "source": native.source_root(),
    }
    environment = native.child_environment()
    # os.environ is case-insensitive on Windows, but dict(os.environ) is not:
    # enumeration uppercases SDK/toolset names such as WindowsSdkDir.
    lookup = {key.casefold() if host == "windows" else key: value for key, value in environment.items()}
    compiler_selection: dict[str, str] = {}
    if host == "windows":
        # Capture the original selected environment before the repository rule
        # replaces resolver scratch TMP. The watched request is the only channel.
        request["compiler_environment"] = {
            name: lookup.get(name.casefold(), "") for name in bazel_environment.WINDOWS_COMPILER_CAPTURE_KEYS
        }
        try:
            compiler_selection = bazel_environment.windows_compiler_selection(environment)
        except bazel_environment.EnvironmentError as error:
            raise native.BazelWorkspaceError(str(error)) from error
    native.write_json(prepared / "request.json", request)
    selected = {
        "AOBUS_BAZEL_CONFIGURATION": (prepared / "request.json").as_posix(),
        "AOBUS_BAZEL_PYTHON": Path(sys.executable).absolute().as_posix(),
    }
    for name in PINNED_REPO_ENV:
        key = name.casefold() if host == "windows" else name
        if key in lookup:
            selected[name] = lookup[key]
    selected.update(compiler_selection)
    # Bazel's Windows shell toolchain needs a bash, which the checkout's Git
    # for Windows provides unless the caller selects another one.
    if host == "windows" and "BAZEL_SH" not in selected and (bash := _git_bash()):
        selected["BAZEL_SH"] = str(bash)
    if host == "windows" and "AOBUS_NUGET_PACKAGES_DIR" not in selected:
        from .bazel_provision import restored_nuget_packages

        # An explicit selection wins; otherwise use the managed cache once it is completely restored.
        if packages := restored_nuget_packages(environment):
            selected["AOBUS_NUGET_PACKAGES_DIR"] = str(packages)
    # This metadata selector is for ready SDK discovery, never compiler flags.
    # child_environment intentionally keeps it out of action execution.
    if roots := os.environ.get("NIXPKGS_CMAKE_PREFIX_PATH"):
        selected["NIXPKGS_CMAKE_PREFIX_PATH"] = roots
    if lint_configuration is not None:
        selected["AOBUS_BAZEL_LINT_CONFIGURATION"] = lint_configuration.absolute().as_posix()
    state = (
        Path(os.environ["AOBUS_STATE_ROOT"])
        if os.environ.get("AOBUS_STATE_ROOT")
        else (
            builddir.windows_state_root()
            if host == "windows"
            else Path.home() / "Library/Caches/Aobus"
            if host == "macos"
            else Path(os.environ.get("XDG_CACHE_HOME", str(Path.home() / ".cache"))) / "Aobus"
        )
    )
    cache = state / "cache/bazel"
    # Downloads use Bazel's default content-addressed repository cache under the
    # shared output root; the repository rc keeps the contents cache disabled.
    lines = [
        "# Generated launch configuration; repository rules own dependency refresh.",
        "startup " + json.dumps("--output_user_root=" + (state / "bazel").as_posix(), ensure_ascii=False),
        "startup " + json.dumps("--output_base=" + (tree / "state/output").as_posix(), ensure_ascii=False),
        "startup --max_idle_secs=900",
        "common --experimental_install_base_gc_max_age=0",
        "build " + json.dumps("--config=" + config, ensure_ascii=False),
        "build " + json.dumps("--symlink_prefix=" + tree.as_posix() + "/bazel-", ensure_ascii=False),
        "build " + json.dumps("--disk_cache=" + cache.as_posix(), ensure_ascii=False),
        "build --experimental_disk_cache_gc_max_size=50G",
    ]
    lines.extend("build " + value for value in graph_options)
    lines.extend(
        "common " + json.dumps("--repo_env=" + key + "=" + value, ensure_ascii=False)
        for key, value in sorted(selected.items())
    )
    content = ("\n".join(lines) + "\n").encode("utf-8")
    rc = prepared / "aobus.bazelrc"
    if not rc.is_file() or rc.read_bytes() != content:
        rc.write_bytes(content)
    return prepared


def _linux_server_javabase() -> Path:
    selected = os.environ.get("AOBUS_BAZEL_SERVER_JAVABASE", "")
    if not selected:
        raise native.BazelWorkspaceError("Bazel completion requires the selected Nix JDK; re-enter through ./ao")
    path = Path(selected)
    if not path.is_absolute() or not path.resolve().is_relative_to("/nix/store") or not (path / "bin/java").is_file():
        raise native.BazelWorkspaceError("Bazel completion server JDK is unavailable in the selected Nix store")
    return path


def invocation(executable: Path, generated: Path, *, isolate_rc: bool = False) -> list[str]:
    """Select the launch rc files without allowing personal completion filters."""
    argv = [str(executable)]
    if isolate_rc:
        argv.extend(
            [
                "--nosystem_rc",
                "--nohome_rc",
                "--noworkspace_rc",
                f"--bazelrc={native.PROJECT_ROOT / '.bazelrc'}",
            ]
        )
        if native.native_platform() == "linux":
            argv.append(f"--server_javabase={_linux_server_javabase()}")
    return [*argv, f"--bazelrc={generated / 'aobus.bazelrc'}"]


def execute(
    executable: Path,
    tree: Path,
    generated: Path,
    action: str,
    config: str,
    targets: list[str],
    *,
    jobs: int = 2,
    remote_cache: str | None = None,
    isolate_rc: bool = False,
) -> int:
    native.require_supported_source()
    environment = native.child_environment(isolate_rc=isolate_rc)
    if action not in {"build", "clean"}:
        raise native.BazelWorkspaceError(f"unsupported portal build action: {action}")
    argv = [*invocation(executable, generated, isolate_rc=isolate_rc), action]
    if action == "clean":
        if targets:
            raise native.BazelWorkspaceError("governed clean does not accept expunge or output-root overrides")
    else:
        if not targets:
            raise native.BazelWorkspaceError("Bazel build requires explicit targets")
        argv.append(f"--jobs={jobs}")
        if remote_cache:
            argv.append(f"--remote_cache={remote_cache}")
        argv.extend(targets)
    log = tree / "build.log"
    print(f"Bazel {action} ({config}); output: {log}", flush=True)
    with log.open("wb") as sink:
        result = subprocess.run(argv, cwd=native.PROJECT_ROOT, env=environment, stdout=sink, stderr=subprocess.STDOUT)
    status = result.returncode
    if status:
        print(log.read_text(encoding="utf-8", errors="replace"))
    return status
