---
name: use-python-lint
description: Run or review Aobus Python hygiene checks through the project portal and, when explicitly requested, fix Ruff, mypy, or formatting findings. Use when the user asks for Python linting or cleanup in the current session.
---

# Use Python Lint

## Session Opt-In

Do not activate this skill unless the user explicitly asks for Python linting, Ruff, mypy, or
Python cleanup. A request to inspect findings is read-only; edit files only when fixes were also
requested.

Use Aobus commands as the entry points for Python hygiene. Do not call `ruff` or `mypy` directly
during normal repository work; the commands own target discovery and use the project configuration in
`pyproject.toml`.

Examples use the POSIX portal. On native Windows substitute `ao.bat` for `./ao`.

Do not treat `./ao test --lint` as Python lint. That suite verifies the custom C++ clang-tidy checker
fixtures.

## Workflow

Run the smallest useful scope:

```bash
./ao tidy script/ao/core/pythoncheck.py
./ao hygiene script/ao/core/pythoncheck.py

# Only when formatting changes were requested
./ao format script/ao/core/pythoncheck.py
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
`./ao test --all` catch Python tooling regressions on Linux and Windows without touching any files.
macOS does not own the tooling suite; its `tidy` and `hygiene` commands still check Python files in
scope. `./ao hygiene` itself is not part of `./ao check`.

When files changed, finish with the completion policy in
`doc/development/test/validation-and-review.md`.
