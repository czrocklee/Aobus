"""Shared ``AOBUS_TEST_CONTEXT`` parsing for the native Bazel test entries.

The Bazel macros (``bazel/testing.bzl`` and ``test/executables.bzl``) inject
the owned private-fixture markers into a test's environment. This module is
the one parser for that value, so the native smoke entrypoint and the fixture
runner agree on whitespace, duplicates and the admitted marker set. It imports
only the standard library: both entrypoints run from narrow declared Python
libraries, never the whole portal package.
"""

from __future__ import annotations

from collections.abc import Mapping

CONTEXT_ENV = "AOBUS_TEST_CONTEXT"
LINUX_CONTEXTS = frozenset({"audio", "dbus", "gtk"})
UIA_CONTEXT = "uia"
ALLOWED_CONTEXTS = LINUX_CONTEXTS | {UIA_CONTEXT}


def parse(value: str) -> frozenset[str]:
    """Read the comma-separated private-fixture markers from one value."""
    return frozenset(part.strip() for part in value.split(",") if part.strip())


def contexts(environment: Mapping[str, str]) -> frozenset[str]:
    """Read the private-fixture markers from a test environment."""
    return parse(environment.get(CONTEXT_ENV, ""))
