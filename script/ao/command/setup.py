"""ao setup — perform explicit, scoped host setup actions."""

import argparse

from ..core import gitfiles, winui
from ..core.paths import PROJECT_ROOT
from ..core.proc import die, run

HELP = "Perform an explicit, scoped development-host setup action"
NAME = "setup"
REQUIRES_BUILD_ENV = False


def register(subparsers: "argparse._SubParsersAction[argparse.ArgumentParser]") -> None:
    parser = subparsers.add_parser(NAME, help=HELP, description=HELP)
    parser.add_argument("component", choices=("git-hooks", "winui-runtime"), help="host component to install")
    parser.set_defaults(func=run_command)


def run_command(args: argparse.Namespace) -> int:
    if args.component == "git-hooks":
        status = run(gitfiles._git_command("config", "--local", "core.hooksPath", "script/git-hook"), cwd=PROJECT_ROOT)
        if status == 0:
            print("Git hooks configured for this repository: script/git-hook")
        return status
    try:
        installed = winui.setup_runtime()
    except RuntimeError as exc:
        raise die(str(exc)) from exc
    print(f"Windows App Runtime {installed.version} {installed.architecture} is ready.")
    return 0
