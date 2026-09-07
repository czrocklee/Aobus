---
name: review-concurrency
description: Review Aobus races, cancellation, executor affinity, or asynchronous teardown when those contracts change or failures implicate them.
---

# Review Aobus concurrency

Review is read-only unless fixes are requested. Establish ownership, executor,
and lifetime assumptions, then trace accesses, suspension, cancellation, and
teardown against them. Findings need a concrete violated contract.

Use the review model and applicable test-matrix rows in
`doc/development/test/concurrency-and-sanitizer.md`; read its sanitizer guidance
when a sanitizer is involved.

For changes, follow `doc/development/test/validation-and-review.md`.
C++ sanitizer gates and native Python process regressions validate different
runtimes; select the affected one.
