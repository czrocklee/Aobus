"""Read literal toolchain policy for Python consumers."""

from __future__ import annotations

import ast
from typing import cast

from ao.core.paths import PROJECT_ROOT

MSVC_POLICY = "bazel/msvc_policy.bzl"
SHARED_POLICY = "bazel/compiler_policy.bzl"


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
