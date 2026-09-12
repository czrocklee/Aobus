"""Answers which portal commands need the native build environment.

Each command module declares REQUIRES_BUILD_ENV next to its NAME; ao.bat
and the macOS branch of ./ao query this module after their Python bootstrap
instead of keeping their own copies of the command list.
"""

import argparse
import json
import os
import sys
from collections.abc import Sequence
from pathlib import Path

from .paths import PROJECT_ROOT
from .proc import die

BUILD_ENV_REQUIRED_EXIT_CODE = 10


def _help_requested(arguments: Sequence[str]) -> bool:
    portal_arguments = arguments[: arguments.index("--")] if "--" in arguments else arguments
    return any(argument in {"-h", "--help"} for argument in portal_arguments)


def parse_command_arguments(
    name: str,
    arguments: Sequence[str],
) -> argparse.Namespace:
    """Use the owning command parser when preparation depends on its options."""
    from ..__main__ import make_parser, parse_arguments

    return parse_arguments(make_parser(), [name, *arguments])


def _scope_key(args: argparse.Namespace) -> dict[str, object]:
    return {
        "root": str(PROJECT_ROOT),
        **{key: getattr(args, key, None) for key in ("command", "files", "all", "folder", "commit")},
    }


def requires_source_build_env(args: argparse.Namespace, files: list[str]) -> bool:
    """Hand the selected files to the native invocation, without a second Git scan."""
    from .gitfiles import CPP_SUFFIXES

    args._resolved_sources = list(files)
    if destination := os.environ.get("AOBUS_PREFLIGHT_SCOPE"):
        Path(destination).write_text(json.dumps({"scope": _scope_key(args), "files": files}), encoding="utf-8")
    return any(name.endswith(CPP_SUFFIXES) and (PROJECT_ROOT / name).is_file() for name in files)


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
    ):
        raise die("native preflight source scope does not match this invocation")
    args._resolved_sources = record["files"]


def requires_build_env(command: str, arguments: Sequence[str] = ()) -> bool:
    """Return True when `command` needs the native compiler and dependencies."""
    from ..command import COMMAND_MODULES

    if _help_requested(arguments):
        return False
    for module in COMMAND_MODULES:
        if module.NAME == command:
            if hasattr(module, "requires_build_environment"):
                return requires_parsed_build_env(parse_command_arguments(command, arguments))
            return bool(module.REQUIRES_BUILD_ENV)
    return False


def requires_parsed_build_env(args: argparse.Namespace) -> bool:
    """Use the invocation's parsed options and any prepared source scope."""
    from ..command import COMMAND_MODULES

    for module in COMMAND_MODULES:
        if module.NAME == args.command:
            if policy := getattr(module, "requires_build_environment", None):
                return bool(policy(args))
            return bool(module.REQUIRES_BUILD_ENV)
    return False


def requires_python_tools(command: str, arguments: Sequence[str] = ()) -> bool:
    """Return True when `command` needs the managed Ruff/mypy environment."""
    from ..command import COMMAND_MODULES

    if _help_requested(arguments):
        return False
    for module in COMMAND_MODULES:
        if module.NAME == command:
            required = bool(getattr(module, "REQUIRES_PYTHON_TOOLS", False))
            parse_command_arguments(command, arguments)
            return required
    return False


def main(argv: list[str] | None = None) -> int:
    arguments = sys.argv[1:] if argv is None else argv
    exit_code = bool(arguments and arguments[0] == "--exit-code")
    if exit_code:
        arguments = arguments[1:]
    python_tools = bool(arguments and arguments[0] == "--python-tools")
    command_index = 1 if python_tools else 0
    command = arguments[command_index] if len(arguments) > command_index else ""
    command_arguments = arguments[command_index + 1 :]
    required = (
        requires_python_tools(command, command_arguments)
        if python_tools
        else requires_build_env(command, command_arguments)
    )
    if exit_code:
        return BUILD_ENV_REQUIRED_EXIT_CODE if required else 0
    print("1" if required else "0")
    return 0


if __name__ == "__main__":
    sys.exit(main())
