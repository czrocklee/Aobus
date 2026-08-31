---
name: improve-test-coverage
description: "Measure and improve C++ unit test coverage. Delegates to write-unit-test for implementation details and coverage-specific patterns."
---

# improve-test-coverage

Use this skill when the task is explicitly about coverage percentage, missing lines, or systematically filling coverage gaps.

A request to measure or report coverage is read-only. Add tests only when the user asks to improve coverage or implement the missing cases.

For coverage workflow, common Aobus coverage gaps, and execution steps, read:

- `doc/development/test/coverage-workflow.md`

For how to write each test, use the `write-unit-test` skill.

Deciding what to test and which boundaries to cover requires task-specific judgment. When implementation was requested, review the resulting tests before landing.

Use `review-concurrency` when the requested coverage is about thread safety;
line coverage and concurrency validation are separate workflows.
