"""ao deps — inspect current governed Bazel SDK facts and concept metrics."""

import argparse
import json
import subprocess
import tempfile
from collections.abc import Callable
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from typing import Any

from ..core import bazel_native as native
from ..core import bazel_provision, bazel_workspace, dependency_policy, jobs
from ..core.buildlock import build_tree_lock
from ..core.paths import PROJECT_ROOT
from ..core.proc import die

NAME = "deps"
HELP = "Provision native dependencies, verify SDK admission, or measure public concepts"
REQUIRES_BUILD_ENV = True
_MANIFESTS = ("@aobus_native_deps//:manifest.json", "@aobus_test_deps//:manifest.json")
_MANIFEST_FILES = (
    "json.encode([{'owner': str(f.owner), 'path': f.path, 'source': f.is_source} "
    "for f in providers(target)['DefaultInfo'].files.to_list()])"
)


def REQUIRES_PROVIDER_TOOLS(args: argparse.Namespace) -> bool:
    return bool(args.deps_action == "provision")


def register(subparsers: "argparse._SubParsersAction[argparse.ArgumentParser]") -> None:
    parser = subparsers.add_parser(NAME, help=HELP, description=HELP)
    actions = parser.add_subparsers(dest="deps_action", required=True)
    provision = actions.add_parser("provision", help="explicitly install the governed native SDK for a tree")
    provision.add_argument("-p", "--path", help="external Bazel tree (honors BUILD_DIR)")
    provision.add_argument("--config", choices=("debug", "release"), default="debug")
    provision.set_defaults(func=run_command)
    for action in ("report", "verify"):
        command = actions.add_parser(action, help=f"{action} actual governed native SDK facts")
        command.add_argument("-p", "--path", help="external Bazel tree (honors BUILD_DIR)")
        command.add_argument("--config", choices=("debug", "release"), default="debug")
        command.add_argument("--vcpkg-prefix", help="existing governed native SDK prefix")
        command.add_argument("-j", "--jobs", type=int, default=jobs.default_workers())
        if action == "report":
            command.add_argument("--json", type=Path, help="write the complete native-facts report")
            command.add_argument(
                "--concepts",
                action="store_true",
                help="measure public-header AST and fresh compiler-observed include fan-out",
            )
        command.set_defaults(func=run_command)


def _verify_manifest(value: dict[str, Any], *, host: str, config: str, workers: int) -> dict[str, Any]:
    """Check native admission identity and every recorded input, never a freshness stamp."""
    configuration = value["configuration"]
    if configuration.get("platform") != host or configuration.get("config") != config.title():
        raise native.BazelWorkspaceError("dependency manifest configuration differs from the actual request")
    inputs = value["inputs"]

    def verify(entry: dict[str, Any]) -> None:
        path = Path(entry["path"])
        if not path.is_file() or native.sha256(path) != entry["sha256"]:
            raise native.BazelWorkspaceError(
                f"native dependency input changed or disappeared: {path}; query its governed repository again"
            )

    with ThreadPoolExecutor(max_workers=min(workers, len(inputs))) as pool:
        list(pool.map(verify, inputs))
    return value


def _manifest_file(
    target: str,
    config: str,
    workers: int,
    query: Callable[[str, list[str]], str],
) -> tuple[Path, dict[str, Any]]:
    """Resolve the selected repository source without a build-created execroot link."""
    (row,) = json.loads(
        query(
            "cquery",
            [
                target,
                "--output=starlark",
                f"--starlark:expr={_MANIFEST_FILES}",
                "--lockfile_mode=error",
                f"--jobs={workers}",
            ],
        )
    )
    # "<absolute path>:<line>:<column>: source file <label>"
    location = query("query", [f"'{row['owner']}'", "--output=location", "--lockfile_mode=error"]).strip()
    return Path(location.split(": source file ", 1)[0].rsplit(":", 2)[0]), row


