"""ao bazel — bounded, CMake-free native development builds."""

import argparse
import os
import re
import subprocess
import sys
from contextlib import AbstractContextManager, nullcontext
from urllib.parse import urlparse

from ..core import bazel_native as native
from ..core import bazel_workspace, dependency_policy
from ..core.buildlock import build_tree_lock
from ..core.proc import die

NAME = "bazel"
HELP = "Provision, build, test, verify and lint native targets with Bazel and governed Nix/vcpkg dependencies"
REQUIRES_BUILD_ENV = False


def requires_build_environment(args: argparse.Namespace) -> bool:
    return bool(args.bazel_action != "setup")


def _jobs(value: str) -> int:
    count = int(value)
    if count < 1:
        raise argparse.ArgumentTypeError("jobs must be positive")
    return count


def _remote_cache(value: str) -> str:
    if urlparse(value).scheme not in {"http", "https", "grpc", "grpcs"}:
        raise argparse.ArgumentTypeError("remote cache requires an http(s) or grpc(s) URL")
    return value


# CI maps its remote-cache helper's output to this in each Bazel step.
REMOTE_CACHE_ENVIRONMENT = "AOBUS_BAZEL_REMOTE_CACHE"


def _add_remote_cache_argument(parser: argparse.ArgumentParser, help_text: str) -> None:
    parser.add_argument(
        "--remote-cache",
        type=_remote_cache,
        default=os.environ.get(REMOTE_CACHE_ENVIRONMENT) or None,
        help=f"{help_text} (default: ${REMOTE_CACHE_ENVIRONMENT})",
    )


def validate_forwarded_arguments(arguments: list[str]) -> None:
    """Keep invocation options from replacing the prepared provider/toolchain contract."""
    reserved = {
        # Startup options belong to the generated rc and the managed Bazel server.
        "autodetect_server_javabase",
        "batch",
        "batch_cpu_scheduling",
        "bazelrc",
        "block_for_lock",
        "client_debug",
        "connect_timeout_secs",
        "digest_function",
        "failure_detail_out",
        "home_rc",
        "host_jvm_args",
        "host_jvm_debug",
        "idle_server_tasks",
        "ignore_all_rc_files",
        "install_base",
        "io_nice_level",
        "local_startup_timeout_secs",
        "macos_qos_class",
        "max_idle_secs",
        "oom_more_eagerly",
        "oom_more_eagerly_threshold",
        "output_base",
        "output_user_root",
        "preemptible",
        "server_javabase",
        "server_jvm_out",
        "shutdown_on_low_sys_mem",
        "system_rc",
        "unix_digest_hash_attribute_name",
        "watchfs",
        "windows_enable_symlinks",
        "workspace_rc",
        "write_command_log",
        # These options can replace governed configuration or hide further overrides.
        "action_env",
        "host_action_env",
        "repo_env",
        "config",
        "compilation_mode",
        "override_repository",
        "inject_repository",
        "override_module",
        "registry",
        "flagfile",
        "flag_alias",
        "invocation_policy",
        "platforms",
        "host_platform",
        "platform_mappings",
        "extra_execution_platforms",
        "extra_toolchains",
        "crosstool_top",
        "host_crosstool_top",
        "compiler",
        "cpu",
        "host_cpu",
        "symlink_prefix",
        "disk_cache",
        "experimental_disk_cache_gc_max_size",
        "enable_platform_specific_config",
        "incompatible_strict_action_env",
        "experimental_strict_repo_env",
        "enable_bzlmod",
        "enable_workspace",
        "repo_contents_cache",
        "repository_contents_cache",
        "repository_cache",
        "experimental_repository_cache",
        "experimental_install_base_gc_max_age",
        "run_validations",
        "experimental_run_validations",
        "experimental_use_validation_aspect",
    }
    compiler_options = {
        "copt",
        "cxxopt",
        "conlyopt",
        "host_copt",
        "host_cxxopt",
        "host_conlyopt",
        "linkopt",
        "host_linkopt",
        "per_file_copt",
        "per_file_ltobackendopt",
        "define",
        "features",
        "host_features",
    }
    protected_definition = re.compile(
        r"(?<![A-Za-z0-9_])(?:[-/][dDuU])?"
        r"(?:gsl_CONFIG_\w+|GSL_\w+|_ITERATOR_DEBUG_LEVEL|_SECURE_SCL|_HAS_ITERATOR_DEBUGGING|"
        r"_DEBUG|NDEBUG|_DLL|_MT|_GLIBCXX_DEBUG(?:_PEDANTIC)?|_LIBCPP_HARDENING_MODE)"
        r"(?:[=,\s]|$)"
    )
    protected_runtime = re.compile(
        r"(?:^|[\s,@])[-/](?:M[DT]d?|(?:NO)?DEFAULTLIB(?::[^\s,]+)?)(?:$|[\s,])|"
        r"-fms-runtime-lib(?:=|$)|msvcrt",
        re.IGNORECASE,
    )
    skip_value = False
    for index, argument in enumerate(arguments):
        if skip_value:
            skip_value = False
            continue
        # A second separator belongs to `bazel run`, not to Bazel's option parser.
        if argument == "--":
            break
        if not argument.startswith("-"):
            continue
        option, separator, value = argument.partition("=")
        name = option.removeprefix("--").replace("-", "_")
        if (
            (option.startswith("-c") and not option.startswith("--"))
            or name in reserved
            or name.removeprefix("no") in reserved
            or option.startswith("--@aobus_")
        ):
            raise native.BazelWorkspaceError(f"Bazel option is managed by ao and cannot be forwarded: {option}")
        if name in compiler_options:
            if not separator:
                value = arguments[index + 1] if index + 1 < len(arguments) else ""
                skip_value = True
            if protected_definition.search(value) or protected_runtime.search(value):
                raise native.BazelWorkspaceError(
                    f"Bazel option overrides the governed CRT/GSL configuration: {argument}"
                )
        elif name == "test_arg" and not separator:
            skip_value = True


