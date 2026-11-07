"""Repository-rule composition of governed native observations.

This process owns dispatch and read inventories, not a portal workspace or Bazel
program generation. Checked-in Starlark projects library, tool and compiler facts.
The independent WinUI SDK owner retains its existing lazy native admission.
"""

from __future__ import annotations

import argparse
import json
import os
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
    result: dict[str, Any] = {"imports": {}, "watch_files": [], "watch_directories": []}
    status = 0
    try:
        # Isolated Python deliberately does not add the checkout to sys.path.
        sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
        from ao.core import bazel_dependencies as dependencies
        from ao.core import bazel_native as native

        with native.capture_probes(lambda path: files.add(str(path.absolute()))):
            request = json.loads(args.request.read_text(encoding="utf-8"))
            if request.get("schema") != 1 or request["host"] != native.native_platform():
                raise ValueError("prepared configuration does not match the native host")
            host = request["host"]
            if "config" in request:
                raise ValueError("repository request must be mode-neutral")
            prefix = Path(request["prefix"]) if request.get("prefix") else None
            if prefix is not None and args.group in {"native", "test", "tools"}:
                # Include absence/ambiguity decisions, not just successful opens.
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

            def imports(config: str, group: str) -> None:
                projection = dependencies.resolve_dependencies(
                    tree / config, config, prefix=str(prefix) if prefix else None, group=group
                )
                files.update(projection["watch_files"])
                directories.update(projection["watch_directories"])
                result["imports"][config] = {
                    "packages": projection["packages"],
                    "manifest": projection["manifest"],
                }

            if args.group in {"native", "test"}:
                for config in ("debug", "release"):
                    imports(config, args.group)
            elif args.group == "winui":
                from ao.core.bazel_winui import resolve_sdk

                result["winui"] = {}
                for config in ("debug", "release"):
                    result["winui"][config] = resolve_sdk(
                        tree / ("winui-" + config),
                        {**request, "config": config},
                        environment,
                    )
            elif args.group == "tools":
                from ao.core import bazel_host_tools

                tools = bazel_host_tools.resolve_host_tools(tree, prefix, host, source=request.get("source"))
                files.update(tools.pop("watch_files"))
                directories.update(tools.pop("watch_directories"))
                result["host_tools"] = tools
                for config in ("debug", "release"):
                    imports(config, "catalog")
            else:
                from ao.core import bazel_compiler_probe

                compiler = bazel_compiler_probe.probe_compiler(host)
                files.update(item["path"] for item in compiler["identity"]["files"])
                result["toolchain"] = compiler
    except Exception as exc:
        result["error"] = f"{type(exc).__name__}: {exc}"
        result["traceback"] = traceback.format_exc()
        status = 1
    finally:
        # Nix outputs are immutable provider/runtime identities. Their selection
        # is watched via report/policy; payload bytes remain action inputs.
        result["watch_files"] = sorted(path for path in files if outside(path) and not path.startswith("/nix/store/"))
        result["watch_directories"] = sorted(
            path for path in directories if outside(path) and not path.startswith("/nix/store/")
        )
        (output / "repository.json").write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return status


if __name__ == "__main__":
    raise SystemExit(main())
