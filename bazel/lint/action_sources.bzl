"""Declared graph and policy inputs for lint actions and SDK preparation."""

LINT_ACTION_SOURCES = [
    "script/ao/__init__.py",
    "script/ao/core/__init__.py",
    "script/ao/graph/__init__.py",
    "script/ao/policy/__init__.py",
    "script/ao/graph/bazel_lint_schema.py",
    "script/ao/core/paths.py",
    "script/ao/policy/tidy_config.py",
    "script/ao/policy/tidy_diagnostics.py",
    "script/ao/policy/tidy_policy.py",
]

# Preparation additionally imports fixture discovery; analysis never does.
LINT_POLICY_SOURCES = LINT_ACTION_SOURCES + [
    "script/ao/graph/bazel_lint_action.py",
    "script/ao/policy/lint_fixtures.py",
]

# Fixtures execute caller-verified tools, not portal discovery or preparation.
LINT_FIXTURE_SOURCES = LINT_POLICY_SOURCES + [
    "script/ao/core/command_registry.py",
    "script/ao/core/phases.py",
    "script/ao/core/proc.py",
    "script/ao/graph/bazel_lint_fixture.py",
    "script/ao/graph/lint_test.py",
    "script/ao/policy/compiler_policy.py",
    "script/ao/policy/lint_toolchain.py",
    "script/ao/policy/windows_llvm_sdk.py",
    "script/ao/windows-llvm-sdk.json",
    "//bazel:compiler_policy.bzl",
    "//bazel:msvc_policy.bzl",
]
