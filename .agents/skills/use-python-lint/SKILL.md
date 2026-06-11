---
name: use-python-lint
description: Run Aobus Python hygiene checks through ./ao tidy and ./ao format. Use when the user explicitly asks for Python linting, Ruff, mypy, or Python cleanup in the current session.
---

# Use Python Lint

## Session Opt-In

Use Aobus commands as the entry points for Python hygiene. Do not call `ruff` or `mypy` directly
during normal repository work; the commands own target discovery and use the project configuration in
`pyproject.toml`.

Do not treat `./ao test --lint` as Python lint. That suite verifies the custom C++ clang-tidy checker
fixtures.

## Workflow

Run the smallest useful scope:

```bash
./ao tidy script/ao/core/pythoncheck.py
./ao format script/ao/core/pythoncheck.py
./ao hygiene script/ao/core/pythoncheck.py
```

`./ao tidy` runs Python Ruff checks and mypy for Python files in scope. `./ao format` runs Ruff format
for Python files in scope. There is no separate public Python lint command; `./ao tidy` is the
Python-check entry point. For C++ files in the same scope, `./ao tidy` runs clang-tidy (see the
use-clang-tidy skill).

## Gate Policy

`./ao hygiene` aggregates `./ao format --check` and `./ao tidy`; it is the check-only commit gate
and never modifies files. Formatting timing and the mid-session prohibition are governed by
AGENTS.md Rule 8.

Ruff and mypy also run read-only inside `./ao test --tooling`, so `./ao check` and
`./ao test --all` catch Python tooling regressions without touching any files. `./ao hygiene`
itself is not part of `./ao check`.
