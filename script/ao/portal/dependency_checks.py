"""Governed SDK admission and digest reports, without provisioning or execution."""

from __future__ import annotations

import json
import subprocess
import tempfile
from collections.abc import Callable
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from ao.core import bazel_native as native
from ao.core import jobs
from ao.core.build_lock import build_tree_lock
from ao.core.paths import PROJECT_ROOT
from ao.portal import bazel_profiles, bazel_workspace

_MANIFESTS = ("@aobus_native_deps//:manifest.json", "@aobus_test_deps//:manifest.json")
_MANIFEST_FILES = (
    "json.encode([{'owner': str(f.owner), 'path': f.path, 'source': f.is_source} "
    "for f in providers(target)['DefaultInfo'].files.to_list()])"
)


@dataclass(frozen=True, kw_only=True)
class Request:
    config: str = "fastbuild"
    path: str | None = None
    vcpkg_prefix: str | None = None
    jobs: int = field(default_factory=jobs.default_workers)


def validate_request(request: Request) -> None:
    """Pure selection admission, before SDK queries or tree selection."""
    if request.config not in {"debug", "fastbuild", "release"}:
        raise native.BazelWorkspaceError("SDK reports require debug, fastbuild or release")
    bazel_profiles.validate_request(request.config, native.native_platform())
    if type(request.jobs) is not int or request.jobs < 1:
        raise native.BazelWorkspaceError("dependency verification jobs must be positive")


def _verify_manifest(value: dict[str, Any], *, host: str, config: str, workers: int) -> dict[str, Any]:
    """Check native admission identity and every recorded input, never a freshness stamp."""
    configuration = value["configuration"]
    # Fastbuild consumes the existing Release SDK manifest.
    sdk_config = bazel_profiles.compilation_config(config).title()
    if configuration.get("platform") != host or configuration.get("config") != sdk_config:
        raise native.BazelWorkspaceError("dependency manifest configuration differs from the actual request")
    inputs = value["inputs"]

    def verify(entry: dict[str, Any]) -> None:
        path = Path(entry["path"])
        if not path.is_file() or native.sha256(path) != entry["sha256"]:
            raise native.BazelWorkspaceError(
                f"native dependency input changed or disappeared: {path}; query its governed repository again"
            )

    with ThreadPoolExecutor(max_workers=min(workers, max(1, len(inputs)))) as pool:
        list(pool.map(verify, inputs))
    return value


def _manifest_file(
    target: str,
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
    location = query("query", [f"'{row['owner']}'", "--output=location", "--lockfile_mode=error"]).strip()
    return Path(location.split(": source file ", 1)[0].rsplit(":", 2)[0]), row


def verified_report(request: Request) -> dict[str, Any]:
    validate_request(request)
    host = native.native_platform()
    tree = bazel_workspace.workspace_path(request.path, request.config)
    with build_tree_lock(tree):
        native.require_supported_source()
        directory = tree / "dependencies"
        directory.mkdir(parents=True, exist_ok=True)
        attempt = Path(tempfile.mkdtemp(prefix="verify-", dir=directory))
        executable = bazel_workspace.bazel_executable()
        prepared = bazel_workspace.prepare_workspace(tree, request.config, prefix=request.vcpkg_prefix)
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
            path, declared = _manifest_file(target, request.jobs, query)
            manifest = _verify_manifest(native.read_json(path), host=host, config=request.config, workers=request.jobs)
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
            "config": request.config,
            "tree": str(tree),
            "manifests": manifests,
            "claim": (
                "governed repository admission plus current recorded native input digests; "
                "no compilation or runtime execution"
            ),
        }
        native.write_json(attempt / "report.json", report)
        return report


def write_report(report: dict[str, Any], destination: Path | None) -> None:
    if destination is not None:
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes((json.dumps(report, indent=2, sort_keys=True) + "\n").encode("utf-8"))


def check(request: Request, *, json_output: Path | None = None) -> int:
    """One operation admits the SDK, verifies digests, and reports the actual facts."""
    report = verified_report(request)
    for manifest in report["manifests"]:
        facts = manifest["facts"]
        print(
            f"{manifest['target']}: {len(facts['packages'])} admitted packages, "
            f"{len(facts['inputs'])} verified native inputs"
        )
    print(f"Native dependency admission verified ({report['platform']}, {report['config']}).")
    write_report(report, json_output)
    return 0
