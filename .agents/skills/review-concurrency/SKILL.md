---
name: review-concurrency
description: Review and validate Aobus thread safety, cancellation, executor affinity, and asynchronous lifetimes. Use for races, deadlocks, sanitizer reports, callback teardown bugs, or changes involving mutexes, atomics, threads, stop tokens, worker pools, and strands.
---

# review-concurrency

Read `doc/dev/testing/concurrency-and-sanitizers.md` completely before acting; it
routes to the authoritative style, tag, and suite references.

## Workflow

1. Record ownership, executor, and lifetime assumptions before editing.
2. Reproduce a reported failure at a deterministic synchronization point.
3. Make the smallest fix that makes those assumptions true and test the
   applicable matrix rows.
4. Run the validation prescribed by the reference, then the repository's normal
   completion gate.
