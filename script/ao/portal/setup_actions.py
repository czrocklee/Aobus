"""Explicit, scoped provisioning through each component's existing authority."""

from ao.core import bazel_native
from ao.core.paths import PROJECT_ROOT
from ao.core.proc import die, run
from ao.policy import windows_llvm_sdk
from ao.portal import bazel_provision, bazel_workspace, build_dir, buildifier, git_files, prettier, winui

COMPONENTS = ("bazel", "deps", "llvm-sdk", "winui-runtime", "buildifier", "prettier", "git-hooks")


def _setup_llvm_sdk() -> int:
    if build_dir.platform_profile().name != "windows":
        raise die("setup llvm-sdk provisions the pinned Windows LLVM SDK; it is defined only on native Windows hosts.")
    try:
        root = bazel_provision.install_llvm_sdk()
        manifest = windows_llvm_sdk.read_manifest(windows_llvm_sdk.MANIFEST)
    except (bazel_native.BazelWorkspaceError, OSError, ValueError) as error:
        raise die(str(error)) from error
    print(f"Pinned LLVM SDK {manifest.version} ready at {root}.")
    return 0


def setup(component: str, *, path: str | None = None, config: str = "fastbuild") -> int:
    if component not in COMPONENTS:
        raise bazel_native.BazelWorkspaceError(f"unsupported setup component: {component!r}")
    if component != "deps" and (path is not None or config != "fastbuild"):
        raise bazel_native.BazelWorkspaceError("only setup deps accepts path/config selection")
    if component == "deps":
        if config not in {"debug", "fastbuild", "release"}:
            raise bazel_native.BazelWorkspaceError("dependency provisioning requires debug, fastbuild or release")
        bazel_provision.provision(bazel_workspace.workspace_path(path, config))
        return 0
    if component == "bazel":
        try:
            print(f"Bazel ready: {bazel_workspace.bazel_executable(setup=True)}")
        except (bazel_native.BazelWorkspaceError, OSError, ValueError) as error:
            raise die(str(error)) from error
        return 0
    if component == "git-hooks":
        status = run(git_files.git_command("config", "--local", "core.hooksPath", "script/git-hook"), cwd=PROJECT_ROOT)
        if status == 0:
            print("Git hooks configured for this repository: script/git-hook")
        return status
    if component == "llvm-sdk":
        return _setup_llvm_sdk()
    if component == "buildifier":
        try:
            tool = buildifier.executable()
        except buildifier.BuildifierError as error:
            raise die(str(error)) from error
        print(f"Pinned Buildifier ready at {tool}.")
        return 0
    if component == "prettier":
        try:
            command = prettier.command()
        except prettier.PrettierError as error:
            raise die(str(error)) from error
        print(f"Pinned Prettier ready: {' '.join(command)}")
        return 0
    try:
        installed = winui.setup_runtime()
    except RuntimeError as error:
        raise die(str(error)) from error
    print(f"Windows App Runtime {installed.version} {installed.architecture} is ready.")
    return 0