def _has_explicit_target(arguments: list[str]) -> bool:
    """Return whether a forwarded label remains after option values are skipped."""
    skip_value = False
    for argument in arguments:
        if skip_value:
            skip_value = False
            continue
        if argument == "--":
            break
        option, separator, _ = argument.partition("=")
        if not separator and option.removeprefix("--").replace("-", "_") in {"test_arg", "test_env"}:
            skip_value = True
            continue
        if argument.startswith(("//", ":", "@")):
            return True
    return False


def _without_test_env_overrides(arguments: list[str], environment: dict[str, str]) -> list[str]:
    """Remove joined or separate test_env assignments for keys the owned service supplies."""
    filtered: list[str] = []
    index = 0
    while index < len(arguments):
        argument = arguments[index]
        if argument == "--":
            filtered.extend(arguments[index:])
            break
        option, separator, value = argument.partition("=")
        if option.removeprefix("--").replace("-", "_") == "test_env":
            if not separator:
                value = arguments[index + 1] if index + 1 < len(arguments) else ""
                key = value.removeprefix("=").partition("=")[0]
                if key not in environment:
                    filtered.extend(arguments[index : index + 2])
                index += 2
                continue
            key = value.removeprefix("=").partition("=")[0]
            if key not in environment:
                filtered.append(argument)
            index += 1
            continue
        filtered.append(argument)
        index += 1
    return filtered


