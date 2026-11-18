---
name: run-lint
description: Run, review, or fix requested lint, static-analysis, or formatting work across C++ (clang-tidy, analyze), Python (Ruff, mypy), Bazel (Buildifier), and workflows (actionlint). Skip during ordinary completion hygiene.
---

# Run Aobus lint and static analysis

Findings review is read-only unless fixes or formatting changes are requested;
reserve `--all` for explicitly requested whole-repository runs.

- Enter through `./ao check lint`, `./ao check analyzer`, `./ao check format`,
  or `./ao format`; requested workflow lint runs through `./ao check lint`.
  Do not invoke `clang-tidy`, `Ruff`, `mypy`,
  or `Buildifier` directly.
- Route rules through [linting policy](../../../doc/development/linting.md):
  [triage](../../../doc/development/linting.md#triage),
  [`NOLINT` governance](../../../doc/development/linting.md#repository-wide-suppression-governance),
  [Objective-C diagnostics](../../../doc/development/linting.md#objective-c-diagnostics),
  [workflow lint](../../../doc/development/linting.md#github-actions-workflow-lint),
  and the [tooling runner](../../../doc/development/test/test-suite.md#tooling-and-lint-fixtures).
  For custom `aobus-*` checker implementation, load `develop-lint-checker`.
- Silent `--tidy-check` traps
  ([scope behavior](../../../doc/development/linting.md#scope-behavior),
  [Bazel-backed reports](../../../doc/development/linting.md#bazel-backed-reports)):
  `./ao check lint --tidy-check <name>` selects only C++ diagnostics, exits 0 without diagnostics on
  `test/` files when `<name>` is disabled in `RELAXED` mode, and cannot run on
  `test/lint/fixture/` files (no `cc_*` compile owner).
