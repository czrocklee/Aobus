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

## Test matrix

Cover the applicable boundaries with observable outcomes:

| Boundary | Required observation |
|---|---|
| Cancel before start | The task body or first side effect does not run. |
| Cancel while suspended | Work exits without completion or a user-visible error. |
| Completion races with cancel | Exactly one terminal path wins. |
| Callback queued, then owner destroyed | The callback becomes a safe no-op. |
| Callback requests owner teardown | By default, teardown is deferred until publication unwinds and synchronous destruction is rejected; only an explicitly documented stronger reentrant lifetime model permits an exception (see below). |
| Shutdown with active work | Producers stop and callbacks quiesce before the state they use is destroyed. |
| Repeated cancellation | Cancellation and teardown are idempotent. |
| Executor hop | The callback runs on its documented executor. |
| Multiple workers | The contract holds with more than one worker. |

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
marker; [the test-suite reference](test-suite.md) owns `--repeat` behavior.
The `--concurrency` gate skips hidden `[.manual]` probes.

## Validation

Run the concurrency gate prescribed by [validation and review](validation-and-review.md). Use
repetition only after a deterministic regression exists; [the test-suite reference](test-suite.md) owns suite
selection and `--repeat` behavior.

## Sanitizer findings

Reproduce a sanitizer finding on the affected native host before expanding validation.
For a C++ memory-safety fix, run the full native AddressSanitizer gate on affected
supported hosts, following [validation and review](validation-and-review.md).
Reading this reference for a review or Python process change does not require a
C++ sanitizer build.

```bash
# Linux and macOS
./ao check --asan
```

```bat
ao.bat check --asan
```

A sanitizer check runs the full normal Debug completion first, then a separate
managed sanitizer profile in the same selected tree; a
Debug result is never relabeled as a sanitized one
([sanitizer profiles](../bazel.md#sanitizer-profiles)). Cached sanitizer
results are admitted unless `--fresh` is given; validate a memory-safety or
concurrency fix with `--fresh`.

Linux and macOS combine AddressSanitizer and UndefinedBehaviorSanitizer. The
managed runtime preserves caller-supplied `UBSAN_OPTIONS` but forces
`halt_on_error=1` and `print_stacktrace=1`, so an undefined-behavior report
cannot leave the gate green. Windows uses MSVC AddressSanitizer for Aobus
translation units. Dependencies from the normal vcpkg triplet remain
uninstrumented, so the Windows build disables MSVC STL container annotations at
that binary boundary. MSVC provides neither UndefinedBehaviorSanitizer nor
ThreadSanitizer, and resumable coroutine bodies are not fully instrumented.
Windows `--tsan` therefore fails before tree or SDK work rather than silently
running an unsanitized gate.

The Linux TSan group covers core, backend, and GTK; the macOS group covers core
and backend:

```bash
./ao check --tsan
```

The Linux Nix GTK dependency closure is not TSan-instrumented, so TSan cannot observe its internal synchronization even though it observes calls into intercepted runtime functions.
`script/ao/tsan.supp` treats the reviewed UI dependency modules as opaque by using only `called_from_lib` rules.
Those rules ignore interceptor accesses originating in the named modules while leaving Aobus code instrumented, including Aobus callbacks entered from GTK.
They are injected for every TSan run but are inert in binaries that do not load the named modules.
When `TSAN_OPTIONS` already names a suppression file, the test portal merges its contents with the governed project rules and passes one process-local combined file to TSan so neither source silently replaces the other.

Before adding or widening a suppression:

1. Reproduce the narrowest public contract.
2. Compare with the unmodified base revision in a separate build directory.
3. Classify relevant frames as project-owned or dependency-owned.
4. Add an exact `called_from_lib` module only for an uninstrumented dependency; never suppress an Aobus binary, libc, libpthread, libstdc++, or libtsan.
5. Verify the focused report, the full GTK TSan suite, and `./ao check --tsan`.

## Explicit Bazel sanitizer route

The Bazel graph exposes the same sanitizer families as explicit,
scoped instrumentation runs; [Bazel development](../bazel.md#sanitizer-and-coverage-instrumentation)
owns the profile design, admission rules and command surface. The managed portal
commands are:

```bash
./ao test --asan
./ao test --tsan
./ao verify --config asan
```

`verify --config asan|tsan` checks only the Debug compile contracts of the
sanitizer-configured graph. Instrumentation is proven by the [sanitizer
profile canary](../../../test/core/SanitizerProfileTest.cpp) in
`//test:utility_test`: a sanitizer run whose compiler did not instrument it, or
a normal run that is instrumented, fails to build. It proves one first-party
translation unit per profile; per-action flags such as frame pointers,
non-recovering UBSan and the MSVC `/Z7`/`/MDd` policies are no longer
independently verified ([sanitizer profiles](../bazel.md#sanitizer-profiles)).

`./ao build`, `./ao run` and `./ao check` also accept the profiles;
`./ao hygiene` and `./ao deps provision` do not. Managed
`./ao test --asan/--tsan` runs a fixed graph-owned scope and rejects labels or
forwarded Bazel arguments that would narrow it; explicit targets are only
for unmanaged `./ao build`/`./ao run` use at the caller's own runtime. Bare Bazel
commands select the profile through the repository rc, not a compilation
mode: `bazel build -c dbg --config=asan //:asan_tests` is correct and
`-c asan` is invalid. A bare run gets none of the managed runtime admission
below; the caller owns its runtime.

Scope: `//:asan_tests` covers Core, the application layer, CLI, TUI, Linux GTK
and Linux system media; `//:tsan_tests` covers Core, the application layer and
Linux GTK. The managed run adds the `//:check_native_tests` backend roots on
every host; Windows TSan is rejected before configuration exactly as above.
A raw `//:asan_tests` invocation alone excludes the native backend suite.
Manual GUI and performance targets and hidden hardware cases are outside the
sanitizer scope.

The managed runtime enforces the same report discipline this reference
requires, plus stricter positive exit codes: `halt_on_error=1` is forced, UBSan
reports print stack traces, and other caller-supplied options — including
container-annotation settings — are preserved. Caller TSan suppression files
are merged with the governed project rules into one immutable file without
adding suppressions; Linux LSAN keeps the existing replace contract. Caller
ASan, UBSan and macOS LSan suppression files are each frozen in place as
copies, preserving option order. The frozen copies are content-addressed, so
a changed rule file never reuses a cached result.

This managed Bazel route parses compiler-rt options before filtering or freezing
rules. Colons, commas, spaces, tabs and CR/LF separate options outside matching
single or double quotes; backslashes are literal, not shell escapes. Quote values that
contain separators, for example `suppressions="C:\path with spaces\rules.supp"`.
Frozen suppression and symbolizer paths are quoted when needed. Malformed
options and values that cannot be quoted without unsupported escaping fail
before tests run; they are never silently discarded or kept as mutable rule
references.

Tests run locally against frozen runtime inputs under
the private Linux display/session-bus/PipeWire context; see
[Bazel development](../bazel.md#sanitizer-and-coverage-instrumentation) for the
frozen-rc, service-context and symbolizer contracts.

The dependency-closure limits above apply unchanged: the Linux GTK closure is
not TSan-instrumented, and Windows dependencies from the normal vcpkg triplet
remain uninstrumented. A scoped sanitizer run is not the whole completion
ownership gate; the ordinary Debug/Release completion
check and its lint ownership are separate and still required.
