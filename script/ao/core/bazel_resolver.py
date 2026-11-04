"""Repository-rule entrypoint for the existing governed native resolver.

Bazel owns invocation, dependency invalidation and materialization. This process
only validates interfaces and returns metadata plus explicit artifact links.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import sys
import traceback
from pathlib import Path
from typing import Any


def _audit_executable(value: object) -> str | None:
    # Windows subprocess audit events can leave executable=None even when argv
    # names an absolute tool. Explicit tool inventories remain authoritative.
    return os.fsdecode(value) if isinstance(value, (str, bytes, os.PathLike)) else None


def _external_input(path: object, repository: Path) -> str | None:
    if not isinstance(path, (str, bytes)):
        return None
    value = Path(os.fsdecode(path)).absolute()
    if value.is_relative_to(repository) or value.resolve().is_relative_to(repository.resolve()):
        return None
    return str(value)


def _app_version(root: Path) -> str:
    # This is a text projection of the sole project declaration, not CMake
    # evaluation or source identity. Opening the input retains the audit watch.
    text = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    text = re.sub(r"#\[\[.*?\]\]", "", text, flags=re.DOTALL)
    text = re.sub(r"#[^\n]*", "", text)
    declarations = re.findall(r"\bproject\s*\(([^)]*)\)", text, flags=re.IGNORECASE)
    if len(declarations) != 1:
        raise ValueError("expected one project(aobus VERSION x.y.z) declaration")
    match = re.fullmatch(r"\s*aobus\s+VERSION\s+(\d+\.\d+\.\d+)\s*", declarations[0])
    if match is None:
        raise ValueError("expected project(aobus VERSION x.y.z)")
    return match[1]


def configured_repository(plans: dict[str, dict[str, Any]]) -> dict[str, Any]:
    """Select an SDK interface in its consumer configuration, including exec tools."""
    if set(plans) != {"debug", "release"}:
        raise ValueError("configured repositories require Debug and Release interfaces")
    targets = set(plans["debug"]["targets"])
    if targets != set(plans["release"]["targets"]):
        raise ValueError("Debug and Release SDKs expose different consumer interfaces")
    result: dict[str, Any] = {"files": {}, "links": []}
    for config, plan in plans.items():
        for name, content in plan["files"].items():
            result["files"][config + "/" + name] = content
        result["links"].extend({**link, "destination": config + "/" + link["destination"]} for link in plan["links"])
    lines = [
        'package(default_visibility = ["//visibility:public"])',
        'config_setting(name = "_debug", values = {"compilation_mode": "dbg"})',
        'config_setting(name = "_release", values = {"compilation_mode": "opt"})',
    ]
    for target in sorted(targets):
        lines.append(
            "alias(name = "
            + json.dumps(target)
            + ", actual = select({"
            + '":_debug": '
            + json.dumps("//debug:" + target)
            + ", "
            + '":_release": '
            + json.dumps("//release:" + target)
            + '}, no_match_error = "Native SDK imports support dbg or opt; fastbuild is not supported"))'
        )
    result["files"]["BUILD.bazel"] = "\n".join(lines) + "\n"
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--request", type=Path, required=True)
    parser.add_argument("--group", choices=("native", "test", "tools", "toolchain", "winui"), required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--environment-json", required=True)
    args = parser.parse_args()
    environment = json.loads(args.environment_json)
    if not isinstance(environment, dict) or any(
        not isinstance(k, str) or not isinstance(v, str) for k, v in environment.items()
    ):
        parser.error("environment must be an explicit string mapping")
    os.environ.clear()
    os.environ.update(environment)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    files: set[str] = {str(args.request.absolute()), str(Path(__file__).resolve()), sys.executable}
    directories: set[str] = set()

    def outside(path: object) -> str | None:
        # Includes Python/tool search probes at cwd, not only generated files
        # under _resolution. A repository cannot watch its own working tree.
        return _external_input(path, output.parent)

    def audit(event: str, values: tuple[Any, ...]) -> None:
        # Supplemental read inventory. Explicit metadata inventories below cover
        # native tools; this is not an OS sandbox or a complete syscall trace.
        if event == "open" and values:
            if path := outside(values[0]):
                files.add(path)
        elif event in {"os.listdir", "os.scandir"} and values:
            if path := outside(values[0]):
                directories.add(path)
        elif event == "subprocess.Popen" and values:
            executable = _audit_executable(values[0])
            selected = (
                (executable if Path(executable).is_absolute() else shutil.which(executable)) if executable else None
            )
            if selected and (path := outside(selected)):
                files.add(path)

    sys.addaudithook(audit)
    result: dict[str, Any] = {"files": {}, "links": [], "watch_files": [], "watch_directories": []}
    status = 0
    try:
        # Isolated Python deliberately does not add the checkout to sys.path.
        sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
        from ao.core import bazel_workspace as workspace
        from ao.core.paths import PROJECT_ROOT

        workspace._INPUT_PROBE = lambda path: files.add(str(path.absolute()))
        request = json.loads(args.request.read_text(encoding="utf-8"))
        if request.get("schema") != 1 or request["host"] != workspace.native_platform():
            raise ValueError("prepared configuration does not match the native host")
        host, config = request["host"], request.get("config")
        if config not in {None, "debug", "release"}:
            raise ValueError("unsupported provider configuration")
        prefix = Path(request["prefix"]) if request.get("prefix") else None
        if prefix is not None and args.group in {"native", "test", "tools"}:
            # These searches include absence/ambiguity decisions made by the
            # resolver and pkgconf, not just Python's successful open calls.
            directories.update(
                str(path)
                for path in (
                    prefix.parent / "vcpkg/info",
                    prefix / "lib",
                    prefix / "debug/lib",
                    prefix / "bin",
                    prefix / "debug/bin",
                    prefix / "lib/pkgconfig",
                    prefix / "debug/lib/pkgconfig",
                    prefix / "tools",
                    prefix / "include",
                )
            )
            files.add(str(prefix.parent / "vcpkg/status"))
        tree = output / "resolution"
        tree.mkdir(exist_ok=True)
        if args.group in {"native", "test"}:
            plans = {}
            for selected in (config,) if config else ("debug", "release"):
                generated = workspace.refresh_dependencies(
                    tree / selected, selected, prefix=str(prefix) if prefix else None, group=args.group
                )
                repository = generated / args.group
                plan = json.loads((repository / "repository.json").read_text(encoding="utf-8"))
                manifest = json.loads((repository / "manifest.json").read_text(encoding="utf-8"))
                files.update(plan["watch_files"])
                directories.update(plan["watch_directories"])
                plans[selected] = {
                    "targets": manifest["targets"],
                    "links": plan["links"],
                    "files": {
                        name: (repository / name).read_text(encoding="utf-8")
                        for name in ("BUILD.bazel", "manifest.json")
                    },
                }
            projection = plans[config] if config else configured_repository(plans)
            result["links"] = projection["links"]
            result["files"] = projection["files"]
        elif args.group == "winui":
            from ao.core.bazel_winui import repository_plan

            if config:
                projection = repository_plan(tree / "winui", request, environment)
            else:
                plans = {}
                for selected in ("debug", "release"):
                    plan = repository_plan(tree / ("winui-" + selected), {**request, "config": selected}, environment)
                    plans[selected] = {
                        **plan,
                        "targets": ["headers", "bootstrap", "runtime", "tools", "sdk.json", "sdk-diagnostics.json"],
                    }
                projection = configured_repository(plans)
            result["links"] = projection["links"]
            result["files"] = projection["files"]
        elif args.group == "tools":
            identity, executable, runtime, evidence = (
                workspace._linux_gperf() if prefix is None else workspace._native_gperf(prefix, host)
            )
            import subprocess

            version = subprocess.run(
                [str(executable), "--version"], env=environment, capture_output=True, text=True, check=True
            )
            if version.stdout.splitlines()[:1] != [f"GNU gperf {identity['version']}"]:
                raise ValueError("gperf executable version differs from its provider")
            artifacts = [executable, *runtime]
            if len({path.name.casefold() for path in artifacts}) != len(artifacts):
                raise ValueError("ambiguous gperf runtime basenames")
            result["links"] = [{"source": str(path.resolve()), "destination": "bin/" + path.name} for path in artifacts]
            files.update(str(path) for path in [*evidence, *artifacts])
            directories.add(str(executable.parent))
            manifest_tools = {}
            manifest_inputs = []
            if host == "windows":
                system = Path(os.environ.get("SYSTEMROOT", ""))
                powershell = workspace._probe(system / "System32/WindowsPowerShell/v1.0/powershell.exe")
                if not system.is_absolute() or not powershell.is_file():
                    raise ValueError("selected Windows PowerShell runtime is missing")
                for name, tool in {"mt": workspace._windows_mt(), "powershell": powershell}.items():
                    manifest_tools[name] = tool.resolve().as_posix()
                    destination = "windows-manifest/" + name + ".exe"
                    manifest_inputs.append(destination)
                    result["links"].append({"source": str(tool.resolve()), "destination": destination})
                    files.update({str(tool.absolute()), str(tool.resolve())})
            # Starlark strings accept UTF-8, not JSON's Unicode escape syntax.
            result["files"]["windows_manifest_tools.bzl"] = (
                "# Selected native tool locations; executable bytes are also action inputs.\n"
                + "WINDOWS_MANIFEST_TOOLS = "
                + json.dumps(manifest_tools, sort_keys=True, ensure_ascii=False)
                + "\n"
            )
            catalog_manifests = {}
            for catalog_config in ("debug", "release"):
                generated = workspace.refresh_dependencies(
                    tree / ("catalog-" + catalog_config),
                    catalog_config,
                    prefix=str(prefix) if prefix else None,
                    group="catalog",
                )
                repository = generated / "catalog"
                plan = json.loads((repository / "repository.json").read_text(encoding="utf-8"))
                subpackage = "catalog-" + catalog_config
                result["links"].extend(
                    {**link, "destination": subpackage + "/" + link["destination"]} for link in plan["links"]
                )
                files.update(plan["watch_files"])
                directories.update(plan["watch_directories"])
                for name in ("BUILD.bazel", "manifest.json"):
                    result["files"][subpackage + "/" + name] = (repository / name).read_text(encoding="utf-8")
                catalog_manifests[catalog_config] = subpackage + "/manifest.json"
            catalog_tools, sdk_artifacts, sdk_evidence = workspace._catalog_sdk(tree, prefix, host)
            sdk_inputs = []
            for index, path in enumerate(sdk_artifacts):
                destination = f"catalog-sdk/{index}/{path.name}"
                result["links"].append({"source": str(path.resolve()), "destination": destination})
                sdk_inputs.append(destination)
                files.update({str(path.absolute()), str(path.resolve())})
            files.update(str(path) for path in sdk_evidence)
            directories.update(str(Path(catalog_tools[name]).parent) for name in ("python", "genrb", "pkgdata"))
            test_tools = {}
            # The WinUI quality probe embeds the original checkout, not Bazel's execution root.
            source = request.get("source")
            if source is not None:
                if (
                    not isinstance(source, str)
                    or not source
                    or not Path(source).is_absolute()
                    or not Path(source).is_dir()
                ):
                    raise ValueError("prepared request source must be an existing absolute checkout root")
                test_tools["source_root"] = source
            dbus_inputs = []
            resource_tool: dict[str, Any] = {}
            resource_inputs = []
            resource_alias = ""
            if host == "linux":
                resource_tool, resource_compiler, resource_runtime, resource_evidence = workspace._linux_gresource()
                resource_label = "resource-tools/bin/" + resource_compiler.name
                result["links"].append({"source": str(resource_compiler.resolve()), "destination": resource_label})
                for index, path in enumerate([*resource_runtime, *resource_evidence]):
                    destination = f"resource-tools/runtime/{index}/{path.name}"
                    result["links"].append({"source": str(path.resolve()), "destination": destination})
                    resource_inputs.append(destination)
                    files.update({str(path.absolute()), str(path.resolve())})
                files.update({str(resource_compiler.absolute()), str(resource_compiler.resolve())})
                directories.add(str(resource_compiler.parent))
                resource_alias = (
                    f"exports_files({json.dumps([resource_label], ensure_ascii=False)})\n"
                    + 'alias(name = "glib_compile_resources", actual = '
                    + json.dumps(resource_label, ensure_ascii=False)
                    + ")\n"
                )
                dbus_identity, daemon, dbus_runtime, dbus_evidence = workspace._linux_dbus()
                test_tools["dbus_daemon"] = str(daemon)
                test_tools["dbus_daemon_directory"] = str(daemon.parent)
                for index, path in enumerate([daemon, *dbus_runtime, *dbus_evidence]):
                    destination = f"test-tools/{index}/{path.name}"
                    result["links"].append({"source": str(path.resolve()), "destination": destination})
                    dbus_inputs.append(destination)
                    files.update({str(path.absolute()), str(path.resolve())})
                directories.add(str(daemon.parent))
                result["files"]["test-tools.json"] = json.dumps(dbus_identity, indent=2) + "\n"
                dbus_inputs.append("test-tools.json")
            result["files"]["test_tools.bzl"] = (
                "TEST_TOOLS = " + json.dumps(test_tools, sort_keys=True, ensure_ascii=False) + "\n"
            )
            result["files"]["catalog_tools.bzl"] = (
                "# Installed native paths; SDK bytes are declared action inputs.\nCATALOG_TOOLS = "
                + json.dumps(catalog_tools, sort_keys=True, ensure_ascii=False)
                + "\n"
            )
            result["files"]["app_version.bzl"] = "APP_VERSION = " + json.dumps(_app_version(PROJECT_ROOT)) + "\n"
            result["files"]["BUILD.bazel"] = (
                'package(default_visibility = ["//visibility:public"])\n'
                + f"exports_files({json.dumps(['bin/' + path.name for path in artifacts], ensure_ascii=False)})\n"
                + f'alias(name = "gperf", actual = {json.dumps("bin/" + executable.name, ensure_ascii=False)})\n'
                + 'filegroup(name = "gperf_runtime", srcs = '
                + json.dumps(["bin/" + path.name for path in runtime], ensure_ascii=False)
                + ")\n"
                + 'filegroup(name = "windows_manifest_tools", srcs = '
                + json.dumps(manifest_inputs, ensure_ascii=False)
                + ")\n"
                + 'exports_files(["windows_manifest_tools.bzl", "catalog_tools.bzl", '
                '"app_version.bzl", "test_tools.bzl"])\n'
                + resource_alias
                + 'filegroup(name = "glib_compile_resources_runtime", srcs = '
                + json.dumps(resource_inputs, ensure_ascii=False)
                + ")\n"
                + f'filegroup(name = "dbus_daemon", srcs = {json.dumps(dbus_inputs, ensure_ascii=False)})\n'
                + f'filegroup(name = "catalog_tools", srcs = {json.dumps(sdk_inputs, ensure_ascii=False)})\n'
                + 'config_setting(name = "_catalog_debug", values = {"compilation_mode": "dbg"})\n'
                + 'config_setting(name = "_catalog_release", values = {"compilation_mode": "opt"})\n'
                + 'alias(name = "catalog_icu", actual = select({\n'
                + '    ":_catalog_debug": "//catalog-debug:icu",\n'
                + '    ":_catalog_release": "//catalog-release:icu",\n'
                + '}, no_match_error = "Catalog ICU requires dbg or opt"))\n'
            )
            result["files"]["manifest.json"] = (
                json.dumps(
                    {
                        "tool": identity,
                        "artifacts": result["links"],
                        "windows_manifest_tools": manifest_tools,
                        "catalog_tools": catalog_tools,
                        "catalog_imports": catalog_manifests,
                        "resource_tool": resource_tool,
                    },
                    indent=2,
                )
                + "\n"
            )
        else:
            generated = tree / "generated"
            if host == "macos":
                workspace._macos_toolchain(generated)
            elif host == "linux":
                result["links"] = workspace._linux_toolchain(generated)
            else:
                result["links"] = workspace._windows_toolchain(generated, request.get("compiler_environment"))
            if host == "windows":
                workspace._toolchain_identity(generated, host, compiler_environment=request["compiler_environment"])
            else:
                workspace._toolchain_identity(generated, host)
            repository = generated / "toolchain"
            identity_bytes = (repository / "identity.json").read_bytes()
            identity = json.loads(identity_bytes)
            files.update(item["path"] for item in identity["files"])
            result["files"]["BUILD.bazel"] = (repository / "BUILD.bazel").read_text(encoding="utf-8")
            result["files"]["identity.json"] = identity_bytes.decode("utf-8")
            result["files"]["native_paths.bzl"] = (repository / "native_paths.bzl").read_text(encoding="utf-8")
    except Exception as exc:
        result["error"] = f"{type(exc).__name__}: {exc}"
        result["traceback"] = traceback.format_exc()
        status = 1
    finally:
        # Nix outputs are trusted immutable provider/runtime identities. Their
        # selection is tracked through environment/report/policy; payload bytes
        # remain Bazel action inputs, not thousands of mutable repository probes.
        result["watch_files"] = sorted(path for path in files if outside(path) and not path.startswith("/nix/store/"))
        result["watch_directories"] = sorted(
            path for path in directories if outside(path) and not path.startswith("/nix/store/")
        )
        (output / "repository.json").write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return status


if __name__ == "__main__":
    raise SystemExit(main())
