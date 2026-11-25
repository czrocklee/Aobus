"""The single validation command, with explicit partial-check subjects."""

import argparse
import subprocess

from ao.core import bazel_native, jobs
from ao.core.proc import die
from ao.policy import tidy_config
from ao.portal import (
    bazel_workflows,
    completion,
    contract_checks,
    dependency_checks,
    documentation_checks,
    host_checks,
    measurements,
    source_analyzer,
    source_format,
    source_lint,
)

from . import options

NAME = "check"
HELP = "Validate the complete native graph, or one explicitly selected subject"
REQUIRES_BUILD_ENV = True


class _ContractConfiguration(argparse.Action):
    def __call__(
        self,
        parser: argparse.ArgumentParser,
        namespace: argparse.Namespace,
        values: object,
        option_string: str | None = None,
    ) -> None:
        if getattr(namespace, "_contract_config_seen", False):
            parser.error("--config may be supplied only once; choose one contract configuration")
        namespace._contract_config_seen = True
        setattr(namespace, self.dest, values)


def _analysis_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--tidy-check", help="select only this native checker")
    parser.add_argument("--tidy-arg", action="append", default=[], type=tidy_config.diagnostic_argument)
    parser.add_argument("--debug", action="store_true", help="show analysis evidence and configuration")
    parser.add_argument("-o", "--output", help="write diagnostics to this path")
    parser.add_argument(
        "--report-only", action="store_true", help="report findings without failing; tool failures still fail"
    )


def register(subparsers: argparse._SubParsersAction[argparse.ArgumentParser]) -> None:
    parser = subparsers.add_parser(NAME, help=HELP, description=HELP)
    bazel_workflows.add_arguments(parser)
    parser.add_argument("--clean", action="store_true", help="clean the selected outputs before complete validation")
    parser.add_argument("--fresh", action="store_true", help="disable Bazel test-result caching")
    parser.add_argument(
        "--ci", action="store_true", help="validate the native CI partition, including Windows desktop tests"
    )
    parser.set_defaults(func=run_command, check_action=None)
    checks = parser.add_subparsers(dest="check_action", metavar="<subject>")

    repository = checks.add_parser(
        "repository", help="validate the repository CI partition on Linux", allow_abbrev=False
    )
    repository.add_argument("--path", help="external Bazel tree (honors BUILD_DIR)")
    repository.add_argument("--jobs", type=bazel_workflows.positive_jobs, default=jobs.default_workers())
    formatting = checks.add_parser("format", help="verify formatting without rewriting sources")
    options.add_source_arguments(formatting)
    formatting.add_argument("--docs", action="store_true", help="select the global documentation Markdown inventory")
    lint = checks.add_parser("lint", help="check source rules without formatting or global documentation checks")
    options.add_source_arguments(lint)
    bazel_workflows.add_arguments(lint)
    _analysis_arguments(lint)
    lint.add_argument("--target", action="append", default=[], dest="owner_targets", help="native compile owner")
    lint.add_argument("--header-filter", help="cache-keyed diagnostic header filter")
    analyzer = checks.add_parser("analyzer", help="run path-sensitive native analysis")
    options.add_source_arguments(analyzer)
    _analysis_arguments(analyzer)
    analyzer.add_argument("-p", "--path", help="external native analysis tree (honors BUILD_DIR)")
    analyzer.add_argument("-j", "--jobs", type=bazel_workflows.positive_jobs, default=jobs.default_workers())
    analyzer.add_argument("--alpha", action="store_true", help="include experimental analyzer checks")
    analyzer.add_argument("--include-external-diagnostics", action="store_true")
    docs = checks.add_parser("docs", help="validate global documentation links, identities and semantic references")
    contracts = checks.add_parser("contracts", help="analyze native compile contracts without product execution")
    contracts.add_argument(
        "--config", choices=bazel_workflows.CONFIGURATIONS, default="fastbuild", action=_ContractConfiguration
    )
    contracts.add_argument("-p", "--path", help="external native analysis tree (honors BUILD_DIR)")
    contracts.add_argument("--vcpkg-prefix", help="existing governed native SDK prefix")
    contracts.add_argument("--compiler", choices=("gcc", "clang"))
    contracts.add_argument("-j", "--jobs", type=bazel_workflows.positive_jobs, default=jobs.default_workers())
    dependencies = checks.add_parser("deps", help="validate current governed SDK admission and input digests")
    options.add_dependency_arguments(dependencies)
    winui = checks.add_parser("winui", help="check WinUI development prerequisites without repairing them")
    winui.add_argument("--build-only", action="store_true", help="omit runtime launch prerequisites")
    instrumentation = checks.add_parser(
        "instrumentation", help="analyze coverage instrumentation without collecting it"
    )
    options.add_coverage_arguments(instrumentation, measurement=False)
    for check in (repository, formatting, lint, analyzer, docs, contracts, dependencies, winui, instrumentation):
        check.set_defaults(func=run_command)


def validate_syntax(args: argparse.Namespace, arguments: list[str], parser: argparse.ArgumentParser) -> None:
    """Keep complete-check flags out of partial checks before any preflight work."""
    if args.check_action is not None and (len(arguments) < 2 or arguments[1] != args.check_action):
        parser.error(
            "put subject-specific options after 'check <subject>'; complete-check options require bare 'check'"
        )


