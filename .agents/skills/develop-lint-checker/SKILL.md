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
`doc/development/lint/checker-development.md`; naming-proof details are in
`doc/development/lint/naming-checks.md`. Use the native
compile database and toolchain. `./ao tidy --no-build --check <alias> <source>`
allows focused diagnostic inspection with a prepared plugin and compile database;
it does not run the fixture's diagnostic, FixIt, or compilation assertions.

Completion follows `doc/development/test/validation-and-review.md`.
