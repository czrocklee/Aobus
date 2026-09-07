---
name: use-python-lint
description: Run or review requested Aobus Python linting or formatting, and fix Ruff/mypy findings when asked.
---

# Use Python lint

A findings review is read-only unless fixes are requested. Required completion
hygiene does not require this skill or a separate lint request.

Use `./ao tidy <scope>` for Ruff/mypy and `./ao format <scope>` for requested
formatting corrections; on Windows use `ao.bat`. The portal owns discovery and
locked tools. Do not call Ruff or mypy directly during normal repository work.
`./ao test --lint` is the C++ checker-fixture suite, not Python lint.

`./ao test --tooling` owns Python tooling validation on Linux and Windows.
macOS checks Python through scoped tidy/hygiene and has no tooling suite.

Suppression policy lives in `doc/development/linting.md`; completion, authorized
format corrections, and result reuse live in
`doc/development/test/validation-and-review.md`.
