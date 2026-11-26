---
id: development.test.concurrency-and-sanitizer
---

# Concurrency and sanitizer validation

Use this reference for cross-thread state, callback affinity, cancellation races,
and teardown while work is in flight. Production synchronization rules belong
to [C++ coding style](../coding-style.md); test tag grammar and command semantics belong to
[test naming and assertions](naming-and-assertion.md) and [test suites](test-suite.md).

## Review model

For each shared object or asynchronous callback, identify:

- Its owner and the operation that ends its lifetime.
- The executor or thread allowed to access it.
- The synchronization used by every other access.
- The cancellation checkpoint before owner access after suspension.
- The shutdown step that proves producers and queued callbacks have quiesced.

Then trace outward calls, suspension points, and teardown in that order. Apply
the threading rules in `doc/development/coding-style.md`, especially callback invocation
outside locks and the distinction between requesting stop and joining work.
Ground every review finding in the specific ownership, executor-affinity,
synchronization, or quiescence contract it violates.

## Test matrix

Cover the applicable boundaries with observable outcomes:

| Boundary                              | Required observation                                                                                                                                                                                  |
| ------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Cancel before start                   | The task body or first side effect does not run.                                                                                                                                                      |
| Cancel while suspended                | Work exits without completion or a user-visible error.                                                                                                                                                |
| Completion races with cancel          | Exactly one terminal path wins.                                                                                                                                                                       |
| Callback queued, then owner destroyed | The callback becomes a safe no-op.                                                                                                                                                                    |
| Callback requests owner teardown      | By default, teardown is deferred until publication unwinds and synchronous destruction is rejected; only an explicitly documented stronger reentrant lifetime model permits an exception (see below). |
| Shutdown with active work             | Producers stop and callbacks quiesce before the state they use is destroyed.                                                                                                                          |
| Repeated cancellation                 | Cancellation and teardown are idempotent.                                                                                                                                                             |
| Executor hop                          | The callback runs on its documented executor.                                                                                                                                                         |
| Multiple workers                      | The contract holds with more than one worker.                                                                                                                                                         |

