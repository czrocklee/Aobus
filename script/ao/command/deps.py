"""ao deps — inspect and verify governed native dependency resolutions."""

import argparse
import json
from pathlib import Path

from ..core import builddir, dependency_policy
from ..core.proc import die

HELP = "Report or verify the governed dependency resolution for a configured build"
NAME = "deps"
REQUIRES_BUILD_ENV = True


def register(subparsers: "argparse._SubParsersAction[argparse.ArgumentParser]") -> None:
    parser = subparsers.add_parser(NAME, help=HELP, description=HELP)
    actions = parser.add_subparsers(dest="deps_action", metavar="<action>", required=True)

    report = actions.add_parser("report", help="verify and print the dependency report")
    report.add_argument("-p", "--path", metavar="<dir>", help="configured build directory")
    report.add_argument("--json", type=Path, metavar="<path>", help="write the complete report as JSON")
    report.set_defaults(func=run_report)

    verify = actions.add_parser("verify", help="fail unless the dependency report is current and valid")
    verify.add_argument("-p", "--path", metavar="<dir>", help="configured build directory")
    verify.set_defaults(func=run_verify)


def _build_dir(args: argparse.Namespace) -> Path:
    return Path(args.path) if args.path else builddir.build_dir("debug")


def _verified(args: argparse.Namespace) -> dict[str, object]:
    try:
        return dependency_policy.verified_report(_build_dir(args))
    except dependency_policy.DependencyPolicyError as exc:
        raise die(str(exc)) from exc


def run_report(args: argparse.Namespace) -> int:
    report = _verified(args)
    print(dependency_policy.format_summary(report))
    if args.json:
        try:
            args.json.parent.mkdir(parents=True, exist_ok=True)
            args.json.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        except OSError as exc:
            raise die(f"cannot write dependency report to {args.json}: {exc}") from exc
        print(f"Full report: {args.json}")
    return 0


def run_verify(args: argparse.Namespace) -> int:
    report = _verified(args)
    host = report["host"]
    assert isinstance(host, dict)
    print(f"Dependency report verified for {host['platform']}: {_build_dir(args)}")
    return 0
