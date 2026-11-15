"""Pure native-profile preflight and SDK mapping shared by CLI admission.

This module owns no SDK discovery, command dispatch, cache or flag registry.
Callers run it before Windows VS/SDK setup, workspace paths, tree creation or
locking; the portal owns the actual command-side wiring. Contract expectations
stay independent of the Starlark platform producers.
"""

from __future__ import annotations

from . import bazel_native as native

# "fastbuild" is the ordinary default: unoptimized target compilation that
# consumes the existing Release SDK variant on every platform.
CONFIGURATIONS = ("fastbuild", "debug", "release", "profile", "asan", "tsan")
SANITIZER_CONFIGS = ("asan", "tsan")


def validate_request(config: str, platform: str) -> None:
    """Pure preflight, safe before VS/SDK setup, tree selection or locking."""
    if config not in CONFIGURATIONS:
        raise native.BazelWorkspaceError(f"unknown Bazel configuration: {config}")
    if platform not in ("linux", "macos", "windows"):
        raise native.BazelWorkspaceError(f"unsupported native Bazel host: {platform}")
    if config == "tsan" and platform == "windows":
        raise native.BazelWorkspaceError("Windows does not support ThreadSanitizer")
    if config == "profile" and platform == "windows":
        raise native.BazelWorkspaceError("Windows does not support the sampling profile")


def compilation_config(config: str) -> str:
    """Map a native profile to the existing mode-neutral SDK configuration.

    The actual target compilation mode stays the requested profile (fastbuild
    compiles unoptimized); only the dependency SDK variant is selected here.
    fastbuild maps to the Release SDK because the governed Windows toolchain
    selects /MD outside dbg, matching the Release CRT and iterator ABI.
    """
    if config in SANITIZER_CONFIGS or config == "debug":
        return "debug"
    if config in ("fastbuild", "release", "profile"):
        return "release"
    raise native.BazelWorkspaceError(f"unknown Bazel configuration: {config}")
