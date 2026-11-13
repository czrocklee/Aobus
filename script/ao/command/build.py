"""ao build — build native products with the governed Bazel toolchains."""

import argparse
from pathlib import Path

from ..core import bazel_native as native
from ..core import bazel_workflows, bazel_workspace
from ..core.buildlock import build_tree_lock
from ..core.proc import die

NAME = "build"
HELP = "Build native products and tests with Bazel (without running them)"
REQUIRES_BUILD_ENV = True

# Existing public product names remain conveniences, not another source graph.
_PRODUCT_TARGETS = {
    "aobus": "//app/cli:aobus",
    "aobus-tui": "//app/tui:aobus-tui",
    "aobus-gtk": "//app/linux-gtk:aobus-gtk",
    "aobus-appkit": "//app/macos-appkit:aobus-appkit",
    "aobus-winui": "//app/windows-winui:aobus-winui",
    "winui": "//app/windows-winui:aobus-winui",
    "ao_perf_baseline": "//test:ao_perf_baseline",
    "ao_tui_bench": "//test:ao_tui_bench",
}
_DEFAULT_TARGETS = ("//:check_products", "//:check_tests", "//:check_native_tests")


def add_build_arguments(parser: argparse.ArgumentParser, *, default_flavor: str = "debug") -> None:
    bazel_workflows.add_arguments(parser, default_flavor=default_flavor)
    parser.add_argument("--clean", action="store_true", help="clean Bazel outputs first; retain provisioned SDKs")
    parser.add_argument("--verbose", action="store_true", help="show executed native command lines")


def requires_build_environment(args: argparse.Namespace) -> bool:
    bazel_workflows.validate_request(args)
    return True


def register(subparsers: "argparse._SubParsersAction[argparse.ArgumentParser]") -> None:
    parser = subparsers.add_parser(NAME, help=HELP, description=HELP)
    add_build_arguments(parser)
    parser.add_argument(
        "--target", action="append", default=[], help="Bazel label or existing product name (repeatable)"
    )
    parser.add_argument("--event-log", type=Path, help="write Bazel build events as JSON to this file")
    parser.add_argument(
        "--msbuild-capture",
        metavar="<dir>",
        help="retired CMake/MSBuild capture option; rejected rather than silently ignored",
    )
    parser.set_defaults(func=run_command)


def _targets(values: list[str]) -> list[str]:
    if not values or values == ["all"]:
        return list(_DEFAULT_TARGETS)
    selected = []
    for value in values:
        if value in _PRODUCT_TARGETS:
            selected.append(_PRODUCT_TARGETS[value])
        elif value.startswith("//") and ":" in value and not any(character.isspace() for character in value):
            selected.append(value)
        else:
            raise native.BazelWorkspaceError(
                f"unknown native target {value!r}; use a Bazel //package:target label or a supported product name. "
                "CMake-only targets are retired; ao check owns native completion probes."
            )
    return list(dict.fromkeys(selected))


def run_command(args: argparse.Namespace) -> int:
    try:
        config, _host = bazel_workflows.validate_request(args)
        if args.msbuild_capture is not None:
            raise native.BazelWorkspaceError(
                "--msbuild-capture belongs to the retired CMake product graph; "
                "use Bazel action queries and the selected tree's build.log. No private capture was created."
            )
        targets = _targets(args.target)
        options = ["--lockfile_mode=error", *bazel_workflows.feature_options(args)]
        if event_log := getattr(args, "event_log", None):
            destination = Path(event_log).absolute()
            destination.parent.mkdir(parents=True, exist_ok=True)
            options.append(f"--build_event_json_file={destination}")
        if args.verbose:
            options.append("--subcommands")
        tree = bazel_workspace.workspace_path(args.path, config)
        with build_tree_lock(tree):
            native.require_supported_source()
            tree.mkdir(parents=True, exist_ok=True)
            prepared = bazel_workspace.prepare_workspace(tree, config, prefix=args.vcpkg_prefix)
            executable = bazel_workspace.bazel_executable()
            if args.clean:
                status = bazel_workspace.execute(executable, tree, prepared, "clean", config, [], isolate_rc=True)
                if status:
                    return status
                # Preserve the explicit clean's output before the build owns build.log.
                (tree / "clean.log").write_bytes((tree / "build.log").read_bytes())
            return bazel_workspace.execute(
                executable,
                tree,
                prepared,
                "build",
                config,
                [*targets, *options],
                jobs=args.jobs,
                remote_cache=args.remote_cache,
                isolate_rc=True,
            )
    except (native.BazelWorkspaceError, OSError, ValueError) as error:
        raise die(str(error)) from error
