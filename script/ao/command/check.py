"""ao check — complete local and CI validation over the native Bazel graph."""

import argparse
import copy

from ..core import bazel_check, bazel_sanitizers, bazel_workflows, bazel_workspace, tooltest
from ..core import bazel_native as native
from ..core.buildlock import build_tree_lock
from ..core.proc import die
from . import docs, hygiene

NAME = "check"
HELP = "Validate documentation, tooling, native products and tests, then scoped hygiene"
REQUIRES_BUILD_ENV = True
REQUIRES_PYTHON_TOOLS = True


def register(subparsers: argparse._SubParsersAction[argparse.ArgumentParser]) -> None:
    parser = subparsers.add_parser(NAME, help=HELP, description=HELP)
    bazel_workflows.add_arguments(parser)
    parser.add_argument("--clean", action="store_true", help="clean the selected Bazel outputs before validation")
    parser.add_argument(
        "--fresh",
        action="store_true",
        help="require new normal and sanitizer test executions instead of cached results",
    )
    parser.add_argument("--commit", metavar="<rev>", help="hygiene scope: changed files since <rev> plus local changes")
    parser.set_defaults(func=run_command)


def requires_build_environment(args: argparse.Namespace) -> bool:
    config, _host = bazel_workflows.validate_request(args)
    if config == "profile":
        raise die("sampling is a build/performance profile; use bazel build --config=profile or ao perf")
    return True


def run_command(args: argparse.Namespace) -> int:
    try:
        config, host = bazel_workflows.validate_request(args)
        if config == "profile":
            raise native.BazelWorkspaceError(
                "sampling is a build/performance profile; use bazel build --config=profile or ao perf"
            )
        selected_tree = bazel_workspace.workspace_path(args.path, config)
        if config in bazel_sanitizers.SANITIZER_CONFIGS:
            # The instrumented subset stays dbg-based; the uninstrumented normal
            # completion keeps an explicitly requested Debug flavor and otherwise
            # the ordinary fastbuild default, never a relabeled configuration.
            normal_config = "debug" if args.flavor == "debug" else "fastbuild"
        else:
            normal_config = config
        with build_tree_lock(selected_tree):
            native.require_supported_source()
            selected_tree.mkdir(parents=True, exist_ok=True)
            print("=== Documentation integrity ===")
            if (status := docs.run_check(args)) != 0:
                return status
            if host != "macos":
                print("=== Repository tooling suite ===")
                tooling_log = selected_tree / "tooling.log"
                tooling_log.write_text("", encoding="utf-8")
                if (status := tooltest.run(log=tooling_log, jobs=args.jobs)) != 0:
                    return status
            if args.clean:
                prepared = bazel_workspace.prepare_workspace(
                    selected_tree,
                    normal_config,
                    prefix=args.vcpkg_prefix,
                    graph_options=bazel_workflows.feature_options(args),
                )
                status = bazel_workspace.execute(
                    bazel_workspace.bazel_executable(),
                    selected_tree,
                    prepared,
                    "clean",
                    normal_config,
                    [],
                    isolate_rc=True,
                )
                (selected_tree / "clean.log").write_bytes((selected_tree / "build.log").read_bytes())
                if status:
                    return status
            normal = copy.copy(args)
            normal.path = str(selected_tree)
            normal.config = normal_config
            normal.asan = normal.tsan = False
            # The normal completion remains independent of the optional
            # instrumented test scope, and both use this one mode-neutral tree.
            if config in bazel_sanitizers.SANITIZER_CONFIGS:
                print(f"=== Whole-project {normal_config.title()} completion before the independent sanitizer gate ===")
            if (status := bazel_check.run(normal)) != 0:
                return status
            if config in bazel_sanitizers.SANITIZER_CONFIGS:
                instrumented = copy.copy(args)
                instrumented.path = str(selected_tree)
                instrumented.config = config
                instrumented.targets = []
                print(f"=== Independent {config} execution ===")
                if (status := bazel_sanitizers.run(instrumented)) != 0:
                    return status
            print("=== Source hygiene ===")
            scoped = copy.copy(normal)
            scoped.files = []
            scoped.all = False
            scoped.folder = []
            return hygiene.run_command(scoped)
    except (native.BazelWorkspaceError, OSError, ValueError) as error:
        raise die(str(error)) from error