def _verified(args: argparse.Namespace) -> dict[str, Any]:
    if type(args.jobs) is not int or args.jobs < 1:
        raise native.BazelWorkspaceError("dependency verification jobs must be positive")
    host = native.native_platform()
    tree = bazel_workspace.workspace_path(args.path, args.config)
    with build_tree_lock(tree):
        native.require_supported_source()
        directory = tree / "dependencies"
        directory.mkdir(parents=True, exist_ok=True)
        attempt = Path(tempfile.mkdtemp(prefix="verify-", dir=directory))
        executable = bazel_workspace.bazel_executable()
        prepared = bazel_workspace.prepare_workspace(tree, args.config, prefix=args.vcpkg_prefix)
        sequence = 0

        def query(action: str, arguments: list[str]) -> str:
            nonlocal sequence
            sequence += 1
            argv = [*bazel_workspace.invocation(executable, prepared, isolate_rc=True), action, *arguments]
            output, errors = attempt / f"{sequence}-{action}.stdout.log", attempt / f"{sequence}-{action}.stderr.log"
            with output.open("xb") as out, errors.open("xb") as err:
                result = subprocess.run(
                    argv,
                    cwd=PROJECT_ROOT,
                    env=native.child_environment(isolate_rc=True),
                    stdout=out,
                    stderr=err,
                    check=False,
                )
            native.write_json(
                attempt / f"{sequence}-{action}.invocation.json", {"argv": argv, "exit": result.returncode}
            )
            if result.returncode:
                raise native.BazelWorkspaceError(f"native SDK {action} failed ({result.returncode}); see {errors}")
            return output.read_text(encoding="utf-8")

        root = Path(query("info", ["execution_root"]).strip())
        output_base = (tree / "state/output").resolve()
        if not root.is_absolute() or not root.resolve().is_relative_to(output_base):
            raise native.BazelWorkspaceError("dependency query execution_root escapes its selected tree")
        manifests = []
        for target in _MANIFESTS:
            path, declared = _manifest_file(target, args.config, args.jobs, query)
            manifest = _verify_manifest(native.read_json(path), host=host, config=args.config, workers=args.jobs)
            manifests.append(
                {
                    "target": target,
                    "path": str(path),
                    "declared_source": declared,
                    "sha256": native.sha256(path),
                    "facts": manifest,
                }
            )
        report = {
            "schema": 1,
            "producer": "aobus-governed-native-sdk-report",
            "platform": host,
            "config": args.config,
            "tree": str(tree),
            "manifests": manifests,
            "claim": (
                "governed repository admission plus current recorded native input digests; "
                "no compilation or runtime execution"
            ),
        }
        native.write_json(attempt / "report.json", report)
        return report


def run_command(args: argparse.Namespace) -> int:
    try:
        if args.deps_action == "provision":
            bazel_provision.provision(bazel_workspace.workspace_path(args.path, args.config))
            return 0
        report: dict[str, Any]
        if getattr(args, "concepts", False):
            from ..core import bazel_concepts, concept_report

            tree = bazel_workspace.workspace_path(args.path, args.config)
            with build_tree_lock(tree):
                sdk_facts = _verified(args)
                report = bazel_concepts.write_bazel_report(args, sdk_facts=sdk_facts)
            print(concept_report.format_summary(report))
        else:
            report = _verified(args)
            for manifest in report["manifests"]:
                facts = manifest["facts"]
                print(
                    f"{manifest['target']}: {len(facts['packages'])} admitted packages, "
                    f"{len(facts['inputs'])} verified native inputs"
                )
            print(f"Native dependency admission verified ({report['platform']}, {report['config']}).")
        if destination := getattr(args, "json", None):
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes((json.dumps(report, indent=2, sort_keys=True) + "\n").encode("utf-8"))
        return 0
    except (
        native.BazelWorkspaceError,
        dependency_policy.DependencyPolicyError,
        OSError,
        subprocess.SubprocessError,
        ValueError,
    ) as error:
        raise die(str(error)) from error
