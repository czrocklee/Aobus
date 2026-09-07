---
name: improve-test-coverage
description: Measure Aobus C++ coverage or improve explicitly requested coverage gaps.
---

# Improve Aobus test coverage

Use `doc/development/test/coverage-workflow.md` for measurement, supported
platforms, scope selection, and gap analysis. A measurement request does not
authorize adding tests; when implementation is requested, use `write-unit-test`.

Line coverage does not establish thread safety. For concurrency contracts, use
`review-concurrency` and its validation route.
