"""ao verify — analyze governed native compile contracts without execution."""

import argparse
import subprocess
import sys

from ..core import bazel_contract, bazel_native, bazel_sanitizers, bazel_workflows, bazel_workspace, jobs
from ..core.buildlock import build_tree_lock
from ..core.proc import die

NAME = "verify"
HELP = "Analyze native compile contracts without building or running tests"
REQUIRES_BUILD_ENV = True


class _Configuration(argparse.Action):
    def __call__(
        self,
        parser: argparse.ArgumentParser,
        namespace: argparse.Namespace,
        values: object,
        option_string: str | None = None,
    ) -> None:
        if getattr(namespace, "_config_seen", False):
            parser.error("--config may be supplied only once; choose one build profile")
        namespace._config_seen = True
        setattr(namespace, self.dest, values)


def register(subparsers: "argparse._SubParsersAction[argparse.ArgumentParser]") -> None:
    parser = subparsers.add_parser(NAME, help=HELP, description=HELP)
    parser.add_argument("--config", choices=bazel_workflows.CONFIGURATIONS, default="debug", action=_Configuration)
    parser.add_argument("-p", "--path", help="external Bazel tree (honors BUILD_DIR)")
    parser.add_argument("--vcpkg-prefix", help="existing governed native SDK prefix")
    parser.add_argument("-j", "--jobs", type=bazel_workflows.positive_jobs, default=jobs.default_workers())
    parser.set_defaults(func=run_command)


def requires_build_environment(args: argparse.Namespace) -> bool:
    bazel_workflows.validate_request(args)
    return True


def run_command(args: argparse.Namespace) -> int:
    try:
        config, host = bazel_workflows.validate_request(args)
        tree = bazel_workspace.workspace_path(args.path, config)
        with build_tree_lock(tree):
            bazel_native.require_supported_source()
            tree.mkdir(parents=True, exist_ok=True)
            prepared = bazel_workspace.prepare_workspace(tree, config, prefix=args.vcpkg_prefix)
            print(f"Prepared Bazel configuration: {prepared}", file=sys.stderr)
            # A sanitizer request analyzes its Debug compile contracts. The
            # managed sanitizer gate, not this command, proves instrumentation.
            return bazel_contract.verify(
                bazel_workspace.bazel_executable(),
                tree,
                prepared,
                "debug" if config in bazel_sanitizers.SANITIZER_CONFIGS else config,
                jobs=args.jobs,
                compiler_include_roots=bazel_sanitizers.compiler_include_roots(config, host),
            )
    except (bazel_native.BazelWorkspaceError, OSError, subprocess.SubprocessError, ValueError, KeyError) as error:
        raise die(str(error)) from error
