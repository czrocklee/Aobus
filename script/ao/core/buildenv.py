"""Answers which portal commands need the native Windows build environment.

Each command module declares REQUIRES_BUILD_ENV next to its NAME; ao.bat
queries this module (``python -m ao.core.buildenv <command>``) after the
Python bootstrap instead of keeping its own copy of the command list.
"""

import sys


def requires_build_env(command: str) -> bool:
    """Return True when `command` needs cl.exe and the vcpkg toolchain."""
    from ..command import COMMAND_MODULES

    for module in COMMAND_MODULES:
        if module.NAME == command:
            return bool(module.REQUIRES_BUILD_ENV)
    return False


def main(argv: list[str] | None = None) -> int:
    arguments = sys.argv[1:] if argv is None else argv
    command = arguments[0] if arguments else ""
    print("1" if requires_build_env(command) else "0")
    return 0


if __name__ == "__main__":
    sys.exit(main())
