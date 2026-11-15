"""Portal module inputs for cacheable lint actions and repository preparation."""

LINT_ACTION_SOURCES = [
    "script/ao/__init__.py",
    "script/ao/core/__init__.py",
    "script/ao/core/bazel_lint_reports.py",
    "script/ao/core/dedup.py",
    "script/ao/core/paths.py",
    "script/ao/core/tidyconfig.py",
    "script/ao/core/tidydiagnostics.py",
    "script/ao/core/tidypolicy.py",
]

# Preparation additionally imports fixture discovery; analysis never does.
LINT_POLICY_SOURCES = LINT_ACTION_SOURCES + [
    "script/ao/core/bazel_lint_action.py",
    "script/ao/core/lintfixtures.py",
]
