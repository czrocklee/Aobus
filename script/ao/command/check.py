"""ao check — complete native Bazel validation, with optional sanitizer execution."""

import argparse
import copy

from ..core import bazel_check, bazel_sanitizers, bazel_workflows, bazel_workspace, builddir
from ..core import bazel_native as native
from ..core.proc import die
from . import build

NAME = "check"
HELP = "Gate whole-project audits, test ownership, products and native tests"
REQUIRES_BUILD_ENV = True


def register(subparsers: "argparse._SubParsersAction[argparse.ArgumentParser]") -> None:
    parser = subparsers.add_parser(NAME, help=HELP, description=HELP)
    build.add_build_arguments(parser)
    parser.add_argument(
        "--fresh",
        action="store_true",
        help="disable test-result caching: normal and sanitizer tests must execute anew",
    )
    parser.set_defaults(func=run_command, targets=[])


def requires_build_environment(args: argparse.Namespace) -> bool:
    config, _host = bazel_workflows.validate_request(args)
    if config == "profile":
        raise die("sampling profile is a build/performance workflow; use ao build profile or ao perf")
    return True


def requires_python_tools(args: argparse.Namespace) -> bool:
    requires_build_environment(args)
    return builddir.platform_profile().name != "macos"


REQUIRES_PYTHON_TOOLS = requires_python_tools


def run_command(args: argparse.Namespace) -> int:
    try:
        config, _host = bazel_workflows.validate_request(args)
        if config == "profile":
            raise native.BazelWorkspaceError(
                "sampling profile is a build/performance workflow; use ao build profile or ao perf"
            )
        selected_tree = bazel_workspace.workspace_path(args.path, config)
        if args.clean:
            clean_args = copy.copy(args)
            clean_args.target = []
            clean_args.msbuild_capture = None
            # The build command owns clean safety and preserves its log. Do not
            # select a second tree or remove an installed SDK to clean outputs.
            clean_args.path = str(selected_tree)
            clean_args.target = ["//bazel:compile_contract"]
            if (status := build.run_command(clean_args)) != 0:
                return status
        normal = copy.copy(args)
        normal.path = str(selected_tree)
        normal.config = "debug" if config in bazel_sanitizers.SANITIZER_CONFIGS else config
        # Sanitizer profiles deliberately exclude whole-project ownership and
        # manual harnesses. First run the real normal completion gate, then
        # independently prove and execute the requested instrumented profile.
        # Both configurations use the explicitly selected mode-neutral Bazel
        # tree; Debug evidence is never relabelled sanitizer evidence.
        if config in bazel_sanitizers.SANITIZER_CONFIGS:
            print("=== Whole-project Debug completion before the independent sanitizer gate ===")
        if (status := bazel_check.run(normal)) != 0:
            return status
        if builddir.platform_profile().name != "macos":
            from ..core import tooltest

            print("=== Repository tooling suite ===")
            if (status := tooltest.run()) != 0:
                return status
        if config not in bazel_sanitizers.SANITIZER_CONFIGS:
            return 0
        instrumented = copy.copy(args)
        instrumented.path = str(selected_tree)
        instrumented.config = config
        instrumented.targets = []
        print(f"=== Independent {config} execution ===")
        return bazel_sanitizers.run(instrumented)
    except (native.BazelWorkspaceError, OSError, ValueError) as error:
        raise die(str(error)) from error
