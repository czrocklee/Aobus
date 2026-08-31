---
name: review-concurrency
description: Review and validate Aobus thread safety, cancellation, executor affinity, and asynchronous lifetimes. Use for races, deadlocks, sanitizer reports, callback teardown bugs, or changes involving mutexes, atomics, threads, stop tokens, worker pools, and strands.
---

# review-concurrency

Review is read-only unless the user also asks for fixes.

Read `doc/development/test/concurrency-and-sanitizer.md` completely before acting; it
routes to the authoritative style, tag, and suite references.

## Workflow

1. Record ownership, executor, and lifetime assumptions before assessing the code.
2. Trace accesses, suspension points, callbacks, cancellation, and teardown against those
   assumptions.
3. When a failure was reported, reproduce it at a deterministic synchronization point.
4. Report findings with the violated assumption and concrete evidence.
5. When fixes were requested, make the smallest correction that restores the
   assumption and test the applicable matrix rows.
6. For code changes, run the validation prescribed by the reference, then the
   repository's normal completion gate.
