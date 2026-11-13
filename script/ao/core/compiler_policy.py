"""Read literal toolchain policy for Python consumers and the native rc view."""

from __future__ import annotations

import ast
from typing import cast

from .paths import PROJECT_ROOT

MSVC_POLICY = "bazel/msvc_policy.bzl"
SHARED_POLICY = "bazel/compiler_policy.bzl"
_RC_BEGIN = "# BEGIN GENERATED MSVC FLAGS: bazel/msvc_policy.bzl"
_RC_END = "# END GENERATED MSVC FLAGS"


def literal(source: str, name: str) -> object:
    """Read an authored literal without executing Starlark or importing a compiler."""
    nodes = [
        node.value
        for node in ast.parse((PROJECT_ROOT / source).read_text(encoding="utf-8")).body
        if isinstance(node, ast.Assign)
        and len(node.targets) == 1
        and isinstance(node.targets[0], ast.Name)
        and node.targets[0].id == name
    ]
    if len(nodes) != 1:
        raise ValueError(f"{source} requires one literal declaration of {name}")
    return ast.literal_eval(nodes[0])


def unix_standard() -> str:
    return f"-std=c++{literal(SHARED_POLICY, 'CXX_STANDARD')}"


def llvm_copts(host: str, rtti: str, assertions: str) -> list[str]:
    """Project SDK ABI facts through the compiling toolchain's flag spelling."""
    if host == "windows":
        flags = cast(dict[str, list[str]], literal(MSVC_POLICY, "MSVC_LLVM_RTTI_COPTS"))[rtti]
        return [*flags, *cast(list[str], literal(MSVC_POLICY, "MSVC_LLVM_COPTS"))]
    flags = cast(dict[str, list[str]], literal(SHARED_POLICY, "UNIX_LLVM_RTTI_COPTS"))[rtti]
    return [
        *flags,
        *(cast(list[str], literal(SHARED_POLICY, "UNIX_LLVM_RELEASE_COPTS")) if assertions == "OFF" else []),
    ]


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