def register(subparsers: "argparse._SubParsersAction[argparse.ArgumentParser]") -> None:
    parser = subparsers.add_parser(NAME, help=HELP, description=HELP)
    actions = parser.add_subparsers(dest="bazel_action", required=True, metavar="<action>")
    setup = actions.add_parser("setup", help="verify Nix Bazel, or download the pinned native executable")
    setup.set_defaults(func=run)
    provision = actions.add_parser(
        "provision",
        help="install the native vcpkg manifest, and on Windows the WinUI NuGet closure and LLVM SDK, for a tree",
        epilog="Linux needs no provisioning: the pinned Nix shell supplies its dependencies.",
    )
    provision.add_argument("--config", choices=("debug", "release"), default="debug")
    provision.add_argument("-p", "--path", help="separate external Bazel build tree")
    provision.set_defaults(func=run)
    for action in ("deps", "build", "test", "query", "cquery", "aquery", "run", "verify"):
        command = actions.add_parser(
            action,
            help="fetch governed native repositories"
            if action == "deps"
            else "analyze native compile contracts without building or running tests"
            if action == "verify"
            else f"Bazel {action} with governed native imports",
            epilog=(
                "Pass ordinary Bazel options after '--', for example: "
                "./ao bazel test -- --test_filter='Decoder*' //test:media_test. "
                "Query expressions are accepted unchanged. For build/test without labels, use '--name=value' "
                "options to retain the default supported test binaries. Configuration, provider, startup and "
                "CRT/GSL overrides are managed by ao and cannot be forwarded."
            )
            if action not in {"deps", "verify"}
            else "Checks the native default graph; Linux also checks Nix driver/SPA policy. Retains analysis receipts."
            if action == "verify"
            else None,
        )
        command.add_argument("--config", choices=("debug", "release"), default="debug")
        command.add_argument("-p", "--path", help="separate external Bazel build tree")
        command.add_argument(
            "--vcpkg-prefix", help="installed/triplet prefix replacing the tree's provisioned vcpkg (native hosts)"
        )
        if action not in {"deps", "verify"}:
            command.add_argument(
                "targets",
                nargs="*",
                metavar="bazel-arg",
                help="Bazel targets or query expression; put forwarded Bazel options after '--'",
            )
            _add_remote_cache_argument(command, "opt in to a trusted remote action cache")
            if action == "test":
                command.add_argument(
                    "--headless",
                    action="store_true",
                    help="run explicit Linux GTK test labels under an owned Xvfb and private D-Bus",
                )
        if action != "deps":
            command.add_argument("-j", "--jobs", type=_jobs, default=2)
        command.set_defaults(func=run, targets=[], jobs=2)
        if action in {"deps", "verify"}:
            command.set_defaults(remote_cache=None)

    from ..core import tidyconfig, tidyengine

    lint = actions.add_parser("lint", help="cache native C++ analysis reports and gate the selected findings")
    tidyengine.add_scope_arguments(lint, "lint")
    lint.add_argument("--config", choices=("debug", "release"), default="debug")
    lint.add_argument(
        "--vcpkg-prefix", help="installed/triplet prefix replacing the tree's provisioned vcpkg (native hosts)"
    )
    lint.add_argument(
        "--target", action="append", default=[], dest="owner_targets", help="explicit native compile owner (repeatable)"
    )
    lint.add_argument("--header-filter", help="override the cache-keyed per-main diagnostic header filter")
    lint.add_argument(
        "--clang-tidy-arg",
        action="append",
        default=[],
        type=tidyconfig.diagnostic_argument,
        help="diagnostic-only clang-tidy option (repeatable)",
    )
    _add_remote_cache_argument(lint, "opt in to a trusted native action cache")
    lint.set_defaults(func=run, targets=[], jobs=2)

    hygiene = actions.add_parser(
        "hygiene",
        help="run the check-only commit gate with cached Bazel lint in place of CMake tidy",
        epilog="Selects files as ao hygiene does; Windows formats with the provisioned LLVM SDK.",
    )
    hygiene.add_argument("files", nargs="*", metavar="file", help="explicit files to check")
    hygiene.add_argument("--all", action="store_true", help="check every source in the project folders")
    hygiene.add_argument(
        "--folder", action="append", default=[], metavar="<dir>", help="all files under <dir> (repeatable)"
    )
    hygiene.add_argument("--commit", metavar="<rev>", help="changed files since <rev> + working tree + untracked")
    hygiene.add_argument("--config", choices=("debug", "release"), default="debug")
    hygiene.add_argument("-p", "--path", help="separate external Bazel build tree")
    hygiene.add_argument(
        "--vcpkg-prefix", help="installed/triplet prefix replacing the tree's provisioned vcpkg (native hosts)"
    )
    hygiene.add_argument("-j", "--jobs", type=_jobs, default=2)
    _add_remote_cache_argument(hygiene, "opt in to a trusted native action cache")
    hygiene.set_defaults(func=run, targets=[], jobs=2)


def _lint_args(args: argparse.Namespace, sources: list[str], explicit: bool) -> argparse.Namespace:
    """Parse lint's real defaults, then select the gate's tree and resolved C++ scope."""
    parser = argparse.ArgumentParser()
    register(parser.add_subparsers())
    lint_args = parser.parse_args([NAME, "lint"])
    for key in ("config", "path", "vcpkg_prefix", "jobs", "remote_cache"):
        setattr(lint_args, key, getattr(args, key))
    lint_args.files = list(sources) if explicit else []
    lint_args._resolved_sources = sources
    return lint_args


