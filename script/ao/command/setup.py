"""ao setup — perform explicit, scoped host setup actions."""

import argparse
import os

from ..core import compiler_cache, gitfiles, winui
from ..core.paths import PROJECT_ROOT
from ..core.proc import die, run

HELP = "Perform an explicit, scoped development-host setup action"
NAME = "setup"
REQUIRES_BUILD_ENV = False


def register(subparsers: "argparse._SubParsersAction[argparse.ArgumentParser]") -> None:
    parser = subparsers.add_parser(NAME, help=HELP, description=HELP)
    parser.add_argument(
        "component",
        choices=("compiler-cache", "git-hooks", "winui-runtime"),
        help="host component to install",
    )
    shared_workspaces = parser.add_mutually_exclusive_group()
    shared_workspaces.add_argument(
        "--shared-workspaces",
        action="store_true",
        default=None,
        help="enable safe compiler-cache reuse across supported workspace layouts",
    )
    shared_workspaces.add_argument(
        "--no-shared-workspaces",
        action="store_false",
        dest="shared_workspaces",
        help="disable compiler-cache reuse across workspaces",
    )
    parser.set_defaults(func=run_command)


def run_command(args: argparse.Namespace) -> int:
    if args.component != "compiler-cache" and args.shared_workspaces is not None:
        raise die("--shared-workspaces options apply only to setup compiler-cache")
    if args.component == "git-hooks":
        status = run(gitfiles._git_command("config", "--local", "core.hooksPath", "script/git-hook"), cwd=PROJECT_ROOT)
        if status == 0:
            print("Git hooks configured for this repository: script/git-hook")
        return status
    if args.component == "compiler-cache":
        overrides = [key for key in ("CCACHE_DIR", "CCACHE_MAXSIZE") if os.environ.get(key)]
        try:
            updates = compiler_cache.setup_local(shared_workspaces=args.shared_workspaces)
        except compiler_cache.CompilerCacheError as exc:
            raise die(str(exc)) from exc
        print(
            "Compiler cache configured: "
            f"{updates.get('CMAKE_CXX_COMPILER_LAUNCHER') or os.environ['CMAKE_CXX_COMPILER_LAUNCHER']}"
        )
        print(f"Cache directory: {updates.get('CCACHE_DIR') or os.environ['CCACHE_DIR']}")
        print(f"Cache capacity: {updates.get('CCACHE_MAXSIZE') or os.environ['CCACHE_MAXSIZE']}")
        enabled = updates.get(compiler_cache.SHARED_WORKSPACES_EFFECTIVE) == "1"
        print(f"Shared workspaces (current shell): {'enabled' if enabled else 'disabled'}")
        if fallback := updates.get(compiler_cache.SHARED_WORKSPACES_FALLBACK):
            print(f"Notice: {fallback}.")
        for key in overrides:
            print(f"Notice: using {key} override from environment")
        return 0
    try:
        installed = winui.setup_runtime()
    except RuntimeError as exc:
        raise die(str(exc)) from exc
    print(f"Windows App Runtime {installed.version} {installed.architecture} is ready.")
    return 0
