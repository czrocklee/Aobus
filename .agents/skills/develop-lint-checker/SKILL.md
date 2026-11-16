---
name: develop-lint-checker
description: Create, debug, or extend custom Aobus Clang-Tidy checkers in tool/lint/ and their integration fixtures in test/lint/fixture/. Do not load for running ./ao tidy on product code or triaging ordinary lint findings.
---

# Develop an Aobus lint checker

Checker review or diagnosis is read-only unless code changes are requested; an
implementation request authorizes the required lint runs.

- Sources and shared AST helpers live in [`tool/lint/check/`](../../../tool/lint/check);
  register new checks in [`AobusLintModule.cpp`](../../../tool/lint/AobusLintModule.cpp)
  and the `lint_checker` `srcs` list in [`tool/lint/BUILD.bazel`](../../../tool/lint/BUILD.bazel)
  (not `tool/lint/build.bzl`).
- Route AST matcher traps, lookup proofs, and marker rules through
  [checker development](../../../doc/development/lint/checker-development.md)
  ([fixture contract](../../../doc/development/lint/checker-development.md#fixture-contract)),
  [naming checks](../../../doc/development/lint/naming-checks.md), and
  [test suites](../../../doc/development/test/test-suite.md#tooling-and-lint-fixtures).
- Silent workflow traps ([fixture contract](../../../doc/development/lint/checker-development.md#fixture-contract),
  [Bazel-backed reports](../../../doc/development/linting.md#bazel-backed-reports)):
  registered `aobus-*` checks and `<check-alias>/` directories with non-blank
  supported files under [`test/lint/fixture/`](../../../test/lint/fixture) must
  match 1:1; validate fixtures with
  `bazel test //test/lint:lint_test --nocache_test_results` (excluded from
  `//:tests` and batch `ao tidy`), and keep
  `./ao tidy --check <alias> <owned-source>` to compile-owned TUs where
  `<alias>` is not `RELAXED`-disabled.