def _completion_request(args: argparse.Namespace) -> completion.Request:
    return completion.Request(**options.native_values(args), ci=args.ci, fresh=args.fresh, clean=args.clean)


def _lint_request(args: argparse.Namespace) -> source_lint.LintRequest:
    return source_lint.LintRequest(
        **options.native_values(args),
        tidy_check=args.tidy_check,
        tidy_args=tuple(args.tidy_arg),
        owner_targets=tuple(args.owner_targets),
        header_filter=args.header_filter,
        output=args.output,
        debug=args.debug,
        report_only=args.report_only,
    )


def _analyzer_request(args: argparse.Namespace) -> source_analyzer.AnalyzerRequest:
    return source_analyzer.AnalyzerRequest(
        path=args.path,
        jobs=args.jobs,
        tidy_check=args.tidy_check,
        tidy_args=tuple(args.tidy_arg),
        output=args.output,
        debug=args.debug,
        alpha=args.alpha,
        include_external_diagnostics=args.include_external_diagnostics,
        report_only=args.report_only,
    )


def _dependency_request(args: argparse.Namespace) -> dependency_checks.Request:
    return dependency_checks.Request(config=args.config, path=args.path, vcpkg_prefix=args.vcpkg_prefix, jobs=args.jobs)


def _instrumentation_request(args: argparse.Namespace) -> measurements.CoverageRequest:
    return measurements.CoverageRequest(
        path=args.path,
        jobs=args.jobs,
        compiler=args.compiler,
        coverage_suite=args.suite,
        remote_cache=args.remote_cache,
        analysis_only=True,
    )


def _owner_only(args: argparse.Namespace) -> bool:
    return bool(args.owner_targets and not (args.paths or args.all or args.commit is not None))


def _admit(args: argparse.Namespace) -> None:
    action = args.check_action
    if action is None:
        completion.validate(_completion_request(args))
    elif action == "repository":
        completion.validate_repository()
    elif action == "lint":
        source_lint.validate(_lint_request(args))
        options.selection(args)
    elif action == "analyzer":
        source_analyzer.validate(_analyzer_request(args))
        options.selection(args)
    elif action == "contracts":
        bazel_workflows.validate_request(bazel_workflows.Request(**options.native_values(args)))
    elif action == "deps":
        dependency_checks.validate_request(_dependency_request(args))
    elif action == "instrumentation":
        measurements.validate_coverage_request(_instrumentation_request(args))
    elif action == "format":
        selected = options.selection(args)
        if args.docs and (selected.paths or selected.all or selected.commit is not None):
            raise ValueError("--docs cannot be combined with paths, --all or --commit")


def requires_build_environment(args: argparse.Namespace) -> bool:
    try:
        _admit(args)
        action = args.check_action
        if action in ("docs", "winui"):
            return False
        if action == "format":
            # The documentation inventory contains only Markdown and needs no native formatter.
            return not args.docs and options.needs_formatter_environment(options.resolved_scope(args))
        if action == "lint":
            return _owner_only(args) or options.has_native_sources(options.resolved_scope(args))
        if action == "analyzer":
            return options.has_native_sources(options.resolved_scope(args, analyzer=True))
        return True
    except (bazel_native.BazelWorkspaceError, OSError, ValueError) as error:
        raise die(str(error)) from error


def requires_python_tools(args: argparse.Namespace) -> bool:
    try:
        _admit(args)
        action = args.check_action
        # Full and repository checks use the graph's Python tools, not the portal's.
        if action == "docs":
            return True
        if action == "format":
            return not args.docs and options.has_python_sources(options.resolved_scope(args))
        if action == "lint":
            return not (args.tidy_check or _owner_only(args)) and options.has_python_sources(
                options.resolved_scope(args)
            )
        return False
    except (bazel_native.BazelWorkspaceError, OSError, ValueError) as error:
        raise die(str(error)) from error


REQUIRES_PYTHON_TOOLS = requires_python_tools


def run_command(args: argparse.Namespace) -> int:
    try:
        _admit(args)
        action = args.check_action
        if action is None:
            return completion.run(_completion_request(args))
        if action == "repository":
            return completion.run_repository(path=args.path, jobs=args.jobs)
        if action == "format":
            return source_format.check(options.resolved_scope(args))
        if action == "lint":
            scope = None if _owner_only(args) else options.resolved_scope(args)
            return source_lint.run(scope, _lint_request(args))
        if action == "analyzer":
            return source_analyzer.run(options.resolved_scope(args, analyzer=True), _analyzer_request(args))
        if action == "docs":
            return documentation_checks.check()
        if action == "contracts":
            return contract_checks.check(bazel_workflows.Request(**options.native_values(args)))
        if action == "deps":
            return dependency_checks.check(_dependency_request(args), json_output=args.json)
        if action == "winui":
            return host_checks.check_winui(build_only=args.build_only)
        return measurements.check_instrumentation(_instrumentation_request(args))
    except (bazel_native.BazelWorkspaceError, OSError, ValueError, subprocess.SubprocessError, KeyError) as error:
        raise die(str(error)) from error