def _hygiene(args: argparse.Namespace) -> int:
    """Run ao hygiene's stages with Bazel lint in place of the CMake tidy tree."""
    from ..core import bazel_lint, compiler_cache, pythoncheck, pythonenv, windows_llvm_sdk
    from ..core.paths import PROJECT_ROOT
    from . import format as format_command
    from . import hygiene, tidy

    # Reuse hygiene's source scope contract and hand it back without another Git scan.
    files = format_command.resolve_files(args)
    args._resolved_sources = files
    cpp_files, python_files = tidy.split_existing(files)
    host = native.native_platform()
    clang_format = None
    if cpp_files and host == "windows":
        # A Bazel tree has no CMake lint tree; provisioning installs the same SDK.
        manifest = windows_llvm_sdk.read_manifest(windows_llvm_sdk.MANIFEST)
        clang_format = str(windows_llvm_sdk.ready_root(os.environ, manifest)[0] / "bin/clang-format.exe")
    python = None
    if python_files and host != "linux":
        # Bazel records this portal's base interpreter, so only Ruff and mypy run in
        # the managed tooling environment; the Nix shell's interpreter carries them.
        state = compiler_cache.state_root(system="Windows" if host == "windows" else "Darwin")
        try:
            python = pythonenv.ensure_environment(PROJECT_ROOT, state, platform_name=host)
        except RuntimeError as exc:
            raise native.BazelWorkspaceError(f"Aobus native Python tooling bootstrap failed: {exc}") from exc

    def lint(files: list[str], explicit: bool) -> int:
        cpp_files, python_files = tidy.split_existing(files)
        failed = False
        if python_files:
            print(f"=== Python hygiene — {len(python_files)} file(s) ===")
            failed = pythoncheck.run_paths(python_files, python=python) != 0
        if cpp_files and bazel_lint.run(_lint_args(args, cpp_files, explicit)) != 0:
            failed = True
        return 1 if failed else 0

    return hygiene.run_command(args, clang_format=clang_format, python=python, lint=("bazel lint", lint))


def run(args: argparse.Namespace) -> int:
    # Importing command parsers must not require dependency generator initialization.
    from ..core.bazel_deps import BazelDependencyError

    try:
        if args.bazel_action == "setup":
            print(f"Bazel ready: {bazel_workspace.bazel_executable(setup=True)}")
            return 0
        if args.bazel_action == "provision":
            from ..core import bazel_provision

            bazel_provision.provision(bazel_workspace.workspace_path(args.path, args.config))
            return 0
        if args.bazel_action == "lint":
            from ..core import bazel_lint

            return bazel_lint.run(args)
        if args.bazel_action == "hygiene":
            return _hygiene(args)
        validate_forwarded_arguments(args.targets)
        headless = args.bazel_action == "test" and getattr(args, "headless", False)
        if headless:
            if native.native_platform() != "linux":
                raise native.BazelWorkspaceError("Bazel test --headless currently supports native Linux only")
            # Do not let an unlabelled invocation fall through to the default common suite.
            if not _has_explicit_target(args.targets):
                raise native.BazelWorkspaceError("Bazel test --headless requires an explicit target or forwarded label")
        if args.bazel_action == "verify" and native.native_platform() not in {"linux", "macos", "windows"}:
            raise native.BazelWorkspaceError("Bazel compile-contract verify requires native Linux, macOS or Windows")
        tree = bazel_workspace.workspace_path(args.path, args.config)
        with build_tree_lock(tree):
            tree.mkdir(parents=True, exist_ok=True)
            prepared = bazel_workspace.prepare_workspace(tree, args.config, prefix=args.vcpkg_prefix)
            print(f"Prepared Bazel configuration: {prepared}", file=sys.stderr)
            if args.bazel_action == "verify":
                from ..core import bazel_contract

                return bazel_contract.verify(
                    bazel_workspace.bazel_executable(), tree, prepared, args.config, jobs=args.jobs
                )
            context: AbstractContextManager[list[dict[str, str]]]
            if headless:
                from . import test as test_command

                # The GTK suite owns the Xvfb/private-session-bus lifecycle.
                context = test_command.virtual_gtk_displays(1)
            else:
                context = nullcontext([dict[str, str]()])
            with context as environments:
                targets = list(args.targets)
                if headless:
                    # Test runtime environment is not a reserved toolchain channel; append it only after the
                    # user's forwarded arguments have been validated. Drop user assignments for owned keys
                    # so a personal display or session bus cannot replace the service context.
                    environment = environments[0]
                    targets = _without_test_env_overrides(targets, environment)
                    targets.extend(f"--test_env={key}={value}" for key, value in environment.items())
                return bazel_workspace.execute(
                    bazel_workspace.bazel_executable(),
                    tree,
                    prepared,
                    args.bazel_action,
                    args.config,
                    targets,
                    jobs=args.jobs,
                    remote_cache=args.remote_cache,
                )
    except (
        native.BazelWorkspaceError,
        BazelDependencyError,
        dependency_policy.DependencyPolicyError,
        OSError,
        subprocess.SubprocessError,
        ValueError,
        KeyError,
    ) as exc:
        raise die(str(exc)) from exc
