"""ao setup — perform explicit, scoped host setup actions."""

import argparse

from ..core import (
    bazel_native,
    bazel_provision,
    bazel_workspace,
    builddir,
    buildifier,
    gitfiles,
    windows_llvm_sdk,
    winui,
)
from ..core.paths import PROJECT_ROOT
from ..core.proc import die, run

HELP = "Perform an explicit, scoped development-host setup action"
NAME = "setup"
REQUIRES_BUILD_ENV = False


def requires_build_environment(args: argparse.Namespace) -> bool:
    """Explicit tool setup never configures a product or requires a VS generator."""
    return False


def register(subparsers: "argparse._SubParsersAction[argparse.ArgumentParser]") -> None:
    parser = subparsers.add_parser(NAME, help=HELP, description=HELP)
    parser.add_argument(
        "component",
        choices=("bazel", "buildifier", "git-hooks", "llvm-sdk", "winui-runtime"),
        help="host component to install",
    )
    parser.set_defaults(func=run_command)


def _setup_llvm_sdk() -> int:
    """Explicitly provision the pinned SDK through its existing native owner."""
    if builddir.platform_profile().name != "windows":
        raise die("setup llvm-sdk provisions the pinned Windows LLVM SDK; it is defined only on native Windows hosts.")
    root = bazel_provision.install_llvm_sdk()
    manifest = windows_llvm_sdk.read_manifest(windows_llvm_sdk.MANIFEST)
    print(f"Pinned LLVM SDK {manifest.version} ready at {root}.")
    return 0


def run_command(args: argparse.Namespace) -> int:
    if args.component == "bazel":
        try:
            print(f"Bazel ready: {bazel_workspace.bazel_executable(setup=True)}")
        except (bazel_native.BazelWorkspaceError, OSError, ValueError) as error:
            raise die(str(error)) from error
        return 0
    if args.component == "git-hooks":
        status = run(gitfiles.git_command("config", "--local", "core.hooksPath", "script/git-hook"), cwd=PROJECT_ROOT)
        if status == 0:
            print("Git hooks configured for this repository: script/git-hook")
        return status
    if args.component == "llvm-sdk":
        return _setup_llvm_sdk()
    if args.component == "buildifier":
        try:
            tool = buildifier.executable()
        except buildifier.BuildifierError as exc:
            raise die(str(exc)) from exc
        print(f"Pinned Buildifier ready at {tool}.")
        return 0
    try:
        installed = winui.setup_runtime()
    except RuntimeError as exc:
        raise die(str(exc)) from exc
    print(f"Windows App Runtime {installed.version} {installed.architecture} is ready.")
    return 0
