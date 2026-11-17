"""Maintain the generated MSVC repository-environment region in ``.bazelrc``.

``bazel/msvc_policy.bzl`` stays the semantic owner of the MSVC flags; this
module only projects that authored literal into the derived rc region and
rejects hand edits outside it.
"""

from __future__ import annotations

from typing import cast

from ao.core.paths import PROJECT_ROOT
from ao.policy.compiler_policy import MSVC_POLICY, literal

_RC_BEGIN = "# BEGIN GENERATED MSVC FLAGS: bazel/msvc_policy.bzl"
_RC_END = "# END GENERATED MSVC FLAGS"


def msvc_rc_region() -> str:
    environment = cast(dict[str, list[str]], literal(MSVC_POLICY, "MSVC_REPOSITORY_ENV"))
    return "\n".join(
        [
            _RC_BEGIN,
            *(
                "build:windows --repo_env=" + name + "=" + ":".join(flag.replace(":", "%:") for flag in flags)
                for name, flags in sorted(environment.items())
            ),
            _RC_END,
        ]
    )


def format_msvc_rc(*, check: bool) -> int:
    """Maintain only the derived region; leave native startup/profile wiring alone."""
    path = PROJECT_ROOT / ".bazelrc"
    text = path.read_text(encoding="utf-8")
    if text.count(_RC_BEGIN) != 1 or text.count(_RC_END) != 1:
        raise ValueError(".bazelrc requires one generated MSVC flags region")
    before, region = text.split(_RC_BEGIN, 1)
    _, after = region.split(_RC_END, 1)
    environment = cast(dict[str, list[str]], literal(MSVC_POLICY, "MSVC_REPOSITORY_ENV"))
    for line in (before + after).splitlines():
        line = line.strip()
        if line.startswith("#"):
            continue
        for name in environment:
            if name in line and line != "common --repo_env==" + name:
                raise ValueError(
                    f"MSVC flags must be edited in {MSVC_POLICY}, not outside the generated rc region: {name}"
                )
    expected = before + msvc_rc_region() + after
    if text == expected:
        return 0
    if check:
        print(".bazelrc MSVC flags are stale; run ./ao format bazel/msvc_policy.bzl")
        return 1
    path.write_text(expected, encoding="utf-8", newline="\n")
    return 0


def selects_msvc_rc(files: list[str]) -> bool:
    selected = {str((PROJECT_ROOT / name).absolute()) for name in files}
    return any(str((PROJECT_ROOT / name).absolute()) in selected for name in (MSVC_POLICY, ".bazelrc"))
