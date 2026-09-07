---
name: develop-lint-checker
description: Create, debug, or extend Aobus custom Clang-Tidy checkers and their integration fixtures.
---

# Develop an Aobus lint checker

Checker review or diagnosis is read-only unless implementation or fixes are
requested. A checker implementation request authorizes the required lint runs.

`tool/lint/check/` owns checkers and AST helpers. Follow the matching check's
namespace and register new checks in `tool/lint/AobusLintModule.cpp` and
`tool/lint/CMakeLists.txt`.

Use `doc/development/test/test-suite.md` for fixture markers and runner behavior.
Fixtures live under `test/integration/lint/fixture/<check-alias>/`.
`./ao test --lint` owns diagnostic, FixIt, and fixed-output compilation checks.
Extend the owning fixture for new behavior; wording-only edits can reuse it and
inspect the emitted text because markers do not assert diagnostic wording.

For symbol identity, macro safety, and uncertain AST shapes, consult
`doc/development/linting.md` under **Custom checker development**. Use the native
compile database and toolchain; the portal's `--no-build --check <alias>` route
allows focused fixture diagnosis with an already prepared plugin.

Completion follows `doc/development/test/validation-and-review.md`.
