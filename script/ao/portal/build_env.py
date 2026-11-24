"""Answers which portal commands need the native build environment.

Each command module declares REQUIRES_BUILD_ENV next to its NAME. The Windows
portal uses its parsed Namespace in-process; the macOS launcher queries this
module after Python bootstrap. Neither keeps a separate command list. Known
commands are parsed before scope resolution or native environment selection.
The query API answers help flags without parsing. Platform launchers may still
enter Nix or bootstrap Python before this module runs.
"""

import argparse
import json
import os
import sys
from collections.abc import Sequence
from pathlib import Path
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from ao.portal.source_scope import SourceScope

from ao.core.paths import PROJECT_ROOT
from ao.core.proc import die

BUILD_ENV_REQUIRED_EXIT_CODE = 10


def windows_bootstrap_requires_python_tools(arguments: Sequence[str], *, scope_file: Path | None = None) -> bool:
    """Select tooling from the parsed command, not its first argv spelling.

    Flat native commands and explicit Bazel commands use the same governed
    base runtime. Only actual Python lint/docs/tooling consumers need wheels.
    Invalid arguments fail before environment selection.
    """
    if not arguments or arguments[0] == "help" or _help_requested(arguments):
        return False
    args = parse_command_arguments(arguments[0], arguments[1:])
    required = requires_parsed_python_tools(args)
    if scope_file is not None and hasattr(args, "_resolved_sources"):
        write_source_scope(args, scope_file)
    return required


def _help_requested(arguments: Sequence[str]) -> bool:
    portal_arguments = arguments[: arguments.index("--")] if "--" in arguments else arguments
    return any(argument in {"-h", "--help"} for argument in portal_arguments)


def parse_command_arguments(
    name: str,
    arguments: Sequence[str],
) -> argparse.Namespace:
    """Use the owning command parser when preparation depends on its options."""
    from ao.__main__ import make_parser, parse_arguments

    return parse_arguments(make_parser(), [name, *arguments])


def _scope_key(args: argparse.Namespace) -> dict[str, object]:
    return {
        "root": str(PROJECT_ROOT),
        **{
            key: getattr(args, key, None)
            for key in ("command", "check_action", "measure_action", "paths", "all", "commit", "docs")
        },
    }


def record_source_scope(args: argparse.Namespace, scope: SourceScope) -> None:
    """Freeze discovered files and their individual-file selection provenance."""
    args._resolved_sources = list(scope.files)
    args._explicit_sources = list(scope.explicit_files)
    if destination := os.environ.get("AOBUS_PREFLIGHT_SCOPE"):
        write_source_scope(args, Path(destination))


def write_source_scope(args: argparse.Namespace, destination: Path) -> None:
    """Publish one launcher's private, invocation-bound scope transfer."""
    destination.write_text(
        json.dumps(
            {
                "scope": _scope_key(args),
                "files": list(args._resolved_sources),
                "explicit_files": list(args._explicit_sources),
            }
        ),
        encoding="utf-8",
    )


def load_source_scope(args: argparse.Namespace, path: str | None) -> None:
    """Consume only a scope prepared for this command; the shim owns file cleanup."""
    if path is None or not Path(path).is_file() or Path(path).stat().st_size == 0:
        return
    record = json.loads(Path(path).read_text(encoding="utf-8"))
    if (
        not isinstance(record, dict)
        or record.get("scope") != _scope_key(args)
        or not isinstance(record.get("files"), list)
        or not all(isinstance(name, str) for name in record["files"])
        or not isinstance(record.get("explicit_files"), list)
        or not all(isinstance(name, str) for name in record["explicit_files"])
        or not set(record["explicit_files"]).issubset(record["files"])
    ):
        raise die("native preflight source scope does not match this invocation")
    args._resolved_sources = record["files"]
    args._explicit_sources = record["explicit_files"]


def requires_build_env(command: str, arguments: Sequence[str] = ()) -> bool:
    """Return True when `command` needs the native compiler and dependencies.

    Every known command is parsed first, including commands whose need is a
    static flag. Invalid or incomplete arguments fail at that parser instead of
    selecting a native environment. Unknown commands stay false and are not parsed.
    """
    from ao.command import COMMAND_MODULES

    if _help_requested(arguments):
        return False
    for module in COMMAND_MODULES:
        if module.NAME == command:
            return requires_parsed_build_env(parse_command_arguments(command, arguments))
    return False


def requires_parsed_build_env(args: argparse.Namespace) -> bool:
    """Use the invocation's parsed options and any prepared source scope."""
    from ao.command import COMMAND_MODULES

    for module in COMMAND_MODULES:
        if module.NAME == args.command:
            if policy := getattr(module, "requires_build_environment", None):
                return bool(policy(args))
            return bool(module.REQUIRES_BUILD_ENV)
    return False


def requires_python_tools(command: str, arguments: Sequence[str] = ()) -> bool:
    """Return True when `command` needs the portal's managed Python tools."""
    from ao.command import COMMAND_MODULES

    if _help_requested(arguments):
        return False
    for module in COMMAND_MODULES:
        if module.NAME == command:
            return requires_parsed_python_tools(parse_command_arguments(command, arguments))
    return False


def requires_parsed_python_tools(args: argparse.Namespace) -> bool:
    """Evaluate the owning command's static or option-dependent tooling intent."""
    from ao.command import COMMAND_MODULES

    for module in COMMAND_MODULES:
        if module.NAME == args.command:
            policy = getattr(module, "REQUIRES_PYTHON_TOOLS", False)
            return bool(policy(args) if callable(policy) else policy)
    return False


def requires_provider_tools(command: str, arguments: Sequence[str] = ()) -> bool:
    """Port-build tools are needed only by explicit dependency provisioning."""
    from ao.command import COMMAND_MODULES

    if _help_requested(arguments):
        return False
    for module in COMMAND_MODULES:
        if module.NAME == command:
            args = parse_command_arguments(command, arguments)
            policy = getattr(module, "REQUIRES_PROVIDER_TOOLS", False)
            return bool(policy(args) if callable(policy) else policy)
    return False


def main(argv: list[str] | None = None) -> int:
    arguments = sys.argv[1:] if argv is None else argv
    exit_code = bool(arguments and arguments[0] == "--exit-code")
    if exit_code:
        arguments = arguments[1:]
    python_tools = bool(arguments and arguments[0] == "--python-tools")
    provider_tools = bool(arguments and arguments[0] == "--provider-tools")
    command_index = 1 if python_tools or provider_tools else 0
    command = arguments[command_index] if len(arguments) > command_index else ""
    command_arguments = arguments[command_index + 1 :]
    if python_tools:
        required = requires_python_tools(command, command_arguments)
    elif provider_tools:
        required = requires_provider_tools(command, command_arguments)
    else:
        required = requires_build_env(command, command_arguments)
    if exit_code:
        return BUILD_ENV_REQUIRED_EXIT_CODE if required else 0
    print("1" if required else "0")
    return 0


if __name__ == "__main__":
    sys.exit(main())
