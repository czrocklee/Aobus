# Concurrency and sanitizer validation

Use this reference for cross-thread state, callback affinity, cancellation races,
and teardown while work is in flight. Production synchronization rules belong
to `doc/dev/coding-style.md`; test tag grammar and command semantics belong to
`naming-and-assertions.md` and `test-suites.md`.

## Review model

For each shared object or asynchronous callback, identify:

- Its owner and the operation that ends its lifetime.
- The executor or thread allowed to access it.
- The synchronization used by every other access.
- The cancellation checkpoint before owner access after suspension.
- The shutdown step that proves producers and queued callbacks have quiesced.

Then trace outward calls, suspension points, and teardown in that order. Apply
the threading rules in `doc/dev/coding-style.md`, especially callback invocation
outside locks and the distinction between requesting stop and joining work.

## Test matrix

Cover the applicable boundaries with observable outcomes:

| Boundary | Required observation |
|---|---|
| Cancel before start | The task body or first side effect does not run. |
| Cancel while suspended | Work exits without completion or a user-visible error. |
| Completion races with cancel | Exactly one terminal path wins. |
| Callback queued, then owner destroyed | The callback becomes a safe no-op. |
| Callback requests owner teardown | Teardown is deferred until publication unwinds; synchronous destruction is rejected by contract. |
| Shutdown with active work | Producers stop and callbacks quiesce before destruction. |
| Repeated cancellation | Cancellation and teardown are idempotent. |
| Executor hop | The callback runs on its documented executor. |
| Multiple workers | The contract holds with more than one worker. |

Timer-like components additionally cover expiry winning, cancellation winning,
their collision, and obsolete reschedule generations.

Prefer controlled executors, barriers, latches, and captured callbacks. A
timeout may guard against hangs, but elapsed time is not proof. Tag applicable
tests `[concurrency]`; reserve `[stress]` for additional repetition or schedule
exploration. See `naming-and-assertions.md` for the permitted tag forms.

## Validation

Run the concurrency gate prescribed by `validation-and-review.md`. Use repetition
only after a deterministic regression exists; `test-suites.md` owns suite
selection and `--repeat` behavior.

## Sanitizer findings

The green Linux TSan gate currently covers core. Full GTK has baseline GLib/GIO
reports, so investigate project-owned GTK concurrency with an explicit focused
run rather than broad library-symbol suppressions:

```bash
./ao test --gtk --tsan "[concurrency]"
```

Before suppressing a report:

1. Reproduce the narrowest public contract.
2. Compare with the unmodified base revision in a separate build directory.
3. Classify relevant frames as project-owned or dependency-owned.
4. Prefer a clean-suite boundary or focused test over a broad suppression.