The [threading and callback rules](../coding-style.md#threading-and-callbacks)
allow a stronger lifetime model only where the API documents it.
For the [BackendProvider exception](../../system/playback/audio-execution.md#backend-lifetime-and-properties),
verify that callback-origin facade destruction leaves independently retained
state to complete quiescence after the callback returns.
Callback-origin shutdown does not wait for the invoking callback to return.
This does not guarantee that the shutdown call returns immediately or avoids
other blocking work. External shutdown callers still wait for the same
provider-quiescence completion boundary.
This exception does not relax deferred teardown required by Player, Engine, or
GTK owner APIs.

Timer-like components additionally cover expiry winning, cancellation winning,
their collision, and obsolete reschedule generations.

Prefer controlled executors, barriers, latches, and captured callbacks. A
timeout may guard against hangs, but elapsed time is not proof.

Tag a case `[concurrency]` when any section asserts one of these boundaries
and, in production, that boundary crosses threads or processes: the component
is reached from another thread, it hands the work to a worker, audio, or
monitor thread and back through an executor, or another live process contends
for the same resource. The test may still drive the interleaving from
one thread; a controlled schedule that rejects a stale worker completion
qualifies. A thread that only toolkit internals use, such as GIO's, does not
count when Aobus code sees just owner-thread callbacks.

When the component is confined to one owner thread and the case asserts
ordering or lifetime across later executor or main-loop turns — a queued,
idle, or timer callback, a stale owner-thread completion, deferred teardown,
or a retained handle after retirement — it is still a real asynchronous
contract, but it takes no marker: `[concurrency]` is only for boundaries that
cross threads or processes. Synchronous reentrancy and notification, an
incidental worker, a coroutine that merely returns a value, and destruction
with nothing outstanding take no marker either. Added repetition is not a
marker; [the test-suite reference](test-suite.md) owns native repetition, and
[reviewing existing tests](validation-and-review.md#reviewing-existing-tests)
owns per-case tag audits.
A concurrency filter must exclude hidden `[.manual]` probes with `~[.]`.

## Validation

For C++ concurrency-sensitive changes, run the affected cases, then run their owning tests under
[ThreadSanitizer](#sanitizer-findings) with fresh execution on affected supported hosts
as part of [completion validation](validation-and-review.md#validation). Use
repetition only after a deterministic regression exists; [the test-suite reference](test-suite.md) owns suite
selection and native `--runs_per_test` behavior.

## Sanitizer findings

Reproduce a sanitizer finding on the affected native host before expanding validation.
For a C++ memory-safety fix, run the affected tests under AddressSanitizer on affected
supported hosts, following [validation and review](validation-and-review.md).
Reading this reference for a review or Python process change does not require a
C++ sanitizer build.

```bash
bazel test --config=asan --nocache_test_results //test:utility_test
bazel test --config=tsan --nocache_test_results //test:async_test
```

Choose the target that owns the regression. These profiles do not run normal completion
or lint. Use `//:asan_tests` or `//:tsan_tests` for broader coverage when the affected
boundary spans suites. `--nocache_test_results` requests new test executions.
A cached result cannot sample another interleaving or heap layout, even when
the instrumented binary is unchanged.

Linux and macOS combine AddressSanitizer and UndefinedBehaviorSanitizer. The test
profile sets `halt_on_error=1` and enables UBSan stack traces so a report fails the test. Windows uses MSVC AddressSanitizer for Aobus
translation units. Dependencies from the normal vcpkg triplet remain
uninstrumented, so the Windows build disables MSVC STL container annotations at
that binary boundary. MSVC provides neither UndefinedBehaviorSanitizer nor
ThreadSanitizer, and resumable coroutine bodies are not fully instrumented.
Bazel rejects `--config=tsan` on Windows.

The Linux Nix GTK dependency closure is not TSan-instrumented, so TSan cannot observe its internal synchronization even though it observes calls into intercepted runtime functions.
[The test suppression list](../../../test/sanitizer/tsan.supp) treats the reviewed UI dependency modules as opaque by using only `called_from_lib` rules.
Those rules ignore interceptor accesses originating in the named modules while leaving Aobus code instrumented, including Aobus callbacks entered from GTK.
The Linux test binaries embed these rules; they are inert in binaries that do not load the named modules.

Before adding or widening a suppression:

1. Reproduce the narrowest public contract.
2. Compare with the unmodified base revision in a separate build directory.
3. Classify relevant frames as project-owned or dependency-owned.
4. Add an exact `called_from_lib` module only for an uninstrumented dependency; never suppress an Aobus binary, libc, libpthread, libstdc++, or libtsan.
5. Verify the focused report, `//test:gtk_test`, and `//:tsan_tests` under `--config=tsan --nocache_test_results`.

## Explicit Bazel sanitizer route

See [sanitizer profiles](../bazel.md#sanitizer-profiles) for graph selection and admission rules.

`check contracts --config asan|tsan` checks only the Debug compile contracts of the
sanitizer-configured graph, not runtime behavior. The [profile canary](../../../test/helper/SanitizerProfile.cpp) uses compile-time assertions to check instrumentation in one first-party translation unit, not every per-action flag.

`check lint` accepts fastbuild, Debug, or Release, not sanitizer profiles;
`./ao setup deps` does not select instrumentation. Explicit native
`bazel build`, `test`, or `run` requests select the profile through the
repository rc, not a compilation mode:
`bazel build -c dbg --config=asan //:asan_tests` is correct and `-c asan` is
invalid. Building a test does not execute it. The sanitizer suites include the native
backend, exclude manual GUI and performance harnesses, and leave hidden hardware
cases opt-in. On Linux, context-bearing tests start only their declared private fixtures.

An instrumented test run does not replace the [completion gate](../bazel.md#completion-gate). Where the change requires Release evidence, validate Release separately.
