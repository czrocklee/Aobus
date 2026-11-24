"""Probe the interpreter that will install the locked macOS/Windows checker wheels."""

import os
import sys
import sysconfig


def checker_arch() -> str:
    if sysconfig.get_config_var("Py_GIL_DISABLED") == 1:
        raise SystemExit("The locked Ruff/mypy wheels require standard CPython, not a free-threaded CPython build.")
    if sys.platform == "win32":
        platform = sysconfig.get_platform()
        return "x86_64" if platform == "win-amd64" else platform
    return os.uname().machine


if __name__ == "__main__":
    print(checker_arch())
