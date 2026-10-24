"""Lightweight command identities shared by dispatch and phase validation.

Module names are the registration authority. Public command names use hyphens
where their Python module names use underscores; command loading verifies that
each module declares the same name. Reading these identities loads no commands.
"""

COMMAND_MODULE_NAMES = (
    "build",
    "check",
    "test",
    "test_audit",
    "name_audit",
    "coverage",
    "deps",
    "doctor",
    "setup",
    "docs",
    "tidy",
    "analyze",
    "format",
    "hygiene",
    "perf",
    "run",
)
COMMAND_NAMES = frozenset(name.replace("_", "-") for name in COMMAND_MODULE_NAMES)
