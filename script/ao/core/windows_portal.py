"""Windows preparation and dispatch in the selected portal interpreter."""

import json
import os
import subprocess
import sys
from pathlib import Path

from ..__main__ import dispatch, parse_invocation
from . import buildenv
from .paths import PROJECT_ROOT


def prepare_build_environment() -> None:
    """Acquire CMD-only VS setup without sending application arguments through CMD."""
    environment = dict(os.environ)
    environment["_AOBUS_ENV_SCRIPT"] = str(PROJECT_ROOT / "script" / "ao" / "windows-build-env.bat")
    environment["_AOBUS_ENV_PYTHON"] = sys.executable
    result = subprocess.run(
        'cmd.exe /d /v:off /s /c ""%_AOBUS_ENV_SCRIPT%""',
        executable=environment.get("COMSPEC", "cmd.exe"),
        env=environment,
        capture_output=True,
        check=False,
    )
    if result.stderr:
        if hasattr(sys.stderr, "buffer"):
            sys.stderr.buffer.write(result.stderr)
            sys.stderr.buffer.flush()
        else:
            sys.stderr.write(result.stderr.decode("utf-8", errors="replace"))
    if result.returncode:
        raise subprocess.CalledProcessError(result.returncode, result.args)
    try:
        captured = json.loads(result.stdout)
    except ValueError as exc:
        # Never echo this channel: it contains the complete process environment.
        raise RuntimeError("Visual Studio setup did not return a valid environment") from exc
    if (
        not isinstance(captured, dict)
        or not captured
        or any(not isinstance(key, str) or not isinstance(value, str) for key, value in captured.items())
    ):
        raise RuntimeError("Visual Studio setup did not return a string environment")
    captured.pop("_AOBUS_ENV_SCRIPT", None)
    captured.pop("_AOBUS_ENV_PYTHON", None)
    os.environ.clear()
    os.environ.update(captured)


def main(argv: list[str] | None = None) -> int:
    working_directory = os.environ.pop("_AOBUS_PORTAL_WORKING_DIRECTORY", str(PROJECT_ROOT))
    os.environ.pop("_AOBUS_UNC_MAPPING", None)
    # Only the legacy Unix launcher owns the scope-file protocol. Windows keeps
    # this invocation's resolved scope on its parsed Namespace instead.
    os.environ.pop("AOBUS_PREFLIGHT_SCOPE", None)
    args = parse_invocation(list(sys.argv[1:] if argv is None else argv))
    if args is None:
        return 0
    try:
        required = buildenv.requires_parsed_build_env(args)
        if required:
            prepare_build_environment()
        os.environ["PATH"] = str(Path(sys.executable).parent) + os.pathsep + os.environ.get("PATH", "")
        os.chdir(working_directory)
    except (OSError, RuntimeError, subprocess.CalledProcessError) as exc:
        print(f"Aobus Windows preparation failed: {exc}", file=sys.stderr)
        return exc.returncode if isinstance(exc, subprocess.CalledProcessError) else 1
    return dispatch(args, build_environment_required=required)


if __name__ == "__main__":
    raise SystemExit(main())
