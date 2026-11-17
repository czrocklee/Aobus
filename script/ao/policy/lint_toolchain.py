"""Caller-verified native checker toolchain record for fixture execution.

The portal preflight verifies the checker host, plugin, resource directory and
SDK root once; the declared lint fixture helper and its runner only consume
that record. This module is a pure data contract: it performs no SDK
discovery, build-directory resolution, Git access or process exit, so a
declared Python script action can import it without the portal tidy engine.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class TidyToolchain:
    """A caller-verified checker host and resources, not a product build cache."""

    clang_tidy: str
    plugin: Path | None
    resource_dir: Path | None
    expected_llvm_version: str | None = None
    sdk_root: Path | None = None
