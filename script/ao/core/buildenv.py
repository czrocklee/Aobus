"""Answers which portal commands need the native build environment.

Each command module declares REQUIRES_BUILD_ENV next to its NAME; ao.bat
and the macOS branch of ./ao query this module after their Python bootstrap
instead of keeping their own copies of the command list.
"""

import sys


def requires_build_env(command: str) -> bool:
    """Return True when `command` needs the native compiler and dependencies."""
    from ..command import COMMAND_MODULES

    for module in COMMAND_MODULES:
        if module.NAME == command:
            return bool(module.REQUIRES_BUILD_ENV)
    return False


def requires_python_tools(command: str) -> bool:
    """Return True when `command` needs the managed Ruff/mypy environment."""
    from ..command import COMMAND_MODULES

    for module in COMMAND_MODULES:
        if module.NAME == command:
            return bool(getattr(module, "REQUIRES_PYTHON_TOOLS", False))
    return False


def main(argv: list[str] | None = None) -> int:
    arguments = sys.argv[1:] if argv is None else argv
    python_tools = bool(arguments and arguments[0] == "--python-tools")
    command_index = 1 if python_tools else 0
    command = arguments[command_index] if len(arguments) > command_index else ""
    required = requires_python_tools(command) if python_tools else requires_build_env(command)
    print("1" if required else "0")
    return 0


if __name__ == "__main__":
    sys.exit(main())
